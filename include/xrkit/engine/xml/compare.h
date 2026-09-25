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

}  // namespace xrkit::engine::xml
