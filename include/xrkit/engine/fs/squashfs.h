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
};

struct ListResult {
    bool ok = false;
    std::string error;
    std::vector<Entry> entries;
    double elapsed_ms = 0.0;
};

// 只读 96 字节超块。magic 不是 hsqs/sqsh 时返回 false。
bool read_superblock(const std::string& path, SuperBlock& out, std::string& err);

// 列出全部条目（含目录项），按 path 字节序排序（确定性，便于逐条对拍）。
ListResult list_entries(const std::string& path);

}  // namespace xrkit::engine::fs
