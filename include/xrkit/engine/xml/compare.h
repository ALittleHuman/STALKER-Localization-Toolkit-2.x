#pragma once
// xrkit::engine::xml —— XML 校对（对照表 ⑩ 的判据面，无 Qt）。
//
// 逐条照 1.x `apps/xml_compare_app.py` + `apps/xml_modes.py` 的**代码**：
//   · 10.1 两种内置模式是**声明**（列/筛选/行取值/明细/order），插件与内置走同一张表；
//   · 10.6 `lines` 口径 = Python `len(text.splitlines())` —— **只有真正多出一行内容才算
//     多一行**（1.x 曾对结尾换行额外 +1，于是"A 有结尾换行、B 没有"被误判成不一致）；
//   · 10.3 `detail` 列的单行摘要**只按计数拼**，绝不调完整明细 builder（1.x 实测：
//     1 万行时填充从 89ms 涨到 327ms，因为它每行都调了一次 detail_builder）；
//   · 10.4 切模式**清空旧结果、不重跑 I/O**（结果留在模型里，切模式只重算视图）；
//   · ⑫12.8 内置模式的哨兵值**只有一个**（1.x 是 `"<builtin:xml>"` 与 `"<builtin>"`
//     两套，插件重名就能顶掉出厂内置模式）。
//
// 本文件先落**统计模式**（行数/ID 计数）；文本模式要一个容错 XML 解析器，另开一片。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xrkit::engine::xml {

// ── 10.1 模式声明 ────────────────────────────────────────────────────

struct XmlColumn {
    std::string key;      // "rel"/"status"/"detail" 是**保留键**，其余按同名取值
    std::string heading;
    int width = 0;
};

struct XmlFilter {
    std::string label;
    std::vector<std::string> row_kinds;   // 命中的行类别
    std::vector<std::string> differ_types;  // 需要同时命中的差异子类型（空=不限）
};

struct XmlModeSpec {
    std::string name;      // 模式显示名（下拉取值）
    std::string key;       // **内部键**（"stats" / "text"）—— 不许拿显示名当取值
    int order = 0;
    std::vector<XmlColumn> columns;
    std::vector<XmlFilter> filters;
    std::string plugin;    // 注册者；内置＝唯一哨兵 kBuiltinPlugin
};

// 内置注册者的**唯一**哨兵值（⑫12.8：1.x 有两套，导致插件重名能顶掉内置）
inline constexpr const char* kBuiltinPlugin = "<builtin:xml>";

// 内置模式表（顺序 = order 升序：行数/ID 统计(10) → ID 文本对比(20)）
const std::vector<XmlModeSpec>& builtin_modes();
const XmlModeSpec* find_mode(const std::string& key_or_name);

// 插件模式 + 内置模式**同一张表**：按 order 升序、同 order 按注册顺序稳定排序。
// `registration_errors` 收集被拒的注册（键为空 / 键与显示名撞车 / 重复注册）。
struct ModeRegistration {
    XmlModeSpec spec;
    std::vector<std::string> registration_errors;
};

// ── 10.6 行数口径 ────────────────────────────────────────────────────

// Python `str.splitlines()` 的等价实现：边界 = \n \r \r\n \v \f \x1c \x1d \x1e
// \x85 \u2028 \u2029；**结尾是边界不额外多一行**；空串 0 行。
uint64_t split_lines(const std::string& utf8_text);

// ── 统计模式 ────────────────────────────────────────────────────────

struct XmlSideStats {
    bool ok = false;
    std::string error;
    uint64_t lines = 0;
    std::map<std::string, uint64_t> id_counts;
    std::string encoding;  // 实际用的编码名
};

// 一段文本 → 行数 + ID 计数（ID 统计**忽略 XML 声明**，与 1.x 一致）
XmlSideStats parse_stats_text(const std::string& text, const std::string& encoding);

struct XmlCompareResult {
    std::string rel;
    std::string mode = "stats";  // 内部键（显示名只用于展示/导出表头）
    bool consistent = false;
    bool only_one_side = false;
    std::string only_side;  // "a" / "b"（only_one_side 时有效）
    uint64_t lines_a = 0;
    uint64_t lines_b = 0;
    uint64_t ids_a = 0;
    uint64_t ids_b = 0;
    std::vector<std::string> only_a;    // 已排序
    std::vector<std::string> only_b;
    std::vector<std::array<std::string, 3>> count_diff;  // (id, A 次数, B 次数)
    std::vector<std::array<std::string, 3>> text_diff;   // (id, A 文本, B 文本)
    std::map<std::string, std::string> id_text_a;        // 明细里要看原文（文本模式）
    std::map<std::string, std::string> id_text_b;
    bool untrusted_a = false;  // 一侧解析失败 ⇒ 结果不可信（界面必须告警）
    bool untrusted_b = false;
    std::string warning;
    std::string encoding_a;
    std::string encoding_b;
    std::string error;
};

// 10.3 的"有没有偷调完整明细"计数器：摘要路径必须让它保持 0。
struct DetailCallStats {
    uint64_t full_detail_calls = 0;
};

// 10.3：**只按计数**拼一句摘要（每行都要算一次，代价必须与差异条目数无关）
std::string inline_detail(const XmlCompareResult& result, const std::string& name_a,
                          const std::string& name_b);
// 完整明细（1.x 的 detail_builder；只在"选中一行/双击"时调）。会 ++stats.full_detail_calls。
std::string full_detail(const XmlCompareResult& result, const std::string& name_a,
                        const std::string& name_b, DetailCallStats* stats = nullptr);

// 两侧同一相对路径的结果；one_side 表示只在一侧存在
XmlCompareResult compare_stats(const std::string& rel, const std::string& text_a,
                               const std::string& text_b, const std::string& enc_a,
                               const std::string& enc_b);
XmlCompareResult only_one_side(const std::string& rel, const std::string& side);

// ── 文本模式（ID 文本对比）──────────────────────────────────────────

struct XmlTextSide {
    bool ok = false;       // 解析成功（结构可理解）
    bool trusted = true;   // false = 解析失败，结果**不可信**（调用方必须告警，不许静默）
    std::string warning;
    std::string encoding;
    std::map<std::string, std::string> id_text;  // 后出现的同名 id 覆盖先出现的（照 ET）
};

// 1.x `_fix_entities` + ElementTree 取值的等价实现：
//   ① 修裸 `&`（不构成合法实体时补成 `&amp;`）、修裸露的 `<`（后面不是 !/?/字母时补 `&lt;`）、
//      修 `<!--` 后多余的 `-`；
//   ② 扫描元素：带 `id` 属性的元素，优先取**第一个直接子元素 `<text>` 的文本**，
//      没有就取它自己的直接文本；两者都 `.strip()`；实体解码。
//   ③ 结构不可理解（标签不闭合等）⇒ ok=false、trusted=false + warning
//      （1.x 在这里 log_summary 告警："结果不可信"）。
XmlTextSide parse_text_by_id(const std::string& text, const std::string& encoding);

// 文本模式比较（两侧文本不同的 id 记进 text_diff；同名 id 取最后出现的文本）
XmlCompareResult compare_text(const std::string& rel, const std::string& text_a,
                              const std::string& text_b, const std::string& enc_a,
                              const std::string& enc_b);

// 文本模式的完整明细（1.x `build_full_text_detail`：含每个差异 id 的首个差异位置与两侧全文）
std::string full_text_detail(const XmlCompareResult& result, const std::string& name_a,
                             const std::string& name_b, DetailCallStats* stats = nullptr);

// ── 10.5 CSV 导出 ───────────────────────────────────────────────────

struct XmlCsvReport {
    std::string mode_name;      // 显示名（导出表头用）
    std::string folder_a;
    std::string folder_b;
    // ★ 2026-10-06（1.x `xml_compare_app.py:675-676 / 1321`）：统计行里用的是**文件夹名**
    //   （`self.name_a = root_a.name`），不是字面量 "A"/"B"。留空 ⇒ 退回 "A"/"B"
    //   （CLI 的 CSV 探针不设它 ⇒ 那份参照逐字节不变）。
    std::string name_a;
    std::string name_b;
    std::string enc_a = "auto";
    std::string enc_b = "auto";
    std::string filter_label;
    std::string generated_at;   // 由调用方给（引擎不取系统时间，便于对拍）
    std::vector<std::string> columns;             // 表头（来自模式声明）
    std::vector<std::vector<std::string>> rows;   // 当前**筛选后**的行
    uint64_t consistent = 0;
    uint64_t inconsistent = 0;
    uint64_t only_a = 0;
    uint64_t only_b = 0;
    std::string encodings_used;  // "实际使用编码"那行（空则不出）
};

// 渲染成 CSV 文本：**先写 `\ufeff`**（Excel 不认无 BOM 的 UTF-8），
// 行结束符 `\r\n`（Python `csv.writer` 的 excel 方言），
// 需要时按 excel 规则加引号（含分隔符/引号/换行 ⇒ 包起来、内部引号翻倍）。
// 写盘时**不做任何换行翻译**（1.x 用 `newline=""`）。
std::string render_csv(const XmlCsvReport& report);

// 渲染 + 原子写盘（同目录 .part → 逐字节复核 → rename）
bool write_csv_file(const std::string& path, const XmlCsvReport& report, std::string& error);

}  // namespace xrkit::engine::xml
