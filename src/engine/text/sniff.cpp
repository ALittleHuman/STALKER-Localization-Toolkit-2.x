#include "xrkit/engine/text/sniff.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/text/encoding.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::engine::text {
namespace {

constexpr size_t kDeclScan = 500;  // 1.x 只看 raw[:500]

bool starts_with(const std::vector<uint8_t>& raw, const uint8_t* sig, size_t n) {
    return raw.size() >= n && std::memcmp(raw.data(), sig, n) == 0;
}

bool ascii_ws(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool iequal_at(const std::vector<uint8_t>& raw, size_t pos, const char* word) {
    const size_t n = std::strlen(word);
    if (pos + n > raw.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = static_cast<char>(raw[pos + i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return true;
}

// 1.x：`re.compile(rb'<\?xml[^>]*encoding\s*=\s*["\']([^"\']+)["\']', re.I|re.S)` 搜 raw[:500]
bool xml_declaration_encoding(const std::vector<uint8_t>& raw, std::string& out) {
    const size_t limit = raw.size() < kDeclScan ? raw.size() : kDeclScan;
    for (size_t i = 0; i + 5 <= limit; ++i) {
        if (!iequal_at(raw, i, "<?xml")) continue;
        // 从 <?xml 之后找第一个能解析出 encoding="..." 的位置（正则的回溯语义）
        for (size_t j = i + 5; j < limit; ++j) {
            if (raw[j] == '>') break;  // [^>]* 到此为止
            if (!iequal_at(raw, j, "encoding")) continue;
            size_t k = j + 8;
            while (k < limit && ascii_ws(raw[k])) ++k;
            if (k >= limit || raw[k] != '=') continue;
            ++k;
            while (k < limit && ascii_ws(raw[k])) ++k;
            if (k >= limit || (raw[k] != '"' && raw[k] != '\'')) continue;
            const uint8_t quote = raw[k];
            ++k;
            const size_t begin = k;
            while (k < limit && raw[k] != quote) ++k;
            if (k >= limit) break;
            out.assign(reinterpret_cast<const char*>(raw.data() + begin), k - begin);
            return true;
        }
    }
    return false;
}

// 编码名归一（1.x ENCODING_ALIASES）
std::string normalize_name(const std::string& in) {
    std::string s = in;
    // strip + lower（Python 的 .strip().lower()）
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) {
        --e;
    }
    s = s.substr(b, e - b);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s == "cp1251" || s == "cp-1251" || s == "windows1251") return kWindows1251;
    if (s == "cp1252" || s == "cp-1252" || s == "windows1252") return kWindows1252;
    if (s == "utf8" || s == "utf-8-sig") return kUtf8;
    return s;
}

// 1.x `is_multibyte_utf8`：能按 UTF-8 严格解码**且确实含多字节**（存在 ≥0x80）
bool is_multibyte_utf8(const std::vector<uint8_t>& raw) {
    if (raw.empty()) return false;
    bool has_high = false;
    for (uint8_t b : raw) {
        if (b >= 0x80) {
            has_high = true;
            break;
        }
    }
    if (!has_high) return false;
    std::string text;
    std::string err;
    return decode_strict(kUtf8, raw.data(), raw.size(), text, err);
}

// 1.x `bytes_fit_encoding`：严格解码通过才算"字节能证实这条声明"
bool bytes_fit_encoding(const std::vector<uint8_t>& raw, const std::string& enc) {
    if (enc.empty()) return false;
    std::string text;
    std::string err;
    return decode_strict(enc, raw.data(), raw.size(), text, err);
}

// ★ 统计步的**确定性**子集：复刻 chardet 在"纯 ASCII"上的判据。
// 实测（_2x_ref/sniff_probe3.py）边界：
//   "hello" / "a\tb" / "a\r\nb" → ascii(1.000) 被采信；
//   NUL / 0x01 / ESC / 0x0B / 0x0C / 0x7F / 空文件 → chardet 给 None 或低置信度的单字节猜测
//   ⇒ 第 5 步不采信、落第 6 步。
// 所以规则是：非空 + 全 <0x80 + 无 0x7F + 所有控制字节 ∈ {09,0A,0D}。
bool ascii_like(const std::vector<uint8_t>& raw) {
    if (raw.empty()) return false;
    for (uint8_t b : raw) {
        if (b >= 0x80) return false;
        if (b == 0x7F) return false;
        if (b < 0x20 && b != 0x09 && b != 0x0A && b != 0x0D) return false;
    }
    return true;
}

}  // namespace

std::string bom_encoding(const std::vector<uint8_t>& raw) {
    // 宽字符 BOM 必须**先长后短**（utf-32 的 BOM 以 utf-16 的 BOM 开头）
    static const uint8_t kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
    if (starts_with(raw, kUtf8Bom, 3)) return kUtf8;
    static const uint8_t kUtf32Le[4] = {0xFF, 0xFE, 0x00, 0x00};
    static const uint8_t kUtf32Be[4] = {0x00, 0x00, 0xFE, 0xFF};
    static const uint8_t kUtf16Le[2] = {0xFF, 0xFE};
    static const uint8_t kUtf16Be[2] = {0xFE, 0xFF};
    if (starts_with(raw, kUtf32Le, 4) || starts_with(raw, kUtf32Be, 4)) return kUtf32;
    if (starts_with(raw, kUtf16Le, 2) || starts_with(raw, kUtf16Be, 2)) return kUtf16;
    return {};
}

const char* sniff_step_label(SniffStep step) {
    switch (step) {
        case SniffStep::Bom:
            return "BOM";
        case SniffStep::MultibyteUtf8:
            return "多字节";
        case SniffStep::Override:
            return "指定";
        case SniffStep::XmlDeclaration:
            return "声明";
        case SniffStep::Statistic:
            return "统计";
        case SniffStep::Default:
            return "默认";
    }
    return "未知";
}

std::string sniff_result_label(const SniffResult& result) {
    const std::string label = sniff_step_label(result.step);
    if (result.encoding.empty()) return label;
    return label + "(" + result.encoding + ")";
}

SniffResult sniff_encoding(const std::vector<uint8_t>& raw, const std::string& override_enc) {    SniffResult res;

    // 1. BOM 是物理事实，优先于任何猜测
    const std::string bom = bom_encoding(raw);
    if (!bom.empty()) {
        res.step = SniffStep::Bom;
        res.encoding = bom;
        res.reason = "BOM";
        return res;
    }

    // 2. 多字节检查（最高优先的编码判定）
    if (is_multibyte_utf8(raw)) {
        res.step = SniffStep::MultibyteUtf8;
        res.encoding = kUtf8;
        res.reason = "严格 UTF-8 且含多字节序列";
        return res;
    }

    // 3. 手动指定（走到这里说明多字节证据不足，用户选择是最终裁决）
    const std::string manual = normalize_name(override_enc);
    if (!manual.empty() && manual != "auto") {
        res.step = SniffStep::Override;
        res.encoding = manual;
        res.reason = "手动指定";
        return res;
    }

    // 4. XML 声明 —— 但**必须能被字节证实**
    std::string decl;
    if (xml_declaration_encoding(raw, decl)) {
        const std::string cand = normalize_name(decl);
        if (!cand.empty() && bytes_fit_encoding(raw, cand)) {
            res.step = SniffStep::XmlDeclaration;
            res.encoding = cand;
            res.reason = "XML 声明（字节已证实）";
            return res;
        }
    }

    // 5. 统计探测（只实现已实测对齐的确定性子集 —— 见文件头）
    if (ascii_like(raw)) {
        res.step = SniffStep::Statistic;
        res.encoding = kAscii;
        res.reason = "纯 ASCII（统计探测的确定性分支）";
        return res;
    }

    // 6. 默认
    res.step = SniffStep::Default;
    res.encoding = kWindows1251;
    res.reason = "默认 windows-1251";
    return res;
}

DecodedBytes decode_bytes(const std::vector<uint8_t>& raw, const std::string& override_enc) {
    DecodedBytes out;
    const std::string manual_name = normalize_name(override_enc);
    const bool manual = !manual_name.empty() && manual_name != "auto";

    bool had_utf8_bom = false;
    std::vector<uint8_t> body = raw;

    if (!manual) {
        // 宽字符 BOM：**剥之前先认**，并把整段字节交给对应 codec（由它自己吃掉 BOM）
        const std::string bom = bom_encoding(raw);
        if (bom == kUtf16 || bom == kUtf32) {
            std::string text;
            std::string err;
            if (decode_strict(bom, raw.data(), raw.size(), text, err)) {
                out.ok = true;
                out.text = std::move(text);
                out.used_encoding = bom;
                out.chars = utf8_codepoint_count(out.text);
                return out;
            }
        }
    }

    {
        static const uint8_t kUtf8Bom[3] = {0xEF, 0xBB, 0xBF};
        if (starts_with(body, kUtf8Bom, 3)) {
            had_utf8_bom = true;
            body.erase(body.begin(), body.begin() + 3);
        }
    }

    const std::string first = had_utf8_bom ? std::string(kUtf8) : sniff_encoding(body, override_enc).encoding;
    // 1.x BYTE_DECODE_CANDIDATES = [windows-1251, windows-1252, gbk]，末位 latin-1 保底
    const char* kCandidates[] = {kWindows1251, kWindows1252, kGbk};
    std::vector<std::string> tried;
    tried.push_back(first);
    for (const char* c : kCandidates) {
        if (std::string(c) != first) tried.emplace_back(c);
    }
    for (const std::string& enc : tried) {
        std::string text;
        std::string err;
        if (decode_strict(enc, body.data(), body.size(), text, err)) {
            out.ok = true;
            out.text = std::move(text);
            out.used_encoding = enc;
            out.chars = utf8_codepoint_count(out.text);
            return out;
        }
    }
    // 保底：latin-1（永不失败）—— 1.x 用 errors=errors（默认 strict），这里同样严格，
    // 只是 latin-1 本来就不会失败
    std::string text;
    std::string err;
    decode_strict(kLatin1, body.data(), body.size(), text, err);
    out.ok = true;
    out.text = std::move(text);
    out.used_encoding = kLatin1;
    out.forced = true;
    out.chars = utf8_codepoint_count(out.text);
    return out;
}

DecodedBytes read_text_bytes(const std::string& path, const std::string& override_enc) {
    std::ifstream in(core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        DecodedBytes out;
        out.error = "打不开文件：" + path;
        return out;
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    return decode_bytes(raw, override_enc);
}

}  // namespace xrkit::engine::text
