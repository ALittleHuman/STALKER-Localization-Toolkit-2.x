#include "xrkit/engine/xml/compare.h"

#include "probe_decls.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/engine/text/encoding.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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

// ── 1.x `_fix_entities` 的三个正则 ───────────────────────────────────
// `&(?!amp;|lt;|gt;|quot;|apos;|#x?[0-9a-fA-F]+;)` → `&amp;`
// `<(?![/!?a-zA-Z])` → `&lt;`
// `<!--(-+)` → `<!--`
bool starts_valid_entity(const std::string& text, size_t pos) {
    // text[pos] == '&'
    static const char* kNames[] = {"amp;", "lt;", "gt;", "quot;", "apos;"};
    for (const char* name : kNames) {
        const size_t len = std::char_traits<char>::length(name);
        if (pos + 1 + len <= text.size() && text.compare(pos + 1, len, name) == 0) return true;
    }
    size_t i = pos + 1;
    if (i < text.size() && text[i] == '#') {
        ++i;
        if (i < text.size() && (text[i] == 'x' || text[i] == 'X')) ++i;
        size_t digits = 0;
        while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
            ++digits;
        }
        if (digits > 0 && i < text.size() && text[i] == ';') return true;
    }
    return false;
}

std::string fix_entities(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&' && !starts_valid_entity(text, i)) {
            out += "&amp;";
            continue;
        }
        if (c == '<') {
            const char next = (i + 1 < text.size()) ? text[i + 1] : '\0';
            const bool ok = next == '!' || next == '/' || next == '?' ||
                            std::isalpha(static_cast<unsigned char>(next)) != 0;
            if (!ok) {
                out += "&lt;";
                continue;
            }
            // `<!--(-+)` → `<!--`：注释开头后多余的 '-' 折叠掉
            if (text.compare(i, 4, "<!--") == 0) {
                out += "<!--";
                i += 3;
                while (i + 1 < text.size() && text[i + 1] == '-') ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// ── 实体解码（ElementTree 会解），未知实体按 1.x 修完后的样子当字面量 ──
std::string decode_entities(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const size_t semi = text.find(';', i);
        if (semi == std::string::npos || semi - i > 12) {
            out += text[i];
            continue;
        }
        const std::string name = text.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        bool known = true;
        if (name == "amp") cp = '&';
        else if (name == "lt") cp = '<';
        else if (name == "gt") cp = '>';
        else if (name == "quot") cp = '"';
        else if (name == "apos") cp = '\'';
        else if (!name.empty() && name[0] == '#') {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            try {
                cp = static_cast<uint32_t>(std::stoul(name.substr(hex ? 2 : 1), nullptr,
                                                      hex ? 16 : 10));
            } catch (...) {
                known = false;
            }
        } else {
            known = false;
        }
        if (!known) {
            out += text[i];
            continue;
        }
        text::utf8_append(out, cp);
        i = semi;
    }
    return out;
}

std::string strip_ascii(const std::string& text) {
    size_t b = 0;
    size_t e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])) != 0) --e;
    return text.substr(b, e - b);
}

// 元素：名字 + id 属性 + "文本"（到第一个子元素为止的字符数据）+ 子元素列表
struct XmlElementNode {
    std::string name;
    std::string id;
    std::string text;
    std::vector<XmlElementNode> children;
};

// 容错扫描：把 `<tag ...>` / `</tag>` / `<![CDATA[...]]>` / 注释 / PI 解析成一棵树。
// 只实现 STALKER XML 用得到的子集；结构坏到无法理解时返回 false（结果不可信）。
bool scan_elements(const std::string& text, XmlElementNode& root, std::string& error) {
    std::vector<XmlElementNode*> stack{&root};
    size_t i = 0;
    bool saw_text_at_top = false;
    while (i < text.size()) {
        if (text[i] != '<') {
            size_t next = text.find('<', i);
            if (next == std::string::npos) next = text.size();
            const std::string chunk = text.substr(i, next - i);
            XmlElementNode* cur = stack.back();
            if (cur->children.empty() && !(stack.size() == 1 && saw_text_at_top)) {
                cur->text += decode_entities(chunk);  // 只吃"第一个子元素之前"的文本
            }
            i = next;
            continue;
        }
        if (text.compare(i, 4, "<!--") == 0) {
            const size_t end = text.find("-->", i);
            if (end == std::string::npos) {
                error = "注释没有结束";
                return false;
            }
            // XML 不允许注释体里出现 `--`；ElementTree 会直接抛 ⇒ 1.x 判"解析失败
            // （结果不可信）"。我们照做：**不许比 1.x 更宽**，否则坏文件会被静默判成"没差异"。
            const std::string body = text.substr(i + 4, end - (i + 4));
            if (body.find("--") != std::string::npos) {
                error = "注释体里含 --（XML 非法）";
                return false;
            }
            i = end + 3;
            continue;
        }
        if (text.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = text.find("]]>", i);
            if (end == std::string::npos) {
                error = "CDATA 没有结束";
                return false;
            }
            XmlElementNode* cur = stack.back();
            if (cur->children.empty()) cur->text += text.substr(i + 9, end - i - 9);
            i = end + 3;
            continue;
        }
        if (text.compare(i, 2, "<?") == 0) {
            const size_t end = text.find("?>", i);
            if (end == std::string::npos) {
                error = "处理指令没有结束";
                return false;
            }
            i = end + 2;
            continue;
        }
        if (text.compare(i, 2, "<!") == 0) {  // DOCTYPE 之类：跳到 '>'
            const size_t end = text.find('>', i);
            if (end == std::string::npos) {
                error = "声明没有结束";
                return false;
            }
            i = end + 1;
            continue;
        }
        if (text.compare(i, 2, "</") == 0) {
            const size_t end = text.find('>', i);
            if (end == std::string::npos) {
                error = "结束标签没有结束";
                return false;
            }
            const std::string name = strip_ascii(text.substr(i + 2, end - i - 2));
            if (stack.size() <= 1) {
                error = "结束标签比开始标签多：" + name;
                return false;
            }
            if (stack.back()->name != name) {
                error = "标签不匹配：期望 </" + stack.back()->name + ">，得到 </" + name + ">";
                return false;
            }
            stack.pop_back();
            i = end + 1;
            continue;
        }
        // 开始标签（含自闭合）
        const size_t end = text.find('>', i);
        if (end == std::string::npos) {
            error = "开始标签没有结束";
            return false;
        }
        std::string inner = text.substr(i + 1, end - i - 1);
        bool self_closing = false;
        if (!inner.empty() && inner.back() == '/') {
            self_closing = true;
            inner.pop_back();
        }
        // 名字
        size_t name_end = 0;
        while (name_end < inner.size() &&
               std::isspace(static_cast<unsigned char>(inner[name_end])) == 0 &&
               inner[name_end] != '/') {
            ++name_end;
        }
        XmlElementNode node;
        node.name = inner.substr(0, name_end);
        if (node.name.empty()) {
            error = "空元素名";
            return false;
        }
        // 属性：name = "value"（单/双引号都认）
        size_t p = name_end;
        while (p < inner.size()) {
            while (p < inner.size() && std::isspace(static_cast<unsigned char>(inner[p])) != 0) ++p;
            if (p >= inner.size()) break;
            size_t attr_start = p;
            while (p < inner.size() && inner[p] != '=' &&
                   std::isspace(static_cast<unsigned char>(inner[p])) == 0) {
                ++p;
            }
            const std::string attr_name = inner.substr(attr_start, p - attr_start);
            while (p < inner.size() && std::isspace(static_cast<unsigned char>(inner[p])) != 0) ++p;
            if (p < inner.size() && inner[p] == '=') {
                ++p;
                while (p < inner.size() &&
                       std::isspace(static_cast<unsigned char>(inner[p])) != 0) {
                    ++p;
                }
                if (p < inner.size() && (inner[p] == '"' || inner[p] == '\'')) {
                    const char quote = inner[p];
                    const size_t value_start = p + 1;
                    const size_t value_end = inner.find(quote, value_start);
                    if (value_end == std::string::npos) {
                        error = "属性值没有结束：" + attr_name;
                        return false;
                    }
                    const std::string value = inner.substr(value_start, value_end - value_start);
                    if (lowered(attr_name) == "id") node.id = decode_entities(value);
                    p = value_end + 1;
                    continue;
                }
            }
            if (attr_name.empty()) break;  // 防死循环
        }
        if (stack.size() == 1 && !node.name.empty()) saw_text_at_top = true;

        XmlElementNode* parent = stack.back();
        parent->children.push_back(std::move(node));
        if (!self_closing) stack.push_back(&parent->children.back());
        i = end + 1;
    }
    if (stack.size() != 1) {
        error = "标签没有闭合：" + stack.back()->name;
        return false;
    }
    return true;
}

void collect_by_id(const XmlElementNode& node, std::map<std::string, std::string>& out) {
    for (const XmlElementNode& child : node.children) {
        if (!child.id.empty()) {
            // 优先取第一个直接子元素 `<text>` 的文本；没有就取自己的直接文本
            const XmlElementNode* text_child = nullptr;
            for (const XmlElementNode& grand : child.children) {
                if (grand.name == "text") {
                    text_child = &grand;
                    break;
                }
            }
            if (text_child != nullptr && !text_child->text.empty()) {
                // ★ 判据照 1.x 的**原始文本**（truthy）而不是 strip 之后的值：
                //   `elif text_elem.text:` 只要求非空，存进去的才是 `.strip()`。
                //   实测差别很大：`<specific_character id=...>` 的直接文本是 "\n\t\t"
                //   （truthy ⇒ 1.x 会记一条 id → ""），用 strip 判空就会整片漏掉。
                out[child.id] = strip_ascii(text_child->text);
            } else if (!child.text.empty()) {
                out[child.id] = strip_ascii(child.text);
            }
        }
        collect_by_id(child, out);
    }
}

// `_first_diff_info` + `_char_desc`（首个差异位置的字符描述）
bool first_diff_info(const std::string& a, const std::string& b, size_t& pos, std::string& ca,
                     std::string& cb) {
    const size_t limit = std::min(a.size(), b.size());
    for (size_t i = 0; i < limit; ++i) {
        if (a[i] != b[i]) {
            pos = i;
            ca = a.substr(i, 1);
            cb = b.substr(i, 1);
            return true;
        }
    }
    return false;
}

std::string char_desc(const std::string& ch) {
    if (ch == "\xEF\xBF\xBD") return "� U+FFFD(坏字符/替换符)";
    std::string out = "'" + ch + "' U+";
    // 码位（按 UTF-8 解，失败就按字节）
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ch.data());
    size_t pos = 0;
    uint32_t cp = 0;
    if (text::utf8_next(data, ch.size(), pos, cp)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04X", cp);
        out += buf;
    } else {
        out += "?";
    }
    return out;
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
    if (!result.text_diff.empty()) {
        append("文本不同 " + std::to_string(result.text_diff.size()));
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

XmlTextSide parse_text_by_id(const std::string& text, const std::string& encoding) {
    XmlTextSide side;
    side.encoding = encoding;
    const std::string fixed = fix_entities(text);
    XmlElementNode root;
    std::string error;
    if (!scan_elements(fixed, root, error)) {
        // 1.x 在这里 `log_summary("XML 解析失败（结果不可信）…", "warn")`：
        // 解析失败**绝不能静默** —— 此时"两侧都没有 id"是假象。
        side.ok = false;
        side.trusted = false;
        side.warning = "XML 解析失败（结果不可信）：" + error;
        return side;
    }
    collect_by_id(root, side.id_text);
    side.ok = true;
    side.trusted = true;
    return side;
}

XmlCompareResult compare_text(const std::string& rel, const std::string& text_a,
                              const std::string& text_b, const std::string& enc_a,
                              const std::string& enc_b) {
    const XmlTextSide a = parse_text_by_id(text_a, enc_a);
    const XmlTextSide b = parse_text_by_id(text_b, enc_b);
    XmlCompareResult result;
    result.rel = rel;
    result.mode = "text";
    result.ids_a = a.id_text.size();
    result.ids_b = b.id_text.size();
    result.encoding_a = a.encoding;
    result.encoding_b = b.encoding;
    result.untrusted_a = !a.trusted;
    result.untrusted_b = !b.trusted;
    result.id_text_a = a.id_text;
    result.id_text_b = b.id_text;
    if (!a.trusted) result.warning = a.warning;
    if (!b.trusted) {
        if (!result.warning.empty()) result.warning += "\n";
        result.warning += b.warning;
    }

    for (const auto& [id, value] : a.id_text) {
        const auto it = b.id_text.find(id);
        if (it == b.id_text.end()) {
            result.only_a.push_back(id);
        } else if (it->second != value) {
            result.text_diff.push_back({id, value, it->second});
        }
    }
    for (const auto& [id, value] : b.id_text) {
        (void)value;
        if (a.id_text.find(id) == a.id_text.end()) result.only_b.push_back(id);
    }
    std::sort(result.only_a.begin(), result.only_a.end());
    std::sort(result.only_b.begin(), result.only_b.end());
    std::sort(result.text_diff.begin(), result.text_diff.end(),
              [](const std::array<std::string, 3>& x, const std::array<std::string, 3>& y) {
                  return x[0] < y[0];
              });
    result.consistent = result.only_a.empty() && result.only_b.empty() && result.text_diff.empty();
    return result;
}

std::string full_text_detail(const XmlCompareResult& result, const std::string& name_a,
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
    if (!result.encoding_a.empty() || !result.encoding_b.empty()) {
        line("编码: " + na + "=" + (result.encoding_a.empty() ? "-" : result.encoding_a) + " / " +
             nb + "=" + (result.encoding_b.empty() ? "-" : result.encoding_b));
    }
    line("ID 数: " + na + "=" + std::to_string(result.ids_a) + ", " + nb + "=" +
         std::to_string(result.ids_b));
    const auto preview = [](const std::string& text) {
        return text.size() > 60 ? text.substr(0, 60) + "…" : text;
    };
    if (!result.only_a.empty()) {
        line("");
        line("仅 " + na + " 有的 id (" + std::to_string(result.only_a.size()) + " 个):");
        for (const std::string& id : result.only_a) {
            const auto it = result.id_text_a.find(id);
            line("  id=\"" + id + "\"  →  " +
                 preview(it == result.id_text_a.end() ? std::string() : it->second));
        }
    }
    if (!result.only_b.empty()) {
        line("");
        line("仅 " + nb + " 有的 id (" + std::to_string(result.only_b.size()) + " 个):");
        for (const std::string& id : result.only_b) {
            const auto it = result.id_text_b.find(id);
            line("  id=\"" + id + "\"  →  " +
                 preview(it == result.id_text_b.end() ? std::string() : it->second));
        }
    }
    if (!result.text_diff.empty()) {
        line("");
        line("文本不同的 id (" + std::to_string(result.text_diff.size()) + " 个):");
        for (const auto& item : result.text_diff) {
            line("  id=\"" + item[0] + "\"");
            size_t pos = 0;
            std::string ca;
            std::string cb;
            if (first_diff_info(item[1], item[2], pos, ca, cb)) {
                line("    首个差异@" + std::to_string(pos) + ": " + na + "=" + char_desc(ca) +
                     "  vs  " + nb + "=" + char_desc(cb));
            }
            line("    " + na + ": " + item[1]);
            line("    " + nb + ": " + item[2]);
            line("");
        }
    }
    if (result.only_a.empty() && result.only_b.empty() && result.text_diff.empty()) {
        line("(无可列出的差异)");
    }
    return out;
}

std::string render_csv(const XmlCsvReport& report) {
    // Python `csv.writer` 的 excel 方言：分隔符 `,`、行结束符 `\r\n`、
    // 含分隔符/引号/换行时用 `"` 包起来并把内部引号翻倍。
    const auto field = [](const std::string& value) {
        bool quote = false;
        for (char c : value) {
            if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                quote = true;
                break;
            }
        }
        if (!quote) return value;
        std::string out = "\"";
        for (char c : value) {
            if (c == '"') out += "\"\"";
            else out += c;
        }
        out += "\"";
        return out;
    };
    std::string out;
    out += "\xEF\xBB\xBF";  // 10.5：先写 BOM（Excel 不认无 BOM 的 UTF-8）
    const auto row = [&out, &field](const std::vector<std::string>& cells) {
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i != 0) out += ",";
            out += field(cells[i]);
        }
        out += "\r\n";
    };
    row({"XML校对报告"});
    row({"生成时间: " + report.generated_at});
    row({"比较模式: " + report.mode_name});
    row({"文件夹 A: " + report.folder_a});
    row({"文件夹 B: " + report.folder_b});
    row({"编码（auto=自动判定）: A=" + report.enc_a + " / B=" + report.enc_b});
    if (!report.encodings_used.empty()) row({"实际使用编码: " + report.encodings_used});
    row({"筛选条件: " + report.filter_label});
    // ★ 2026-10-06（1.x `:1321` `f"一致: {cons} | 不一致: {incon} | 仅 {na}: {oa} | 仅 {nb}: {ob}"`）：
    //   统计行用**文件夹名**；调用方没给名字（CLI 探针）才退回 "A"/"B"。
    const std::string na = report.name_a.empty() ? "A" : report.name_a;
    const std::string nb = report.name_b.empty() ? "B" : report.name_b;
    row({"一致: " + std::to_string(report.consistent) +
         " | 不一致: " + std::to_string(report.inconsistent) +
         " | 仅 " + na + ": " + std::to_string(report.only_a) +
         " | 仅 " + nb + ": " + std::to_string(report.only_b)});
    row({});
    row(report.columns);
    for (const std::vector<std::string>& cells : report.rows) row(cells);
    return out;
}

bool write_csv_file(const std::string& path, const XmlCsvReport& report, std::string& error) {
    const std::string text = render_csv(report);
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    // 写盘**不做换行翻译**（1.x 用 `newline=""`）—— 原子写只按字节落盘，正好等价
    return core::write_file_atomic(path, bytes, /*atomic=*/true, error);
}

}  // namespace xrkit::engine::xml

// ── ★★ 2026-10-06（审计第 4 组）：模块**在编**探针（见 `probe_decls.h`）────────
// 本模块被摘掉 ⇒ 链接失败，而不是 `status_line()` 里少报一个数字。
extern "C" const char* xrkit_engine_probe_xml_tag() { return "xrkit-engine-module-xml"; }

namespace xrkit::engine {
const char* probe_xml() { return xrkit_engine_probe_xml_tag(); }
}  // namespace xrkit::engine

