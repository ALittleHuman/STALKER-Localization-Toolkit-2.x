#include "xrkit/engine/fs/squashfs.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/plugin/registry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace xrkit::engine::fs {
namespace {

// 元数据块解压后恒为 8 KiB（**不是** block_size=128 KiB —— 别混）
constexpr size_t kMetaBlockSize = 8192;
constexpr uint32_t kMagicLE = 0x73717368u;  // "hsqs"
constexpr uint32_t kMagicBE = 0x68737173u;  // "sqsh"
constexpr size_t kCacheBlocks = 256;

constexpr uint16_t kTypeDir = 1;
constexpr uint16_t kTypeFile = 2;
constexpr uint16_t kTypeSlink = 3;
constexpr uint16_t kTypeDirX = 8;
constexpr uint16_t kTypeFileX = 9;
constexpr uint16_t kTypeSlinkX = 10;

uint16_t rd16(const uint8_t* p, bool be) {
    return be ? static_cast<uint16_t>((p[0] << 8) | p[1])
              : static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t* p, bool be) {
    if (be) {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t* p, bool be) {
    if (be) {
        return (static_cast<uint64_t>(rd32(p, true)) << 32) | rd32(p + 4, true);
    }
    return static_cast<uint64_t>(rd32(p, false)) | (static_cast<uint64_t>(rd32(p + 4, false)) << 32);
}

class FileReader {
public:
    bool open(const std::string& path, std::string& err) {
        // 路径一律按 UTF-8 传入，碰文件系统时才转本地宽字符（否则西里尔目录找不到）
        const std::filesystem::path fs_path = core::path_from_utf8(path);
        std::error_code ec;
        const auto size = std::filesystem::file_size(fs_path, ec);
        if (ec) {
            err = "取文件大小失败：" + ec.message();
            return false;
        }
        size_ = size;
        in_.open(fs_path, std::ios::binary);
        if (!in_) {
            err = "打不开文件：" + path;
            return false;
        }
        return true;
    }

    uint64_t size() const { return size_; }

    bool read_at(uint64_t off, void* dst, size_t n, std::string& err) const {
        if (off > size_ || n > size_ - off) {
            err = "读取越界（off=" + std::to_string(off) + " n=" + std::to_string(n) +
                  " size=" + std::to_string(size_) + "）";
            return false;
        }
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        if (!in_) {
            err = "seek 失败";
            return false;
        }
        in_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        if (in_.gcount() != static_cast<std::streamsize>(n)) {
            err = "短读";
            return false;
        }
        return true;
    }

private:
    mutable std::ifstream in_;
    uint64_t size_ = 0;
};

struct MetaBlock {
    std::vector<uint8_t> data;
    uint64_t next = 0;  // 下一块的绝对偏移 = 本块 + 2 + comp_size
};

struct Inode {
    uint16_t type = 0;
    uint64_t size = 0;       // DIR: 目录表字节数；FILE: 文件大小；SLINK: 目标长度
    uint64_t dir_block = 0;  // DIR/DIR_X：目录表内偏移（**不是**块号）
    uint16_t dir_offset = 0;
    uint64_t start_block = 0;   // FILE/FILE_X：数据块起始绝对偏移
    uint32_t fragment = 0;      // FILE/FILE_X：fragment 索引；0xFFFFFFFF = 无
    uint32_t block_offset = 0;  // FILE/FILE_X：在本文件尾部 fragment 内的偏移
    uint32_t block_array_off = 32;  // block_sizes 数组相对 inode 起点的偏移（REG=32，LREG=56）
};

// SquashFS 块大小字段：bit24 **置位 = 未压缩**，低 24 位是压缩后长度
constexpr uint32_t kUncompressedBit = 1u << 24;
constexpr uint32_t kInvalidFrag = 0xFFFFFFFFu;
constexpr size_t kFragEntrySize = 16;  // start_block u64 + size u32 + unused u32

// ★ 实测纠正：SquashFS 4.0 的 FILE inode **没有** fragment 长度字段。
//   它的头部就是 32 字节（type/mode/uid/gid/mtime/inode_number/start_block/fragment/
//   offset/file_size），紧跟 block_sizes 数组；尾部 fragment 的载荷长度是**算出来的**：
//     fragment_size = file_size - 数据块数 × block_size
//   第一版我按"数组后还有一个 u32"读，读到的是**下一个 inode 的类型字段**（实测 138 字节的
//   文件 offset=115288、算出 32309249 的荒谬长度），被探针当场抓住。
//   扩展 inode（LREG/FILE_X）头部是 56 字节，fragment/offset 在 [44..48)/[48..52)。

class Image {
public:
    bool open(const std::string& path, std::string& err);
    const SuperBlock& sb() const { return sb_; }
    bool list(std::vector<Entry>& out, std::string& err);

private:
    bool meta_block(uint64_t abs_off, const MetaBlock*& out, std::string& err);
    bool meta_read(uint64_t base, uint64_t rel_block, uint32_t off, uint32_t len,
                   std::vector<uint8_t>& out, std::string& err);
    bool read_inode(uint64_t ref, Inode& ino, std::string& err);
    bool fragment_entry(uint32_t index, uint64_t& start_block, uint32_t& size, std::string& err);
    bool read_file_data(uint64_t ref, const Inode& ino, std::vector<uint8_t>& out, std::string& err);

public:
    // 按镜像内路径读一个文件的完整内容（列表结果复用同一次遍历）
    bool read_entry_file(const std::string& path, std::vector<uint8_t>& out, std::string& err);

private:

    FileReader file_;
    SuperBlock sb_;
    std::unordered_map<uint64_t, MetaBlock> cache_;
};

bool parse_superblock(const uint8_t* d, size_t n, SuperBlock& sb, std::string& err) {
    if (n < 96) {
        err = "超块不足 96 字节";
        return false;
    }
    const uint32_t le = rd32(d, false);
    if (le == kMagicLE) {
        sb.big_endian = false;
    } else if (le == kMagicBE) {
        sb.big_endian = true;
    } else {
        err = "不是 SquashFS 镜像（magic 既非 hsqs 也非 sqsh）";
        return false;
    }
    const bool be = sb.big_endian;
    sb.inodes = rd32(d + 4, be);
    sb.mkfs_time = rd32(d + 8, be);
    sb.block_size = rd32(d + 12, be);
    sb.fragments = rd32(d + 16, be);
    sb.compression = static_cast<Compression>(rd16(d + 20, be));
    sb.block_log = rd16(d + 22, be);
    sb.flags = rd16(d + 24, be);
    sb.no_ids = rd16(d + 26, be);
    sb.s_major = rd16(d + 28, be);
    sb.s_minor = rd16(d + 30, be);
    sb.root_inode = rd64(d + 32, be);
    sb.bytes_used = rd64(d + 40, be);
    sb.id_table_start = rd64(d + 48, be);
    sb.xattr_id_table_start = rd64(d + 56, be);
    sb.inode_table_start = rd64(d + 64, be);
    sb.directory_table_start = rd64(d + 72, be);
    sb.fragment_table_start = rd64(d + 80, be);
    return true;
}

bool Image::open(const std::string& path, std::string& err) {
    if (!file_.open(path, err)) return false;
    uint8_t head[96] = {};
    if (!file_.read_at(0, head, sizeof(head), err)) return false;
    return parse_superblock(head, sizeof(head), sb_, err);
}

bool Image::meta_block(uint64_t abs_off, const MetaBlock*& out, std::string& err) {
    const auto it = cache_.find(abs_off);
    if (it != cache_.end()) {
        out = &it->second;
        return true;
    }

    uint8_t hdr[2] = {};
    if (!file_.read_at(abs_off, hdr, 2, err)) {
        err = "读元数据块头失败 @" + std::to_string(abs_off) + "：" + err;
        return false;
    }
    const uint16_t h = rd16(hdr, sb_.big_endian);
    const uint32_t comp_size = static_cast<uint32_t>(h & 0x7FFFu);
    const bool stored = (h & 0x8000u) != 0;

    MetaBlock mb;
    // ★ 前进必须用 2 + comp_size：8192 只是**解压后**大小（1.x 踩过这个坑）
    mb.next = abs_off + 2 + comp_size;
    if (comp_size > 0) {
        std::vector<uint8_t> raw(comp_size);
        if (!file_.read_at(abs_off + 2, raw.data(), raw.size(), err)) {
            err = "读元数据块体失败 @" + std::to_string(abs_off) + "：" + err;
            return false;
        }
        if (stored) {
            // bit15 = 未压缩，直接就是数据
            mb.data = std::move(raw);
        } else if (!decompress_block(sb_.compression, raw.data(), raw.size(), kMetaBlockSize,
                                     mb.data, err)) {
            err = "元数据块解压失败 @" + std::to_string(abs_off) + "：" + err;
            return false;
        }
    }

    if (cache_.size() >= kCacheBlocks) cache_.clear();
    const auto res = cache_.emplace(abs_off, std::move(mb));
    out = &res.first->second;
    return true;
}

bool Image::meta_read(uint64_t base, uint64_t rel_block, uint32_t off, uint32_t len,
                      std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    out.reserve(len);
    uint64_t block_off = base + rel_block;
    uint32_t cur_off = off;
    // ★ 2026-09-27 真机整树跑抓到的缺陷：**offset 是"跨块的逻辑偏移"**，不是"块内偏移"。
    //   实证：`gamedata.sqzy_patch` 里 `textures/verg/masks/hud_sci_1_5.dds`（普通 REG、
    //   3686528 字节 / 29 块）的 inode 起始偏移是 **8200** —— 落在 8 KiB 块**之后**。
    //   参照 reader（rdsquashfs）照样读得出（它顺着 next 指针跨块跳），而我们原先直接报
    //   "元数据块越界（off=8200，块内只有 8192 字节）"。抽样跑（门禁 [11] 默认 200 文件）
    //   碰不到这种布局，只有整树才暴露。
    //   新语义：`cur_off >= 块大小` 时不是错误，而是**减掉这一块、跳到 next 块继续**；
    //   循环有上限，坏镜像仍会以"读元数据块失败"这种**可见**方式失败。
    int hops = 0;
    while (out.size() < len) {
        const MetaBlock* mb = nullptr;
        if (!meta_block(block_off, mb, err)) return false;
        const size_t block_size = mb->data.size();
        if (cur_off >= block_size) {
            if (block_size == 0 || ++hops > 4096) {
                err = "元数据跨块跳过次数异常（off=" + std::to_string(cur_off) + "）";
                return false;
            }
            cur_off -= static_cast<uint32_t>(block_size);
            block_off = mb->next;
            continue;
        }
        const size_t take = std::min<size_t>(block_size - cur_off, len - out.size());
        out.insert(out.end(), mb->data.begin() + cur_off, mb->data.begin() + cur_off + take);
        cur_off = 0;
        block_off = mb->next;
    }
    return true;
}

bool Image::read_inode(uint64_t ref, Inode& ino, std::string& err) {
    const uint64_t rel_block = ref >> 16;
    const uint32_t off = static_cast<uint32_t>(ref & 0xFFFFu);
    const bool be = sb_.big_endian;

    std::vector<uint8_t> head;
    if (!meta_read(sb_.inode_table_start, rel_block, off, 2, head, err)) return false;
    const uint16_t type = rd16(head.data(), be);

    // ★ 这里读的是"**解析所需的前缀**"，不是 inode 的完整记录长度 —— 两者别混：
    //   · 完整记录长度（格式常量）：DIR 32 / DIR_X **40**（16 基头 + 24）/ REG 32 /
    //     LREG 56 / SLINK 24（+ 目标串）；
    //   · 解析前缀：只覆盖本函数**实际访问**的最后一个字段（DIR_X 是 offset@34 的 u16，
    //     所以 36 就够）。
    //   2026-09-24 订正：原先把 36 直接当"记录长度"写着，与 writer 写出的 40 字节
    //   不一致（功能上没错，但常量是错的、容易误导）。多读那 4 字节没有收益，
    //   而且在"EXT_DIR 恰好是 inode 表最后一条"时会把读取跨进下一个元数据块，
    //   所以解析前缀保持 36，格式常量单列出来。
    constexpr size_t kRecDir = 32;
    constexpr size_t kRecDirXFull = 40;  // 格式常量（本函数不用，供 writer/文档对齐）
    constexpr size_t kRecDirXPrefix = 36;
    constexpr size_t kRecFile = 32;
    constexpr size_t kRecFileX = 56;
    constexpr size_t kRecSlink = 24;
    static_assert(kRecDirXFull == 16 + 24, "EXT_DIR 记录长度必须是 40");

    size_t rec = 2;
    switch (type) {
        case kTypeDir: rec = kRecDir; break;
        case kTypeDirX: rec = kRecDirXPrefix; break;
        case kTypeFile: rec = kRecFile; break;
        case kTypeFileX: rec = kRecFileX; break;
        case kTypeSlink:
        case kTypeSlinkX: rec = kRecSlink; break;
        default: rec = 2; break;  // 未知类型：只取类型，不猜长度
    }

    std::vector<uint8_t> buf;
    if (!meta_read(sb_.inode_table_start, rel_block, off, static_cast<uint32_t>(rec), buf, err)) {
        return false;
    }
    const uint8_t* p = buf.data();
    ino.type = type;

    switch (type) {
        case kTypeDir:
            ino.dir_block = rd32(p + 16, be);
            ino.size = rd16(p + 24, be);
            ino.dir_offset = rd16(p + 26, be);
            break;
        case kTypeDirX:
            ino.size = rd32(p + 20, be);
            ino.dir_block = rd32(p + 24, be);
            ino.dir_offset = rd16(p + 34, be);
            break;
        case kTypeFile:
            ino.start_block = rd32(p + 16, be);
            ino.fragment = rd32(p + 20, be);
            ino.block_offset = rd32(p + 24, be);
            ino.size = rd32(p + 28, be);
            break;
        case kTypeFileX:
            // LREG：start_block u64 @16、file_size u64 @24、sparse u64 @32、nlink @40、
            //       fragment @44、offset @48、xattr @52，block_sizes 从 56 开始
            ino.start_block = rd64(p + 16, be);
            ino.size = rd64(p + 24, be);
            ino.fragment = rd32(p + 44, be);
            ino.block_offset = rd32(p + 48, be);
            ino.block_array_off = 56;
            break;
        case kTypeSlink:
        case kTypeSlinkX:
            ino.size = rd32(p + 20, be);
            break;
        default:
            ino.size = 0;
            break;
    }
    return true;
}

bool Image::list(std::vector<Entry>& out, std::string& err) {
    struct Pending {
        uint64_t ref;
        std::string prefix;
    };
    std::vector<Pending> stack;
    stack.push_back({sb_.root_inode, std::string()});

    while (!stack.empty()) {
        const Pending cur = stack.back();
        stack.pop_back();

        Inode ino;
        if (!read_inode(cur.ref, ino, err)) return false;
        if (ino.type != kTypeDir && ino.type != kTypeDirX) {
            err = "目录项指向的不是目录 inode（type=" + std::to_string(ino.type) + "）";
            return false;
        }

        std::vector<uint8_t> dir;
        if (!meta_read(sb_.directory_table_start, ino.dir_block, ino.dir_offset,
                       static_cast<uint32_t>(ino.size), dir, err)) {
            return false;
        }

        size_t i = 0;
        while (i + 12 <= dir.size()) {
            const uint32_t count = rd32(dir.data() + i, sb_.big_endian);
            const uint32_t sblk = rd32(dir.data() + i + 4, sb_.big_endian);
            i += 12;  // 头块 12 字节（第 3 个 u32 语义未确认，跳过）

            for (uint32_t k = 0; k <= count; ++k) {  // ★ count + 1：末条是哨兵
                if (i + 8 > dir.size()) {
                    err = "目录项越界";
                    return false;
                }
                const uint16_t e_off = rd16(dir.data() + i, sb_.big_endian);
                const uint16_t e_type = rd16(dir.data() + i + 4, sb_.big_endian);
                const uint16_t nsz =
                    static_cast<uint16_t>(rd16(dir.data() + i + 6, sb_.big_endian) + 1);
                i += 8;
                if (i + nsz > dir.size()) {
                    err = "目录名越界";
                    return false;
                }
                const std::string name(reinterpret_cast<const char*>(dir.data() + i), nsz);
                i += nsz;

                if (e_type == 0) break;  // 哨兵：该组的下一组
                const std::string path = cur.prefix.empty() ? name : cur.prefix + "/" + name;
                const uint64_t child_ref = (static_cast<uint64_t>(sblk) << 16) | e_off;
                if (e_type == kTypeDir || e_type == kTypeDirX) {
                    stack.push_back({child_ref, path});
                    out.push_back({path, true, 0, child_ref});
                } else {
                    Inode child;
                    if (!read_inode(child_ref, child, err)) return false;
                    out.push_back({path, false, child.size, child_ref});
                }
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return true;
}

bool Image::fragment_entry(uint32_t index, uint64_t& start_block, uint32_t& size,
                           std::string& err) {
    if (sb_.fragments == 0) {
        err = "inode 指向 fragment，但镜像的 fragments 计数为 0";
        return false;
    }
    // fragment_table_start 指向一张「元数据块位置数组」：每项 u64 = 该元数据块的**绝对**偏移。
    // 真正的条目在那些元数据块里，每条 16 字节。
    const uint64_t per_block = kMetaBlockSize / kFragEntrySize;
    const uint64_t lookup_index = index / per_block;
    const uint64_t in_block = index % per_block;

    uint8_t pos[8] = {};
    if (!file_.read_at(sb_.fragment_table_start + lookup_index * 8, pos, sizeof(pos), err)) {
        err = "读 fragment 查找表失败：" + err;
        return false;
    }
    const uint64_t block_off = rd64(pos, sb_.big_endian);

    std::vector<uint8_t> entry;
    if (!meta_read(0, block_off, static_cast<uint32_t>(in_block * kFragEntrySize),
                   static_cast<uint32_t>(kFragEntrySize), entry, err)) {
        err = "读 fragment 条目失败：" + err;
        return false;
    }
    start_block = rd64(entry.data(), sb_.big_endian);
    size = rd32(entry.data() + 8, sb_.big_endian);
    return true;
}

bool Image::read_file_data(uint64_t ref, const Inode& ino, std::vector<uint8_t>& out,
                           std::string& err) {
    const uint64_t bs = sb_.block_size ? sb_.block_size : 131072;
    const bool has_frag = (ino.fragment != kInvalidFrag);
    // 有 fragment 时：整块数 = size / bs，尾部不足一块的进 fragment；
    // 无 fragment 时：块数要向上取整（最后一块可能不满）
    const uint64_t nblocks = has_frag ? (ino.size / bs) : ((ino.size + bs - 1) / bs);

    // block_sizes 数组紧跟在 inode 头之后（REG 从 32 起、LREG 从 56 起）
    std::vector<uint8_t> tail;
    const uint32_t tail_len = static_cast<uint32_t>(nblocks) * 4u;
    if (tail_len > 0) {
        if (!meta_read(sb_.inode_table_start, ref >> 16,
                       static_cast<uint32_t>(ref & 0xFFFFu) + ino.block_array_off, tail_len, tail,
                       err)) {
            err = "读 block_sizes 失败：" + err;
            return false;
        }
    }

    out.clear();
    out.reserve(static_cast<size_t>(ino.size));

    uint64_t offset = ino.start_block;
    for (uint64_t i = 0; i < nblocks; ++i) {
        uint32_t raw = rd32(tail.data() + i * 4, sb_.big_endian);
        const bool uncompressed = (raw & kUncompressedBit) != 0;
        raw &= ~kUncompressedBit;
        if (out.size() >= ino.size) {
            err = "block_sizes 比文件长";
            return false;
        }
        const size_t want = static_cast<size_t>(std::min<uint64_t>(bs, ino.size - out.size()));
        if (raw == 0) {
            out.insert(out.end(), want, 0);  // 稀疏空洞：补 0，不读盘
        } else {
            std::vector<uint8_t> rawbuf(raw);
            if (!file_.read_at(offset, rawbuf.data(), rawbuf.size(), err)) return false;
            if (uncompressed) {
                if (rawbuf.size() < want) {
                    err = "未压缩块长度不足";
                    return false;
                }
                out.insert(out.end(), rawbuf.begin(), rawbuf.begin() + want);
            } else {
                std::vector<uint8_t> plain;
                if (!decompress_block(sb_.compression, rawbuf.data(), rawbuf.size(), want, plain,
                                      err)) {
                    err = "数据块解压失败（块 " + std::to_string(i) + "）：" + err;
                    return false;
                }
                if (plain.size() < want) {
                    err = "数据块解压后长度不足（块 " + std::to_string(i) + "）";
                    return false;
                }
                out.insert(out.end(), plain.begin(), plain.begin() + want);
            }
        }
        offset += raw;
    }

    if (has_frag) {
        uint64_t frag_start = 0;
        uint32_t frag_field = 0;
        if (!fragment_entry(ino.fragment, frag_start, frag_field, err)) return false;
        const bool frag_uncompressed = (frag_field & kUncompressedBit) != 0;
        const uint32_t frag_stored = frag_field & ~kUncompressedBit;

        std::vector<uint8_t> rawbuf(frag_stored);
        if (frag_stored > 0 && !file_.read_at(frag_start, rawbuf.data(), rawbuf.size(), err)) {
            return false;
        }
        std::vector<uint8_t> plain;
        if (frag_uncompressed) {
            plain = std::move(rawbuf);
        } else if (!decompress_block(sb_.compression, rawbuf.data(), rawbuf.size(),
                                     static_cast<size_t>(bs), plain, err)) {
            err = "fragment 解压失败：" + err;
            return false;
        }
        // fragment 载荷长度是**算出来**的，不是存的（见 Inode 那边的实测说明）
        const uint32_t frag_size = static_cast<uint32_t>(ino.size - nblocks * bs);
        if (static_cast<uint64_t>(ino.block_offset) + frag_size > plain.size()) {
            err = "fragment 越界（offset=" + std::to_string(ino.block_offset) + " size=" +
                  std::to_string(frag_size) + " 块内只有 " + std::to_string(plain.size()) + "）";
            return false;
        }
        out.insert(out.end(), plain.begin() + ino.block_offset,
                   plain.begin() + ino.block_offset + frag_size);
    }

    // 长度必须与 inode 声明一致：**宁可报错也不给半截数据**（1.x 的空文件教训）
    if (out.size() != ino.size) {
        err = "提取长度与 inode 声明不符（得到 " + std::to_string(out.size()) + "，声明 " +
              std::to_string(ino.size) + "）";
        return false;
    }
    return true;
}

bool Image::read_entry_file(const std::string& path, std::vector<uint8_t>& out,
                            std::string& err) {
    std::vector<Entry> entries;
    if (!list(entries, err)) return false;
    const Entry* found = nullptr;
    for (const Entry& e : entries) {
        if (e.path == path) {
            found = &e;
            break;
        }
    }
    if (found == nullptr) {
        err = "镜像里没有该条目：" + path;
        return false;
    }
    if (found->is_dir) {
        err = "该条目是目录，不是文件：" + path;
        return false;
    }
    Inode ino;
    if (!read_inode(found->inode_ref, ino, err)) return false;
    return read_file_data(found->inode_ref, ino, out, err);
}

}  // namespace

bool read_superblock(const std::string& path, SuperBlock& out, std::string& err) {
    FileReader fr;
    if (!fr.open(path, err)) return false;
    uint8_t head[96] = {};
    if (!fr.read_at(0, head, sizeof(head), err)) return false;
    return parse_superblock(head, sizeof(head), out, err);
}

ListResult list_entries(const std::string& path, const xrkit::plugin::Registry* registry) {
    ListResult res;
    const auto t0 = std::chrono::steady_clock::now();
    // ★ 2026-10-04 解密闸门（⑫12.1 消费端）：`--dump` 的 SquashFS 分支走的就是这个入口，
    //   CLI 与 GUI 共用同一枚闸门（判据见 registry.h）。没有活动注册表 / 没有解密器命中
    //   ⇒ `source == path`，下面逐字节与今天一致。
    const xrkit::plugin::DecryptGateResult gate =
        xrkit::plugin::gate_active_read(path, registry);
    if (!gate.ok) {
        res.error = gate.error;
        return res;
    }
    const std::string& source = gate.used_plugin ? gate.path : path;
    Image img;
    std::string err;
    if (!img.open(source, err)) {
        res.error = err;
        return res;
    }
    if (!img.list(res.entries, err)) {
        res.error = err;
        return res;
    }
    const auto t1 = std::chrono::steady_clock::now();
    res.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.ok = true;
    return res;
}

FileData read_file(const std::string& image_path, const std::string& entry_path) {
    FileData res;
    const auto t0 = std::chrono::steady_clock::now();
    Image img;
    std::string err;
    if (!img.open(image_path, err)) {
        res.error = err;
        return res;
    }
    if (!img.read_entry_file(entry_path, res.data, err)) {
        res.error = err;
        return res;
    }
    const auto t1 = std::chrono::steady_clock::now();
    res.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.ok = true;
    return res;
}

}  // namespace xrkit::engine::fs
