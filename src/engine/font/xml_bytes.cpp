#include "xrkit/engine/font/xml_bytes.h"

#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/sniff.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace xrkit::engine::font {
namespace {

bool is_bom_utf8(const std::vector<uint8_t>& raw) {
    return raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF;
}

namespace {

// 2026-09-27（Hana 第二次审计 §四-2）：1.x 用的是 `re.I`（**大小写不敏感**），我们原先用
// 大小写敏感的 `find` ⇒ "头文件注释说不敏感 / 实现敏感 / 与 1.x 不一致"三处不符。
// 合法 XML 恒小写，触发面窄，但既然写着"不敏感"就必须真的不敏感 —— 统一走这个匹配。
size_t find_ci(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() &&
               std::tolower(static_cast<unsigned char>(hay[i + k])) ==
                   std::tolower(static_cast<unsigned char>(needle[k]))) {
            ++k;
        }
        if (k == needle.size()) return i;
    }
    return std::string::npos;
}

}  // namespace

// 1.x `_declared_encoding`：XML 声明里的 encoding（小写）；只看前 500 字节。
std::string declared_encoding(const std::vector<uint8_t>& raw) {
    const size_t n = std::min<size_t>(raw.size(), 500);
    const std::string head(reinterpret_cast<const char*>(raw.data()), n);
    size_t pos = 0;
    while (true) {
        const size_t x = find_ci(head, "<?xml", pos);
        if (x == std::string::npos) return "";
        const size_t gt = head.find('>', x);
        const size_t end = (gt == std::string::npos) ? head.size() : gt;
        const std::string tag = head.substr(x, end - x);
        size_t e = 0;
        while ((e = find_ci(tag, "encoding", e)) != std::string::npos) {
            size_t p = e + 8;
            while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\r' ||
                                      tag[p] == '\n')) {
                ++p;
            }
            if (p >= tag.size() || tag[p] != '=') {
                e += 8;
                continue;
            }
            ++p;
            while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t')) ++p;
            if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) return "";
            const char quote = tag[p++];
            const size_t close = tag.find(quote, p);
            if (close == std::string::npos) return "";
            std::string value = tag.substr(p, close - p);
            for (char& c : value) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            return value;
        }
        if (gt == std::string::npos) return "";
        pos = gt + 1;
    }
}

// 1.x `_encoding_trusted`：windows-1251 只有 XML 声明**明确写了**才算可信。
bool encoding_trusted(const std::string& enc, const std::string& declared) {
    if (enc == "windows-1251" || enc == "cp1251") {
        return declared == "windows-1251" || declared == "cp1251";
    }
    return true;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

bool decode_xml_bytes(const std::vector<uint8_t>& raw, std::string& text, std::string& enc,
                      bool& trusted) {
    const std::string declared = declared_encoding(raw);
    enc = text::sniff_encoding(raw).encoding;
    const size_t skip = is_bom_utf8(raw) ? 3 : 0;
    std::string out;
    std::string err;
    if (text::decode_strict(enc, raw.data() + skip, raw.size() - skip, out, err)) {
        text = out;
        trusted = encoding_trusted(enc, declared);
        return true;
    }
    const text::DecodedBytes fallback = text::decode_bytes(raw);
    if (!fallback.ok) return false;
    text = fallback.text;
    enc = fallback.used_encoding;
    trusted = !fallback.forced && encoding_trusted(enc, declared);
    return true;
}

std::string rewrite_xml_declaration_encoding(const std::string& text) {
    // 1.x：`re.sub(r'<\?xml[^>]*\?>', lambda m: re.sub(r'(encoding\s*=\s*["\'])[^"\']*(["\'])',
    //        r'\1utf-8\2', m.group(0), flags=re.I), text, count=1, flags=re.I)`
    const size_t start = find_ci(text, "<?xml", 0);
    if (start == std::string::npos) return text;
    const size_t gt = text.find('>', start);
    if (gt == std::string::npos) return text;
    std::string tag = text.substr(start, gt - start + 1);
    // 只替换第一条声明里的第一个 encoding
    size_t e = 0;
    while ((e = find_ci(tag, "encoding", e)) != std::string::npos) {
        size_t p = e + 8;
        while (p < tag.size() && is_space(tag[p])) ++p;
        if (p >= tag.size() || tag[p] != '=') {
            e += 8;
            continue;
        }
        ++p;
        while (p < tag.size() && is_space(tag[p])) ++p;
        if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) break;
        const char quote = tag[p];
        const size_t value_start = p + 1;
        const size_t close = tag.find(quote, value_start);
        if (close == std::string::npos) break;
        tag = tag.substr(0, value_start) + "utf-8" + tag.substr(close);
        break;
    }
    return text.substr(0, start) + tag + text.substr(gt + 1);
}

}  // namespace xrkit::engine::font
