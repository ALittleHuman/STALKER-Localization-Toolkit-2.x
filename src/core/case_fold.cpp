// xrkit::core —— Unicode 小写折叠的实现（**唯一一处包含折叠表**）。
//
// ★★ H3(a)（2026-10-25）：实现从 `include/xrkit/engine/text/case_fold.h` **原样搬来**，
//   只做了两件事：① 命名空间改成 `xrkit::core`；② 表从 `engine/text/…inc` 移到
//   `core/case_fold_table.inc`。**算法与表一个字节都没改** —— 所以本阶段的输出必须与搬前逐字节相同。
//   表 21.2 KB，只在这里 include：别把它放回头文件（每个 TU 会各背一份常量）。

#include "xrkit/core/case_fold.h"

#include "xrkit/core/case_fold_table.inc"

namespace xrkit::core {
namespace {

// 单个码位的小写映射：返回**追加到 out 的字节数**（0 = 无映射，调用方按原样追加）。
// 多字符映射（`U+0130` 那种）按 UTF-8 直接追加进去。
std::size_t case_fold_append_one(std::uint32_t cp, std::string& out) {
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

}  // namespace

std::string case_fold_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(ascii_lower_char(c));
    return out;
}

std::string case_fold_lower(std::string_view utf8) {
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
            cp = b0 & 0x1Fu;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0Fu;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07u;
            extra = 3;
        } else {
            out.push_back(static_cast<char>(b0));   // 非法首字节：原样透传
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {
            out.append(utf8.substr(i));
            break;
        }
        bool ok = true;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char bk = static_cast<unsigned char>(utf8[i + k]);
            if ((bk & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        if (!ok) {
            out.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        if (case_fold_append_one(cp, out) == 0) out.append(utf8.substr(i, extra + 1));
        i += extra + 1;
    }
    return out;
}

}  // namespace xrkit::core
