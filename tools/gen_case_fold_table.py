#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P17-112（复审计 `01 F-10`）：**从本机 CPython 导出 Unicode 小写映射表**。

为什么要有它（修前事实，实测）
------------------------------
1.x 的路径排序键是 `str(path).lower()` —— **Python 的 Unicode 小写映射**。
2.x 原来用 `ascii_lower`（只折 A-Z）；P17-44 改成 **Windows `LCMapStringEx`**
（`LOCALE_NAME_INVARIANT` ＋ `LCMAP_LOWERCASE`），当时的实测结论是
"与 Python 的 simple lowercase 对西里尔/拉丁/希腊一致"。

★★ 本批把那个结论**放大到全域复核**（`0x20..0x2FFFF`）后不成立：
   · 单字符内分歧：**131276** 个码位（如 `U+01C5 ǅ` Python 折成 `ǆ`、Windows 保持 `ǅ`；
     `U+0524 Ԥ` Python 折成 `ԥ`、Windows 保持 `Ԥ`；`U+03F4 ϴ` → `θ` vs 不动）；
   · **折成多字符**的分歧：`U+0130 İ` → Python `"i" + U+0307`（两码位）、Windows 保持 `İ`。
   ⇒ 也就是说"用 Windows API 去模仿 Python"这条路**本来就跟不上**（Unicode 版本也不同步），
     而这正是本仓已有的那类处置：**从本机 CPython 导出表**（见 `tools/gen_encoding_tables.py`
     的先例：编码表由本机 CPython 的同名 codec 导出，`--check` 是门禁）。

产出的表口径
------------
只记**值不等于自身**的码位（`chr(cp).lower() != chr(cp)`），按码位升序：
  · `SINGLE`：结果是**单个**字符 ⇒ `(cp, lower_cp)`；
  · `MULTI` ：结果是**多个**字符 ⇒ `(cp, "折叠后的 UTF-8 字节")`（只有 `U+0130` 这种极少数）。
用游程压缩成区间：同一段"连续码位、结果也连续"的合并成一条。

用法：
    python tools/gen_case_fold_table.py             # 生成（写 include/xrkit/engine/text/case_fold_table.inc）
    python tools/gen_case_fold_table.py --check     # 只校验：生成结果与磁盘上那份**逐字节**比（门禁用）
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "include" / "xrkit" / "engine" / "text" / "case_fold_table.inc"
MAX_CP = 0x30000


def collect() -> tuple:
    """返回 (singles, multis)：singles = [(cp, low_cp)]；multis = [(cp, str)]。"""
    singles, multis = [], []
    for cp in range(0x00, MAX_CP):
        if 0xD800 <= cp <= 0xDFFF:          # 代理区：不是合法码位
            continue
        ch = chr(cp)
        low = ch.lower()
        if low == ch:
            continue
        if len(low) == 1:
            singles.append((cp, ord(low)))
        else:
            multis.append((cp, low))
    return singles, multis


def compress(singles: list) -> list:
    """把 (cp, low) 里"连续上升且两侧都连续"的段压成 (cp0, low0, count)。"""
    runs = []
    for cp, low in singles:
        if runs and runs[-1][0] + runs[-1][2] == cp and runs[-1][1] + runs[-1][2] == low:
            cp0, low0, n = runs[-1]
            runs[-1] = (cp0, low0, n + 1)
        else:
            runs.append((cp, low, 1))
    return runs


def _unicode_version() -> str:
    import unicodedata
    return unicodedata.unidata_version


def context_sensitive_codepoints() -> list:
    """★ P17-117（用户 2026-10-13 裁定 A 的第 3 条）：**枚举"上下文相关"的小写映射码位**。

    为什么要有它：本表是"**逐码位**"映射（一个码位 → 一个结果），而 Python 的 `str.lower()`
    里有一类**看上下文**的规则 ⇒ 那类码位本表**必然**复现不了。要让"已知缺口"是**有界的**
    （而不是"说不清的疑点"），就得把这一类**当场数出来** —— 所以放进生成器：
    每次重跑生成器都会连同表一起重新量一遍（"界"跟着数据走，不是一句手写的断言）。

    做法（**行为穷举**，不依赖 `SpecialCasing.txt` 的本地副本 —— 本机 `unicodedata` 不暴露它、
    也找不到该文件）：对每个码位，把它放进**几种不同邻域**里折小写，只看**它自己**被折成什么；
    结果不止一种 ⇒ 这个小写**依赖上下文**。

    ★ 实测结论（Unicode 16.0.0，CPython 3.14）：**只有 `U+03A3 Σ` 一个**（`ς` / `σ`，
      即 `Final_Sigma` 规则）⇒ 本表的缺口**有界且只有一条**。
    """
    def lower_in_context(before: str, ch: str, after: str):
        got = (before + ch + after).lower()
        led, tail = before.lower(), after.lower()
        if not got.startswith(led) or not got.endswith(tail) or len(got) < len(led) + len(tail):
            return None                      # 邻域自身也变了 ⇒ 这一格不能用
        return got[len(led):len(got) - len(tail)]

    out = []
    for cp in range(0x00, MAX_CP):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        ch = chr(cp)
        variants = set()
        for before, after in (("Α", "Α"), ("Α", "."), ("a", "a"), ("a", "."), ("", "a"), ("a", "")):
            got = lower_in_context(before, ch, after)
            if got is not None:
                variants.add(got)
        if len(variants) > 1:
            out.append((cp, ch, sorted(variants)))
    return out


def render_v2() -> bytes:
    """最终形态：区间表 ＋ 多字符例外表（两个平行数组，都是 plain C 数组）。"""
    singles, multis = collect()
    runs = compress(singles)
    ctx = context_sensitive_codepoints()
    out = []
    out.append("// ★★ 本文件**由 `tools/gen_case_fold_table.py` 生成，不要手改**。")
    out.append("// 来源：本机 CPython 的 `str.lower()`（Unicode %s）。"
               % _unicode_version())
    # ★ P17-117（用户 2026-10-13 裁定 A 的第 1 条）：**"同源"要说准** ——
    #   原来这里写"与 1.x `str(path).lower()` **同源**"，那把话说满了：本表是**逐码位**映射，
    #   而复现不了 `str.lower()` 里**看上下文**的那一类（见下面的已知分歧）。
    out.append("// 口径：**逐码位**映射，与 1.x `str(path).lower()` 同源 ——"
               "**除上下文相关规则外**（详见下面的 KNOWN_DIVERGENCE）。")
    out.append("// 生成：python tools/gen_case_fold_table.py")
    out.append("// 校验：python tools/gen_case_fold_table.py --check（门禁 `case-fold-table` 步会真跑）")
    out.append("//")
    out.append("// 为什么不用 Windows `LCMapStringEx` 模仿 Python（实测，0x20..0x2FFFF 全域）：")
    out.append("//   · 单字符内分歧 **131276** 个码位（如 U+01C5 ǅ / U+0524 Ԥ / U+03F4 ϴ）；")
    out.append("//   · 折成多字符的分歧：U+0130 İ（Python 给 \"i\"+U+0307）。")
    out.append("//   ⇒ 根因是两边各自一套 Unicode 版本/口径 ⇒ 改为**从 CPython 导出**（本仓已有先例：")
    out.append("//     `tools/gen_encoding_tables.py` 从本机 CPython 导出编码表）。")
    out.append("//")
    # ★ P17-117（裁定 A 的第 2/3 条）：**把它钉成台账项**（带症状）＋**当场数清楚这一类有几条**。
    out.append("// ══ KNOWN_DIVERGENCE（已知分歧台账，★ 生成器每次重跑都会重新量一遍）══════")
    out.append("// 类别：**上下文相关的小写映射**（`str.lower()` 里看邻域的那一类）。")
    out.append("// 本表是逐码位的 ⇒ 这一类**必然**复现不了；下面是**穷举**出来的全部成员：")
    if ctx:
        for cp, ch, variants in ctx:
            out.append("//   · U+%04X %s → 随上下文 %s（本表恒取 %s）"
                       % (cp, ch, "／".join(repr(v) for v in variants), repr(ch.lower())))
    else:
        out.append("//   · （本次穷举**一个都没有** —— 那本表就无此类缺口）")
    out.append("// 症状（可复现）：`'ΟΔΟΣ'.lower()` 得 `'οδος'`（末位是 **ς**），而本表恒给 **σ**。")
    out.append("// 界（有界声明）：**小写这条规则下，上下文相关的码位就上面这几个** ——")
    out.append("//   实测 %d 个（Unicode %s，CPython 3.14）；**不是「可能还有别的」**。"
               % (len(ctx), _unicode_version()))
    out.append("// 触发条件（★ 到这一步就该升 B，别再靠人想起来）：")
    out.append("//   **当语料出现希腊文，或与 1.x 对拍时出现 `ς`** ⇒ 本缺口会真实影响排序键 ⇒")
    out.append("//   那时再引入上下文感知（需 `Cased`/`Case_Ignorable` 一套新表 ＋ 一条对账判据）。")
    out.append("//   在触发之前**不补**：素材面是西里尔＋中文，为一个不存在的场景铸通用件不划算。")
    out.append("//   （用户 2026-10-13 裁定：**投 A** —— 记为有界已知缺口，不做 B。）")
    out.append("// ══════════════════════════════════════════════════════════════════════")
    out.append("#pragma once")
    out.append("")
    out.append("// (first_cp, lower_first_cp, count)：count 个连续码位映射到同样连续的小写段")
    out.append("static const unsigned int kCaseFoldRuns[][3] = {")
    for cp0, low0, n in runs:
        out.append("    {0x%04Xu, 0x%04Xu, %du}," % (cp0, low0, n))
    out.append("};")
    out.append("static const unsigned int kCaseFoldRunCount = %du;" % len(runs))
    out.append("")
    out.append("// 折成**多字符**的例外（码位 → UTF-8 字节）：count=%d" % len(multis))
    out.append("static const unsigned int kCaseFoldMultiCp[] = {")
    out.append("    " + ", ".join("0x%04Xu" % cp for cp, _ in multis) if multis else "    0u")
    out.append("};")
    out.append("static const char* const kCaseFoldMultiUtf8[] = {")
    for _, low in multis:
        out.append('    "%s",' % "".join("\\x%02X" % b for b in low.encode("utf-8")))
    if not multis:
        out.append('    "",')
    out.append("};")
    out.append("static const unsigned int kCaseFoldMultiCount = %du;" % len(multis))
    out.append("")
    return ("\n".join(out) + "\n").encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="导出 Unicode 小写映射表（跟 CPython 走）")
    parser.add_argument("--check", action="store_true", help="只校验：与磁盘上那份逐字节比")
    args = parser.parse_args()

    produced = render_v2()
    if args.check:
        if not TARGET.is_file():
            print("!! 表不存在：%s" % TARGET)
            return 1
        on_disk = TARGET.read_bytes()
        if on_disk == produced:
            print("CASE-FOLD-TABLE ok 表与 CPython 导出结果逐字节一致（%d 字节）" % len(on_disk))
            return 0
        a = on_disk.decode("utf-8", "replace").splitlines()
        b = produced.decode("utf-8", "replace").splitlines()
        for i in range(max(len(a), len(b))):
            x = a[i] if i < len(a) else "(缺)"
            y = b[i] if i < len(b) else "(缺)"
            if x != y:
                print("CASE-FOLD-TABLE fail 首个差异 行%d：库里「%s」/ 生成「%s」"
                      % (i + 1, x[:90], y[:90]))
                break
        return 1

    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_bytes(produced)          # ★ bytes + LF（与 gen_encoding_tables.py 同口径）
    print("已写 %s（%d 字节）" % (TARGET.relative_to(ROOT), len(produced)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
