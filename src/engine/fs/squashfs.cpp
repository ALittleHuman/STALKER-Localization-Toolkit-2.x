#include "xrkit/engine/fs/squashfs.h"

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
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            err = "取文件大小失败：" + ec.message();
            return false;
        }
        size_ = size;
        in_.open(path, std::ios::binary);
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
};

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
    while (out.size() < len) {
        const MetaBlock* mb = nullptr;
        if (!meta_block(block_off, mb, err)) return false;
        if (cur_off >= mb->data.size()) {
            err = "元数据块越界（off=" + std::to_string(cur_off) +
                  "，块内只有 " + std::to_string(mb->data.size()) + " 字节）";
            return false;
        }
        const size_t take = std::min<size_t>(mb->data.size() - cur_off, len - out.size());
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

    size_t rec = 2;
    switch (type) {
        case kTypeDir: rec = 32; break;
        case kTypeDirX: rec = 36; break;
        case kTypeFile: rec = 32; break;
        case kTypeFileX: rec = 32; break;
        case kTypeSlink:
        case kTypeSlinkX: rec = 24; break;
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
            ino.size = rd32(p + 28, be);
            break;
        case kTypeFileX:
            ino.size = rd64(p + 24, be);
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
                    out.push_back({path, true, 0});
                } else {
                    Inode child;
                    if (!read_inode(child_ref, child, err)) return false;
                    out.push_back({path, false, child.size});
                }
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return true;
}

}  // namespace

bool read_superblock(const std::string& path, SuperBlock& out, std::string& err) {
    FileReader fr;
    if (!fr.open(path, err)) return false;
    uint8_t head[96] = {};
    if (!fr.read_at(0, head, sizeof(head), err)) return false;
    return parse_superblock(head, sizeof(head), out, err);
}

ListResult list_entries(const std::string& path) {
    ListResult res;
    const auto t0 = std::chrono::steady_clock::now();
    Image img;
    std::string err;
    if (!img.open(path, err)) {
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

}  // namespace xrkit::engine::fs
