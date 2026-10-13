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


def render_v2() -> bytes:
    """最终形态：区间表 ＋ 多字符例外表（两个平行数组，都是 plain C 数组）。"""
    singles, multis = collect()
    runs = compress(singles)
    out = []
    out.append("// ★★ 本文件**由 `tools/gen_case_fold_table.py` 生成，不要手改**。")
    out.append("// 来源：本机 CPython 的 `str.lower()`（Unicode %s）—— 与 1.x `str(path).lower()` **同源**。"
               % _unicode_version())
    out.append("// 生成：python tools/gen_case_fold_table.py")
    out.append("// 校验：python tools/gen_case_fold_table.py --check（门禁 `case-fold-table` 步会真跑）")
    out.append("//")
    out.append("// 为什么不用 Windows `LCMapStringEx` 模仿 Python（实测，0x20..0x2FFFF 全域）：")
    out.append("//   · 单字符内分歧 **131276** 个码位（如 U+01C5 ǅ / U+0524 Ԥ / U+03F4 ϴ）；")
    out.append("//   · 折成多字符的分歧：U+0130 İ（Python 给 \"i\"+U+0307）。")
    out.append("//   ⇒ 根因是两边各自一套 Unicode 版本/口径 ⇒ 改为**从 CPython 导出**（本仓已有先例：")
    out.append("//     `tools/gen_encoding_tables.py` 从本机 CPython 导出编码表）。")
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
