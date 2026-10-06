#include "xrkit/engine/text/encoding.h"

#include "xrkit/core/path.h"

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace xrkit::engine::text {
namespace {

// 生成物（tools/gen_encoding_tables.py 从 Python 的同名 codec 导出，可 --check 校验）
#include "cp1251_tables.inc"   // NOLINT
#include "cp1252_tables.inc"   // NOLINT
#include "gb18030_tables.inc"  // NOLINT
#include "gb2312_tables.inc"  // NOLINT
#include "gbk_tables.inc"      // NOLINT
// D3 批补的三张东亚表（同一姿势：生成器从 Python 同名 codec 导出，--check 锁住）
#include "big5_tables.inc"       // NOLINT
#include "euc_kr_tables.inc"     // NOLINT
#include "shift_jis_tables.inc"  // NOLINT

constexpr uint16_t kUndefined = 0xFFFF;
constexpr size_t kGb18030FourByteCount = sizeof(kGb18030FourByte) / sizeof(kGb18030FourByte[0]);

std::string hex_byte(uint8_t b) {
    static const char* kHex = "0123456789ABCDEF";
    std::string s = "0x";
    s.push_back(kHex[(b >> 4) & 0xF]);
    s.push_back(kHex[b & 0xF]);
    return s;
}

// ── UTF-8（严格，与 Python 的 utf-8 解码器同判据）─────────────────────
void append_utf8(std::string& out, uint32_t cp) {    if (cp <= 0x7F) {
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

// 解码一个码位；拒绝过长编码 / 代理区 / > U+10FFFF（Python 同样拒绝）
bool utf8_decode_one(const uint8_t* p, size_t n, size_t& i, uint32_t& cp) {
    if (i >= n) return false;
    const uint8_t b0 = p[i];
    if (b0 < 0x80) {
        cp = b0;
        ++i;
        return true;
    }
    size_t need = 0;
    uint32_t value = 0;
    uint32_t min_value = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        value = b0 & 0x1Fu;
        min_value = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        value = b0 & 0x0Fu;
        min_value = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        value = b0 & 0x07u;
        min_value = 0x10000;
    } else {
        return false;
    }
    if (i + need >= n) return false;
    for (size_t k = 1; k <= need; ++k) {
        const uint8_t bk = p[i + k];
        if ((bk & 0xC0) != 0x80) return false;
        value = (value << 6) | (bk & 0x3Fu);
    }
    if (value < min_value || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
        return false;
    }
    cp = value;
    i += need + 1;
    return true;
}

bool utf8_validate(const uint8_t* data, size_t size, size_t& bad_offset) {
    size_t i = 0;
    while (i < size) {
        uint32_t cp = 0;
        if (!utf8_decode_one(data, size, i, cp)) {
            bad_offset = i;
            return false;
        }
    }
    return true;
}

// ── 单字节表 ─────────────────────────────────────────────────────────
bool decode_single_byte(const uint16_t* table, const uint8_t* data, size_t size,
                        std::string& text, std::string& err, const char* name) {
    text.clear();
    text.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            text.push_back(static_cast<char>(b));
            continue;
        }
        const uint16_t cp = table[b - 0x80];
        if (cp == kUndefined) {
            err = std::string(name) + " 里未定义的字节 " + hex_byte(b) + "（严格解码必须失败）";
            return false;
        }
        append_utf8(text, cp);
    }
    return true;
}

bool encode_single_byte(const uint16_t* table, const std::string& text, std::vector<uint8_t>& out,
                        std::string& err, const char* name) {
    out.clear();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            err = std::string("正文不是合法 UTF-8（") + name + " 编码前）";
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<uint8_t>(cp));
            continue;
        }
        bool found = false;
        for (size_t k = 0; k < 128; ++k) {
            if (table[k] == cp) {
                out.push_back(static_cast<uint8_t>(0x80 + k));
                found = true;
                break;
            }
        }
        if (!found) {
            err = "码位 U+" + std::to_string(cp) + " 无法用 " + name + " 表示";
            return false;
        }
    }
    return true;
}

// ── gb18030 ──────────────────────────────────────────────────────────
bool gb18030_decode(const uint8_t* data, size_t size, std::string& text, std::string& err) {
    text.clear();
    text.reserve(size * 2);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        if (b1 < 0x81 || b1 > 0xFE) {
            // ★★ 2026-10-06（审计处理批·2-1 / H1-3 / A1-1）：**严格**路径修前只挡了 `0x80`。
            //   表 `kGb18030TwoByte[24066]` 的合法首字节域是 `0x81..0xFE`（生成器
            //   `tools/gen_encoding_tables.py:68 GB_TWO_W = 126 * 191` 的同一条算式），
            //   而 `0xFF` 落下来会算出下标 `(0xFF-0x81)*191 + (b2-0x40)` = 24066..24256
            //   ⇒ **越界读最多 191 项**；读到 ≠ kUndefined 时还把**非法序列当合法解出**
            //   （实测修前：`FF 40` → `ef a6 a1`、`FF FE` → `c2 a6`，都不是合法解码结果）。
            //   有损路径早就挡住了 0xFF（本文件 `:256`、`:650`）⇒ 这是漏写、不是有意。
            //   gbk/gb2312 走下面同一个 `gb_two_byte_decode`，同一处一并收口。
            err = "gb18030：非法首字节 " + hex_byte(b1);
            return false;
        }
        if (i + 1 >= size) {
            err = "gb18030：序列被截断";
            return false;
        }
        const uint8_t b2 = data[i + 1];
        if (b2 >= 0x40 && b2 <= 0xFE) {
            const uint16_t cp = kGb18030TwoByte[(b1 - 0x81) * 191 + (b2 - 0x40)];
            if (cp != kUndefined) {
                append_utf8(text, cp);
                i += 2;
                continue;
            }
        }
        if (i + 3 >= size) {
            err = "gb18030：四字节序列被截断";
            return false;
        }
        const uint8_t b3 = data[i + 2];
        const uint8_t b4 = data[i + 3];
        if (b2 < 0x30 || b2 > 0x39 || b3 < 0x81 || b3 > 0xFE || b4 < 0x30 || b4 > 0x39) {
            err = "gb18030：非法字节序列";
            return false;
        }
        const uint32_t linear = ((static_cast<uint32_t>(b1) - 0x81) * 10 + (b2 - 0x30)) * 1260 +
                                (static_cast<uint32_t>(b3) - 0x81) * 10 + (b4 - 0x30);
        // 段表二分：段**只覆盖合法序列**，查不到即非法空洞
        size_t lo = 0;
        size_t hi = kGb18030FourByteCount;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (kGb18030FourByte[mid].to < linear) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo >= kGb18030FourByteCount || linear < kGb18030FourByte[lo].from) {
            err = "gb18030：四字节序列落在非法空洞里";
            return false;
        }
        const uint32_t cp = static_cast<uint32_t>(
            static_cast<int64_t>(linear) + kGb18030FourByte[lo].delta);
        append_utf8(text, cp);
        i += 4;
    }
    return true;
}

// ── 有损解码/编码（Python 的 errors="replace"）───────────────────────
//
// 转换页的两条写出路径（转换分支与"已是目标编码"分支）都用 `errors="replace"`，
// 而 replace 的输出**直接进正文**、最终进产物 —— 所以它必须逐字节对齐 Python，
// 不能"看着差不多"。下面的规则是**实测**出来的（`tests/golden_lossy_codec.py` 会持续对拍）：
//
//   UTF-8（CPython 的 replace）：
//     · 非法前导（<C2 或 >F4）⇒ 一个 U+FFFD，吃掉 1 字节；
//     · 合法前导但后续字节越界 ⇒ 一个 U+FFFD，吃掉**到越界字节之前**为止；
//     · 合法前导但在**输入末尾被截断** ⇒ 一个 U+FFFD，吃掉**已经吃掉的**那些字节。
//   实测样本：e4b8→1 个；e4b8e4→2 个；e441→1 个 + 'A'；e08080→3 个（E0 的次字节必须
//   A0..BF，80 越界 ⇒ 第一个 FFFD 只吃 E0）；eda080→3 个；f4908080→4 个；c080→2 个。
//   gb18030：非法/截断各自一个 U+FFFD，规则同上（截断时吃到末尾）。
//   单字节表：未定义字节（表里 0xFFFF）⇒ 一个 U+FFFD。
//   编码方向：目标编码表示不了的码位 ⇒ `?`（0x3F）。
void append_replacement(std::string& text) { text += "\xEF\xBF\xBD"; }  // U+FFFD

// gbk / gb2312 的有损解码：**只有两字节路径**（gb18030 才认四字节）。
// 实测（Python 同名 codec，errors="replace"）：结构合法但表里没定义的序列
// ⇒ 一个 U+FFFD 且**只吃前导字节**（`fe fe` → 两个替换；`81 30` → 替换 + '0'；
// gb2312 里的 `81 40` 也如此，因为那张表比 gbk 小）。
bool gb_two_byte_decode_lossy(const uint16_t* table, const uint8_t* data, size_t size,
                              std::string& text) {
    text.clear();
    text.reserve(size * 3);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        if (b1 == 0x80 || b1 == 0xFF) {
            append_replacement(text);
            ++i;
            continue;
        }
        if (i + 1 < size) {
            const uint8_t b2 = data[i + 1];
            if (b2 >= 0x40 && b2 <= 0xFE) {
                const uint16_t cp = table[(b1 - 0x81) * 191 + (b2 - 0x40)];
                if (cp != kUndefined) {
                    append_utf8(text, cp);
                    i += 2;
                    continue;
                }
            }
        }
        append_replacement(text);
        ++i;  // 只吃前导：后面的字节按自己的身份继续处理
    }
    return true;
}

// ── big5 / shift_jis / euc-kr（D3 批，2026-10-05）────────────────────────
//
// 这三个编码**形状各不相同**，照 gb 那套 `(b1-0x81)*191+(b2-0x40)` 硬套一定错：
//   · trail 是**离散集合**，不是区间。big5 的 trail ∈ [0x40..0x7E] ∪ [0xA1..0xFE]
//     —— 中间的 [0x7F..0xA0] 与 0xFF **不是 trail**。实测区别（Python 同名 codec）：
//     `A1 7F` → 一个 U+FFFD + 0x7F（0x7F 不是 trail ⇒ 只吃 lead）；而
//     `A1 80` → **两个** U+FFFD。所以判据必须是"是不是 trail"的布尔表，不能写成
//     `tmin <= b2 && b2 <= tmax`。
//   · shift_jis 另有**单字节半角片假名** 0xA1..0xDF → U+FF61..U+FF9F，而且这些字节
//     同时又是双字节形态的 lead ⇒ 必须"**先试双字节、失败再落单字节**"。
//     实测：`A1 A1` → `'｡｡'`（双字节表里 `A1 A1` 有定义），`A1 7F` → `'｡\x7f'`
//     （双字节没定义、但有单字节形态 ⇒ 吃 1 字节出 '｡'），
//     `A0 20` → `'� '`（0xA0 既不是 lead 也没单字节形态 ⇒ 替换），
//     `81 20` → `'� '`（lead 但 trail 非法，且 0x81 无单字节形态 ⇒ 替换）。
//     这三条穷举验证过（`tests/golden_lossy_codec.py` + 生成器同源）。
// 字节区间的显式清单（**不能写成 `lo <= b && b <= hi` 一条区间**：三个编码的 lead/trail
// 集合都有空洞与不连续段，实测 `A1 7F`（big5）与 `A1 80` 的结果完全不同就是这条）
struct ByteRange {
    uint8_t lo;
    uint8_t hi;
};

bool in_ranges(const ByteRange* ranges, size_t n, uint8_t b) {
    for (size_t i = 0; i < n; ++i) {
        if (b >= ranges[i].lo && b <= ranges[i].hi) return true;
    }
    return false;
}

struct MultibyteSpec {
    const uint16_t* table;
    uint8_t lead_first;   // 表的第一行 lead
    uint16_t cols;        // 表覆盖的 trail 列数（含非 trail 的空档列）
    uint8_t trail_first;  // 表的第一列 trail
    const ByteRange* lead_ranges;
    size_t lead_range_count;
    const ByteRange* trail_ranges;
    size_t trail_range_count;
    const uint16_t* single_hi;  // 单字节表（shift_jis 的半角片假名；其余为 nullptr）
    uint8_t single_first;
    uint8_t single_last;
    bool is_euc_kr;  // euc-kr 专有的那条"A4 D4 丢弃剩余缓冲"实测语义（见 mw_decode_lossy）
    uint16_t rows;   // 表的真实行数（建反向表的循环上界；写错会读出表尾之外）
};

bool is_lead_byte(const MultibyteSpec& s, uint8_t b) {
    return b >= 0x80 && in_ranges(s.lead_ranges, s.lead_range_count, b);
}

bool is_trail_byte(const MultibyteSpec& s, uint8_t b) {
    return in_ranges(s.trail_ranges, s.trail_range_count, b);
}

const ByteRange kBig5Lead[] = {{0xA1, 0xF9}};
const ByteRange kBig5Trail[] = {{0x40, 0x7E}, {0xA1, 0xFE}};
const ByteRange kShiftJisLead[] = {{0x81, 0x9F}, {0xE0, 0xFC}};
const ByteRange kShiftJisTrail[] = {{0x40, 0x7E}, {0x80, 0xFC}};
const ByteRange kEucKrLead[] = {{0xA1, 0xFD}};
const ByteRange kEucKrTrail[] = {{0xA1, 0xFE}};

// 表尺寸（与生成器 `emit_mw` 写出来的下标算法一一对应，**改了表就必须同步改这里**）：
//   big5      89 行 × 190 列（trail 0x40..0xFE，含 0x7F..0xA0 的空档列）
//   shift_jis 124 行 × 189 列（lead 0x81..0xFC；其中 0xA0..0xDF 整段是空档行）
//   euc-kr    93 行 × 94 列（lead 0xA1..0xFD；trail 0xA1..0xFE）
// ★ `rows` 只用来给"建反向表"的循环定上界，必须是**表的真实行数**：写成 188
//   （= 0xFC-0x40）会让循环读出表尾之后的内存，凭空造出 `FD44 → U+FF61` 这种
//   假映射（实测踩到：`shift_jis` 编码侧把 `｡` 写成 `fd44` 而不是 `a1`）。
constexpr uint16_t kBig5Cols = 0xFE - 0x40 + 1;      // 190
constexpr uint16_t kShiftJisCols = 0xFC - 0x40 + 1;  // 189
constexpr uint16_t kEucKrCols = 0xFE - 0xA1 + 1;     // 94
constexpr uint16_t kBig5Rows = 0xF9 - 0xA1 + 1;      // 89
constexpr uint16_t kShiftJisRows = 0xFC - 0x81 + 1;  // 124
constexpr uint16_t kEucKrRows = 0xFD - 0xA1 + 1;     // 93

const MultibyteSpec kBig5Spec = {kBig5TwoByte,         0xA1, kBig5Cols,     0x40,
                                 kBig5Lead,            1,    kBig5Trail,     2,
                                 nullptr,              0,    0,              false,
                                 kBig5Rows};
const MultibyteSpec kShiftJisSpec = {kShiftJisTwoByte,    0x81, kShiftJisCols,    0x40,
                                     kShiftJisLead,       2,    kShiftJisTrail,    2,
                                     kShiftJisSingle,     0xA1, 0xDF,              false,
                                     kShiftJisRows};
const MultibyteSpec kEucKrSpec = {kEucKrTwoByte,      0xA1, kEucKrCols,    0xA1,
                                  kEucKrLead,         1,    kEucKrTrail,    1,
                                  nullptr,            0,    0,              true,
                                  kEucKrRows};

// 双字节表查表（调用方必须已确认 lead/trail 合法）
uint16_t mw_lookup(const MultibyteSpec& s, uint8_t b1, uint8_t b2) {
    return s.table[static_cast<size_t>(b1 - s.lead_first) * s.cols + (b2 - s.trail_first)];
}

// (b1,b2) → 表下标（编码方向的 4 处 big5 覆盖项要用）
uint32_t mw_slot(const MultibyteSpec& s, uint8_t b1, uint8_t b2) {
    return static_cast<uint32_t>(b1 - s.lead_first) * s.cols + (b2 - s.trail_first);
}

// ── 严格解码 ─────────────────────────────────────────────────────────
bool mw_decode_strict(const MultibyteSpec& s, const char* name, const uint8_t* data, size_t size,
                      std::string& text, std::string& err) {
    text.clear();
    text.reserve(size * 2);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        // shift_jis：单字节半角片假名与其他形态**共用同一段字节区间** ⇒ 先试双字节
        const bool single_ok =
            s.single_hi != nullptr && b1 >= s.single_first && b1 <= s.single_last;
        if (is_lead_byte(s, b1) && i + 1 < size && is_trail_byte(s, data[i + 1])) {
            const uint16_t cp = mw_lookup(s, b1, data[i + 1]);
            if (cp != kUndefined) {
                append_utf8(text, cp);
                i += 2;
                continue;
            }
        }
        if (single_ok) {
            const uint16_t cp = s.single_hi[b1 - s.single_first];
            if (cp != kUndefined) {
                append_utf8(text, cp);
                ++i;
                continue;
            }
        }
        err = std::string(name) + "：非法字节序列（首字节 " + hex_byte(b1) + "）";
        return false;
    }
    return true;
}

// ── 有损解码（Python `errors="replace"`；逐字节对齐，见下面的实测规则）────
//   · 合法双字节 → 码位；
//   · "首字节是合法 lead、次字节是合法 trail、但表里没定义" ⇒ **只吃首字节**、写一个
//     U+FFFD（实测 big5 `A3C0` → 两个替换、shift_jis `81E9` → 两个替换）；
//   · 其余（非法首字节 / 截断 / trail 非法）也各自只吃 1 字节、写一个 U+FFFD。
//   ★ **唯一的例外**是 euc-kr 的 `A4 D4`：它是 KS X 1001:1998 的"字母拼合（make-up）
//     序列"领头（`A4` + FILLER），Python 的 `bytes.decode()` 里 FILLER 常量的值写成了
//     0xD4 而判断要求 0xA4，于是这条分支永不成立 ⇒ 解码器报"输入不完整"，错误处理器写
//     **一个**替换符后**丢弃整个剩余缓冲**。实测 `A4D4` / `A4D44141` / `A4D4CAF7D6`
//     全都是恰好一个 `U+FFFD`。这一格必须原样复刻，否则门禁 [12] 的对拍立刻红。
bool mw_decode_lossy(const MultibyteSpec& s, const uint8_t* data, size_t size,
                     std::string& text) {
    text.clear();
    text.reserve(size * 3);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        if (s.is_euc_kr && b1 == 0xA4 && i + 1 < size && data[i + 1] == 0xD4) {
            append_replacement(text);
            i = size;  // 错误处理器把剩余缓冲整段丢掉（实测，不是"吃掉两个字节"）
            continue;
        }
        const bool single_ok =
            s.single_hi != nullptr && b1 >= s.single_first && b1 <= s.single_last;
        if (is_lead_byte(s, b1) && i + 1 < size && is_trail_byte(s, data[i + 1])) {
            const uint16_t cp = mw_lookup(s, b1, data[i + 1]);
            if (cp != kUndefined) {
                append_utf8(text, cp);
                i += 2;
                continue;
            }
        }
        if (single_ok) {
            const uint16_t cp = s.single_hi[b1 - s.single_first];
            if (cp != kUndefined) {
                append_utf8(text, cp);
                ++i;
                continue;
            }
        }
        append_replacement(text);
        ++i;  // 只吃首字节：后面的字节按自己的身份继续处理
    }
    return true;
}

// ── 严格编码 ─────────────────────────────────────────────────────────
// 反向表在**函数内静态构造**（第一次用到时建，避免静态初始化顺序问题）：
// 扫一遍双字节表，**较小的字节序优先**（CPython 的生成表就是这个顺序）。实测：
// 三个编码照这条规则建出来的表与 `str.encode(codec)` 在**全部可编码位**上一致
// （big5 13706 / shift_jis 6942 / euc-kr 8225 项，逐项对过）。
// ★ 不能想当然：big5 有 4 处**同义重复**码位，CPython 的编码器选的是顺序**较大**的
//   那一格（U+FF0F→A241、U+FF3C→A242、U+5341→A451、U+5345→A4CA，而最小序是
//   A1FE/A240/A2CC/A2CE）⇒ 这 4 格必须显式列出来，否则编码方向会写出另一个字节对。
struct Big5EncOverride {
    uint32_t cp;
    uint16_t bytes;
};
const Big5EncOverride kBig5EncOverrides[] = {
    {0xFF0F, 0xA241}, {0xFF3C, 0xA242}, {0x5341, 0xA451}, {0x5345, 0xA4CA}};

const std::unordered_map<uint32_t, uint32_t>& mw_reverse(const MultibyteSpec& s, int which) {
    static const std::unordered_map<uint32_t, uint32_t> tables[3] = {
        // big5
        [] {
            std::unordered_map<uint32_t, uint32_t> m;
            for (uint32_t idx = 0; idx < kBig5Rows * kBig5Cols; ++idx) {
                const uint16_t cp = kBig5TwoByte[idx];
                if (cp != kUndefined) m.emplace(cp, idx);
            }
            // 4 处同义重复：CPython 的编码器选的是顺序**较大**的那一格，覆盖掉最小序
            for (const auto& ov : kBig5EncOverrides) {
                m[ov.cp] = mw_slot(kBig5Spec, static_cast<uint8_t>(ov.bytes >> 8),
                                   static_cast<uint8_t>(ov.bytes & 0xFF));
            }
            return m;
        }(),
        // shift_jis（双字节表 + 单字节半角片假名）
        // ★ **单字节优先**：U+FF61..FF9F 在双字节表里可能也有格子（CPython 的
        //   `_codecs_jp.c` 编码器把这段**显式路由**到单字节），实测 `'｡'.encode('shift_jis')`
        //   = `a1` 而不是任何双字节对 ⇒ 先放单字节、再放双字节（`emplace` 不覆盖）。
        [] {
            std::unordered_map<uint32_t, uint32_t> m;
            for (uint8_t b = 0xA1; b <= 0xDF; ++b) {
                const uint16_t cp = kShiftJisSingle[b - 0xA1];
                if (cp != kUndefined) m.emplace(cp, 0x1000000u | b);
            }
            for (uint32_t idx = 0; idx < kShiftJisRows * kShiftJisCols; ++idx) {
                const uint16_t cp = kShiftJisTwoByte[idx];
                if (cp != kUndefined) m.emplace(cp, idx);
            }
            return m;
        }(),
        // euc-kr
        [] {
            std::unordered_map<uint32_t, uint32_t> m;
            for (uint32_t idx = 0; idx < kEucKrRows * kEucKrCols; ++idx) {
                const uint16_t cp = kEucKrTwoByte[idx];
                if (cp != kUndefined) m.emplace(cp, idx);
            }
            return m;
        }()};
    return tables[which];
}

bool mw_encode_strict(const MultibyteSpec& s, int which, const std::string& text,
                      std::vector<uint8_t>& out, std::string& err, const char* name) {
    const auto& rev = mw_reverse(s, which);
    out.clear();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            err = std::string("正文不是合法 UTF-8（") + name + " 编码前）";
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<uint8_t>(cp));
            continue;
        }
        const auto it = rev.find(cp);
        if (it == rev.end()) {
            err = "码位 U+" + std::to_string(cp) + " 无法用 " + name + " 表示";
            return false;
        }
        const uint32_t slot = it->second;
        if (slot & 0x1000000u) {  // shift_jis 的单字节半角片假名
            out.push_back(static_cast<uint8_t>(slot & 0xFF));
        } else {
            out.push_back(static_cast<uint8_t>(s.lead_first + slot / s.cols));
            out.push_back(static_cast<uint8_t>(s.trail_first + slot % s.cols));
        }
    }
    return true;
}

bool decode_single_byte_lossy(const uint16_t* table, const uint8_t* data, size_t size,
                              std::string& text) {
    text.clear();
    text.reserve(size * 3);
    for (size_t i = 0; i < size; ++i) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            text.push_back(static_cast<char>(b));
            continue;
        }
        const uint16_t cp = table[b - 0x80];
        if (cp == kUndefined) {
            append_replacement(text);
            continue;
        }
        append_utf8(text, cp);
    }
    return true;
}

bool utf8_decode_lossy(const uint8_t* data, size_t size, std::string& text) {
    text.clear();
    text.reserve(size * 3);
    size_t i = 0;
    while (i < size) {
        const uint8_t b = data[i];
        if (b < 0x80) {
            text.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        if (b < 0xC2 || b > 0xF4) {  // 非法前导（含 C0/C1 过长编码与 F5..FF）
            append_replacement(text);
            ++i;
            continue;
        }
        const size_t need = (b < 0xE0) ? 2 : (b < 0xF0 ? 3 : 4);
        bool truncated = false;
        bool bad_continuation = false;
        size_t j = i + 1;
        for (; j < i + need; ++j) {
            if (j >= size) {  // 输入末尾截断：吃掉已经吃掉的，一个替换
                truncated = true;
                break;
            }
            const uint8_t c = data[j];
            bool ok = (c >= 0x80 && c <= 0xBF);
            if (j == i + 1) {  // 次字节的特殊范围（CPython 的 overlong/surrogate 检查）
                if (b == 0xE0) ok = (c >= 0xA0 && c <= 0xBF);
                else if (b == 0xED) ok = (c >= 0x80 && c <= 0x9F);
                else if (b == 0xF0) ok = (c >= 0x90 && c <= 0xBF);
                else if (b == 0xF4) ok = (c >= 0x80 && c <= 0x8F);
            }
            if (!ok) {
                bad_continuation = true;
                break;
            }
        }
        if (truncated) {
            append_replacement(text);
            i = size;
            continue;
        }
        if (bad_continuation) {
            append_replacement(text);
            i = j;  // 只吃到越界字节之前
            continue;
        }
        // 合法序列：按位拼码位
        uint32_t cp = 0;
        if (need == 2) {
            cp = ((b & 0x1Fu) << 6) | (data[i + 1] & 0x3Fu);
        } else if (need == 3) {
            cp = ((b & 0x0Fu) << 12) | ((data[i + 1] & 0x3Fu) << 6) | (data[i + 2] & 0x3Fu);
        } else {
            cp = ((b & 0x07u) << 18) | ((data[i + 1] & 0x3Fu) << 12) |
                 ((data[i + 2] & 0x3Fu) << 6) | (data[i + 3] & 0x3Fu);
        }
        append_utf8(text, cp);
        i += need;
    }
    return true;
}

bool gb18030_decode_lossy(const uint8_t* data, size_t size, std::string& text) {
    text.clear();
    text.reserve(size * 3);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        if (b1 == 0x80 || b1 == 0xFF) {  // Python 对 0x80 抛错；0xFF 也不是合法首字节
            append_replacement(text);
            ++i;
            continue;
        }
        if (i + 1 >= size) {  // 末尾截断：吃掉这个前导
            append_replacement(text);
            i = size;
            continue;
        }
        const uint8_t b2 = data[i + 1];
        if (b2 >= 0x40 && b2 <= 0xFE) {
            const uint16_t cp = kGb18030TwoByte[(b1 - 0x81) * 191 + (b2 - 0x40)];
            if (cp != kUndefined) {
                append_utf8(text, cp);
                i += 2;
                continue;
            }
        }
        // 四字节路径（b2 必须是数字位）
        if (b2 >= 0x30 && b2 <= 0x39) {
            if (i + 3 >= size) {  // 末尾截断
                append_replacement(text);
                i = size;
                continue;
            }
            const uint8_t b3 = data[i + 2];
            const uint8_t b4 = data[i + 3];
            if (b3 >= 0x81 && b3 <= 0xFE && b4 >= 0x30 && b4 <= 0x39) {
                const uint32_t linear =
                    ((static_cast<uint32_t>(b1) - 0x81) * 10 + (b2 - 0x30)) * 1260 +
                    (static_cast<uint32_t>(b3) - 0x81) * 10 + (b4 - 0x30);
                size_t lo = 0;
                size_t hi = kGb18030FourByteCount;
                while (lo < hi) {
                    const size_t mid = lo + (hi - lo) / 2;
                    if (kGb18030FourByte[mid].to < linear) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo < kGb18030FourByteCount && linear >= kGb18030FourByte[lo].from) {
                    const uint32_t cp = static_cast<uint32_t>(
                        static_cast<int64_t>(linear) + kGb18030FourByte[lo].delta);
                    append_utf8(text, cp);
                    i += 4;
                    continue;
                }
            }
        }
        append_replacement(text);
        ++i;  // 吃掉一个前导字节，剩下的下一轮各自处理
    }
    return true;
}

const std::unordered_map<uint32_t, uint16_t>& gb18030_two_byte_reverse() {    static const std::unordered_map<uint32_t, uint16_t> table = [] {
        std::unordered_map<uint32_t, uint16_t> m;
        m.reserve(24000);
        for (uint32_t idx = 0; idx < 126u * 191u; ++idx) {
            const uint16_t cp = kGb18030TwoByte[idx];
            if (cp != kUndefined) m.emplace(cp, static_cast<uint16_t>(idx));
        }
        return m;
    }();
    return table;
}

bool gb18030_encode(const std::string& text, std::vector<uint8_t>& out, std::string& err) {
    const auto& rev = gb18030_two_byte_reverse();
    out.clear();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            err = "正文不是合法 UTF-8（gb18030 编码前）";
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<uint8_t>(cp));
            continue;
        }
        const auto it = rev.find(cp);
        if (it != rev.end()) {
            const uint16_t idx = it->second;
            out.push_back(static_cast<uint8_t>(0x81 + idx / 191));
            out.push_back(static_cast<uint8_t>(0x40 + idx % 191));
            continue;
        }
        bool done = false;
        for (size_t s = 0; s < kGb18030FourByteCount; ++s) {
            const int64_t linear = static_cast<int64_t>(cp) - kGb18030FourByte[s].delta;
            if (linear >= kGb18030FourByte[s].from && linear <= kGb18030FourByte[s].to) {
                const uint32_t l = static_cast<uint32_t>(linear);
                const uint32_t b2 = 0x30 + (l / (126u * 10u)) % 10u;
                const uint32_t b3 = 0x81 + (l / 10u) % 126u;
                const uint32_t b4 = 0x30 + l % 10u;
                const uint32_t b1 = 0x81 + l / (10u * 126u * 10u);
                out.push_back(static_cast<uint8_t>(b1));
                out.push_back(static_cast<uint8_t>(b2));
                out.push_back(static_cast<uint8_t>(b3));
                out.push_back(static_cast<uint8_t>(b4));
                done = true;
                break;
            }
        }
        if (!done) {
            err = "码位 U+" + std::to_string(cp) + " 无法用 gb18030 表示";
            return false;
        }
    }
    return true;
}

// ── GBK / GB2312（只有双字节形态）────────────────────────────────────
// 两个编码共用同一个循环：非法组合查表得 0xFFFF ⇒ 严格失败。
// gb2312 只用于判定链第 4 步的"声明被字节证实"（1.x `bytes_fit_encoding` 直接
// `raw.decode("gb2312")`），所以表必须与 Python 的 gb2312 codec 逐项一致。
bool gb_two_byte_decode(const uint16_t* table, const char* name, const uint8_t* data,
                        size_t size, std::string& text, std::string& err) {
    text.clear();
    text.reserve(size * 2);
    size_t i = 0;
    while (i < size) {
        const uint8_t b1 = data[i];
        if (b1 < 0x80) {
            text.push_back(static_cast<char>(b1));
            ++i;
            continue;
        }
        if (b1 < 0x81 || b1 > 0xFE) {
            // ★★ 2026-10-06（审计处理批·2-1 / H1-3）：与 `gb18030_decode` 同一处漏写的收口。
            //   两张表的 lead 域同样是 `0x81..0xFE`（生成器 `GB_TWO_W = 126 * 191`），
            //   修前只挡 `0x80` ⇒ `0xFF` 算出的下标直接越过表尾（`gbk` 实测解出 `00`、
            //   `gb2312` 实测解出 `e4 be 99` —— 越界读到的垃圾被当成合法解码结果写出）。
            err = std::string(name) + "：非法首字节 " + hex_byte(b1);
            return false;
        }
        if (i + 1 >= size) {
            err = std::string(name) + "：双字节序列被截断";
            return false;
        }
        const uint8_t b2 = data[i + 1];
        if (b2 < 0x40 || b2 > 0xFE) {
            err = std::string(name) + "：非法尾字节";
            return false;
        }
        const uint16_t cp = table[(b1 - 0x81) * 191 + (b2 - 0x40)];
        if (cp == kUndefined) {
            err = std::string(name) + "：未定义的双字节序列";
            return false;
        }
        append_utf8(text, cp);
        i += 2;
    }
    return true;
}

bool gbk_decode(const uint8_t* data, size_t size, std::string& text, std::string& err) {
    return gb_two_byte_decode(kGbkTwoByte, "gbk", data, size, text, err);
}

bool gb2312_decode(const uint8_t* data, size_t size, std::string& text, std::string& err) {
    return gb_two_byte_decode(kGb2312TwoByte, "gb2312", data, size, text, err);
}

// ── UTF-16 / UTF-32（B 链的 BOM 分支与"按名字解码"都要用）─────────────
enum class ByteOrder { Little, Big, Native };

bool utf16_decode(const uint8_t* data, size_t size, ByteOrder order, bool allow_bom,
                  std::string& text, std::string& err) {
    size_t off = 0;
    bool little = (order != ByteOrder::Big);
    if (allow_bom && size >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) {
            little = true;
            off = 2;
        } else if (data[0] == 0xFE && data[1] == 0xFF) {
            little = false;
            off = 2;
        }
    }
    if ((size - off) % 2 != 0) {
        err = "utf-16：长度不是偶数";
        return false;
    }
    text.clear();
    text.reserve(size);
    for (size_t i = off; i < size; i += 2) {
        const uint16_t u = little ? static_cast<uint16_t>(data[i] | (data[i + 1] << 8))
                                  : static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= size) {
                err = "utf-16：代理对不完整";
                return false;
            }
            const uint16_t lo = little
                                    ? static_cast<uint16_t>(data[i + 2] | (data[i + 3] << 8))
                                    : static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
            if (lo < 0xDC00 || lo > 0xDFFF) {
                err = "utf-16：孤立的高代理";
                return false;
            }
            append_utf8(text, 0x10000u + ((static_cast<uint32_t>(u) - 0xD800u) << 10) +
                                  (static_cast<uint32_t>(lo) - 0xDC00u));
            i += 2;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            err = "utf-16：孤立的低代理";
            return false;
        } else {
            append_utf8(text, u);
        }
    }
    return true;
}

bool utf32_decode(const uint8_t* data, size_t size, ByteOrder order, bool allow_bom,
                  std::string& text, std::string& err) {
    size_t off = 0;
    bool little = (order != ByteOrder::Big);
    if (allow_bom && size >= 4) {
        const bool le_bom =
            data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00;
        const bool be_bom =
            data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF;
        if (le_bom) {
            little = true;
            off = 4;
        } else if (be_bom) {
            little = false;
            off = 4;
        }
    }
    if ((size - off) % 4 != 0) {
        err = "utf-32：长度不是 4 的倍数";
        return false;
    }
    text.clear();
    text.reserve(size / 2);
    for (size_t i = off; i < size; i += 4) {
        uint32_t cp = 0;
        if (little) {
            cp = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                 (static_cast<uint32_t>(data[i + 2]) << 16) |
                 (static_cast<uint32_t>(data[i + 3]) << 24);
        } else {
            cp = (static_cast<uint32_t>(data[i]) << 24) |
                 (static_cast<uint32_t>(data[i + 1]) << 16) |
                 (static_cast<uint32_t>(data[i + 2]) << 8) | static_cast<uint32_t>(data[i + 3]);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            err = "utf-32：非法码位";
            return false;
        }
        append_utf8(text, cp);
    }
    return true;
}

}  // namespace

void utf8_append(std::string& out, uint32_t cp) { append_utf8(out, cp); }

bool utf8_next(const uint8_t* data, size_t size, size_t& pos, uint32_t& cp) {
    return utf8_decode_one(data, size, pos, cp);
}

uint64_t utf8_codepoint_count(const std::string& utf8) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8.data());
    size_t i = 0;
    uint64_t n = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, utf8.size(), i, cp)) return 0;
        ++n;
    }
    return n;
}

std::vector<std::pair<uint32_t, std::string>> codec_encode_table(int which) {
    if (which < 0 || which > 2) return {};
    const MultibyteSpec& s = (which == 0) ? kBig5Spec : (which == 1 ? kShiftJisSpec : kEucKrSpec);
    std::vector<std::pair<uint32_t, std::string>> out;
    for (const auto& kv : mw_reverse(s, which)) {
        std::string bytes;
        if ((kv.second & 0x1000000u) != 0) {
            bytes.push_back(static_cast<char>(kv.second & 0xFF));
        } else {
            bytes.push_back(
                static_cast<char>(s.lead_first + kv.second / s.cols));
            bytes.push_back(
                static_cast<char>(s.trail_first + kv.second % s.cols));
        }
        out.emplace_back(kv.first, bytes);
    }
    return out;
}

bool decode_strict(const std::string& encoding, const uint8_t* data, size_t size,
                   std::string& text, std::string& err) {
    err.clear();
    if (encoding == kUtf8 || encoding == kUtf8Sig) {
        size_t off = 0;
        if (encoding == kUtf8Sig && size >= 3 && data[0] == 0xEF && data[1] == 0xBB &&
            data[2] == 0xBF) {
            off = 3;
        }
        size_t bad = 0;
        if (!utf8_validate(data + off, size - off, bad)) {
            err = "不是合法 UTF-8（偏移 " + std::to_string(bad) + "）";
            return false;
        }
        text.assign(reinterpret_cast<const char*>(data + off), size - off);
        return true;
    }
    if (encoding == kGb18030) return gb18030_decode(data, size, text, err);
    if (encoding == kGbk) return gbk_decode(data, size, text, err);
    if (encoding == kGb2312) return gb2312_decode(data, size, text, err);
    // D3 批补的三张东亚表（big5 / shift_jis / euc-kr）
    if (encoding == kBig5) return mw_decode_strict(kBig5Spec, kBig5, data, size, text, err);
    if (encoding == kShiftJis) {
        return mw_decode_strict(kShiftJisSpec, kShiftJis, data, size, text, err);
    }
    if (encoding == kEucKr) return mw_decode_strict(kEucKrSpec, kEucKr, data, size, text, err);
    if (encoding == kAscii) {
        text.clear();
        text.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (data[i] >= 0x80) {
                err = "ascii：出现高位字节";
                return false;
            }
            text.push_back(static_cast<char>(data[i]));
        }
        return true;
    }
    // utf-16 / utf-32：带 BOM 的走"按名字解码"（codec 自己吃 BOM），
    // -le/-be 变体不认 BOM（与 Python 同名 codec 一致）
    if (encoding == kUtf16) return utf16_decode(data, size, ByteOrder::Native, true, text, err);
    if (encoding == kUtf16Le) return utf16_decode(data, size, ByteOrder::Little, false, text, err);
    if (encoding == kUtf16Be) return utf16_decode(data, size, ByteOrder::Big, false, text, err);
    if (encoding == kUtf32) return utf32_decode(data, size, ByteOrder::Native, true, text, err);
    if (encoding == kUtf32Le) return utf32_decode(data, size, ByteOrder::Little, false, text, err);
    if (encoding == kUtf32Be) return utf32_decode(data, size, ByteOrder::Big, false, text, err);
    if (encoding == kWindows1251) {
        return decode_single_byte(kCp1251ToUnicode, data, size, text, err, "windows-1251");
    }
    if (encoding == kWindows1252) {
        return decode_single_byte(kCp1252ToUnicode, data, size, text, err, "windows-1252");
    }
    if (encoding == kLatin1) {
        text.clear();
        text.reserve(size * 2);
        for (size_t i = 0; i < size; ++i) append_utf8(text, data[i]);
        return true;
    }
    err = "未知编码：" + encoding;
    return false;
}

namespace {

// 规格 §5.3（Python `errors="replace"` 实测语义）：
//   · BOM **只在带 BOM 的编解码器**里被吃（无 BOM ⇒ 本机序 LE 且不吃字节；-le/-be 永不吃）
//   · utf-16 尾部**奇数**字节 ⇒ 恰好 1 个 U+FFFD；代理对合成，问题单位**各** 1 个
//   · utf-32 尾部 1~3 字节 ⇒ **恰好 1 个**；越界/代理区 ⇒ 1 个
bool utf16_decode_lossy_impl(const uint8_t* data, size_t size, bool little, bool bom_aware,
                             std::string& text) {
    size_t i = 0;
    bool le = little;
    if (bom_aware && size >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) {
            le = true;
            i = 2;
        } else if (data[0] == 0xFE && data[1] == 0xFF) {
            le = false;
            i = 2;
        }
    }
    text.clear();
    while (i + 1 < size) {
        const uint16_t unit = le ? static_cast<uint16_t>(data[i] | (data[i + 1] << 8))
                                 : static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        i += 2;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < size) {
                const uint16_t low = le ? static_cast<uint16_t>(data[i] | (data[i + 1] << 8))
                                        : static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    i += 2;
                    append_utf8(text, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
                    continue;
                }
            }
            append_replacement(text);
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            append_replacement(text);
            continue;
        }
        append_utf8(text, unit);
    }
    if (i < size) append_replacement(text);  // 尾部奇数 ⇒ 恰好一个
    return true;
}

bool utf32_decode_lossy_impl(const uint8_t* data, size_t size, bool little, bool bom_aware,
                             std::string& text) {
    size_t i = 0;
    bool le = little;
    if (bom_aware && size >= 4) {
        if (data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00) {
            le = true;
            i = 4;
        } else if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF) {
            le = false;
            i = 4;
        }
    }
    text.clear();
    while (i + 3 < size) {
        uint32_t unit = 0;
        if (le) {
            unit = static_cast<uint32_t>(data[i]) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                   (static_cast<uint32_t>(data[i + 2]) << 16) |
                   (static_cast<uint32_t>(data[i + 3]) << 24);
        } else {
            unit = (static_cast<uint32_t>(data[i]) << 24) |
                   (static_cast<uint32_t>(data[i + 1]) << 16) |
                   (static_cast<uint32_t>(data[i + 2]) << 8) | static_cast<uint32_t>(data[i + 3]);
        }
        i += 4;
        if (unit > 0x10FFFFu || (unit >= 0xD800u && unit <= 0xDFFFu)) {
            append_replacement(text);
            continue;
        }
        append_utf8(text, unit);
    }
    if (i < size) append_replacement(text);  // 尾部 1~3 字节 ⇒ 恰好一个
    return true;
}

}  // namespace

bool decode_lossy(const std::string& encoding, const uint8_t* data, size_t size,
                  std::string& text, std::string& err) {
    err.clear();
    if (encoding == kUtf8 || encoding == kUtf8Sig) {
        if (encoding == kUtf8Sig && size >= 3 && data[0] == 0xEF && data[1] == 0xBB &&
            data[2] == 0xBF) {
            return utf8_decode_lossy(data + 3, size - 3, text);
        }
        return utf8_decode_lossy(data, size, text);
    }
    if (encoding == kUtf16) return utf16_decode_lossy_impl(data, size, /*little=*/true,
                                                      /*bom_aware=*/true, text);
    if (encoding == kUtf16Le)
        return utf16_decode_lossy_impl(data, size, true, false, text);
    if (encoding == kUtf16Be)
        return utf16_decode_lossy_impl(data, size, false, false, text);
    if (encoding == kUtf32)
        return utf32_decode_lossy_impl(data, size, true, true, text);
    if (encoding == kUtf32Le)
        return utf32_decode_lossy_impl(data, size, true, false, text);
    if (encoding == kUtf32Be)
        return utf32_decode_lossy_impl(data, size, false, false, text);
    if (encoding == kGb18030) return gb18030_decode_lossy(data, size, text);
    // gbk / gb2312 只有两字节路径，且各自的表比 gb18030 小 —— 不能借 gb18030 的表，
    // 否则 `81 40`（gbk 有、gb2312 没有）与四字节序列都会判错。
    if (encoding == kGbk) return gb_two_byte_decode_lossy(kGbkTwoByte, data, size, text);
    if (encoding == kGb2312) return gb_two_byte_decode_lossy(kGb2312TwoByte, data, size, text);
    // D3 批：三张新表的有损解码（Python `errors="replace"` 逐字节对拍过）
    if (encoding == kBig5) return mw_decode_lossy(kBig5Spec, data, size, text);
    if (encoding == kShiftJis) return mw_decode_lossy(kShiftJisSpec, data, size, text);
    if (encoding == kEucKr) return mw_decode_lossy(kEucKrSpec, data, size, text);
    if (encoding == kWindows1251) {
        return decode_single_byte_lossy(kCp1251ToUnicode, data, size, text);
    }
    if (encoding == kWindows1252) {
        return decode_single_byte_lossy(kCp1252ToUnicode, data, size, text);
    }
    if (encoding == kAscii) {
        text.clear();
        text.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (data[i] >= 0x80) {
                append_replacement(text);
            } else {
                text.push_back(static_cast<char>(data[i]));
            }
        }
        return true;
    }
    if (encoding == kLatin1) {
        text.clear();
        text.reserve(size * 2);
        for (size_t i = 0; i < size; ++i) append_utf8(text, data[i]);
        return true;
    }
    // 其余编码（utf-16/utf-32 家族）暂不做有损模式：**如实报错**，不假装支持。
    // 转换页的目标/源编码下拉只列这里做过对拍的编码（见 golden_lossy_codec.py）。
    if (decode_strict(encoding, data, size, text, err)) return true;
    err = "有损解码尚未对拍该编码：" + encoding + "（严格解码失败：" + err + "）";
    return false;
}

namespace {

// 规格 §5.3：带 BOM 的编解码器（utf-16 / utf-32）写**小端** BOM；`-le/-be` 不写。
// 单独代理（无法表示）在严格模式下**失败**，在有损模式下写 `?`（按各自字节序与单位宽度）。
bool is_utf16_family(const std::string& encoding) {
    return encoding == kUtf16 || encoding == kUtf16Le || encoding == kUtf16Be;
}
bool is_utf32_family(const std::string& encoding) {
    return encoding == kUtf32 || encoding == kUtf32Le || encoding == kUtf32Be;
}

bool utf16_encode_impl(const std::string& encoding, const std::string& text, bool lossy,
                       std::vector<uint8_t>& out, std::string& err) {
    const bool bom = (encoding == kUtf16);
    const bool little = (encoding != kUtf16Be);
    out.clear();
    if (bom) {
        out.push_back(0xFF);
        out.push_back(0xFE);
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            if (lossy) {
                out.push_back(little ? 0x3F : 0x00);
                out.push_back(little ? 0x00 : 0x3F);
                i = text.size();
                continue;
            }
            err = "正文不是合法 UTF-8（编码方向）";
            return false;
        }
        auto emit = [&](uint16_t unit) {
            out.push_back(static_cast<uint8_t>(little ? (unit & 0xFF) : (unit >> 8)));
            out.push_back(static_cast<uint8_t>(little ? (unit >> 8) : (unit & 0xFF)));
        };
        if (cp <= 0xFFFF) {
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (!lossy) {
                    err = "utf-16 无法表示单独代理";
                    return false;
                }
                emit(0x003F);
                continue;
            }
            emit(static_cast<uint16_t>(cp));
            continue;
        }
        if (cp > 0x10FFFF) {
            if (!lossy) {
                err = "码位超出 Unicode 范围";
                return false;
            }
            emit(0x003F);
            continue;
        }
        const uint32_t v = cp - 0x10000;
        emit(static_cast<uint16_t>(0xD800 + (v >> 10)));
        emit(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
    }
    return true;
}

bool utf32_encode_impl(const std::string& encoding, const std::string& text, bool lossy,
                       std::vector<uint8_t>& out, std::string& err) {
    const bool bom = (encoding == kUtf32);
    const bool little = (encoding != kUtf32Be);
    out.clear();
    if (bom) {
        out.push_back(0xFF);
        out.push_back(0xFE);
        out.push_back(0x00);
        out.push_back(0x00);
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            if (lossy) {
                // ★★ 2026-10-06（审计处理批·2-2 / A1-2）：修前这里只 `cp = 0x3F`，
                //   **`i` 不前进**（`utf8_decode_one` 失败时按契约不推进，见 `encoding.h:87`）
                //   ⇒ `while (i < text.size())` 恒真、`out` 每轮 +4 字节 ⇒ **死循环 + 无限增长**。
                //   两个兄弟分支都处理了（utf-16 的下面那段、`encode_lossy` 的兜底）；
                //   只有 utf-32 有损分支漏了。照 utf-16 同口径收尾：写一个 `?` 就结束
                //   （1.x 对"正文不是合法 UTF-8"的语义是整串替换，不是逐字节打补丁）。
                for (int k = 0; k < 4; ++k) {
                    const int shift = little ? (8 * k) : (8 * (3 - k));
                    out.push_back(static_cast<uint8_t>((0x3Fu >> shift) & 0xFF));
                }
                i = text.size();
                continue;
            }
            err = "正文不是合法 UTF-8（编码方向）";
            return false;
        }
        if ((cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            if (!lossy) {
                err = "utf-32 无法表示该码位";
                return false;
            }
            cp = 0x3F;
        }
        for (int k = 0; k < 4; ++k) {
            const int shift = little ? (8 * k) : (8 * (3 - k));
            out.push_back(static_cast<uint8_t>((cp >> shift) & 0xFF));
        }
    }
    return true;
}

}  // namespace

bool encode_lossy(const std::string& encoding, const std::string& text, std::vector<uint8_t>& out,
                  std::string& err) {
    err.clear();
    if (is_utf16_family(encoding)) {
        return utf16_encode_impl(encoding, text, /*lossy=*/true, out, err);
    }
    if (is_utf32_family(encoding)) {
        return utf32_encode_impl(encoding, text, /*lossy=*/true, out, err);
    }
    // 能严格编码就严格编码（与 replace 的结果一致，因为没触发替换）
    if (encode_strict(encoding, text, out, err)) return true;
    // 否则逐码位来：表示不了的写 `?`（Python `errors="replace"` 在编码方向的行为）
    out.clear();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = i;
        uint32_t cp = 0;
        if (!utf8_decode_one(p, text.size(), i, cp)) {
            err = "正文不是合法 UTF-8（有损编码前）";
            return false;
        }
        std::vector<uint8_t> piece;
        std::string piece_err;
        if (encode_strict(encoding, std::string(text, start, i - start), piece, piece_err)) {
            out.insert(out.end(), piece.begin(), piece.end());
        } else {
            out.push_back('?');
        }
    }
    return true;
}

bool encode_strict(const std::string& encoding, const std::string& text,
                   std::vector<uint8_t>& out, std::string& err) {    err.clear();
    if (encoding == kUtf8) {
        out.assign(text.begin(), text.end());
        return true;
    }
    if (encoding == kUtf8Sig) {
        out.clear();
        out.push_back(0xEF);
        out.push_back(0xBB);
        out.push_back(0xBF);
        out.insert(out.end(), text.begin(), text.end());
        return true;
    }
    if (is_utf16_family(encoding)) {
        return utf16_encode_impl(encoding, text, /*lossy=*/false, out, err);
    }
    if (is_utf32_family(encoding)) {
        return utf32_encode_impl(encoding, text, /*lossy=*/false, out, err);
    }
    if (encoding == kGb18030) return gb18030_encode(text, out, err);
    // D3 批：三张新表的严格编码。★ 这一侧**必须有** —— ALPHA.44 的 `ascii` 就是
    //   只有解码侧、编码侧一行都没有，于是 `encode_strict` 报"未知编码"，上层
    //   `encode_lossy` 走"逐码位、表示不了就写 `?`"的兜底 ⇒ 连纯 ASCII 正文都被
    //   整篇写成 `?`（`期望 6173… 得到 3f3f…`）。三个新编码**两侧都做**。
    if (encoding == kBig5) return mw_encode_strict(kBig5Spec, 0, text, out, err, kBig5);
    if (encoding == kShiftJis) {
        return mw_encode_strict(kShiftJisSpec, 1, text, out, err, kShiftJis);
    }
    if (encoding == kEucKr) return mw_encode_strict(kEucKrSpec, 2, text, out, err, kEucKr);
    if (encoding == kWindows1251) {
        return encode_single_byte(kCp1251ToUnicode, text, out, err, "windows-1251");
    }
    if (encoding == kWindows1252) {
        return encode_single_byte(kCp1252ToUnicode, text, out, err, "windows-1252");
    }
    if (encoding == kLatin1) {
        out.clear();
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = 0;
            if (!utf8_decode_one(p, text.size(), i, cp)) {
                err = "正文不是合法 UTF-8（latin-1 编码前）";
                return false;
            }
            if (cp > 0xFF) {
                err = "码位 U+" + std::to_string(cp) + " 无法用 latin-1 表示";
                return false;
            }
            out.push_back(static_cast<uint8_t>(cp));
        }
        return true;
    }
    // ★★ 2026-10-06（D2 批，**实测查实的真缺陷**）：`ascii` 原来**只有解码侧**，
    //   编码侧一行都没有 ⇒ `encode_strict("ascii", "abc")` 报"未知编码：ascii"，
    //   `encode_lossy` 于是走"逐码位、表示不了就写 `?`"的兜底 ⇒ **连纯 ASCII 正文
    //   也被整篇写成 `?`**（`tests/golden_lossy_codec.py` 加进 ascii 后立刻判红：
    //   `encode ascii 'ascii only'：期望 6173636969206f6e6c79，得到 3f3f…`）。
    //   这就是"目标编码下拉里不能列没对拍过的编码"那条纪律救回来的一次：1.x 的下拉里
    //   有 ascii，我们一直没列，理由正是"没对拍过" —— 补对拍时才发现它根本不能编码。
    //   严格语义照 Python 的 `ascii` codec：任何 > 0x7F 的码位都报错（有损侧由
    //   `encode_lossy` 的逐码位兜底写成 `?`）。
    if (encoding == kAscii) {
        out.clear();
        const uint8_t* p = reinterpret_cast<const uint8_t*>(text.data());
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = 0;
            if (!utf8_decode_one(p, text.size(), i, cp)) {
                err = "正文不是合法 UTF-8（ascii 编码前）";
                return false;
            }
            if (cp > 0x7F) {
                err = "码位 U+" + std::to_string(cp) + " 无法用 ascii 表示";
                return false;
            }
            out.push_back(static_cast<uint8_t>(cp));
        }
        return true;
    }
    err = "未知编码：" + encoding;
    return false;
}

DecodeResult decode_text_chain(const std::vector<uint8_t>& raw) {
    // ★ 顺序 = 1.x `read_text_file` 的顺序，不能改：
    //   utf-8-sig → utf-8 → gb18030 → windows-1251 → windows-1252 → latin-1
    //   gb18030 必须排在 windows-1251 之前：cp1251 对任意字节都能解成功，
    //   放前面会把中文文件整篇吞成乱码（1.x 注释里记着这次回归）。
    static const char* kChain[] = {kUtf8Sig, kUtf8, kGb18030, kWindows1251, kWindows1252, kLatin1};
    DecodeResult res;
    std::string err;
    for (const char* enc : kChain) {
        std::string text;
        if (decode_strict(enc, raw.data(), raw.size(), text, err)) {
            res.ok = true;
            res.text = std::move(text);
            res.encoding = enc;
            res.chars = utf8_codepoint_count(res.text);
            return res;
        }
    }
    // latin-1 永不失败，走到这里说明实现有错
    res.error = "六编码链全部失败（latin-1 兜底不该失败）：" + err;
    return res;
}

void normalize_newlines(std::string& text) {
    // Python 文本模式（newline=None）的通用换行：\r\n 与单独的 \r 都变成 \n
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    text.swap(out);
}

DecodeResult read_text_file(const std::string& path) {
    std::ifstream in(core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        DecodeResult res;
        res.error = "打不开文件：" + path;
        return res;
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    DecodeResult res = decode_text_chain(raw);
    if (res.ok) {
        normalize_newlines(res.text);
        // ★ 码位数必须**归一之后**再数：1.x 的 len(text) 是文本模式读出来的正文
        //   （通用换行已生效）。在归一前数，CRLF 源文件会多算一个字符。
        res.chars = utf8_codepoint_count(res.text);
    }
    return res;
}

}  // namespace xrkit::engine::text
