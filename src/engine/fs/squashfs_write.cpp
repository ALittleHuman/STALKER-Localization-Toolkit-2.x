// SquashFS writer（M1-5）—— 自实现封包，产物必须与 1.x 的 `tar2sqfs` 1.3.2 逐字节一致。
//
// 这份实现不是"照着格式规范重写一遍"，而是**照着参照实现的行为逐项复刻**：
// 每一条规则都在 `docs/2.0.0-SquashFS-writer规格.md` 里带着证据（源码位置或镜像实测）。
// 复刻清单（漏一条就不是逐字节）：
//   ① 目录子节点按**名字字节序**插入（fstree_insert_sorted / strcmp）；
//   ② inode 编号：先递归子目录，再给本层子节点按名字顺序编号，**根最后**；
//      inode 表按编号顺序落盘（所以子节点一定先于父目录）；
//   ③ 数据块按**输入顺序**写盘；块满 128 KiB 即写；文件尾部（不满一块）进 fragment；
//   ④ fragment 块攒满即写、结束时写；碎片同名（校验和 + 长度 + 逐字节）去重；
//   ⑤ 有数据块的文件在结束时做**整段去重**（校验和序列 + 盘上逐字节），命中则
//      把刚写的块截掉、blocks_start 指回旧位置；
//   ⑥ 元数据块 8 KiB 一分块，压不小就置 bit15 原样存；块头 2 字节；
//   ⑦ 超块 flags = NO_XATTRS | NO_DUPLICATES | ALWAYS_FRAGMENTS（碎片全压缩时
//      还会带 UNCOMPRESSED_FRAGMENTS 的清零规则）；
//   ⑧ 表顺序：数据 → inode 表 → 目录表 → fragment 表 → id 表；缺表写 0xFFFF…
//   ⑨ 结尾补零到 4096 的整数倍（bytes_used 不含补零）。

#include "xrkit/engine/fs/compress.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/core/path.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace xrkit::engine::fs {
namespace {

constexpr uint32_t kMagic = 0x73717368u;  // "hsqs"（小端）
constexpr size_t kMetaBlockSize = 8192;   // 元数据块（解压后）固定 8 KiB
constexpr uint32_t kUncompressedBit = 1u << 24;
constexpr uint32_t kNoFrag = 0xFFFFFFFFu;
constexpr uint64_t kNoTable = 0xFFFFFFFFFFFFFFFFull;
constexpr size_t kDirIndexThreshold = 256;  // 参照：目录项数 ≥ 256 就用 EXT_DIR + 索引
constexpr size_t kMaxDirGroup = 256;        // SQFS_MAX_DIR_ENT

constexpr uint16_t kTypeDir = 1;
constexpr uint16_t kTypeFile = 2;
constexpr uint16_t kTypeDirX = 8;
constexpr uint16_t kTypeFileX = 9;

constexpr uint16_t kFlagUncompressedFragments = 0x0008;
constexpr uint16_t kFlagNoFragments = 0x0010;
constexpr uint16_t kFlagAlwaysFragments = 0x0020;
constexpr uint16_t kFlagNoDuplicates = 0x0040;
constexpr uint16_t kFlagNoXattrs = 0x0200;

// ── XXH32 ─────────────────────────────────────────────────────────────
// 参照实现用 xxh32 做去重的**预筛**（真正判定是逐字节比较，哈希不进镜像）。
// 只要哈希"真匹配必相等"，去重结果就与参照一致。
constexpr uint32_t kXXP1 = 2654435761u;
constexpr uint32_t kXXP2 = 2246822519u;
constexpr uint32_t kXXP3 = 3266489917u;
constexpr uint32_t kXXP4 = 668265263u;
constexpr uint32_t kXXP5 = 374761393u;

inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }
inline uint32_t rd32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t xxh32(const uint8_t* p, size_t len, uint32_t seed = 0) {
    const uint8_t* const end = p + len;
    uint32_t h;
    if (len >= 16) {
        const uint8_t* const limit = end - 16;
        uint32_t v1 = seed + kXXP1 + kXXP2;
        uint32_t v2 = seed + kXXP2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - kXXP1;
        do {
            v1 = rotl32(v1 + rd32le(p) * kXXP2, 13) * kXXP1;
            p += 4;
            v2 = rotl32(v2 + rd32le(p) * kXXP2, 13) * kXXP1;
            p += 4;
            v3 = rotl32(v3 + rd32le(p) * kXXP2, 13) * kXXP1;
            p += 4;
            v4 = rotl32(v4 + rd32le(p) * kXXP2, 13) * kXXP1;
            p += 4;
        } while (p <= limit);
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + kXXP5;
    }
    h += static_cast<uint32_t>(len);
    while (p + 4 <= end) {
        h = rotl32(h + rd32le(p) * kXXP3, 17) * kXXP4;
        p += 4;
    }
    while (p < end) {
        h = rotl32(h + (*p) * kXXP5, 11) * kXXP1;
        ++p;
    }
    h ^= h >> 15;
    h *= kXXP2;
    h ^= h >> 13;
    h *= kXXP3;
    h ^= h >> 16;
    return h;
}

// ── 字节序小工具 ──────────────────────────────────────────────────────
inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, static_cast<uint16_t>(x & 0xFFFF));
    put16(v, static_cast<uint16_t>(x >> 16));
}
inline void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x & 0xFFFFFFFFull));
    put32(v, static_cast<uint32_t>(x >> 32));
}
inline bool all_zero(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

// 参照（super.c）：block_log 由 block_size 逐位右移数出来
inline uint32_t block_log_of(uint32_t block_size) {
    uint32_t log = 0;
    for (uint32_t i = block_size; i != 1; i >>= 1) ++log;
    return log;
}

// ── 输出文件（支持截断：块去重要把刚写的重复块去掉）───────────────────
class OutFile {
public:
    bool open(const std::string& path, std::string& err) {
        path_ = path;
        const std::filesystem::path fs_path = core::path_from_utf8(path);
        f_.open(fs_path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f_) {
            err = "打不开输出文件：" + path;
            return false;
        }
        pos_ = 0;
        return true;
    }
    ~OutFile() {
        if (f_.is_open()) f_.close();
    }

    uint64_t size() const { return pos_; }

    bool write(const void* data, size_t n, std::string& err) {
        if (n == 0) return true;
        f_.clear();
        f_.seekp(static_cast<std::streamoff>(pos_), std::ios::beg);
        f_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!f_) {
            err = "写输出文件失败 @ " + std::to_string(pos_);
            return false;
        }
        pos_ += n;
        return true;
    }

    // 超块必须写在**偏移 0**（不是追加）：先在 96 字节处堆放数据区，最后回填。
    // 参照实现也是这么干的（init 时写一次、finish 时再写一次覆盖）。
    bool write_at(uint64_t off, const void* data, size_t n, std::string& err) {
        if (n == 0) return true;
        f_.clear();
        f_.seekp(static_cast<std::streamoff>(off), std::ios::beg);
        f_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
        if (!f_) {
            err = "写输出文件失败 @ " + std::to_string(off);
            return false;
        }
        return true;
    }

    bool read_at(uint64_t off, void* dst, size_t n, std::string& err) const {
        f_.clear();
        f_.seekg(static_cast<std::streamoff>(off), std::ios::beg);
        f_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        if (f_.gcount() != static_cast<std::streamsize>(n)) {
            err = "读输出文件失败 @ " + std::to_string(off);
            return false;
        }
        return true;
    }

    bool truncate(uint64_t new_size, std::string& err) {
        f_.flush();
        if (!f_) {
            err = "flush 输出文件失败";
            return false;
        }
        f_.close();
        std::error_code ec;
        std::filesystem::resize_file(core::path_from_utf8(path_), new_size, ec);
        if (ec) {
            err = "截断输出文件失败：" + ec.message();
            return false;
        }
        f_.open(core::path_from_utf8(path_),
                std::ios::in | std::ios::out | std::ios::binary);
        if (!f_) {
            err = "截断后重新打开输出文件失败";
            return false;
        }
        pos_ = new_size;
        return true;
    }

    // 盘上两段区间是否逐字节相同（复刻 check_file_range_equal）。
    // 返回 false 只在 I/O 出错；不同则 equal=false。
    bool same_range(uint64_t a, uint64_t b, uint64_t size, bool& equal, std::string& err) const {
        equal = false;
        std::vector<uint8_t> x(8192), y(8192);
        uint64_t done = 0;
        while (done < size) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(8192, size - done));
            if (!read_at(a + done, x.data(), n, err)) return false;
            if (!read_at(b + done, y.data(), n, err)) return false;
            if (std::memcmp(x.data(), y.data(), n) != 0) return true;
            done += n;
        }
        equal = true;
        return true;
    }

private:
    mutable std::fstream f_;
    std::string path_;
    uint64_t pos_ = 0;
};

// ── 元数据块写入器（复刻 libsquashfs 的 meta_writer）──────────────────
class MetaWriter {
public:
    MetaWriter(OutFile* file, bool keep_in_memory, Compression comp)
        : file_(file), keep_(keep_in_memory), comp_(comp) {}

    uint64_t block_offset() const { return block_offset_; }
    uint32_t offset() const { return static_cast<uint32_t>(offset_); }

    bool append(const void* data, size_t n, std::string& err) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (n != 0) {
            size_t diff = kMetaBlockSize - offset_;
            if (diff == 0) {
                if (!emit(err)) return false;
                diff = kMetaBlockSize;
            }
            if (diff > n) diff = n;
            std::memcpy(buf_ + offset_, p, diff);
            offset_ += diff;
            n -= diff;
            p += diff;
        }
        if (offset_ == kMetaBlockSize) return emit(err);
        return true;
    }

    bool flush(std::string& err) {
        if (offset_ == 0) return true;
        return emit(err);
    }

    // keep_in_memory 模式：把所有块按顺序写进文件（调用方先记下目录表起点）
    bool write_to_file(std::string& err) {
        for (const std::vector<uint8_t>& blk : blocks_) {
            if (!file_->write(blk.data(), blk.size(), err)) return false;
        }
        blocks_.clear();
        return true;
    }

private:
    bool emit(std::string& err) {
        bool compressed = false;
        std::vector<uint8_t> out;
        if (!compress_block(comp_, buf_, offset_, kMetaBlockSize, out, compressed, err)) {
            return false;
        }
        std::vector<uint8_t> block;
        if (compressed) {
            put16(block, static_cast<uint16_t>(out.size()));
            block.insert(block.end(), out.begin(), out.end());
        } else {
            put16(block, static_cast<uint16_t>(offset_ | 0x8000));
            block.insert(block.end(), buf_, buf_ + offset_);
        }
        offset_ = 0;
        block_offset_ += block.size();
        if (keep_) {
            blocks_.push_back(std::move(block));
            return true;
        }
        return file_->write(block.data(), block.size(), err);
    }

    OutFile* file_;
    bool keep_;
    Compression comp_;
    uint8_t buf_[kMetaBlockSize] = {};
    size_t offset_ = 0;
    uint64_t block_offset_ = 0;
    std::vector<std::vector<uint8_t>> blocks_;
};

// ── 文件树 ────────────────────────────────────────────────────────────
struct Node {
    std::string name;
    std::string path;
    bool is_dir = false;
    bool implicit_dir = false;
    uint32_t mode = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t mtime = 0;
    uint32_t inode_num = 0;
    uint64_t inode_ref = 0;
    Node* parent = nullptr;
    std::vector<Node*> children;  // 按 name 字节序

    // 文件属性
    std::string source;
    uint64_t size = 0;
    uint64_t blocks_start = 0;
    uint64_t sparse = 0;
    uint32_t fragment_index = kNoFrag;
    uint32_t fragment_offset = kNoFrag;
    std::vector<uint32_t> block_sizes;
};

// ★ H7（2026-10-24 全量审计）：这里原来有一个 `free_tree()` —— **全仓零调用**：
//   `Packer` 的 Node 树在成功与失败路径上都不释放（一进程一次封包，量小、退出即回收），
//   但那个函数**看起来像"会释放"**，是死代码里最容易误导人的一类。删掉它，
//   并把"树不释放"这条事实写在这里（要真释放得给 `Packer` 加 RAII，单独立项）。

// ── 写表（复刻 sqfs_write_table：数据块 + 位置数组）────────────────────
bool write_table(OutFile& file, Compression comp, const std::vector<uint8_t>& data,
                 uint64_t& start, std::string& err) {
    size_t remaining = data.size();
    const size_t block_count = (remaining + kMetaBlockSize - 1) / kMetaBlockSize;
    std::vector<uint64_t> locations;
    locations.reserve(block_count);

    if (block_count == 0) {
        start = file.size();
        return true;
    }

    MetaWriter m(&file, false, comp);
    const uint8_t* p = data.data();
    while (remaining > 0) {
        locations.push_back(file.size());
        const size_t diff = std::min(kMetaBlockSize, remaining);
        if (!m.append(p, diff, err)) return false;
        p += diff;
        remaining -= diff;
    }
    if (!m.flush(err)) return false;

    start = file.size();
    std::vector<uint8_t> raw;
    raw.reserve(locations.size() * 8);
    for (uint64_t loc : locations) put64(raw, loc);
    return file.write(raw.data(), raw.size(), err);
}

// ── 打包主流程 ────────────────────────────────────────────────────────
class Packer {
public:
    Packer(OutFile& file, const PackOptions& opt) : file_(file), opt_(opt) {}

    bool run(const std::vector<PackItem>& items, PackResult& res);

private:
    // 树
    Node* root_ = nullptr;
    std::vector<Node*> inodes_;  // 按 inode 编号顺序（= 落盘顺序）
    std::unordered_map<std::string, Node*> by_path_;

    // 数据区
    struct BlockRec {
        uint64_t offset;
        uint32_t size_field;  // 含 bit24
        uint32_t checksum;
    };
    std::vector<BlockRec> blocks_;
    std::unordered_map<uint32_t, std::vector<size_t>> blocks_by_sum_;

    struct FragRef {
        uint32_t index;
        uint32_t offset;
        uint32_t size;
    };
    struct FragEntry {
        uint64_t offset = 0;
        uint32_t size_field = 0;
    };
    std::vector<FragEntry> frag_table_;
    std::vector<uint8_t> pending_frag_;
    uint32_t pending_frag_index_ = 0;
    bool has_pending_frag_ = false;
    std::unordered_map<uint64_t, std::vector<FragRef>> frag_by_key_;
    // 已落盘 fragment 块的内容缓存（逐字节比较用；参照实现同样只缓存一块）
    mutable uint32_t cached_frag_index_ = kNoFrag;
    mutable std::vector<uint8_t> cached_frag_data_;

    // id 表
    std::vector<uint32_t> ids_;

    // 统计
    uint64_t data_blocks_ = 0;
    uint64_t fragment_blocks_ = 0;
    uint64_t dedup_blocks_ = 0;
    uint64_t dedup_fragments_ = 0;

    bool build_tree(const std::vector<PackItem>& items, std::string& err);
    void alloc_inode_numbers(Node* n, uint32_t& counter);
    void map_inodes(Node* n);
    void sort_children(Node* n);

    Node* child_by_name(Node* parent, const std::string& name) const;

    bool process_file(Node* n, std::string& err);
    bool write_fragment_block(std::string& err);
    bool dedup_file_blocks(size_t file_start, uint64_t& location, std::string& err);
    bool fragment_lookup(uint32_t sum, uint32_t size, const uint8_t* data, FragRef& out,
                         std::string& err);
    bool load_frag_block(uint32_t index, std::string& err) const;

    uint16_t id_index(uint32_t id);

    bool serialize(std::string& err);
    void write_file_inode(std::vector<uint8_t>& out, const Node* n);
    void write_dir_inode(std::vector<uint8_t>& out, const Node* n, uint64_t dir_ref,
                         uint32_t dir_size, size_t ent_count,
                         const std::vector<std::pair<uint64_t, uint64_t>>& index_records,
                         const std::vector<std::string>& index_names);

    OutFile& file_;
    PackOptions opt_;
    uint64_t inode_table_start_ = 0;
    uint64_t directory_table_start_ = 0;
    uint64_t fragment_table_start_ = kNoTable;
    uint64_t id_table_start_ = kNoTable;
    uint32_t fragment_entry_count_ = 0;
    uint64_t root_inode_ref_ = 0;
};

Node* Packer::child_by_name(Node* parent, const std::string& name) const {
    for (Node* c : parent->children) {
        if (c->name == name) return c;
    }
    return nullptr;
}

void Packer::sort_children(Node* n) {
    std::sort(n->children.begin(), n->children.end(),
              [](const Node* a, const Node* b) { return a->name < b->name; });
    for (Node* c : n->children) {
        if (c->is_dir) sort_children(c);
    }
}

bool Packer::build_tree(const std::vector<PackItem>& items, std::string& err) {
    root_ = new Node();
    root_->is_dir = true;
    root_->path.clear();
    root_->mode = 0x4000 | opt_.dir_mode;  // S_IFDIR
    root_->uid = opt_.uid;
    root_->gid = opt_.gid;
    root_->mtime = opt_.mtime;

    for (const PackItem& item : items) {
        if (item.path.empty()) {
            err = "条目路径为空";
            return false;
        }
        // 切成组件（'/' 分隔，忽略空段）
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= item.path.size()) {
            const size_t slash = item.path.find('/', start);
            const size_t end = (slash == std::string::npos) ? item.path.size() : slash;
            if (end > start) parts.push_back(item.path.substr(start, end - start));
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        if (parts.empty()) {
            err = "条目路径非法：" + item.path;
            return false;
        }

        Node* cur = root_;
        std::string acc;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            acc = acc.empty() ? parts[i] : acc + "/" + parts[i];
            Node* next = child_by_name(cur, parts[i]);
            if (next == nullptr) {
                next = new Node();
                next->name = parts[i];
                next->path = acc;
                next->is_dir = true;
                next->implicit_dir = true;
                next->mode = 0x4000 | opt_.dir_mode;
                next->uid = opt_.uid;
                next->gid = opt_.gid;
                next->mtime = opt_.mtime;
                next->parent = cur;
                cur->children.push_back(next);
                by_path_[acc] = next;
            } else if (!next->is_dir) {
                err = "路径冲突（同名文件已存在）：" + acc;
                return false;
            }
            cur = next;
        }

        const std::string& leaf = parts.back();
        acc = acc.empty() ? leaf : acc + "/" + leaf;
        Node* found = child_by_name(cur, leaf);
        if (item.is_dir) {
            if (found != nullptr && !found->is_dir) {
                err = "路径冲突（同名文件已存在）：" + acc;
                return false;
            }
            if (found == nullptr) {
                found = new Node();
                found->name = leaf;
                found->path = acc;
                found->is_dir = true;
                found->mode = 0x4000 | opt_.dir_mode;
                found->uid = opt_.uid;
                found->gid = opt_.gid;
                found->mtime = opt_.mtime;
                found->parent = cur;
                cur->children.push_back(found);
                by_path_[acc] = found;
            } else {
                // 参照：隐式建出来的目录被显式条目补全属性
                found->implicit_dir = false;
                found->mode = 0x4000 | opt_.dir_mode;
                found->uid = opt_.uid;
                found->gid = opt_.gid;
                found->mtime = opt_.mtime;
            }
            continue;
        }
        if (found != nullptr) {
            err = "重复条目：" + acc;
            return false;
        }
        found = new Node();
        found->name = leaf;
        found->path = acc;
        found->is_dir = false;
        found->mode = 0x8000 | opt_.file_mode;  // S_IFREG
        found->uid = opt_.uid;
        found->gid = opt_.gid;
        found->mtime = opt_.mtime;
        found->parent = cur;
        found->source = item.source_path;
        cur->children.push_back(found);
        by_path_[acc] = found;
    }

    sort_children(root_);
    return true;
}

void Packer::alloc_inode_numbers(Node* n, uint32_t& counter) {
    bool has_subdirs = false;
    for (Node* c : n->children) {
        if (c->is_dir) {
            has_subdirs = true;
            break;
        }
    }
    if (has_subdirs) {
        for (Node* c : n->children) {
            if (c->is_dir) alloc_inode_numbers(c, counter);
        }
    }
    for (Node* c : n->children) c->inode_num = ++counter;
}

void Packer::map_inodes(Node* n) {
    inodes_[n->inode_num - 1] = n;
    if (n->is_dir) {
        for (Node* c : n->children) map_inodes(c);
    }
}

uint16_t Packer::id_index(uint32_t id) {
    for (size_t i = 0; i < ids_.size(); ++i) {
        if (ids_[i] == id) return static_cast<uint16_t>(i);
    }
    ids_.push_back(id);
    return static_cast<uint16_t>(ids_.size() - 1);
}

bool Packer::load_frag_block(uint32_t index, std::string& err) const {
    if (cached_frag_index_ == index) return true;
    if (index >= frag_table_.size()) {
        err = "fragment 索引越界";
        return false;
    }
    const FragEntry& e = frag_table_[index];
    const uint32_t stored = e.size_field & ~kUncompressedBit;
    if (stored == 0) {
        cached_frag_data_.clear();
        cached_frag_index_ = index;
        return true;
    }
    std::vector<uint8_t> raw(stored);
    if (!file_.read_at(e.offset, raw.data(), raw.size(), err)) return false;
    if (e.size_field & kUncompressedBit) {
        cached_frag_data_ = std::move(raw);
    } else if (!decompress_block(opt_.compressor, raw.data(), raw.size(), opt_.block_size,
                                 cached_frag_data_, err)) {
        return false;
    }
    cached_frag_index_ = index;
    return true;
}

bool Packer::fragment_lookup(uint32_t sum, uint32_t size, const uint8_t* data, FragRef& out,
                            std::string& err) {
    const uint64_t key = (static_cast<uint64_t>(sum) << 32) | size;
    const auto it = frag_by_key_.find(key);
    if (it == frag_by_key_.end()) return false;
    for (const FragRef& ref : it->second) {
        const uint8_t* cand = nullptr;
        if (has_pending_frag_ && ref.index == pending_frag_index_) {
            if (static_cast<size_t>(ref.offset) + size > pending_frag_.size()) continue;
            cand = pending_frag_.data() + ref.offset;
        } else {
            if (!load_frag_block(ref.index, err)) return false;
            if (static_cast<size_t>(ref.offset) + size > cached_frag_data_.size()) continue;
            cand = cached_frag_data_.data() + ref.offset;
        }
        if (std::memcmp(cand, data, size) == 0) {
            out = ref;
            return true;
        }
    }
    return false;
}

bool Packer::write_fragment_block(std::string& err) {
    if (!has_pending_frag_) return true;
    ++fragment_blocks_;
    // 参照：fragment 块也走同一套"全零 → 稀疏（不写盘、表项留 0）"判定
    if (!pending_frag_.empty() && all_zero(pending_frag_.data(), pending_frag_.size())) {
        frag_table_[pending_frag_index_] = FragEntry{0, 0};
        pending_frag_.clear();
        has_pending_frag_ = false;
        return true;
    }
    const uint32_t sum = xxh32(pending_frag_.data(), pending_frag_.size());
    bool compressed = false;
    std::vector<uint8_t> out;
    if (!compress_block(opt_.compressor, pending_frag_.data(), pending_frag_.size(),
                        opt_.block_size, out, compressed, err)) {
        return false;
    }
    const uint64_t location = file_.size();
    uint32_t size_field = 0;
    if (compressed) {
        if (!file_.write(out.data(), out.size(), err)) return false;
        size_field = static_cast<uint32_t>(out.size());
    } else {
        if (!file_.write(pending_frag_.data(), pending_frag_.size(), err)) return false;
        size_field = static_cast<uint32_t>(pending_frag_.size()) | kUncompressedBit;
    }
    frag_table_[pending_frag_index_] = FragEntry{location, size_field};

    // ★ fragment 块必须**也记进块账本**：参照实现里数据块与 fragment 块共用同一张
    //   `wr->blocks` 记录（write_data_block 对 FRAGMENT_BLOCK 同样 store_block_location）。
    //   漏掉它，数据块去重命中后的"截断到上一个记录末尾"就会把刚写下的 fragment 块删掉
    //   （frag 表还指着它）—— 真机抽验（NLC sqz_wasteland）就是这么被抓出来的：
    //   镜像小 700 KB、碎片去重从 206 掉到 136。
    blocks_.push_back(BlockRec{location, size_field, sum});
    blocks_by_sum_[sum].push_back(blocks_.size() - 1);

    pending_frag_.clear();
    has_pending_frag_ = false;
    return true;
}

// 整段去重（复刻 block_writer.c 的 deduplicate_blocks）：
// 命中就返回**旧的起始偏移**（并把刚写的块截掉），未命中返回本文件第一个已写块的偏移。
bool Packer::dedup_file_blocks(size_t file_start, uint64_t& location, std::string& err) {
    const size_t count = blocks_.size() - file_start;
    if (count == 0) {
        location = 0;
        return true;
    }

    size_t found = file_start;
    const uint32_t want_sum = blocks_[file_start].checksum;
    const auto it = blocks_by_sum_.find(want_sum);
    if (it != blocks_by_sum_.end()) {
        for (size_t i : it->second) {
            if (i >= file_start) break;
            if (i + count > blocks_.size()) continue;
            bool same = true;
            for (size_t j = 0; j < count; ++j) {
                if (blocks_[i + j].checksum != blocks_[file_start + j].checksum ||
                    blocks_[i + j].size_field != blocks_[file_start + j].size_field) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            uint64_t sz = 0;
            for (size_t j = 0; j < count; ++j) {
                sz += (blocks_[file_start + j].size_field & 0xFFFFFF);
            }
            bool equal = false;
            if (!file_.same_range(blocks_[i].offset, blocks_[file_start].offset, sz, equal, err)) {
                return false;
            }
            if (!equal) continue;
            found = i;
            break;
        }
    }

    location = blocks_[found].offset;
    if (found >= file_start) return true;  // 没命中

    const size_t kept = (count >= (file_start - found)) ? (found + count) : file_start;
    const uint64_t end = blocks_[kept - 1].offset + (blocks_[kept - 1].size_field & 0xFFFFFF);
    dedup_blocks_ += blocks_.size() - kept;
    blocks_.resize(kept);
    if (!file_.truncate(end, err)) return false;
    // 索引重建（记录已经变短）
    blocks_by_sum_.clear();
    for (size_t i = 0; i < blocks_.size(); ++i) {
        blocks_by_sum_[blocks_[i].checksum].push_back(i);
    }
    return true;
}

bool Packer::process_file(Node* n, std::string& err) {
    std::ifstream in(core::path_from_utf8(n->source), std::ios::binary);
    if (!in) {
        err = "打不开输入文件：" + n->source + "（镜像内 " + n->path + "）";
        return false;
    }
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(core::path_from_utf8(n->source), ec);
    if (ec) {
        err = "取输入文件大小失败：" + n->source;
        return false;
    }
    n->size = size;

    const uint64_t bs = opt_.block_size;
    const uint64_t full_blocks = size / bs;
    const size_t tail = static_cast<size_t>(size % bs);

    std::vector<uint8_t> buf(static_cast<size_t>(bs));
    const size_t file_start = blocks_.size();
    bool any_block = false;

    for (uint64_t i = 0; i < full_blocks; ++i) {
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(bs));
        if (in.gcount() != static_cast<std::streamsize>(bs)) {
            err = "读输入文件短读：" + n->source;
            return false;
        }
        ++data_blocks_;
        any_block = true;
        if (all_zero(buf.data(), buf.size())) {
            n->block_sizes.push_back(0);
            n->sparse += bs;
            continue;
        }
        const uint32_t sum = xxh32(buf.data(), buf.size());
        bool compressed = false;
        std::vector<uint8_t> out;
        if (!compress_block(opt_.compressor, buf.data(), buf.size(), static_cast<size_t>(bs), out,
                            compressed, err)) {
            return false;
        }
        const uint64_t location = file_.size();
        if (compressed) {
            if (!file_.write(out.data(), out.size(), err)) return false;
            n->block_sizes.push_back(static_cast<uint32_t>(out.size()));
            blocks_.push_back(BlockRec{location, static_cast<uint32_t>(out.size()), sum});
        } else {
            if (!file_.write(buf.data(), buf.size(), err)) return false;
            n->block_sizes.push_back(static_cast<uint32_t>(buf.size()) | kUncompressedBit);
            blocks_.push_back(BlockRec{location,
                                       static_cast<uint32_t>(buf.size()) | kUncompressedBit, sum});
        }
        blocks_by_sum_[sum].push_back(blocks_.size() - 1);
    }

    // 参照 end_file：有数据块记过账（走过 FIRST_BLOCK）→ 结束时发一个 0 长度哨兵，
    // 哨兵触发整段去重并把 blocks_start 落到文件的第一个块上。
    if (any_block) {
        if (!dedup_file_blocks(file_start, n->blocks_start, err)) return false;
    }

    if (tail > 0) {
        std::vector<uint8_t> tailbuf(tail);
        in.read(reinterpret_cast<char*>(tailbuf.data()), static_cast<std::streamsize>(tail));
        if (in.gcount() != static_cast<std::streamsize>(tail)) {
            err = "读输入文件短读（尾部）：" + n->source;
            return false;
        }
        const uint32_t sum = xxh32(tailbuf.data(), tailbuf.size());
        if (all_zero(tailbuf.data(), tailbuf.size())) {
            // 稀疏尾：置一块长度 0 的 block_size，fragment 位置保持 0xFFFFFFFF
            n->block_sizes.push_back(0);
            n->sparse += tail;
            return true;
        }
        FragRef ref{};
        if (fragment_lookup(sum, static_cast<uint32_t>(tail), tailbuf.data(), ref, err)) {
            ++dedup_fragments_;
            n->fragment_index = ref.index;
            n->fragment_offset = ref.offset;
            return true;
        }
        if (has_pending_frag_ && pending_frag_.size() + tail > opt_.block_size) {
            if (!write_fragment_block(err)) return false;
        }
        if (!has_pending_frag_) {
            pending_frag_index_ = static_cast<uint32_t>(frag_table_.size());
            frag_table_.push_back(FragEntry{});
            pending_frag_.clear();
            has_pending_frag_ = true;
        }
        const uint32_t offset = static_cast<uint32_t>(pending_frag_.size());
        pending_frag_.insert(pending_frag_.end(), tailbuf.begin(), tailbuf.end());
        const uint64_t key = (static_cast<uint64_t>(sum) << 32) | static_cast<uint32_t>(tail);
        frag_by_key_[key].push_back(
            FragRef{pending_frag_index_, offset, static_cast<uint32_t>(tail)});
        n->fragment_index = pending_frag_index_;
        n->fragment_offset = offset;
    }
    return true;
}

void Packer::write_file_inode(std::vector<uint8_t>& out, const Node* n) {
    const bool extended = (n->blocks_start > 0xFFFFFFFFull) || (n->size > 0xFFFFFFFFull) ||
                          (n->sparse > 0);
    put16(out, extended ? kTypeFileX : kTypeFile);
    put16(out, static_cast<uint16_t>(n->mode & 0x0FFF));
    put16(out, id_index(n->uid));
    put16(out, id_index(n->gid));
    put32(out, n->mtime);
    put32(out, n->inode_num);
    if (extended) {
        put64(out, n->blocks_start);
        put64(out, n->size);
        put64(out, n->sparse);
        put32(out, 1);  // nlink
        put32(out, n->fragment_index);
        put32(out, n->fragment_offset);
        put32(out, kNoFrag);  // xattr_idx
    } else {
        put32(out, static_cast<uint32_t>(n->blocks_start));
        put32(out, n->fragment_index);
        put32(out, n->fragment_offset);
        put32(out, static_cast<uint32_t>(n->size));
    }
    for (uint32_t sz : n->block_sizes) put32(out, sz);
}

void Packer::write_dir_inode(std::vector<uint8_t>& out, const Node* n, uint64_t dir_ref,
                             uint32_t dir_size, size_t ent_count,
                             const std::vector<std::pair<uint64_t, uint64_t>>& index_records,
                             const std::vector<std::string>& index_names) {
    const uint32_t start_block = static_cast<uint32_t>(dir_ref >> 16);
    const uint16_t block_offset = static_cast<uint16_t>(dir_ref & 0xFFFF);
    const uint32_t nlink = static_cast<uint32_t>(ent_count + 2);
    const uint32_t parent_inode = (n->parent == nullptr) ? 0 : n->parent->inode_num;
    // 参照（dir_writer.c）：xattr==0xFFFFFFFF 时，只有"目录表字节数 > 0xFFFC"或
    // "条目数 ≥ 256"才升级成 EXT_DIR（start_block 超 u32 的判据在 32 位字段下不可达）。
    const bool extended = (dir_size > (0xFFFF - 3)) || (ent_count >= kDirIndexThreshold);

    put16(out, extended ? kTypeDirX : kTypeDir);
    put16(out, static_cast<uint16_t>(n->mode & 0x0FFF));
    put16(out, id_index(n->uid));
    put16(out, id_index(n->gid));
    put32(out, n->mtime);
    put32(out, n->inode_num);
    if (extended) {
        put32(out, nlink);
        put32(out, dir_size + 3);
        put32(out, start_block);
        put32(out, parent_inode);
        put16(out, static_cast<uint16_t>(index_records.size()));
        put16(out, block_offset);
        put32(out, kNoFrag);  // xattr_idx
        for (size_t i = 0; i < index_records.size(); ++i) {
            // ★ 磁盘上的 sqfs_dir_index_t 字段顺序是 **index, start_block, size**
            //   （libsquashfs 的内存结构体同序；先写 start_block 会被逐字节对拍抓住）。
            put32(out, static_cast<uint32_t>(index_records[i].second));  // index（未压缩偏移）
            put32(out, static_cast<uint32_t>(index_records[i].first));   // start_block（元数据块偏移）
            put32(out, static_cast<uint32_t>(index_names[i].size() - 1));
            out.insert(out.end(), index_names[i].begin(), index_names[i].end());
        }
    } else {
        put32(out, start_block);
        put32(out, nlink);
        put16(out, static_cast<uint16_t>(dir_size + 3));
        put16(out, block_offset);
        put32(out, parent_inode);
    }
}

bool Packer::serialize(std::string& err) {
    inode_table_start_ = file_.size();
    MetaWriter im(&file_, false, opt_.compressor);
    MetaWriter dm(&file_, true, opt_.compressor);

    for (Node* n : inodes_) {
        std::vector<uint8_t> body;
        if (n->is_dir) {
            // 目录项先写进（内存里的）目录表 —— inode 里要拿它的 (块, 偏移) 引用
            const uint64_t dir_ref = (dm.block_offset() << 16) | dm.offset();
            struct Ent {
                uint64_t inode_ref;
                uint32_t inode_num;
                uint16_t type;
                std::string name;
            };
            std::vector<Ent> ents;
            ents.reserve(n->children.size());
            for (Node* c : n->children) {
                ents.push_back(Ent{c->inode_ref, c->inode_num,
                                   static_cast<uint16_t>(c->is_dir ? kTypeDir : kTypeFile),
                                   c->name});
            }
            uint32_t dir_size = 0;
            std::vector<std::pair<uint64_t, uint64_t>> index_records;
            std::vector<std::string> index_names;
            size_t i = 0;
            while (i < ents.size()) {
                const uint32_t offset = dm.offset();
                const uint64_t block = dm.block_offset();
                size_t count = 0;
                {
                    // 复刻 get_conseq_entry_count：同一 inode 元数据块 + 编号差不越 ±32767
                    // + 不跨 8 KiB 元数据块 + 每组最多 256 条
                    size_t size = (static_cast<size_t>(offset) + 12) % kMetaBlockSize;
                    for (size_t k = i; k < ents.size(); ++k) {
                        if ((ents[k].inode_ref >> 16) != (ents[i].inode_ref >> 16)) break;
                        const long long diff = static_cast<long long>(ents[k].inode_num) -
                                               static_cast<long long>(ents[i].inode_num);
                        if (diff > 32767 || diff < -32767) break;
                        size += 8 + ents[k].name.size();
                        if (count > 0 && size > kMetaBlockSize) break;
                        ++count;
                        if (count == kMaxDirGroup) break;
                    }
                }
                std::vector<uint8_t> hdr;
                put32(hdr, static_cast<uint32_t>(count - 1));
                put32(hdr, static_cast<uint32_t>(ents[i].inode_ref >> 16));
                put32(hdr, ents[i].inode_num);
                if (!dm.append(hdr.data(), hdr.size(), err)) return false;
                index_records.emplace_back(block, dir_size);
                index_names.push_back(ents[i].name);
                dir_size += 12;

                for (size_t k = i; k < i + count; ++k) {
                    std::vector<uint8_t> ent;
                    put16(ent, static_cast<uint16_t>(ents[k].inode_ref & 0xFFFF));
                    put16(ent, static_cast<uint16_t>(static_cast<int16_t>(
                                   static_cast<int32_t>(ents[k].inode_num) -
                                   static_cast<int32_t>(ents[i].inode_num))));
                    put16(ent, ents[k].type);
                    put16(ent, static_cast<uint16_t>(ents[k].name.size() - 1));
                    ent.insert(ent.end(), ents[k].name.begin(), ents[k].name.end());
                    if (!dm.append(ent.data(), ent.size(), err)) return false;
                    dir_size += static_cast<uint32_t>(8 + ents[k].name.size());
                }
                i += count;
            }
            n->inode_ref = (im.block_offset() << 16) | im.offset();
            write_dir_inode(body, n, dir_ref, dir_size, n->children.size(), index_records,
                            index_names);
        } else {
            n->inode_ref = (im.block_offset() << 16) | im.offset();
            write_file_inode(body, n);
        }
        if (!im.append(body.data(), body.size(), err)) return false;
    }

    if (!im.flush(err)) return false;
    if (!dm.flush(err)) return false;
    root_inode_ref_ = root_->inode_ref;
    directory_table_start_ = file_.size();
    return dm.write_to_file(err);
}

bool Packer::run(const std::vector<PackItem>& items, PackResult& res) {
    std::string err;
    if (!build_tree(items, err)) {
        res.error = err;
        return false;
    }

    uint32_t counter = 0;
    alloc_inode_numbers(root_, counter);
    root_->inode_num = ++counter;
    inodes_.assign(counter, nullptr);
    map_inodes(root_);

    // 数据区：按**输入顺序**（= tar 条目顺序）处理
    for (const PackItem& item : items) {
        if (item.is_dir) continue;
        const auto it = by_path_.find(item.path);
        if (it == by_path_.end() || it->second->is_dir) {
            res.error = "内部错误：找不到条目节点 " + item.path;
            return false;
        }
        if (!process_file(it->second, err)) {
            res.error = err;
            return false;
        }
    }
    if (!write_fragment_block(err)) {
        res.error = err;
        return false;
    }

    if (!serialize(err)) {
        res.error = err;
        return false;
    }

    // fragment 表（有碎片才有；否则写 0xFFFF… 并置 NO_FRAGMENTS）
    uint16_t flags = kFlagNoXattrs | kFlagNoDuplicates | kFlagNoFragments;
    if (frag_table_.empty()) {
        fragment_table_start_ = kNoTable;
        fragment_entry_count_ = 0;
    } else {
        std::vector<uint8_t> data;
        data.reserve(frag_table_.size() * 16);
        bool any_compressed = false;
        for (const FragEntry& e : frag_table_) {
            put64(data, e.offset);
            put32(data, e.size_field);
            put32(data, 0);
            if ((e.size_field & kUncompressedBit) == 0) any_compressed = true;
        }
        if (!write_table(file_, opt_.compressor, data, fragment_table_start_, err)) {
            res.error = err;
            return false;
        }
        fragment_entry_count_ = static_cast<uint32_t>(frag_table_.size());
        flags &= ~kFlagNoFragments;
        flags |= kFlagAlwaysFragments;
        // 参照（frag_table.c）：先置"碎片全未压缩"，再**只要有任意一块被压缩就清掉**。
        flags |= kFlagUncompressedFragments;
        if (any_compressed) flags &= ~kFlagUncompressedFragments;
    }

    // id 表
    {
        std::vector<uint8_t> data;
        data.reserve(ids_.size() * 4);
        for (uint32_t id : ids_) put32(data, id);
        if (!write_table(file_, opt_.compressor, data, id_table_start_, err)) {
            res.error = err;
            return false;
        }
    }

    const uint64_t bytes_used = file_.size();
    std::vector<uint8_t> sb;
    sb.reserve(96);
    put32(sb, kMagic);
    put32(sb, static_cast<uint32_t>(inodes_.size()));
    put32(sb, opt_.mtime);
    put32(sb, opt_.block_size);
    put32(sb, fragment_entry_count_);
    // ★★ T-07：这一行是**超块的 compression 编号**（探针的主判据）；上面 7 处同样跟着
    //   `opt_.compressor` 走 —— 特别是 `load_frag_block` 的**解压**必须与**写**用同一个压缩器，
    //   否则去重比对会读到错数据（产物看起来能生成、内容是坏的）。
    put16(sb, static_cast<uint16_t>(opt_.compressor));
    put16(sb, static_cast<uint16_t>(block_log_of(opt_.block_size)));
    put16(sb, flags);
    put16(sb, static_cast<uint16_t>(ids_.size()));
    put16(sb, 4);
    put16(sb, 0);
    put64(sb, root_inode_ref_);
    put64(sb, bytes_used);
    put64(sb, id_table_start_);
    put64(sb, kNoTable);  // xattr_id_table_start
    put64(sb, inode_table_start_);
    put64(sb, directory_table_start_);
    put64(sb, fragment_table_start_);
    put64(sb, kNoTable);  // export_table_start
    if (!file_.write_at(0, sb.data(), sb.size(), err)) {
        res.error = err;
        return false;
    }

    if (opt_.dev_block_size != 0) {
        const uint64_t rem = bytes_used % opt_.dev_block_size;
        if (rem != 0) {
            const size_t pad = static_cast<size_t>(opt_.dev_block_size - rem);
            std::vector<uint8_t> zeros(pad, 0);
            if (!file_.write(zeros.data(), zeros.size(), err)) {
                res.error = err;
                return false;
            }
        }
    }

    res.ok = true;
    res.bytes_used = bytes_used;
    res.file_size = file_.size();
    res.inode_count = inodes_.size();
    res.fragment_count = frag_table_.size();
    res.data_blocks = data_blocks_;
    res.fragment_blocks = fragment_blocks_;
    res.dedup_blocks = dedup_blocks_;
    res.dedup_fragments = dedup_fragments_;
    return true;
}

}  // namespace

PackResult pack_squashfs(const std::string& out_path, const std::vector<PackItem>& items,
                         const PackOptions& options) {
    PackResult res;
    const auto t0 = std::chrono::steady_clock::now();

    if (options.block_size < 4096 || options.block_size > 1024 * 1024 ||
        (options.block_size & (options.block_size - 1)) != 0) {
        res.error = "块大小必须是 4 KiB~1 MiB 的 2 的幂";
        return res;
    }

    // ★★ 2026-10-10（P17-18，审计 01 F-16 Info）：**封包产物改原子写**。
    //   事实（修前）：`OutFile file; file.open(out_path)` 直接**流式写目标文件** ——
    //   中途失败（读盘错、压缩错、进程被杀、断电）会在目标路径留下一个**半截包**，
    //   而它看起来就是个正常的 .sqfs（用户拿去用才发现读不开）。本仓别处（原子写、
    //   逐字节对拍的产物）早有 `write_file_atomic` 的规矩，封包这条却是例外。
    //   改法：先写同目录的 `<out>.part`，成功且文件**已关闭**之后再 rename 到目标
    //   （同目录 ⇒ 同卷 ⇒ rename 是原子的）；失败则把 .part 删掉，目标路径**一个字节都不动**。
    const std::string part_path = out_path + ".part";
    bool packed_ok = false;
    {
        // ★ 全部落在**这一个作用域**里：`OutFile` 析构时才关句柄，rename 必须在它之后
        OutFile file;
        std::string err;
        if (!file.open(part_path, err)) {
            res.error = err;
            return res;
        }

        // ★ 先占住超块那 96 字节：数据区从**偏移 96** 开始（参照实现是 init 时就把超块
        //   写在盘头、finish 时再覆盖一次）。少了这一步，整条数据区会前移 96 字节，
        //   inode 里的 blocks_start 全错 —— 实测就是这么被逐字节对拍抓住的。
        {
            std::vector<uint8_t> head(96, 0);
            if (!file.write(head.data(), head.size(), err)) {
                res.error = err;
                return res;
            }
        }

        Packer packer(file, options);
        packed_ok = packer.run(items, res);
    }   // ← `OutFile` 析构：句柄在这里关闭（它没有公开的 close()，只能靠作用域）

    const auto t1 = std::chrono::steady_clock::now();
    res.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!packed_ok) {
        // ★ 失败：把半截 `.part` 清掉（清不掉也要说出来），目标路径保持原样
        std::error_code rm_ec;
        std::filesystem::remove(part_path, rm_ec);
        if (res.error.empty()) res.error = "封包失败";
        if (rm_ec) res.error += "；临时文件清不掉（" + part_path + "）：" + rm_ec.message();
        res.ok = false;
        return res;
    }

    std::error_code ren_ec;
    std::filesystem::rename(part_path, out_path, ren_ec);
    if (ren_ec) {
        res.ok = false;
        res.error = "封包产物改名失败（" + part_path + " → " + out_path + "）：" + ren_ec.message();
        return res;
    }
    res.ok = true;
    return res;
}

}  // namespace xrkit::engine::fs
