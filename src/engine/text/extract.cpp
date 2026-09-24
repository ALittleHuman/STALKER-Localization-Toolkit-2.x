#include "xrkit/engine/text/extract.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/text/encoding.h"

// ★ PCRE2 要求先定义代码单元宽度（我们只用 8-bit），否则 pcre2.h 直接 #error
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace xrkit::engine::text {
namespace {

// ── PCRE2 薄封装 ─────────────────────────────────────────────────────
class Regex {
public:
    Regex() = default;
    ~Regex() {
        if (code_ != nullptr) pcre2_code_free_8(code_);
    }
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

    bool compile(const char* pattern, uint32_t extra, std::string& err) {
        int errcode = 0;
        PCRE2_SIZE erroffset = 0;
        // PCRE2_UTF|PCRE2_UCP 对齐 Python re 的 Unicode 语义（\s 等）
        code_ = pcre2_compile_8(reinterpret_cast<PCRE2_SPTR>(pattern), PCRE2_ZERO_TERMINATED,
                                PCRE2_UTF | PCRE2_UCP | extra, &errcode, &erroffset, nullptr);
        if (code_ == nullptr) {
            PCRE2_UCHAR buf[256] = {};
            pcre2_get_error_message_8(errcode, buf, sizeof(buf));
            err = std::string("正则编译失败：") + reinterpret_cast<const char*>(buf) +
                  "（模式 " + pattern + "）";
            return false;
        }
        return true;
    }

    bool search(const std::string& subject) const {
        pcre2_match_data* md = pcre2_match_data_create_from_pattern_8(code_, nullptr);
        const int rc = pcre2_match_8(code_, reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                     subject.size(), 0, 0, md, nullptr);
        pcre2_match_data_free_8(md);
        return rc >= 0;
    }

    // 取第 1 组（没有分组时取整段）的全部匹配，按出现顺序
    std::vector<std::string> findall(const std::string& subject, size_t group = 1) const {
        std::vector<std::string> out;
        pcre2_match_data* md = pcre2_match_data_create_from_pattern_8(code_, nullptr);
        PCRE2_SIZE offset = 0;
        while (offset <= subject.size()) {
            const int rc = pcre2_match_8(code_, reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                         subject.size(), offset, 0, md, nullptr);
            if (rc < 0) break;
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer_8(md);
            const size_t idx = (rc > static_cast<int>(group)) ? group : 0;
            if (ov[2 * idx] != PCRE2_UNSET) {
                out.emplace_back(subject, ov[2 * idx], ov[2 * idx + 1] - ov[2 * idx]);
            }
            offset = (ov[1] > offset) ? ov[1] : offset + 1;
        }
        pcre2_match_data_free_8(md);
        return out;
    }

    // Python `re.sub(pattern, literal, subject)` 的等价（替换串是纯字面量时）
    std::string sub_literal(const std::string& subject, const std::string& replacement) const {
        std::string out;
        pcre2_match_data* md = pcre2_match_data_create_from_pattern_8(code_, nullptr);
        PCRE2_SIZE offset = 0;
        PCRE2_SIZE copied = 0;
        while (offset <= subject.size()) {
            const int rc = pcre2_match_8(code_, reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                         subject.size(), offset, 0, md, nullptr);
            if (rc < 0) break;
            PCRE2_SIZE* ov = pcre2_get_ovector_pointer_8(md);
            out.append(subject, copied, ov[0] - copied);
            out += replacement;
            copied = ov[1];
            offset = (ov[1] > offset) ? ov[1] : offset + 1;
        }
        out.append(subject, copied, subject.size() - copied);
        pcre2_match_data_free_8(md);
        return out;
    }

private:
    pcre2_code* code_ = nullptr;
};

const Regex& re_config_tag() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        // 1.x: <(?:text|bio|title|name)(?:| [ \S]*?[^/]) *?>([^<>]*?)</(?:text|bio|title|name)>
        x->compile("<(?:text|bio|title|name)(?:| [ \\S]*?[^/]) *?>([^<>]*?)"
                   "</(?:text|bio|title|name)>",
                   0, err);
        return x;
    }();
    return r;
}

const Regex& re_config_attr() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        // 1.x: (?:hint|name)\s*=\s*((?:"[^"]*")|(?:'[^']*'))
        x->compile("(?:hint|name)\\s*=\\s*((?:\"[^\"]*\")|(?:'[^']*'))", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_script_permit() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        // 1.x: ([Mm]essage|[Tt]ext(?!ure)|(?<![a-z])[Nn]ews(?![a-z]))
        x->compile("([Mm]essage|[Tt]ext(?!ure)|(?<![a-z])[Nn]ews(?![a-z]))", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_script_sensitive() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        // 1.x: (exec|write|parse_names|load|(?<!de)script(?!ion)|call|set(?![Tt]ext)|
        //       open|sound|effect|abort|print|console|cmd|return)
        x->compile("(exec|write|parse_names|load|(?<!de)script(?!ion)|call|set(?![Tt]ext)|"
                   "open|sound|effect|abort|print|console|cmd|return)",
                   0, err);
        return x;
    }();
    return r;
}

const Regex& re_match_sensitive() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("(\"[\\s]*return)", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_looks_like_script_1() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile(":\\d", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_looks_like_script_2() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("load\\s+~+", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_self_output() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("_(?:gameplay|scripts)_texts\\.xml$", PCRE2_CASELESS, err);
        return x;
    }();
    return r;
}

const Regex& re_entity_amp() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("&[\\s]+amp;", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_entity_lt() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("&[\\s]+lt;", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_entity_bare() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("&(?!ensp;|emsp;|nbsp;|lt;|gt;|amp;|quot;|copy;|reg;|trade;|times;|divide;)",
                   0, err);
        return x;
    }();
    return r;
}

const Regex& re_xml_comment() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("<!--[\\s\\S]*?-->", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_xml_decl() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("<\\?[^>]+\\?>", 0, err);
        return x;
    }();
    return r;
}

const Regex& re_tag_start() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        // 1.x 的 tc = A-Z_a-z\u00C0-\u00D6\u00D8-\u00F6\u00F8-\u02FF\u0370-\u037D\u037F-\u1FFF
        //                \u200C-\u200D\u2070-\u218F\u2C00-\u2FEF\u3001-\uD7FF\uF900-\uFDCF
        //                \uFDF0-\uFFFD
        x->compile("<(?![A-Z_a-z\\x{00C0}-\\x{00D6}\\x{00D8}-\\x{00F6}\\x{00F8}-\\x{02FF}"
                   "\\x{0370}-\\x{037D}\\x{037F}-\\x{1FFF}\\x{200C}-\\x{200D}"
                   "\\x{2070}-\\x{218F}\\x{2C00}-\\x{2FEF}\\x{3001}-\\x{D7FF}"
                   "\\x{F900}-\\x{FDCF}\\x{FDF0}-\\x{FFFD}/])",
                   0, err);
        return x;
    }();
    return r;
}

const Regex& re_script_comment() {
    static const Regex& r = *[] {
        auto* x = new Regex();
        std::string err;
        x->compile("--[\\s\\S]*$", 0, err);
        return x;
    }();
    return r;
}

// ── 码位级小工具（与 Python str 语义对齐）─────────────────────────────
bool is_rus_letter(uint32_t cp) {
    return (cp >= 0x0410 && cp <= 0x044F) || cp == 0x0401 || cp == 0x0451;
}

bool is_ascii_letter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

bool has_rus_letter(const std::string& s) {
    size_t i = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    while (i < s.size()) {
        uint32_t cp = 0;
        if (!utf8_next(p, s.size(), i, cp)) return false;
        if (is_rus_letter(cp)) return true;
    }
    return false;
}

bool word_pattern_hit(const std::string& s) {
    size_t i = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    while (i < s.size()) {
        uint32_t cp = 0;
        if (!utf8_next(p, s.size(), i, cp)) return false;
        if (is_rus_letter(cp) || is_ascii_letter(cp)) return true;
    }
    return false;
}

// Python str.strip() 的空白集合（Unicode）
bool is_py_space(uint32_t cp) {
    switch (cp) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
        case 0x85: case 0xA0: case 0x1680:
            return true;
        default: break;
    }
    return (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000;
}

std::string py_strip(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    size_t i = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
    while (i < s.size()) {
        const size_t before = i;
        uint32_t cp = 0;
        if (!utf8_next(p, s.size(), i, cp) || !is_py_space(cp)) {
            i = before;
            break;
        }
        begin = i;
    }
    // 从尾部剥：按码位往回找（UTF-8 连续字节 0b10xxxxxx 跳过）
    while (end > begin) {
        size_t k = end - 1;
        while (k > 0 && (static_cast<uint8_t>(s[k]) & 0xC0) == 0x80) --k;
        uint32_t cp = 0;
        size_t j = k;
        if (!utf8_next(reinterpret_cast<const uint8_t*>(s.data()), s.size(), j, cp)) break;
        if (!is_py_space(cp)) break;
        end = k;
    }
    return s.substr(begin, end - begin);
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string ascii_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// ── 候选判定（1.x 同名函数）──────────────────────────────────────────
bool looks_like_id(const std::string& text) {
    if (has_rus_letter(text)) return false;
    if (text.find('_') != std::string::npos) return true;
    return text.find(' ') == std::string::npos;
}

bool looks_like_script(const std::string& text) {
    if (has_rus_letter(text)) return false;
    if (text.find('=') != std::string::npos || text.find('@') != std::string::npos) return true;
    return re_looks_like_script_1().search(text) || re_looks_like_script_2().search(text);
}

std::vector<std::string> sorted_unique(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// ── 三分支的候选提取（1.x 同名函数）──────────────────────────────────
std::vector<std::string> script_texts(const std::string& text) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        line = py_strip(re_script_comment().sub_literal(line, ""));
        bool is_open = false;
        bool is_escape = false;
        std::string ongoing;
        char quote_char = '\0';
        std::set<std::string> line_matches;

        const size_t n = line.size();
        for (size_t i = 0; i < n; ++i) {
            const char ch = line[i];
            if (ch == '"' || ch == '\'') {
                if (ch == quote_char || quote_char == '\0') {
                    if (!is_open) {
                        ongoing.assign(1, ch);
                        quote_char = ch;
                        is_open = true;
                        continue;
                    }
                    if (is_escape) {
                        ongoing.push_back(ch);
                        is_escape = false;
                    } else {
                        ongoing.push_back(ch);
                        is_open = false;
                        quote_char = '\0';
                        if (!re_match_sensitive().search(ongoing) && word_pattern_hit(ongoing)) {
                            line_matches.insert(ongoing);
                        }
                    }
                    continue;
                }
            }
            if (is_open) {
                if (ch == '\\') {
                    is_escape = !is_escape;
                    if (!is_escape) {
                        ongoing += "\\\\";
                    } else {
                        ongoing += '\\';
                    }
                } else if (ch == 'n' && is_escape) {
                    ongoing += '\n';
                    is_escape = false;
                } else {
                    ongoing += ch;
                }
            }
        }

        std::vector<std::string> sorted_lm(line_matches.begin(), line_matches.end());
        // 1.x: sorted(line_matches, key=len, reverse=True)（稳定：等长保持集合迭代序，
        //      而集合序随哈希随机化 —— 只在"等长且都要替换"时才有可见差异，见规格文档）
        std::stable_sort(sorted_lm.begin(), sorted_lm.end(),
                         [](const std::string& a, const std::string& b) {
                             return a.size() > b.size();
                         });

        std::string sac_line = line;
        for (const std::string& lm : sorted_lm) {
            const char qc = lm[0];
            const std::string inner = lm.substr(1, lm.size() >= 2 ? lm.size() - 2 : 0);
            std::string escaped = inner;
            // _escape_literal_text(inner, quote=qc)
            std::string tmp;
            tmp.reserve(escaped.size());
            for (size_t k = 0; k < escaped.size(); ++k) {
                const char c = escaped[k];
                if (c == '\\') {
                    tmp += "\\\\";
                } else if (c == '\n') {
                    tmp += "\\n";
                } else if (c == '"' && qc == '"') {
                    tmp += "\\\"";
                } else if (c == '\'' && qc == '\'') {
                    tmp += "\\'";
                } else {
                    tmp.push_back(c);
                }
            }
            const std::string needle = std::string(1, qc) + tmp + std::string(1, qc);
            // Python str.replace：全部替换
            std::string replaced;
            size_t from = 0;
            while (true) {
                const size_t hit = sac_line.find(needle, from);
                if (hit == std::string::npos) {
                    replaced.append(sac_line, from, std::string::npos);
                    break;
                }
                replaced.append(sac_line, from, hit - from);
                from = hit + needle.size();
            }
            sac_line = replaced;
        }

        const std::string norm = ascii_lower(sac_line);
        if (re_script_permit().search(norm)) {
            // result = set(sorted_lm + list(result))
            std::vector<std::string> merged = sorted_lm;
            merged.insert(merged.end(), result.begin(), result.end());
            result = merged;
        } else if (!re_script_sensitive().search(norm)) {
            std::vector<std::string> merged = sorted_lm;
            merged.insert(merged.end(), result.begin(), result.end());
            result = merged;
        }
    }
    return sorted_unique(result);
}

}  // namespace

// ── 公开接口 ─────────────────────────────────────────────────────────
std::vector<std::string> config_xml_candidates(const std::string& text) {
    std::vector<std::string> candidates;
    for (const std::string& m : re_config_tag().findall(text, 1)) {
        const std::string t = py_strip(m);
        if (!t.empty()) candidates.push_back(t);
    }
    for (const std::string& m : re_config_attr().findall(text, 1)) {
        if (m.size() > 2) candidates.push_back(m.substr(1, m.size() - 2));
    }
    return sorted_unique(candidates);
}

std::vector<std::string> script_candidates(const std::string& text) {
    return script_texts(text);
}

std::vector<std::string> ltx_candidates(const std::string& text) {
    std::vector<std::string> cands;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::string line = text.substr(pos, end - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        // 行内 ';' 之后是注释；再 strip
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line = line.substr(0, semi);
        line = py_strip(line);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ascii_lower(py_strip(line.substr(0, eq)));
        std::string val = py_strip(line.substr(eq + 1));
        if (val.empty()) continue;
        if (key.rfind("inv_name", 0) == 0 || key == "description") {
            // val.strip('"\'')
            size_t b = 0;
            size_t e = val.size();
            while (b < e && (val[b] == '"' || val[b] == '\'')) ++b;
            while (e > b && (val[e - 1] == '"' || val[e - 1] == '\'')) --e;
            cands.push_back(val.substr(b, e - b));
        } else if (key == "title" || key == "descr") {
            std::string plain;
            // re.sub(r"\{[^}]*\}", "", val)
            size_t k = 0;
            while (k < val.size()) {
                if (val[k] == '{') {
                    const size_t close = val.find('}', k);
                    if (close != std::string::npos) {
                        k = close + 1;
                        continue;
                    }
                }
                plain.push_back(val[k]);
                ++k;
            }
            size_t p = 0;
            while (p <= plain.size()) {
                const size_t comma = plain.find(',', p);
                const size_t e2 = (comma == std::string::npos) ? plain.size() : comma;
                std::string piece = py_strip(plain.substr(p, e2 - p));
                size_t b = 0;
                size_t e = piece.size();
                while (b < e && (piece[b] == '"' || piece[b] == '\'')) ++b;
                while (e > b && (piece[e - 1] == '"' || piece[e - 1] == '\'')) --e;
                piece = piece.substr(b, e - b);
                if (!piece.empty()) cands.push_back(piece);
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        }
    }
    return sorted_unique(cands);
}

std::string normalize_xml_string(const std::string& text, bool need_fix_st, bool delete_header) {
    std::string s = text;
    if (need_fix_st) delete_header = true;
    if (s.find("</") != std::string::npos) {
        while (!s.empty() && s[0] != '<') s.erase(0, 1);
    }
    s = re_entity_amp().sub_literal(s, "&amp;");
    s = re_entity_lt().sub_literal(s, "&lt;");
    s = re_entity_bare().sub_literal(s, "&amp;");
    s = re_xml_comment().sub_literal(s, "");
    if (delete_header) s = re_xml_decl().sub_literal(s, "");
    s = re_tag_start().sub_literal(s, "&lt;");
    s = py_strip(s);
    if (s.rfind("&lt;?xml", 0) == 0) s = "<" + s.substr(4);
    if (need_fix_st) {
        if (s.rfind("<string_table>", 0) != 0) {
            s = "<string_table>" + s + "</string_table>";
        }
        // re.sub(r"</string_table>[\s\S]+", "</string_table>", s)：从**第一个**闭合标签起全砍
        const std::string close = "</string_table>";
        const size_t at = s.find(close);
        if (at != std::string::npos) s = s.substr(0, at + close.size());
    }
    return s;
}

namespace {

std::string escape_xml_content(const std::string& text) {
    std::string s;
    s.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&') {
            s += "&amp;";
        } else if (c == '"') {
            s += "&quot;";
        } else if (c == '\'') {
            s += "&apos;";
        } else if (c == '<') {
            s += "&lt;";
        } else {
            s.push_back(c);
        }
    }
    s = py_strip(s);
    if (s.empty()) s = "\xC2\xA0";  // U+00A0（1.x 的 '\xa0'）
    return s;
}

std::vector<std::string> split_text_at_length(const std::string& text, size_t n) {
    // ★ 逐字节切（Python 切的是字符）。这里按**码位**切，与 Python 一致。
    std::vector<size_t> offsets;
    {
        size_t i = 0;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
        while (i < text.size()) {
            offsets.push_back(i);
            uint32_t cp = 0;
            if (!utf8_next(p, text.size(), i, cp)) {
                offsets.push_back(text.size());
                break;
            }
        }
        offsets.push_back(text.size());
    }
    const size_t chars = offsets.size() - 1;
    if (chars == 0) return {std::string()};
    std::vector<std::string> out;
    const size_t full = chars / n;
    for (size_t k = 0; k < full; ++k) {
        out.push_back(text.substr(offsets[k * n], offsets[(k + 1) * n] - offsets[k * n]));
    }
    out.push_back(text.substr(offsets[full * n]));
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

// 1.x `_escape_literal_text(text, quote)`
std::string escape_literal_text(const std::string& text, char quote) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '"' && quote == '"') {
            out += "\\\"";
        } else if (c == '\'' && quote == '\'') {
            out += "\\'";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 1.x `_ltx_source_literal`：定位应被整体替掉的源片段（含引号）；定位不到返回 false
bool ltx_source_literal(const std::string& text, const std::string& value, std::string& out) {
    for (char qc : {'"', '\''}) {
        const std::string needle = std::string(1, qc) + value + std::string(1, qc);
        if (text.find(needle) != std::string::npos) {
            out = needle;
            return true;
        }
    }
    const size_t idx = text.find(value);
    if (idx == std::string::npos) return false;
    const char before = (idx > 0) ? text[idx - 1] : '\0';
    const char after = (idx + value.size() < text.size()) ? text[idx + value.size()] : '\0';
    if (before == '\'' || before == '"' || after == '\'' || after == '"') return false;
    out = value;
    return true;
}

// Python str.replace 的全部替换
std::string replace_all(std::string subject, const std::string& needle,
                        const std::string& replacement) {
    if (needle.empty()) return subject;
    std::string out;
    size_t from = 0;
    while (true) {
        const size_t hit = subject.find(needle, from);
        if (hit == std::string::npos) {
            out.append(subject, from, std::string::npos);
            break;
        }
        out.append(subject, from, hit - from);
        out += replacement;
        from = hit + needle.size();
    }
    return out;
}

std::string read_file_bytes_utf8(const std::string& path, bool& ok) {
    std::ifstream in(core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ok = true;
    return data;
}

// Python 文本模式**写**：把 \n 翻成 os.linesep（Windows = \r\n）。
// 1.x 的 `_generate_output_file` 与 `_generate_text_xml` 都是文本模式写，
// 所以产物里是 CRLF —— 不对拍就发现不了（看代码只会以为写的是 \n）。
std::string to_windows_newlines(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\n') {
            out += "\r\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool write_bytes(const std::string& path, const std::string& data, std::string& err) {
    const std::filesystem::path fs_path = core::path_from_utf8(path);
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);
    std::ofstream out(fs_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "无法写文件：" + path;
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) {
        err = "写文件失败：" + path;
        return false;
    }
    return true;
}

std::string to_utf8(const std::filesystem::path& p) {
    return core::path_to_utf8(p);
}

struct Found {
    std::string rel;   // UTF-8，'/' 分隔
    std::filesystem::path full;
};

// 1.x：`sorted(sp.rglob(pattern))`；PurePath 在 Windows 上按 **str(path).lower()** 比较。
// 我们按 (ASCII 小写 rel, rel 字节) 排序 —— 与 1.x 一致的前提是"路径里没有大小写折叠
// 会改变相对次序的字符"，黄金语料在测试里显式校验这一点。
std::vector<Found> collect(const std::filesystem::path& root, const std::vector<std::string>& exts) {
    std::vector<Found> out;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        std::string ext = to_utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        bool wanted = false;
        for (const std::string& e : exts) {
            if (ext == e) {
                wanted = true;
                break;
            }
        }
        if (!wanted) continue;
        std::error_code ec3;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec3);
        if (ec3) continue;
        std::string rel_utf8 = to_utf8(rel);
        std::replace(rel_utf8.begin(), rel_utf8.end(), '\\', '/');
        out.push_back(Found{rel_utf8, it->path()});
    }
    std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
        const std::string la = ascii_lower(a.rel);
        const std::string lb = ascii_lower(b.rel);
        if (la != lb) return la < lb;
        return a.rel < b.rel;
    });
    return out;
}

bool path_has_segment(const std::string& rel, const std::string& segment_or_prefix,
                      bool prefix_match) {
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string seg =
            rel.substr(pos, (slash == std::string::npos ? rel.size() : slash) - pos);
        if (seg == segment_or_prefix) return true;
        if (prefix_match && seg.rfind(segment_or_prefix, 0) == 0) return true;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

}  // namespace

std::string generate_text_xml_body(
    const std::vector<std::pair<std::string, std::string>>& items) {
    std::vector<std::string> lines;
    lines.emplace_back("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    lines.emplace_back("<string_table>");
    for (const auto& kv : items) {
        const std::string safe_id = escape_xml_content(kv.first);
        const std::string safe_text =
            join(split_text_at_length(escape_xml_content(kv.second), 1000), "\n");
        lines.push_back("\t<string id=\"" + safe_id + "\">");
        lines.push_back("\t\t<text>" + safe_text + "</text>");
        lines.push_back("\t</string>");
    }
    lines.emplace_back("</string_table>");
    return join(lines, "\n") + "\n";
}

bool kind_from_dir_name(const std::string& source_dir, ExtractKind& out, std::string& err) {
    std::filesystem::path p = core::path_from_utf8(source_dir);
    std::string base = to_utf8(p.filename());
    if (base.empty()) base = to_utf8(p.parent_path().filename());
    const std::string lower = ascii_lower(base);
    if (lower == "gameplay") {
        out = ExtractKind::Gameplay;
        return true;
    }
    if (lower == "scripts") {
        out = ExtractKind::Scripts;
        return true;
    }
    err = "请选择 scripts 或 gameplay 文件夹本身（不要选它们的上层目录）";
    return false;
}

std::string validate_request(const ExtractRequest& request) {
    std::error_code ec;
    const std::filesystem::path source = core::path_from_utf8(request.source_dir);
    if (request.source_dir.empty() || !std::filesystem::is_directory(source, ec)) {
        // 1.x 在 `run_extraction` 里 `os.path.isdir` 失败抛 FileNotFoundError
        return "源目录不存在（或不是目录）：" + request.source_dir;
    }
    if (request.target_dir.empty()) {
        // 文案逐字照 1.x：这也是"源目录始终只读"这条口径的用户可见表述
        return "请指定输出目录：改写后的源文件与文本 XML 都写入输出目录，"
               "源目录中的原文件不会被覆盖（原地覆盖后原文不可恢复，故不支持）";
    }
    ExtractKind kind = ExtractKind::Gameplay;
    std::string err;
    if (!kind_from_dir_name(request.source_dir, kind, err)) return err;

    // 7.5（照 `_plan_output_dirs` 的代码，不是照文档措辞）：
    // 真正被拒的是"**目标子目录撞上源子目录**" —— 因为 types 恒等于源目录自己的名字，
    // 所以等价于 `target/源目录名 == 源目录`，即**输出目录是源目录的父目录**。
    //（对照表 7.5 把它写成"输出目录 == 源目录"，那是简化说法：target == source 时
    //  产物落在 source/gameplay|scripts 子目录里，并不覆盖源文件，1.x 并不拒绝。）
    const std::filesystem::path target_sub =
        core::path_from_utf8(request.target_dir) / source.filename();
    if (core::path_equals(core::path_to_utf8(source), core::path_to_utf8(target_sub))) {
        return "输出目录会覆盖源目录中的原文件（且不可恢复），请另选输出目录";
    }
    return {};
}

ExtractStats extract_dir(const std::string& source_dir, const std::string& target_dir,
                         const std::string& xml_out_dir, const ExtractOptions& options) {
    ExtractStats st;
    const auto t0 = std::chrono::steady_clock::now();

    std::string err;
    if (!kind_from_dir_name(source_dir, st.kind, err)) {
        st.error = err;
        return st;
    }
    const bool gameplay = (st.kind == ExtractKind::Gameplay);
    const std::string type_dir = gameplay ? "gameplay" : "scripts";
    const std::string prefix = options.prefix.empty() ? kDefaultIdPrefix : options.prefix;
    const std::string id_prefix =
        options.id_prefix.empty()
            ? ("sgtat_" + prefix + "_" + (gameplay ? "gp" : "sc") + "_0")
            : options.id_prefix;

    const std::filesystem::path src = core::path_from_utf8(source_dir);
    const std::filesystem::path dst = core::path_from_utf8(target_dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(src, ec)) {
        st.error = "源目录不存在：" + source_dir;
        return st;
    }

    if (options.clean) {
        std::filesystem::remove_all(dst, ec);
        std::filesystem::remove(core::path_from_utf8(
            (std::filesystem::path(core::path_from_utf8(xml_out_dir)) /
             (prefix + "_" + type_dir + "_texts.xml")).string()), ec);
    }

    const std::vector<Found> files =
        collect(src, gameplay ? std::vector<std::string>{".xml"}
                              : std::vector<std::string>{".script", ".ltx"});
    std::vector<std::pair<std::string, std::string>> extracted;  // (id, 原文) 保序
    uint64_t counter = 0;

    for (const Found& f : files) {
        // 1.x 的排除：translated_* 路径段 / gameplay 额外排除 text/ 与自己的产物
        if (path_has_segment(f.rel, "translated_", true)) continue;
        if (gameplay && (path_has_segment(f.rel, "text", false) ||
                         re_self_output().search(f.rel))) {
            continue;
        }

        const std::string src_path = to_utf8(f.full);
        const DecodeResult dec = read_text_file(src_path);
        if (!dec.ok) {
            st.skipped.push_back(f.rel + "：" + dec.error);
            continue;
        }

        std::vector<std::string> cands;
        std::string out_text;
        if (gameplay) {
            const std::string whole = normalize_xml_string(dec.text, false, false);
            cands = config_xml_candidates(whole);
            out_text = whole;
        } else {
            const bool file_is_ltx = ascii_lower(to_utf8(f.full.extension())) == ".ltx";
            cands = file_is_ltx ? ltx_candidates(dec.text) : script_candidates(dec.text);
            out_text = dec.text;
        }

        // 替换表：按（长度降序，插入序）—— 1.x `sorted(replacement, key=len, reverse=True)`
        struct Repl {
            std::string old_text;
            std::string id;
            size_t order;
        };
        std::vector<Repl> repls;
        const bool is_ltx = !gameplay && ascii_lower(to_utf8(f.full.extension())) == ".ltx";
        for (const std::string& raw : cands) {
            // ★ 三分支的"内层文本"各不相同（1.x 原文）：
            //   gameplay：候选即内容；script：候选是**带引号字面量**，判定/入库/计数都用去引号后的；
            //   ltx：候选本来就是裸值。
            const std::string inner =
                (!gameplay && !is_ltx && raw.size() >= 2) ? raw.substr(1, raw.size() - 2) : raw;
            if (gameplay || is_ltx) {
                if (py_strip(raw).empty()) continue;
            }
            if (looks_like_id(inner)) continue;
            if (!gameplay && looks_like_script(inner)) continue;

            std::string old_text;
            if (gameplay) {
                old_text = inner;
            } else if (is_ltx) {
                if (!ltx_source_literal(out_text, inner, old_text)) {
                    st.skipped.push_back(f.rel + "：无法安全定位字面量 " + inner);
                    continue;
                }
            } else {
                const char qc = raw[0];
                old_text = std::string(1, qc) + escape_literal_text(inner, qc) + std::string(1, qc);
            }
            const std::string eid = id_prefix + "_" + std::to_string(counter);
            ++counter;
            st.chars += utf8_codepoint_count(inner);
            extracted.emplace_back(eid, inner);
            repls.push_back(Repl{old_text, eid, repls.size()});
        }

        std::stable_sort(repls.begin(), repls.end(), [](const Repl& a, const Repl& b) {
            if (a.old_text.size() != b.old_text.size()) return a.old_text.size() > b.old_text.size();
            return a.order < b.order;
        });
        std::string body = out_text;
        for (const Repl& r : repls) {
            body = replace_all(body, r.old_text,
                               gameplay ? r.id : ("game.translate_string(\"" + r.id + "\")"));
        }
        if (repls.empty()) body = out_text;

        // ★ 两个分支的**写回编码不一样**（1.x 源码就是这么写的，别"统一"）：
        //   · gameplay：`_generate_output_file(out, text, encoding=enc)` —— 用检测到的编码；
        //   · scripts/ltx：同一个函数但**没传 encoding** ⇒ 走默认 `utf-8`。
        //   后果：scripts 侧即使源是 cp1251，产物也是 UTF-8（字节数与源不同）。
        //   这是被"整棵产物树逐字节对拍"抓出来的，只看条数/文本 XML 永远发现不了。
        const std::string out_encoding = gameplay ? dec.encoding : std::string(kUtf8);
        std::vector<uint8_t> encoded;
        if (!encode_strict(out_encoding, to_windows_newlines(body), encoded, err)) {
            st.skipped.push_back(f.rel + "：" + err);
            continue;
        }
        std::filesystem::path out_path = dst / core::path_from_utf8(f.rel);
        if (!write_bytes(to_utf8(out_path), std::string(encoded.begin(), encoded.end()), err)) {
            st.skipped.push_back(f.rel + "：" + err);
            continue;
        }
        ++st.files;
    }

    if (!extracted.empty()) {
        const std::string name = prefix + "_" + type_dir + "_texts.xml";
        const std::string body = to_windows_newlines(generate_text_xml_body(extracted));
        const std::filesystem::path xml_path =
            std::filesystem::path(core::path_from_utf8(xml_out_dir)) / core::path_from_utf8(name);
        if (!write_bytes(to_utf8(xml_path), body, err)) {
            st.error = err;
            return st;
        }
        st.texts_xml_name = name;
    }
    st.strings = extracted.size();

    const auto t1 = std::chrono::steady_clock::now();
    st.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    st.ok = true;
    return st;
}

}  // namespace xrkit::engine::text
