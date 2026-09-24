#include "xrkit/engine/font/charset.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/font/xml_bytes.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/sniff.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

// 生成物（tools/gen_font_templates.py，门禁第 [13] 步 --check 锁）
#include "charset_groups.inc"  // NOLINT

// 跳过 "<" 起始的一个标签（属性值里的 '>' 不算结束）。
size_t skip_tag(const std::string& s, size_t i) {
    char quote = 0;
    for (size_t p = i + 1; p < s.size(); ++p) {
        const char c = s[p];
        if (quote != 0) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return p + 1;
        }
    }
    return s.size();
}

void add_cp(std::set<uint32_t>& chars, uint32_t cp) {
    if (cp == '\r' || cp == '\n') return;
    if (cp < 0x20 && cp != '\t') return;
    if (cp >= 0xD800 && cp <= 0xDFFF) return;
    chars.insert(cp);
}

// XML 实体 → 码位；不是合法实体返回 false（调用方按原样的 '&' 处理）。
bool decode_entity(const std::string& s, size_t i, uint32_t& cp, size_t& next) {
    // s[i] == '&'
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) return false;
    const std::string body = s.substr(i + 1, semi - i - 1);
    if (body == "amp") { cp = '&'; next = semi + 1; return true; }
    if (body == "lt") { cp = '<'; next = semi + 1; return true; }
    if (body == "gt") { cp = '>'; next = semi + 1; return true; }
    if (body == "quot") { cp = '"'; next = semi + 1; return true; }
    if (body == "apos") { cp = '\''; next = semi + 1; return true; }
    if (body.size() >= 2 && body[0] == '#') {
        uint32_t value = 0;
        bool hex = (body[1] == 'x' || body[1] == 'X');
        const size_t start = hex ? 2 : 1;
        if (start >= body.size()) return false;
        for (size_t k = start; k < body.size(); ++k) {
            const char c = body[k];
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            if (digit < 0) return false;
            value = value * (hex ? 16u : 10u) + static_cast<uint32_t>(digit);
            if (value > 0x10FFFF) return false;
        }
        cp = value;
        next = semi + 1;
        return true;
    }
    return false;
}

// 1.x `parse_xml_texts` 的等价：取所有**文本节点**（含 CDATA），解 XML 实体。
// ET 会对坏 XML 抛异常走另两条回退，但那两条最终拿到的也是"标签之外的那段文本"，
// 所以这里用同一个扫描器覆盖三条路径（真语料上的等价性由门禁对拍保证）。
void scan_xml_texts(const std::string& s, std::set<uint32_t>& chars) {
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            if (s.compare(i, 4, "<!--") == 0) {
                const size_t end = s.find("-->", i + 4);
                i = (end == std::string::npos) ? s.size() : end + 3;
                continue;
            }
            if (s.compare(i, 9, "<![CDATA[") == 0) {
                const size_t end = s.find("]]>", i + 9);
                const size_t stop = (end == std::string::npos) ? s.size() : end;
                for (size_t p = i + 9; p < stop;) {
                    uint32_t cp = 0;
                    if (!text::utf8_next(reinterpret_cast<const uint8_t*>(s.data()), stop, p, cp)) {
                        break;
                    }
                    add_cp(chars, cp);
                }
                i = (end == std::string::npos) ? s.size() : end + 3;
                continue;
            }
            if (s.compare(i, 2, "<?") == 0) {
                const size_t end = s.find("?>", i + 2);
                i = (end == std::string::npos) ? s.size() : end + 2;
                continue;
            }
            i = skip_tag(s, i);
            continue;
        }
        if (s[i] == '&') {
            uint32_t cp = 0;
            size_t next = 0;
            if (decode_entity(s, i, cp, next)) {
                add_cp(chars, cp);
                i = next;
                continue;
            }
            add_cp(chars, '&');
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const size_t pos = i;
        if (!text::utf8_next(reinterpret_cast<const uint8_t*>(s.data()), s.size(), i, cp)) {
            i = pos + 1;
            continue;
        }
        add_cp(chars, cp);
    }
}

void add_literal(const char* utf8, std::set<uint32_t>& chars) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
    const size_t n = std::strlen(utf8);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        if (!text::utf8_next(p, n, i, cp)) break;
        chars.insert(cp);
    }
}

std::vector<uint8_t> read_all_bytes(const std::string& path) {
    std::ifstream in(core::path_from_utf8(path), std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

bool ends_with_xml(const std::string& name) {
    if (name.size() < 4) return false;
    std::string tail = name.substr(name.size() - 4);
    for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return tail == ".xml";
}

}  // namespace

std::string charset_to_lines(const std::vector<uint32_t>& chars) {
    std::string out;
    char buf[16];
    for (uint32_t cp : chars) {
        std::snprintf(buf, sizeof(buf), "%X\n", cp);
        out += buf;
    }
    return out;
}

std::vector<uint32_t> required_chars() {
    std::set<uint32_t> chars;
    add_literal(kAsciiChars, chars);
    for (uint32_t cp = kCyrillicBlockFrom; cp <= kCyrillicBlockTo; ++cp) chars.insert(cp);
    add_literal(kAnsiBridge, chars);
    return std::vector<uint32_t>(chars.begin(), chars.end());
}

bool extract_chars(const std::string& xml_dir, bool full_cyrillic, CharsetResult& out) {
    out = CharsetResult{};
    std::error_code ec;
    const std::filesystem::path root = core::path_from_utf8(xml_dir);
    if (!std::filesystem::is_directory(root, ec)) return false;

    std::set<uint32_t> chars;
    // ① 起始集合：ASCII/俄文基础 + 完整西里尔块 + ANSI 桥接
    add_literal(kAsciiChars, chars);
    for (uint32_t cp = kCyrillicBlockFrom; cp <= kCyrillicBlockTo; ++cp) chars.insert(cp);
    add_literal(kAnsiBridge, chars);

    // ② 递归收集 .xml（升序，确定性）
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (ends_with_xml(core::path_to_utf8(it->path().filename()))) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    for (const std::filesystem::path& path : files) {
        ++out.files_scanned;
        const std::vector<uint8_t> raw = read_all_bytes(core::path_to_utf8(path));
        std::string text;
        std::string enc;
        bool trusted = true;
        if (!decode_xml_bytes(raw, text, enc, trusted)) {
            ++out.files_undecodable;
            continue;
        }
        if (!trusted) out.guessed.push_back(core::path_to_utf8(path.filename()));
        scan_xml_texts(text, chars);
    }

    // ⑤ 并入生僻补集；"复刻旧包"模式再剔除丢失的西里尔
    add_literal(kFontgenExtra, chars);
    if (!full_cyrillic) {
        std::set<uint32_t> lost;
        add_literal(kLostCyrillic, lost);
        for (uint32_t cp : lost) chars.erase(cp);
    }

    out.chars.assign(chars.begin(), chars.end());
    return true;
}

}  // namespace xrkit::engine::font
