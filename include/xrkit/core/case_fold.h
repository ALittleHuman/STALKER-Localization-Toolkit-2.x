#pragma once
// xrkit::core —— **Unicode 小写折叠**（表由 CPython 导出）＋ **ASCII 折叠**（旧行为，搬家期用）。
//
// ★★ H3(a)（2026-10-25）：**原语从 `engine/text` 下沉到 `core`**。
//   起因（全量审计 H3）：仓里有两套折叠在并存 ——
//     · `engine/text/case_fold.h` 的 `case_fold_lower()`（Unicode 表，CPython 导出）；
//     · 各处的**手写 A-Z→a-z**（`path_equals` / `dup_key` / `component_equal` / `artifact_key` / `normalize_name`）。
//   而 `text → fs` 的行为依赖已经存在（`text/extract.cpp` include `fs/overlay.h`）⇒
//   `fs → text` 会成环。所以**原语下沉**，而不是让 fs 上调 text。
//
// 本阶段（a）**只搬家、不改语义**：调用点先切到 `ascii_lower_char()`（与旧手写逐字节等价）；
// 下一阶段（b）再把"同一性判定"的站点切到 `case_fold_lower()`（会动基线，单独一次）。
//
// 表的边界（照抄原头，别让它漂）：
//   · 只做 Python `str.lower()` 的单码位/多码位映射，**不做** `casefold()` 的额外等价；
//   · KNOWN_DIVERGENCE：上下文相关的唯一成员 `U+03A3 Σ`（末位 ς vs σ），见表里的台账段；
//   · 表只覆盖到 `U+2FFFF`，更远的码位原样返回。
//   ★ 表 21.2 KB ⇒ **只在一个 .cpp 里包含**（`src/core/case_fold.cpp`），本头不包含表，
//     否则每个链接 core 的目标都会背这份放进各自 TU 的常量。

#include <cstdint>
#include <string>
#include <string_view>

namespace xrkit::core {

// ASCII 折叠（**与旧手写实现逐字节等价**：只折 A-Z，其它字节原样）。
//   `ascii_lower_char` 给"逐字符比较/就地折叠"的站点用（零分配）；
//   `case_fold_ascii` 给"整串折叠"用。
inline char ascii_lower_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string case_fold_ascii(std::string_view text);

// UTF-8 字符串的 Unicode 小写折叠（口径同 Python `str.lower()`；非法字节原样透传）。
std::string case_fold_lower(std::string_view utf8);

}  // namespace xrkit::core
