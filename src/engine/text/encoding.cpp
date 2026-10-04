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
        if (b1 == 0x80) {
            // Python 的 gb18030 对 0x80 抛 UnicodeDecodeError，不是欧元符号
            err = "gb18030：非法首字节 0x80";
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
        if (b1 == 0x80) {
            err = std::string(name) + "：非法首字节 0x80";
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
                cp = 0x3F;
            } else {
                err = "正文不是合法 UTF-8（编码方向）";
                return false;
            }
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
