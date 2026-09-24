#pragma once
// xrkit::engine::fs —— SquashFS 元数据读取（纯自实现，不依赖外部 exe）。
//
// 为什么自实现（1.x 的血账）：
//   · `sqfs2tar` 被 360 **按文件哈希拦截**（复制改名也没用），且会把整包收进内存
//     （54~99 s）；改成自读元数据后是 **0.01~0.02 s**。
//   · 因此 2.0 的引擎**零外部 exe**。
//
// 本文件只做"读元数据"（列条目 + 取大小），不做数据块解压（那是提取路径，M1 后续）。

#include "xrkit/engine/fs/decompress.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

// 超块（磁盘上 96 字节；只取用得到的字段，其余照读以推进偏移）
struct SuperBlock {
    uint32_t inodes = 0;
    uint32_t mkfs_time = 0;
    uint32_t block_size = 0;
    uint32_t fragments = 0;
    Compression compression = Compression::Unknown;
    uint16_t block_log = 0;
    uint16_t flags = 0;
    uint16_t no_ids = 0;
    uint16_t s_major = 0;
    uint16_t s_minor = 0;
    uint64_t root_inode = 0;
    uint64_t bytes_used = 0;
    uint64_t id_table_start = 0;
    uint64_t xattr_id_table_start = 0;
    uint64_t inode_table_start = 0;
    uint64_t directory_table_start = 0;
    uint64_t fragment_table_start = 0;
    bool big_endian = false;  // 大端变体 "sqsh"（1.x 只认小端，2.0 补上）
};

struct Entry {
    std::string path;  // 与 rdsquashfs --describe 同格式：**无前导斜杠**，如 levels/aver/x.dds
    bool is_dir = false;
    uint64_t size = 0;  // 目录恒 0
    uint64_t inode_ref = 0;  // 内部用：定位 inode（列表与提取共用同一次遍历结果）
};

struct ListResult {
    bool ok = false;
    std::string error;
    std::vector<Entry> entries;
    double elapsed_ms = 0.0;
};

struct FileData {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> data;
    double elapsed_ms = 0.0;
};

// 只读 96 字节超块。magic 不是 hsqs/sqsh 时返回 false。
bool read_superblock(const std::string& path, SuperBlock& out, std::string& err);

// 列出全部条目（含目录项），按 path 字节序排序（确定性，便于逐条对拍）。
ListResult list_entries(const std::string& path);

// 读取单个文件的完整内容（进程内解压）：
//   · 数据块（block_size 一块，bit24 = 未压缩，size==0 表示空洞）
//   · 尾部 fragment（块内偏移 + fragment 长度）
// 返回的 data.size() 必须等于 inode 的 size —— 不等就是解析错了，直接报错而不是给半截数据。
FileData read_file(const std::string& image_path, const std::string& entry_path);

// ── 写（M1-5）─────────────────────────────────────────────────────────
//
// 为什么自写 writer（1.x 的血账）：1.x 的封包是调外部 `tar2sqfs.exe`，而外部 exe
// 在这台机器上被 360 按文件哈希拦过。2.0 的引擎**零外部 exe**。
//
// **验收判据（用户拍板，不许降级）：产物逐字节一致** —— 同一份输入，我们的产物
// 与 1.x 的 `sqfs_pack`（tar2sqfs 1.3.2，默认 xz）**sha256 必须相等**。
// 参照实现的全部行为（inode 编号顺序、目录项排序与分组、xz 参数、表顺序、
// 结尾补零、块去重、fragment 打包）已逐项取证，见
// `docs/2.0.0-SquashFS-writer规格.md`。

// 待打进镜像的条目。顺序 = **处理顺序**（数据块在盘上的先后由它决定），
// 与 1.x 里 tar 的条目顺序同义；目录结构本身与顺序无关（每层按名字字节序排序）。
struct PackItem {
    std::string path;         // 镜像内相对路径，UTF-8，'/' 分隔，无前导斜杠
    bool is_dir = false;
    std::string source_path;  // is_dir=false 时：本机文件路径（UTF-8），内容从这里读
};

struct PackOptions {
    uint32_t block_size = 131072;  // 128 KiB（与参照一致）
    uint32_t mtime = 0;            // 1.x 的 tar 里 mtime 全 0，产物 mkfs_time 也是 0
    uint32_t dir_mode = 0755;
    uint32_t file_mode = 0644;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t dev_block_size = 4096;  // 结尾补零到该块的整数倍（tar2sqfs 默认行为）
};

struct PackResult {
    bool ok = false;
    std::string error;
    uint64_t bytes_used = 0;       // 补零之前的长度（超块里的 bytes_used）
    uint64_t file_size = 0;        // 落盘实际长度（含补零）
    uint64_t inode_count = 0;
    uint64_t fragment_count = 0;
    uint64_t data_blocks = 0;      // 实际写盘的数据块数（不含 fragment 块）
    uint64_t fragment_blocks = 0;
    uint64_t dedup_blocks = 0;     // 被去重省掉的数据块数（报账用）
    uint64_t dedup_fragments = 0;  // 被去重省掉的 fragment 数
    double elapsed_ms = 0.0;
};

// 把 items 打进 out_path。失败时返回 ok=false + error，**不留下半截产物**的保证由
// 调用方决定（本函数直接写目标文件，与 1.x 的 sqfs_pack 行为一致）。
PackResult pack_squashfs(const std::string& out_path, const std::vector<PackItem>& items,
                         const PackOptions& options = PackOptions{});

}  // namespace xrkit::engine::fs
