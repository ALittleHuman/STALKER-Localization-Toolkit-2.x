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

// ★★ H3(a)（2026-10-25）：**实现已下沉到 `xrkit/core/case_fold.{h,cpp}`**。
//   原因：`text → fs` 的行为依赖已存在（`text/extract.cpp` include `fs/overlay.h`），
//   而 fs 侧也要判"同一性" ⇒ `fs → text` 会成环。表 21.2 KB 现在只在
//   `src/core/case_fold.cpp` 里包含一次（不再每个 TU 背一份）。
//   本头保留旧名字（`engine::text::case_fold_lower`），**行为不变**；新代码请直接用 `core`。

#include <string>
#include <string_view>

#include "xrkit/core/case_fold.h"

namespace xrkit::engine::text {

// 兼容入口：老调用点（`text/extract.cpp` 的 `path_lower`）继续用这个名字。
inline std::string case_fold_lower(const std::string& utf8) {
    return xrkit::core::case_fold_lower(utf8);
}

// ★★ P17-112：`path_lower()` 是 `src/engine/text/extract.cpp` 里**排序键**用的那一个
//   （定义在 extract.cpp；判据直接考它，不许退回 ASCII）。
std::string path_lower(const std::string& s);

}  // namespace xrkit::engine::text
