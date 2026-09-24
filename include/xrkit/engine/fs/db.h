#pragma once
// xrkit::engine::fs —— X-Ray DB 归档（六种格式）。
//
// 逐值照抄 1.x 的 file_system/stalker_fs.py：条目布局、终止判据（xdb 的 `nf < 16`）、
// 名字长度（`nf - 16`）、"后者为准"的自洽判定（auto_detect **唯一自洽才返回**）都不能改。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

enum class DbFormat { Unknown = 0, Xdb, Ru2947, Ww2947, F2945, F2215, F11xx };

const char* db_format_key(DbFormat f);   // "xdb" / "2947ru" / ...
const char* db_format_name(DbFormat f);  // 用户可见名 "XDB (CS/CoP)" / ...
// 内部键或用户可见名都能认（1.x 有过"显示名直接送进 pack_db"的事故）
std::optional<DbFormat> db_format_from_any(const std::string& name_or_key);

struct DbEntry {
    std::string path;
    uint64_t offset = 0;
    uint64_t size_real = 0;
    uint64_t size_comp = 0;
    uint32_t crc = 0;
    bool is_dir = false;
    bool use_lzhuf = false;
};

struct DbList {
    bool ok = false;
    std::string error;
    DbFormat format = DbFormat::Unknown;
    std::vector<DbEntry> entries;
    double elapsed_ms = 0.0;
};

// 从内存镜像解析条目表。fmt 必须已知（auto 请先调 auto_detect）。
DbList unpack_db(const std::vector<uint8_t>& raw, DbFormat fmt);

// 自洽性判定：每种格式跑一遍，**唯一自洽才返回**，歧义返回 nullopt（让调用方显式选）。
std::optional<DbFormat> auto_detect(const std::vector<uint8_t>& raw);

struct DbExtract {
    bool ok = false;          // ok 且 data 为空 = **合法的空文件**；ok=false 才是解不出来
    std::string error;
    std::vector<uint8_t> data;
};

DbExtract extract_entry(const std::vector<uint8_t>& raw, const DbEntry& entry);

struct DbPackItem {
    std::string path;  // 内部用 '/'，写出时转 '\\'
    std::vector<uint8_t> data;
    bool is_dir = false;
};

// 封包（写路径）：LZHUF 编码 + scrambler，产物应与 1.x 的 pack_db **逐字节一致**。
std::vector<uint8_t> pack_db(const std::vector<DbPackItem>& items, DbFormat fmt);

}  // namespace xrkit::engine::fs
