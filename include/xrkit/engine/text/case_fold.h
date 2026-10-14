// ★★ P17-112（复审计 `01 F-10`）：**Unicode 小写折叠 —— 跟本机 CPython 走**。
//
// 为什么不是"用 Windows API 模仿 Python"（修前是 `LCMapStringEx`，实测不成立）：
//   全域复核 `0x20..0x2FFFF` 后，Windows 的 invariant 小写与 Python 的 `str.lower()`
//   **单字符内分歧 131276 个码位**（`U+01C5 ǅ` / `U+0524 Ԥ` / `U+03F4 ϴ` …），
//   且 `U+0130 İ` 在 Python 下折成**两个**码位（`"i"` + `U+0307`）而 Windows 保持不动。
//   根因是两边各自一套 Unicode 版本与映射口径 ⇒ 模仿这条路**本来就跟不上**。
//
// 口径（与 1.x `str(path).lower()` **同源，但只限"逐码位"那一部分** ——
//   上下文相关规则见下面的 KNOWN_DIVERGENCE；与 `tools/gen_encoding_tables.py` 的先例一致）：
//   表由 `tools/gen_case_fold_table.py` **从本机 CPython 导出**（`case_fold_table.inc`），
//   `--check` 与门禁 `case-fold-table` 步**逐字节**守着"表没漂"。
//
// 边界（如实写在这里，免得被读成"完整 Unicode 大小写"）：
//   · 只做 **`str.lower()` 的单码位/多码位映射**；**不做** `str.casefold()` 的
//     额外等价（`ß` → `ss`、`İ` 的 NFKC 变体等）—— 1.x 用的就是 `.lower()`，不是 `.casefold()`。
//   · ★★ **KNOWN_DIVERGENCE（已知分歧台账，用户 2026-10-13 裁定"投 A"）**：
//     **上下文相关的小写映射**本表复现不了（本表是逐码位的）。这一类**已被穷举并界定**：
//       唯一成员 `U+03A3 Σ` —— `'ΟΔΟΣ'.lower()` 得 `'οδος'`（末位 **ς**），本表恒给 **σ**；
//       实测**只有这 1 个**（Unicode 16.0.0，CPython 3.14），**不是「可能还有别的」**。
//     完整台账（含症状与界）由生成器写进 `case_fold_table.inc` 的 `KNOWN_DIVERGENCE` 段。
//     **触发条件**：当语料出现希腊文、或与 1.x 对拍出现 `ς` ⇒ 这条会真实影响排序键 ⇒
//       那时才升 B（引入上下文感知：需 `Cased`/`Case_Ignorable` 一套新表 ＋ 一条对账判据）。
//       触发之前**不补** —— 素材面是西里尔＋中文，为一个不存在的场景铸通用件不划算。
//   · 表只覆盖到 `U+2FFFF`（生成器的范围；更远的码位原样返回）。
#pragma once

#include <cstdint>
#include <string>

#include "xrkit/engine/text/case_fold_table.inc"

namespace xrkit::engine::text {

// 单个码位的小写映射：返回**追加到 out 的字节数**（0 = 无映射，调用方按原样追加）。
// 多字符映射（`U+0130` 那种）按 UTF-8 直接追加进去。
inline std::size_t case_fold_append_one(std::uint32_t cp, std::string& out) {
    // ① 多字符例外（极少数，先查；数量很小）
    for (unsigned int i = 0; i < kCaseFoldMultiCount; ++i) {
        if (kCaseFoldMultiCp[i] == cp) {
            const char* bytes = kCaseFoldMultiUtf8[i];
            const std::size_t len = std::char_traits<char>::length(bytes);
            out.append(bytes, len);
            return len;
        }
    }
    // ② 区间表：区间按 first_cp 升序 ⇒ 二分
    std::size_t lo = 0;
    std::size_t hi = kCaseFoldRunCount;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        const unsigned int first = kCaseFoldRuns[mid][0];
        const unsigned int count = kCaseFoldRuns[mid][2];
        if (cp < first) {
            hi = mid;
        } else if (cp >= first + count) {
            lo = mid + 1;
        } else {
            const std::uint32_t low = kCaseFoldRuns[mid][1] + (cp - first);
            // 小写结果一律 ≤ U+10FFFF ⇒ 按 UTF-8 编码（这里只需 1~4 字节，手写比依赖 locale 稳）
            char buf[4];
            std::size_t n = 0;
            if (low < 0x80) {
                buf[n++] = static_cast<char>(low);
            } else if (low < 0x800) {
                buf[n++] = static_cast<char>(0xC0 | (low >> 6));
                buf[n++] = static_cast<char>(0x80 | (low & 0x3F));
            } else if (low < 0x10000) {
                buf[n++] = static_cast<char>(0xE0 | (low >> 12));
                buf[n++] = static_cast<char>(0x80 | ((low >> 6) & 0x3F));
                buf[n++] = static_cast<char>(0x80 | (low & 0x3F));
            } else {
                buf[n++] = static_cast<char>(0xF0 | (low >> 18));
                buf[n++] = static_cast<char>(0x80 | ((low >> 12) & 0x3F));
                buf[n++] = static_cast<char>(0x80 | ((low >> 6) & 0x3F));
                buf[n++] = static_cast<char>(0x80 | (low & 0x3F));
            }
            out.append(buf, n);
            return n;
        }
    }
    return 0;   // 无映射（表里只收"值不等于自身"的码位）
}

// UTF-8 字符串的小写折叠（口径同 Python `str.lower()`；非法字节原样透传）。
inline std::string case_fold_lower(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b0 = static_cast<unsigned char>(utf8[i]);
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            extra = 3;
        } else {
            out.push_back(utf8[i]);   // 非法起始字节：原样透传（不猜、不吞）
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {           // 截断的序列：原样透传
            out.append(utf8, i, utf8.size() - i);
            break;
        }
        bool ok = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char bk = static_cast<unsigned char>(utf8[i + k]);
            if ((bk & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (bk & 0x3F);
        }
        if (!ok) {
            out.push_back(utf8[i]);
            ++i;
            continue;
        }
        if (case_fold_append_one(cp, out) == 0) {
            out.append(utf8, i, extra + 1);        // 无映射 ⇒ 原样
        }
        i += extra + 1;
    }
    return out;
}

}  // namespace xrkit::engine::text

// ★★ P17-112：`path_lower()` 是 `src/engine/text/extract.cpp` 里**排序键**用的那一个
//   （1.x 的 `str(path).lower()` 对位物）。这里声明出来是为了让**探针/判据能直接考它** ——
//   ★ 这条不是洁癖：本批第一版判据只考 `case_fold_lower()`，结果"把 `path_lower` 退回
//     `ascii_lower`"**照样绿**（判据考的不是真正在用的那个函数）。判据必须考**在用的那个**。
namespace xrkit::engine::text {
std::string path_lower(const std::string& s);
}  // namespace xrkit::engine::text
