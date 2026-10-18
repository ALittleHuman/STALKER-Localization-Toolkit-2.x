#include "xrkit/engine/text/sniff.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/text/encoding.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::engine::text {
namespace {

// 单字节家族统计剖面（由 tools/gen_sniff_profiles.py 从**已发表字频**生成）。
// kSniffByteWeight[b] = log10( Q_ru(b) / Q_we(b) )，仅 b >= 0x80 非零。
#include "sniff_profiles.inc"

constexpr size_t kDeclScan = 500;  // 1.x 只看 raw[:500]

// ── "鲁棒"步（第 5 步）的两个门槛 ───────────────────────────────────────
// 实测标定（真语料：西语 109 个 cp1252 + 俄语 262 个 cp1251，见
// docs\2.0.0-行为口径对照表.md ⑨9.9 与门禁 [16]）：
//   · 证据量：至少 2 个高位字节（单字节的"证据"不值得推翻一条声明）；
//   · 余量：|每个高位字节的平均对数似然比| >= 0.15
//     （实测决定性区间：俄语 [+0.15, +0.91]，西语 [-1.15, -0.21] —— 两段**不重叠**，
//      安全窗口是 (0.111, 0.154]：0.111 是西语侧唯一的正分样本，0.154 是真俄语侧的下界）。
// ★ 0.15 **同时**就是"声明"的权重：统计只有在余量**超过**声明权重时才是否决声明。
//   这就是用户口径④「声明的优先级，仍然是比鲁棒稍微高一点的」的落地形态 ——
//   没有独立的一套阈值，"声明略高"＝"门槛就是声明权重"。
constexpr size_t kStatMinEvidence = 2;
constexpr double kStatMargin = 0.15;
// 置信度打分里"统计强度"饱和点：|得分| 到 0.90 即认为证据满格（实测俄语上界 0.91）。
constexpr double kStatFull = 0.90;
// 只有声明时的置信度（用户口径②：「大多数声明是正确的」—— 给一半，不敢满）。
constexpr double kDeclOnlyConfidence = 0.5;

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

// ── 第 5 步"鲁棒"：单字节家族统计 ───────────────────────────────────────
//
// 1.x 这一步是 chardet；实测它对用户那份西语**没有分辨力**
// （`windows-1252 conf=0.054`，低于 1.x 自己的 0.30 门槛 ⇒ 1.x 也落 windows-1251）。
// 2.0 按用户口径②④补一条**可解释**的统计：两条单字节假设的**字节级对数似然比** ——
//   H_ru = 俄语文本 / windows-1251，H_we = 西欧文本 / windows-1252
//   score = ( Σ_{b >= 0x80} log10(Q_ru(b)/Q_we(b)) ) / 高位字节数
// Q 表由**已发表字频**经编码表映射而成（`tools/gen_sniff_profiles.py`），
// 与被测语料独立；正 ⇒ 像俄语，负 ⇒ 像西欧。
//
// ★ 为什么不是"高位字节占比"那种更省事的规则：实测俄语 .xml/.ltx 有大量 ASCII 标记，
//   高位占比中位才 5.7%（最低 0.06%，`dialogs_jupiter.xml`），与西语的 0.8% 严重重叠
//   ⇒ 占比**不可用**；能分开的是"这些高位字节解出来像哪种语言"（见规格文档 §5）。
struct SingleByteStat {
    bool decisive = false;
    std::string encoding;    // 决定性时的落点
    double score = 0.0;      // 每个高位字节的平均对数似然比
    uint64_t evidence = 0;   // 高位字节数
};

SingleByteStat single_byte_stat(const std::vector<uint8_t>& raw) {
    SingleByteStat st;
    double acc = 0.0;
    for (uint8_t b : raw) {
        if (b < 0x80) continue;
        ++st.evidence;
        acc += static_cast<double>(kSniffByteWeight[b]);
    }
    if (st.evidence == 0) return st;
    st.score = acc / static_cast<double>(st.evidence);
    if (st.evidence < kStatMinEvidence) return st;
    if (std::fabs(st.score) < kStatMargin) return st;
    if (st.score > 0.0) {
        // 俄语假设还必须**字节上成立**：cp1251 的 0x98 是未定义字节，真俄语文件不会有它。
        if (!bytes_fit_encoding(raw, kWindows1251)) return st;
        st.encoding = kWindows1251;
    } else {
        // 西欧：优先 windows-1252；含 cp1252 未定义字节（0x81/0x8D/0x8F/0x90/0x9D）时
        // 退 latin-1 —— 两者对 0xA0..0xFF 是**同一张表**，是同一个家族的宽松写法。
        st.encoding = bytes_fit_encoding(raw, kWindows1252) ? kWindows1252 : kLatin1;
    }
    st.decisive = true;
    return st;
}

// 1.x `bytes_fit_encoding` 之外，纯 ASCII 的确定性分支（1.x 的 chardet 给 `ascii(1.000)`）。
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

// 统计步能裁决的**家族**：它只建模"俄语/cp1251"对"西欧/cp1252"这一对单字节假设。
// ★ 因此它**无权**推翻别的家族的声明 —— 实测踩到过：`encoding="gb2312"` 且字节真是
//   gb2312 的样本（1.x 与 2.x 都走第 4 步采信 gb2312、正文正确），被统计推成了 cp1252。
//   声明写多字节家族（gbk/big5/shift_jis/utf-16…）时，统计必须**让路**。
bool in_single_byte_family(const std::string& enc) {
    return enc == kWindows1251 || enc == kWindows1252 || enc == kLatin1;
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

std::string sniff_flags_label(const SniffResult& result) {
    std::string out;
    const auto add = [&out](const char* text) {
        if (!out.empty()) out += "、";
        out += text;
    };
    if (result.low_confidence) add("低置信");
    if (result.override_decl) add("否决声明");
    return out;
}

// ★★ P17-87（待办总表 §四·9）：**问句原文**（唯一一份；GUI 弹窗与 CLI 报出都用它）。
//   口径（用户 2026-10-10 拍板）：每一次编码判定都要由用户**显式确认**；
//   用户要看得到"在问哪一份、判定是什么、依据是什么、有多确定" —— 四件缺一不可。
//   ★ 文案只此一份：`sniff.cpp` 之外（页面 / CLI / 插件）**不许自己拼**（⑨9.7 同一条纪律）。
std::string sniff_confirm_question(const SniffResult& result, const std::string& path) {
    std::string out = "按判定结果转换这一份吗？";
    if (!path.empty()) out += "\n文件：" + path;
    out += "\n判定：" + sniff_result_label(result);
    if (!result.reason.empty()) out += "\n理由：" + result.reason;
    const std::string flags = sniff_flags_label(result);
    out += "\n标记：" + (flags.empty() ? std::string("（无）") : flags);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", result.confidence);
    out += "\n置信度：" + std::string(buf);
    return out;
}

// 置信度打分（0..1）—— 只由"不确定的那两项"决定（用户口径③）。
//   · 统计决定性 ⇒ 0.6 + 0.4 × min(1, |得分|/0.90)（实测决定性区间 |得分| ∈ [0.21, 0.91]）
//   · 只有声明   ⇒ 0.5（"大多数声明是正确的"，但没证据就不敢满）
//   · 纯 ASCII   ⇒ 1.0（字节规则，确定）
//   · 第 6 步默认 ⇒ 0.0（**纯猜**，同时置 low_confidence）
double stat_confidence(const SingleByteStat& st) {
    const double strength = std::fmin(1.0, std::fabs(st.score) / kStatFull);
    return 0.6 + 0.4 * strength;
}

// 可读的得分（±0.001，日志/理由里用；不用 std::to_string 那 6 位小数）
std::string fmt_score(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.3f", v);
    return buf;
}

SniffResult sniff_encoding(const std::vector<uint8_t>& raw, const std::string& override_enc) {
    SniffResult res;

    // 1. BOM 是物理事实，优先于任何猜测（**锁**）
    const std::string bom = bom_encoding(raw);
    if (!bom.empty()) {
        res.step = SniffStep::Bom;
        res.encoding = bom;
        res.reason = "BOM";
        return res;
    }

    // 2. 多字节检查（**锁**：能按 UTF-8 严格解且含 ≥0x80 ⇒ 直接按 utf-8 读）
    if (is_multibyte_utf8(raw)) {
        res.step = SniffStep::MultibyteUtf8;
        res.encoding = kUtf8;
        res.reason = "严格 UTF-8 且含多字节序列";
        return res;
    }

    // 3. 手动指定（**锁**：走到这里说明多字节证据不足，用户选择是最终裁决）
    const std::string manual = normalize_name(override_enc);
    if (!manual.empty() && manual != "auto") {
        res.step = SniffStep::Override;
        res.encoding = manual;
        res.reason = "手动指定";
        return res;
    }

    // ── 4/5. 声明与"鲁棒"**一起**进置信度系统（用户口径①②④）───────────────
    // 声明的候选：XML 声明 + **必须能被字节证实**（1.x 的既有判据，不动）。
    std::string decl;
    const bool has_decl =
        xml_declaration_encoding(raw, decl) && !normalize_name(decl).empty() &&
        bytes_fit_encoding(raw, normalize_name(decl));
    const std::string decl_enc = has_decl ? normalize_name(decl) : std::string();

    const SingleByteStat st = single_byte_stat(raw);
    res.stat_evidence = st.evidence;
    res.stat_score = st.score;

    if (st.decisive) {
        res.confidence = stat_confidence(st);
        // 统计**只在单字节家族里有裁决权**：声明写 gb2312/gbk/big5/… 时让路（见上）。
        const bool may_judge_decl = has_decl && in_single_byte_family(decl_enc);
        if (may_judge_decl && decl_enc == st.encoding) {
            // 声明与统计**互相印证** ⇒ 仍走第 4 步（诚实声明照旧直接采用，不是抽奖）
            res.step = SniffStep::XmlDeclaration;
            res.encoding = decl_enc;
            res.reason = "XML 声明（字节已证实；统计一致 " + fmt_score(st.score) + "）";
            return res;
        }
        if (may_judge_decl) {
            // ★ 统计**否决声明** —— 有意与 1.x 不同；必须可见（override_decl）
            res.step = SniffStep::Statistic;
            res.encoding = st.encoding;
            res.override_decl = true;
            res.reason = "统计否决声明 " + decl_enc + " → " + st.encoding + "（逐高位字节似然比 " +
                         fmt_score(st.score) + "，证据 " + std::to_string(st.evidence) + " 字节）";
            return res;
        }
        if (has_decl) {
            // 声明属于别的家族 ⇒ 统计让路，声明照旧生效（第 4 步）
            res.step = SniffStep::XmlDeclaration;
            res.encoding = decl_enc;
            res.confidence = kDeclOnlyConfidence;
            res.reason = "XML 声明（字节已证实；声明属别的家族，统计无权否决）";
            return res;
        }
        res.step = SniffStep::Statistic;
        res.encoding = st.encoding;
        res.reason = "单字节家族统计 " + st.encoding + "（逐高位字节似然比 " +
                     fmt_score(st.score) + "，证据 " + std::to_string(st.evidence) + " 字节）";
        return res;
    }

    // 统计**无决定性** ⇒ 声明照旧生效（"大多数声明是正确的"）
    if (has_decl) {
        res.step = SniffStep::XmlDeclaration;
        res.encoding = decl_enc;
        res.confidence = kDeclOnlyConfidence;
        res.reason = st.evidence == 0
                         ? "XML 声明（字节已证实；无高位字节可供统计）"
                         : "XML 声明（字节已证实；统计无决定性 " + fmt_score(st.score) +
                               "，证据 " + std::to_string(st.evidence) + " 字节）";
        return res;
    }

    // 5b. 纯 ASCII（1.x 的 chardet 在这条上给 ascii(1.000)，用字节规则复刻）
    if (ascii_like(raw)) {
        res.step = SniffStep::Statistic;
        res.encoding = kAscii;
        res.reason = "纯 ASCII（统计探测的确定性分支）";
        return res;
    }

    // 6. 默认 —— ★ 这是**纯猜**：必须可见（零静默红线）
    res.step = SniffStep::Default;
    res.encoding = kWindows1251;
    res.confidence = 0.0;
    res.low_confidence = true;
    res.reason = "无声明、统计无决定性 ⇒ 默认 windows-1251（低置信：" +
                 fmt_score(st.score) + "，证据 " + std::to_string(st.evidence) + " 字节）";
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

    // ★ 判定只算一次：`first` 与 `used_mismatch` 必须来自**同一次**判定，
    //   否则"判定结论 ≠ 实际使用编码"这件事又会被复算掩盖掉。
    const SniffResult decided = had_utf8_bom ? SniffResult{} : sniff_encoding(body, override_enc);
    // ★★ C-10（第三方只读审计 2026-10-18）：**同一个表达式只许算一次**。
    //   修前 `first` 与 `declared` 是**两份拷贝**（字面相同的表达式写了两遍）⇒ 改一处漏一处，
    //   "判定结论 ≠ 实际使用的编码"这件事又会被复算掩盖掉（这条正是那段注释要防的）。
    const std::string declared = had_utf8_bom ? std::string(kUtf8) : decided.encoding;
    const std::string& first = declared;   // 别名，不是第二份
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
            out.used_mismatch = (enc != declared);
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
    out.used_mismatch = (declared != kLatin1);
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
