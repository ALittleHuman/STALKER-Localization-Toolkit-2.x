#pragma once
// xrkit::engine::fs —— X-Ray DB 归档（六种格式）。
//
// 逐值照抄 1.x 的 file_system/stalker_fs.py：条目布局、终止判据（xdb 的 `nf < 16`）、
// 名字长度（`nf - 16`）、"后者为准"的自洽判定（auto_detect **唯一自洽才返回**）都不能改。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xrkit/engine/plugin/registry.h"

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

// ★ 2026-10-06（用户红线）：「**不能静默报错，不然出问题了还不知道**」——
//   cp1251 表示不了的码点会被**逐码点顶替成 `?`**（ALPHA.40 修掉了"截断成空名/撞名"），
//   但那件事以前**只发生在字节里**：产物名字与源名对不上，用户没有任何提示。
//   这里把"改写台账"作为**可选出参**给出来，由调用方（GUI 日志 / CLI 收尾）**逐条打印 + 计数**。
//   口径：**不阻塞、不加 `--strict-names`** —— 封包照样成功，只把事实说清楚。
struct DbNameRewrite {
    std::string source;   // UTF-8 原名（目录名**带尾部 `\`**，与写进包里的形态一致）
    std::string written;  // 实际写进包里的名字（cp1251 字节 → UTF-8 回读，`?` 原样保留）
};

// ★★ 2026-10-18（T-05，第三方只读审计）：**DB 封包的内存闸门**。
//
// 问题：封包全程在内存里（`DbPackItem.data` → `pack_db` 拼的连续 `data` →
//   `write_file_atomic` 又拷一份）⇒ 峰值 2–3 倍总大小，**修前无任何上限**（1.9 GB 级包会 OOM）。
// 处置：一条与 `untrusted_limits.h` **同源**的上限 `XRKIT_DB_MAX_PACK_BYTES`（2 GiB）。
//   · `db_pack_max_bytes()` 返回**生效值**；环境变量 `XRKIT_DB_PACK_LIMIT` **只能往小改**
//     （与 `01 F-09` 的整包读闸门同一套解析规则：下界 >0、上界为默认值）；
//   · **两道闸**：调用方（CLI / 官方 fs 页）在**读文件之前**先量总量（不白分配）；
//     `pack_db` 自己再量一次（公开 API 的任何调用方都不许绕过）。
//   ★ 真正的解法是流式封包；有了它这条上限才能放开 —— 见 `untrusted_limits.h` 的注释。
std::uint64_t db_pack_max_bytes();

// 封包（写路径）：LZHUF 编码 + scrambler，产物应与 1.x 的 pack_db **逐字节一致**。
// `rewrites` 非空 ⇒ 填**逐条**改写台账（返回后为空 vector = 一个名字都没被改写）。
// ★ `error` 是**可选的出错理由**，**不是**开关：不给它照样检查、照样拒绝。
//   T-04（第三方只读审计 2026-10-18）：两条 4 GiB 窄化检查（单条目 / 累计偏移）是
//   **无条件**的；命中时 `pack_db` 返回**空 vector**（调用方**不许写盘**），
//   有 `error` 就把理由写进去，没有就只是不报理由 —— **产物形态不受出参有无影响**。
std::vector<uint8_t> pack_db(const std::vector<DbPackItem>& items, DbFormat fmt,
                             std::vector<DbNameRewrite>* rewrites = nullptr,
                             std::string* error = nullptr);

// ── seek 式读盘（大包的唯一可行路径）─────────────────────────────────
// X-Ray DB 的布局是 `[DATA 块头 + 数据][HEADER 块头 + 条目表]` ⇒ **条目表在文件尾部**。
// 旧实现"整文件读进内存再解析"：1900 MB 的包光读盘就要 8 s（实测耗时只与文件大小成正比、
// 与条目数无关），1.9 GB 级的包更是把整个镜像常驻内存。下面两个入口只读
// **块头 + HEADER 块**（几 MB）：列条目变快，解包不再整包进内存。
//
// 判据：与内存版**逐值一致**（同一个 HEADER 块、同一套自洽判定、同一条错误文案路径）；
// 任何一步对不上都**不猜**，直接退回整文件读（`used_seek == false` = 走的就是旧路径）。
struct DbListing {
    bool ok = false;
    std::string error;
    uint64_t file_size = 0;
    DbFormat format = DbFormat::Unknown;
    std::vector<DbEntry> entries;
    double elapsed_ms = 0.0;
    uint64_t header_bytes = 0;   // 实际读进来的字节数（判据：必须远小于 file_size）
    // ★★ P17-137（复审计 09 分册 ④）：**这两个字段是四态，不是两态** —— 契约写成真值表。
    //   来历：P17-127 加了"**显式选定格式 ⇒ 直走 seek**"的近路 ⇒ 比原来的两态多出一种组合。
    //   | used_seek | reason  | 含义                                                     |
    //   |-----------|---------|----------------------------------------------------------|
    //   | true      | 空      | 正常 seek：只读块头与尾部表，**没有**任何退回/近路（最常见） |
    //   | true      | **非空** | **按显式选定的格式直走 seek**（未整包读；理由说的是"为什么直走"）|
    //   | false     | 非空    | **退回**整包读（理由说的是"为什么不得不整包读"）           |
    //   | false     | 空      | 不该出现（退回却没理由 ⇒ 那本身是缺陷，页面会喊出来）      |
    //   ★ 名字是历史包袱：`whole_file_reason` 现在装的是"这一次**读取方式的理由**"，
    //     不只是"整包读的理由"。改名的代价远大于写清这张表 ⇒ **保留名字、把语义写死在这里**。
    bool used_seek = false;      // true = 只读了块头与尾部表（见上面真值表；**不等于**"没退回"）
    // ★★ 2026-10-10（P17-70，审计 01 F-09 C 步）：**退回整包读的"理由"**（空 = 没退回）。
    //   口径：字节数（`header_bytes`）说"读了多少"，这个字段说"**为什么**不得不整包读"
    //   —— 两个一起露给用户才算可观测（零静默）。
    std::string whole_file_reason;
    bool ambiguous = false;      // 唯一自洽判定出多个候选（此时走旧路径裁决）
    // ★★ T-02（第三方只读审计 2026-10-18）：**这次列条目读的是哪份文件**。
    //   没有插件解密器命中时 == 入参 `path`；命中时 == **解密产物**的路径
    //   （`%TEMP%\xrkit_decrypted\<name>.decrypted`）。
    //   为什么必须露出来：`entries` 里的 `offset` 是**按这份文件**算的。逐条提取如果拿原始
    //   加密路径去读，就会「列条目成功、逐条提取报偏移越界/读出垃圾」—— 修前的真实现状。
    std::string read_path;
};

// ★★ 2026-10-11（P17-85，审计 01 F-09 C 步的**最后一处**）：把"这次是怎么读的"三个事实
//   **独立成一个可跨层携带的小结构**。
//
//   为什么需要它（修前的形状）：`package_entries()`（`overlay.cpp:541`）拿到 `DbListing` 之后
//   **只把 `entries` 搬出去**，`header_bytes` / `used_seek` / `whole_file_reason` 当场丢掉
//   ⇒ 界面与 Python 那两条消费点**根本看不到**"这次读了整包、为什么"。
//   口径：**空/假 = 没有发生"整包读"这件事**（SquashFS 分支就不该报 —— 它本来不整包读；
//   "如实留空"比编一个理由好）。
struct DbReadInfo {
    uint64_t header_bytes = 0;        // 实际读进来的字节数
    bool used_seek = false;           // true = 只读块头与尾部表；false = 退回整文件读
    std::string whole_file_reason;    // 空 = 没退回；非空 = **为什么**不得不整包读
    bool ambiguous = false;           // 自洽判定出多个候选（回退时通常为真）
};

// 列条目（`package_entries` 与 CLI `--dump` 用）。`db_fallback` 语义与原来完全一致：
// 先唯一自洽判定，判不出来才用它。
//
// ★ 2026-10-04：本入口也过**解密闸门**（`plugin::gate_active_read`）—— CLI 的 `--dump`/`--extract`
//   直接走它，闸门不许只出现在"加载包"那条路上。`registry` 显式传入者优先（`package_entries`
//   会把自己的那一份传下来，免得内层又去问全局钩子、把上面那份判定推翻）；
//   为空 ⇒ 用 `plugin::active_registry()`；没有活动注册表 ⇒ 行为与今天逐字节一致。
DbListing list_db_seek(const std::string& path, std::optional<DbFormat> db_fallback,
                       const plugin::Registry* registry = nullptr);

// 单条目 seek 提取：只读那一条的那一段（解包大 DB 时不再整包进内存）。
// 与 `extract_entry(raw, entry)` 的产物**逐值一致**（同一段字节、同一套解码）。
// ★★ T-02（第三方只读审计 2026-10-18）：**本入口也过解密闸门**（`plugin::gate_active_read`）。
//   修前只有 `list_db_seek` 过闸门 ⇒ 加密包"列得出来、逐条提取却读原始密文"。
//   传**解密产物**的路径（`DbListing::read_path`）进来时，闸门认得出"已在产物目录里"⇒ 不二次解密。
DbExtract extract_entry_seek(const std::string& path, const DbEntry& entry);

}  // namespace xrkit::engine::fs
