#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字库**几何/宽度**锁（M1-7 第③步）：2.x 的 C++ 查表与裁决 vs 1.x 的 `font_pack`。

判据：与 1.x **同输入产物逐字节一致**。而产物里的每个矩形都出自 `font_pack` 的那几张表与
四条裁决函数，所以先把这一层钉死 —— 这层错了（推进宽差 1px），整张图集的槽位会一路挪位，
到成品 DDS 上表现为"整片都对不上"，很难定位。

对拍方式：C++ 侧 `--font-metrics` 把全部查询按**固定扫描顺序**导出，本脚本用 1.x 的
`font_pack` 逐条重算。裁决用例的**输入**由 C++ 侧生成、随 dump 一起给出，本脚本只当裁判
⇒ 两端没有第二份需要同步的用例表。

参考实现的导入方式说明：1.x 的 `font_pack/` 是个没有 `__init__.py` 的目录、里面才叫
`font_pack.py`，`import font_pack` 会先命中命名空间包（同名冲突）⇒ 这里用 importlib
按**文件路径**装载（`_load_adv_tables` 用 `__file__` 定位 JSON，照常可用）。

    python tests/golden_font_metrics.py
"""

from __future__ import annotations

import argparse
import importlib.util
import struct
import subprocess
import sys
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_metrics"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
FONT_PACK_PY = TOOLKIT_1X / "font_pack" / "font_pack.py"

MAX_REPORT = 6  # 每节最多打印几条差异（不然一个错表能刷出几万行）


def load_reference():
    """按文件路径装载 1.x 的 `font_pack`（见模块 docstring 的命名冲突说明）。"""
    spec = importlib.util.spec_from_file_location("font_pack_1x", FONT_PACK_PY)
    if spec is None or spec.loader is None:
        raise RuntimeError("装载失败：%s" % FONT_PACK_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Dump:
    """解析 `xrkit_cli --font-metrics` 的导出（格式与 metrics.cpp 顶部注释一致）。"""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def i32(self) -> int:
        v = struct.unpack_from("<i", self.data, self.pos)[0]
        self.pos += 4
        return v

    def u8s(self, n: int) -> bytes:
        v = self.data[self.pos:self.pos + n]
        self.pos += n
        return v

    def i32s(self, n: int) -> tuple:
        v = struct.unpack_from("<%di" % n, self.data, self.pos)
        self.pos += 4 * n
        return v

    def u32s(self, n: int) -> tuple:
        v = struct.unpack_from("<%dI" % n, self.data, self.pos)
        self.pos += 4 * n
        return v


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="字库几何/宽度一致性锁")
    parser.add_argument("--dump", default="", help="复用已有的 dump（默认现场跑 CLI）")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-METRICS: SKIP (cli-missing)")
        return 2
    if not FONT_PACK_PY.is_file():
        print("!! 找不到 1.x 的 %s" % FONT_PACK_PY)
        print("GOLDEN-FONT-METRICS: SKIP (toolkit-1x-missing)")
        return 2

    fp = load_reference()

    WORK.mkdir(parents=True, exist_ok=True)
    dump_path = Path(args.dump) if args.dump else (WORK / "ours.bin")
    if not args.dump:
        proc = subprocess.run([str(CLI), "--font-metrics", str(dump_path)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            print("!! CLI 失败：%s" % proc.stdout.decode("utf-8", "replace")[:200])
            print("GOLDEN-FONT-METRICS: FAIL (cli)")
            return 1
    if not dump_path.is_file():
        print("!! 没有 dump：%s" % dump_path)
        print("GOLDEN-FONT-METRICS: FAIL (no-dump)")
        return 1

    d = Dump(dump_path.read_bytes())
    failures = 0

    def report(name: str, checked: int, bad: list) -> None:
        nonlocal failures
        if bad:
            failures += 1
            print("  ✗ %s：%d 条里 %d 条不符" % (name, checked, len(bad)))
            for item in bad[:MAX_REPORT]:
                print("      %s" % item)
        else:
            print("  ✓ %s：%d 条全部一致" % (name, checked))

    # ── ① 槽位表 ──────────────────────────────────────────────────────
    magic = d.u32()
    if magic != 0x54454D46:
        print("!! dump magic 不符：%s" % hex(magic))
        print("GOLDEN-FONT-METRICS: FAIL (magic)")
        return 1
    n_slots = d.u32()
    slots = [(d.u32(), d.u32(), d.u32()) for _ in range(n_slots)]
    bad = []
    for i, (size, cell, block) in enumerate(slots):
        want_size = fp.SIZES[i][0]
        if size != want_size:
            bad.append("第 %d 档：size %d / 1.x %d" % (i, size, want_size))
            continue
        if cell != fp.CELL_HEIGHTS[size]:
            bad.append("size %d：cell %d / 1.x %d" % (size, cell, fp.CELL_HEIGHTS[size]))
        if block != fp.BLOCK_WIDTHS[size]:
            bad.append("size %d：block %d / 1.x %d" % (size, block, fp.BLOCK_WIDTHS[size]))
    report("槽位表（cell 高 / 方块宽）", n_slots * 3, bad)

    # ── ② 表外字号的 cell 兜底 ────────────────────────────────────────
    n_rsize = d.u32()
    rsize_vals = d.u32s(n_rsize)
    bad = []
    for i, got in enumerate(rsize_vals):
        rsize = i + 1
        want = fp.cell_height_for(rsize)
        if got != want:
            bad.append("渲染字号 %d：%d / 1.x %d" % (rsize, got, want))
    report("cell_height_for（含表外 1.40 比例兜底）", n_rsize, bad)

    # ── ③ 方块字符判定（全码位空间）────────────────────────────────────
    n_block = d.u32()
    block_flags = d.u8s(n_block)
    forced = fp.FORCED_FULLWIDTH
    bad = []
    for cp in range(n_block):
        ch = chr(cp)
        want = unicodedata.east_asian_width(ch) in "FW" or ch in forced
        if bool(block_flags[cp]) != want:
            bad.append("U+%04X：%d / 1.x %d（EAW=%s）"
                       % (cp, block_flags[cp], want, unicodedata.east_asian_width(ch)))
            if len(bad) > 40:
                break
    report("is_block_char（EAW∈{W,F} ∪ FORCED_FULLWIDTH）", n_block, bad)

    # ── ④ 标点分类（BMP）─────────────────────────────────────────────
    ORDER = {None: 0, "open": 1, "close": 2, "single": 3, "wide": 4}
    n_bmp = d.u32()
    punct_vals = d.u8s(n_slots * n_bmp)
    bad = []
    for i in range(n_slots):
        base = i * n_bmp
        for cp in range(n_bmp):
            want = ORDER[fp.punct_class(chr(cp))]
            got = punct_vals[base + cp]
            if got != want:
                bad.append("size %d U+%04X：%d / 1.x %d" % (slots[i][0], cp, got, want))
                if len(bad) > 40:
                    break
        if len(bad) > 40:
            break
    report("punct_class（四类标点）", n_slots * n_bmp, bad)

    # ── ⑤ 推进宽（每档 × 全 BMP）─────────────────────────────────────
    n_rect_slots = d.u32()
    assert n_rect_slots == n_slots, "rect 段槽位数与槽位表不符"
    rect_vals = d.i32s(n_rect_slots * n_bmp)
    bad = []
    for i in range(n_rect_slots):
        size = slots[i][0]
        base = i * n_bmp
        for cp in range(n_bmp):
            want = fp.rect_w_for(size, chr(cp))
            want = -1 if want is None else want
            got = rect_vals[base + cp]
            if got != want:
                bad.append("size %d U+%04X：%d / 1.x %d" % (size, cp, got, want))
                if len(bad) > 40:
                    break
        if len(bad) > 40:
            break
    report("rect_w_for（adv 表 → 方块宽 → -1）", n_rect_slots * n_bmp, bad)

    # ── ⑥ 简→繁单字表 ───────────────────────────────────────────────
    n_st = d.u32()
    st_vals = d.u32s(n_st)
    bad = []
    for cp in range(n_st):
        alt = fp.ST_FALLBACK.get(chr(cp))
        want = ord(alt) if alt else 0
        if st_vals[cp] != want:
            bad.append("U+%04X：%s / 1.x %r" % (cp, hex(st_vals[cp]), alt))
            if len(bad) > 40:
                break
    report("st_fallback（简→繁单字）", n_st, bad)

    # ── ⑦ 框宽裁决全流程（输入由 C++ 侧给出，这里只当裁判）───────────
    n_case = d.u32()
    bad = []
    exercised = 0
    for _ in range(n_case):
        cp = d.u32()
        raw_rw = d.i32()
        left0 = d.i32()
        gw = d.i32()
        cell_h = d.i32()
        got_pl = d.i32()
        got_rw = d.i32()
        got_ink = d.i32()

        ch = chr(cp)
        pl, rw = fp.punct_box_px(ch, raw_rw, left0, gw)
        rw = fp.clamp_box_to_ink(pl, rw, gw)
        if rw > cell_h:
            rw = cell_h
        ink = fp.ink_draw_w(gw, rw, pl)
        if (got_pl, got_rw, got_ink) != (pl, rw, ink):
            bad.append("U+%04X raw=%d left=%d gw=%d cell=%d：我们 (%d,%d,%d) / 1.x (%d,%d,%d)"
                       % (cp, raw_rw, left0, gw, cell_h, got_pl, got_rw, got_ink, pl, rw, ink))
            if len(bad) > 40:
                break
        exercised += 1
    report("框宽裁决（punct_box_px→clamp→封顶→ink_draw_w）", exercised, bad)

    print("")
    print("GOLDEN-FONT-METRICS: %s" % ("FAIL" if failures else "PASS"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
