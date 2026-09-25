#include "xrkit/engine/xml/compare.h"

#include "xrkit/engine/text/encoding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace xrkit::engine::xml {
namespace {

bool is_line_boundary(uint32_t cp) {
    // Python `str.splitlines()` 的边界集合（含 CR/LF 组合的单字符形态）
    return cp == 0x0A || cp == 0x0D || cp == 0x0B || cp == 0x0C || cp == 0x1C || cp == 0x1D ||
           cp == 0x1E || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

bool ascii_iequal(const std::string& text, size_t pos, const char* word) {
    const size_t len = std::char_traits<char>::length(word);
    if (pos + len > text.size()) return false;
    for (size_t i = 0; i < len; ++i) {
        char a = text[pos + i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 1.x `re.sub(r"^\s*\ufeff?\s*<\?xml[^>]*\?>\s*", "", text, count=1, flags=re.I|re.S)` 的等价：
// **只在开头**匹配（`^`），允许前面有空白与 BOM，声明以 `?>` 结束，后面可以有空白。
// 不匹配就原样返回。
std::string strip_leading_declaration(const std::string& text) {
    size_t i = 0;
    // \ufeff 在 UTF-8 里是 EF BB BF
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
    while (i < text.size() && is_space(text[i])) ++i;
    if (text.size() >= 3 && text.compare(i, 3, "\xEF\xBB\xBF") == 0) {
        i += 3;
        while (i < text.size() && is_space(text[i])) ++i;
    }
    if (!ascii_iequal(text, i, "<?xml")) return text;
    const size_t end = text.find("?>", i);
    if (end == std::string::npos) return text;
    size_t after = end + 2;
    while (after < text.size() && is_space(text[after])) ++after;
    return text.substr(after);
}

// 1.x `ID_PATTERN = \bid\s*=\s*["']([^"']+)["']`（re.I）：**要求 id 前面不是词字符**
//（`\b`），等号两侧允许空白，值用单/双引号包起来且至少一个字符。
void collect_ids(const std::string& text, std::map<std::string, uint64_t>& out) {
    const auto word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    size_t i = 0;
    while (i + 2 <= text.size()) {
        if (!ascii_iequal(text, i, "id")) {
            ++i;
            continue;
        }
        // `\b`：前一个字符不能是词字符
        if (i > 0 && word_char(text[i - 1])) {
            ++i;
            continue;
        }
        size_t j = i + 2;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j >= text.size() || text[j] != '=') {
            ++i;
            continue;
        }
        ++j;
        while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
        if (j >= text.size() || (text[j] != '"' && text[j] != '\'')) {
            ++i;
            continue;
        }
        const char quote = text[j];
        const size_t value_start = j + 1;
        const size_t value_end = text.find(quote, value_start);
        if (value_end == std::string::npos || value_end == value_start) {
            ++i;
            continue;
        }
        out[text.substr(value_start, value_end - value_start)] += 1;
        i = value_end + 1;
    }
}

std::string lowered(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return text;
}

}  // namespace

uint64_t split_lines(const std::string& text) {
    if (text.empty()) return 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    uint64_t boundaries = 0;
    size_t pos = 0;
    bool ends_with_boundary = false;
    while (pos < text.size()) {
        uint32_t cp = 0;
        const size_t before = pos;
        if (!text::utf8_next(data, text.size(), pos, cp)) {
            pos = before + 1;  // 非法字节：按单字节推进（不假装理解）
            ends_with_boundary = false;
            continue;
        }
        if (cp == 0x0D) {
            if (pos < text.size() && data[pos] == 0x0A) ++pos;  // \r\n 算一个边界
            ++boundaries;
            ends_with_boundary = true;
        } else if (is_line_boundary(cp)) {
            ++boundaries;
            ends_with_boundary = true;
        } else {
            ends_with_boundary = false;
        }
    }
    // `splitlines()`：结尾是边界时**不**额外多一行；末尾还有内容时最后一段算一行
    return boundaries + (ends_with_boundary ? 0 : 1);
}

XmlSideStats parse_stats_text(const std::string& text, const std::string& encoding) {
    XmlSideStats stats;
    stats.ok = true;
    stats.encoding = encoding;
    stats.lines = split_lines(text);
    collect_ids(strip_leading_declaration(text), stats.id_counts);
    return stats;
}

const std::vector<XmlModeSpec>& builtin_modes() {
    static const std::vector<XmlModeSpec> modes = [] {
        XmlModeSpec stats_mode;
        stats_mode.name = "行数/ID 统计";
        stats_mode.key = "stats";
        stats_mode.order = 10;
        stats_mode.plugin = kBuiltinPlugin;
        stats_mode.columns = {{"rel", "文件 (相对路径)", 280},
                              {"status", "状态", 60},
                              {"lines_a", "行数 A", 90},
                              {"lines_b", "行数 B", 90},
                              {"ids_a", "ID 数 A", 90},
                              {"ids_b", "ID 数 B", 90},
                              {"detail", "差异详情", 400}};
        stats_mode.filters = {{"全部", {"same", "diff", "only_a", "only_b"}, {}},
                              {"仅不一致", {"diff"}, {}},
                              {"仅一侧存在", {"only_a", "only_b"}, {}}};

        XmlModeSpec text_mode;
        text_mode.name = "ID 文本对比";
        text_mode.key = "text";
        text_mode.order = 20;
        text_mode.plugin = kBuiltinPlugin;
        text_mode.columns = {{"rel", "文件 (相对路径)", 280},
                             {"status", "状态", 60},
                             {"diff_count", "差异数", 90},
                             {"ids_a", "ID 数 A", 90},
                             {"ids_b", "ID 数 B", 90},
                             {"detail", "差异详情", 400}};
        text_mode.filters = {{"全部", {"same", "diff", "only_a", "only_b"}, {}},
                             {"仅不一致", {"diff"}, {"only"}},
                             {"仅文本不同", {"diff"}, {"text"}},
                             {"仅一侧存在", {"only_a", "only_b"}, {}}};
        return std::vector<XmlModeSpec>{stats_mode, text_mode};
    }();
    return modes;
}

const XmlModeSpec* find_mode(const std::string& key_or_name) {
    for (const XmlModeSpec& mode : builtin_modes()) {
        if (mode.key == key_or_name || mode.name == key_or_name) return &mode;
    }
    return nullptr;
}

XmlCompareResult only_one_side(const std::string& rel, const std::string& side) {
    XmlCompareResult result;
    result.rel = rel;
    result.only_one_side = true;
    result.only_side = side;
    result.consistent = false;
    return result;
}

XmlCompareResult compare_stats(const std::string& rel, const std::string& text_a,
                               const std::string& text_b, const std::string& enc_a,
                               const std::string& enc_b) {
    const XmlSideStats a = parse_stats_text(text_a, enc_a);
    const XmlSideStats b = parse_stats_text(text_b, enc_b);
    XmlCompareResult result;
    result.rel = rel;
    result.mode = "stats";
    result.lines_a = a.lines;
    result.lines_b = b.lines;
    result.ids_a = a.id_counts.size();
    result.ids_b = b.id_counts.size();
    result.encoding_a = a.encoding;
    result.encoding_b = b.encoding;

    for (const auto& [id, count] : a.id_counts) {
        const auto it = b.id_counts.find(id);
        if (it == b.id_counts.end()) {
            result.only_a.push_back(id);
        } else if (it->second != count) {
            result.count_diff.push_back({id, std::to_string(count), std::to_string(it->second)});
        }
    }
    for (const auto& [id, count] : b.id_counts) {
        (void)count;
        if (a.id_counts.find(id) == a.id_counts.end()) result.only_b.push_back(id);
    }
    std::sort(result.only_a.begin(), result.only_a.end());
    std::sort(result.only_b.begin(), result.only_b.end());
    std::sort(result.count_diff.begin(), result.count_diff.end(),
              [](const std::array<std::string, 3>& x, const std::array<std::string, 3>& y) {
                  return x[0] < y[0];
              });
    result.consistent = result.lines_a == result.lines_b && result.only_a.empty() &&
                        result.only_b.empty() && result.count_diff.empty();
    return result;
}

std::string inline_detail(const XmlCompareResult& result, const std::string& name_a,
                          const std::string& name_b) {
    const std::string na = name_a.empty() ? "A" : name_a;
    const std::string nb = name_b.empty() ? "B" : name_b;
    if (result.only_one_side) {
        return "文件仅在一侧存在（仅 " + (result.only_side == "a" ? na : nb) + "）";
    }
    if (result.consistent) return "无差异";
    std::string out;
    const auto append = [&out](const std::string& part) {
        if (!out.empty()) out += "；";
        out += part;
    };
    if (!result.only_a.empty()) append("仅 " + na + " 有 " + std::to_string(result.only_a.size()));
    if (!result.only_b.empty()) append("仅 " + nb + " 有 " + std::to_string(result.only_b.size()));
    if (!result.count_diff.empty()) {
        append("次数不同 " + std::to_string(result.count_diff.size()));
    }
    if (out.empty()) return "有差异";
    return out;
}

std::string full_detail(const XmlCompareResult& result, const std::string& name_a,
                        const std::string& name_b, DetailCallStats* stats) {
    if (stats != nullptr) ++stats->full_detail_calls;
    const std::string na = name_a.empty() ? "A" : name_a;
    const std::string nb = name_b.empty() ? "B" : name_b;
    if (result.only_one_side) {
        return "文件仅在一侧存在: 仅 " + (result.only_side == "a" ? na : nb) + " 有 " + result.rel;
    }
    std::string out;
    const auto line = [&out](const std::string& text) {
        if (!out.empty()) out += "\n";
        out += text;
    };
    if (result.lines_a != result.lines_b) {
        const long long diff = static_cast<long long>(result.lines_a) -
                               static_cast<long long>(result.lines_b);
        line("行数: " + na + "=" + std::to_string(result.lines_a) + ", " + nb + "=" +
             std::to_string(result.lines_b) + " (差 " + std::to_string(diff < 0 ? -diff : diff) +
             ")");
    }
    if (!result.encoding_a.empty() || !result.encoding_b.empty()) {
        line("编码: " + na + "=" + (result.encoding_a.empty() ? "-" : result.encoding_a) + " / " +
             nb + "=" + (result.encoding_b.empty() ? "-" : result.encoding_b));
    }
    line("ID 数: " + na + "=" + std::to_string(result.ids_a) + ", " + nb + "=" +
         std::to_string(result.ids_b));
    if (!result.only_a.empty()) {
        line("");
        line("仅 " + na + " 有的 id (" + std::to_string(result.only_a.size()) + " 个):");
        for (const std::string& id : result.only_a) line("  id=\"" + id + "\"");
    }
    if (!result.only_b.empty()) {
        line("");
        line("仅 " + nb + " 有的 id (" + std::to_string(result.only_b.size()) + " 个):");
        for (const std::string& id : result.only_b) line("  id=\"" + id + "\"");
    }
    if (!result.count_diff.empty()) {
        line("");
        line("出现次数不同的 id (" + std::to_string(result.count_diff.size()) + " 个):");
        for (const auto& item : result.count_diff) {
            line("  id=\"" + item[0] + "\"  " + na + ":" + item[1] + " / " + nb + ":" + item[2]);
        }
    }
    if (result.only_a.empty() && result.only_b.empty() && result.count_diff.empty() &&
        result.lines_a == result.lines_b) {
        line("(无可列出的差异)");
    }
    return out;
}

}  // namespace xrkit::engine::xml
