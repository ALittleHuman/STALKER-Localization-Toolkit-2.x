#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字库**渲染产物**锁（M1-7 第④⑤步）：2.x 的 `ui_font_NN.dds` + `.ini` vs 1.x `render_font`。

判据（用户 C3 口径）：**实现方式可以改变，生成产物要保持原样** ⇒ 同字符集 + 同字体 +
同字号 + 同格式，两份产物**逐字节一致**。

对拍方式：本脚本**直接调用 1.x 的 `font_pack.render_font` + `write_dds`** 生成基准，
再跑我们的 `xrkit_cli --font-render`，逐字节比 `.dds` 与 `.ini`。两端拿的是**同一个字符集
文件**（每行一个十六进制码位，顺序即槽位顺序）。

本步覆盖 **S=1 档（11/13/15）** 与 **x2 档（16/17/18，走 Pillow 的 LANCZOS）** ——
LANCZOS 那一步另有**孤立**对拍（门禁 [20]，`tests/golden_font_resample.py`）。

    python tests/golden_font_render.py [--size 11] [--font <路径>]
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_render"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
FONT_PACK_PY = TOOLKIT_1X / "font_pack" / "font_pack.py"
DEFAULT_FONT = Path(r"E:\Software\Games\STALKER\Localization\Tools\otherTools"
                    r"\CN_Pack_Generator\msyh.ttf")
# 原版工具成品的字符集（3428 个码位、"整套汉化"量级）—— 用来压大画布路径（1024→2048）
BIG_INI = Path(r"E:\Software\Games\STALKER\Localization\Tools\Tool\汉化包生成工具2024"
               r"\gamedata\textures\ui\ui_font_11_chs.ini")

# S=1 档（11/13/15，不经重采样）与 x2 档（16/17/18，走 Pillow 的 LANCZOS）
S1_SIZES = (11, 13, 15)
X2_SIZES = (16, 17, 18)
REF_LINE = re.compile(r"^(\d{5})=")

# ── ★★ 2026-10-10（P17-3，审计 02 F-01 High）：**带内嵌位图 strike 的字体**必须单独对拍 ────
#   为什么：命中内嵌位图 strike 的条件是"渲染 ppem 落在 strike 窗口内"（本机 simsun 实测
#   **12..17**），而 x2 档的 ppem = fitted×2 ≥ 22 ⇒ **恰好躲开它**。所以这一组必须走
#   **x2=0**，且**必须包含 11**。
#
#   修前事实（实测，见交接区批报告）：`render.cpp::raster_glyph` 的拷贝分支只认
#   `FT_PIXEL_MODE_GRAY`，而 FreeType 的 `FT_LOAD_DEFAULT` 对带 strike 的字体会先试
#   `FT_LOAD_SBITS_ONLY`（ftobjs.c「try to load embedded bitmaps if available」）⇒ simsun
#   在 12..17 加载出来是 1bpp **MONO** ⇒ 画布全 0 ⇒ 整字被判"缺墨迹"：
#   **ini 里槽位框照写、DDS 上空白**（实测 S=1 的 13/15 档 97.2% 槽位全空）。
#   同一个致盲还让 `fit_render_size` 把 MONO 候选当成缺字形**提前 break** ⇒ 定标也错
#   （simsun@11：1.x fitted=17 / 我们=11）⇒ 11 必须在内（只锁"空白"锁不住"定标"）。
#
#   "存在才跑"（与门禁 [17] `golden_font_raster.py` 的 SYSTEM_FONTS 同款）：缺件逐条打
#   `!!` note；**一个都不在 ⇒ 退 2**（前提缺失，release 档零容忍），不许静默变成 0 条判据。
SYSTEM_FONT_CASES = (
    (Path(r"C:\Windows\Fonts\simsun.ttc"), (11, 12, 13, 15, 16, 17)),
    (Path(r"C:\Windows\Fonts\simhei.ttf"), (16,)),
)


def load_reference():
    """按文件路径装载 1.x 的 `font_pack`（同名命名空间包会挡住 `import font_pack`）。"""
    spec = importlib.util.spec_from_file_location("font_pack_1x", FONT_PACK_PY)
    if spec is None or spec.loader is None:
        raise RuntimeError("装载失败：%s" % FONT_PACK_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def small_charset(fp) -> list:
    """小而"拧"的字符集：必需字符 + 定标样本 + 各条边界路径。"""
    cps = {ord(ch) for ch in fp.REQUIRED_CHARS}
    cps |= {ord(ch) for ch in fp.FIT_SAMPLE}
    cps |= {0x00AD, 0x2116, 0x2014, 0x2016,          # 宽度表 0 / 墨迹比框宽 / 强制全角
            0x0483, 0x0484, 0x0485, 0x0486,          # 表值 1px 而墨迹 4–9px（clamp 路径）
            0xFF01, 0xFF08, 0xFF09, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF5E,
            0x3001, 0x3002, 0x300C, 0x300D, 0x3010, 0x3011,
            0x00B7, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
            0x4E2D, 0x6587, 0x5B57, 0x5E93, 0x6D4B, 0x8BD5,   # 中文字库测试
            0xE000, 0xF8FF}                          # 私用区：走 .notdef（实测 Pillow 画方框）
    return sorted(cps)


def big_charset() -> list:
    """原版成品 ini 的槽位顺序（3428 个码位）。"""
    if not BIG_INI.is_file():
        return []
    out = []
    for line in BIG_INI.read_text(encoding="utf-8").splitlines():
        m = REF_LINE.match(line)
        if m:
            out.append(int(m.group(1)))
    return out


def write_charset(path: Path, cps: list) -> None:
    path.write_bytes(("".join("%X\n" % cp for cp in cps)).encode("ascii"))


def sha8(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:12]


def first_diff(a: bytes, b: bytes):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return -1 if len(a) == len(b) else n


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="字库渲染产物一致性锁（S=1 档）")
    parser.add_argument("--font", default=str(DEFAULT_FONT))
    parser.add_argument("--size", type=int, default=0, help="只跑某个字号")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-RENDER: SKIP (cli-missing)")
        return 2
    if not FONT_PACK_PY.is_file():
        print("!! 找不到 1.x 的 %s" % FONT_PACK_PY)
        print("GOLDEN-FONT-RENDER: SKIP (toolkit-1x-missing)")
        return 2
    font_path = Path(args.font)
    if not font_path.is_file():
        print("!! 找不到字体 %s" % font_path)
        print("GOLDEN-FONT-RENDER: SKIP (font-missing)")
        return 2

    fp = load_reference()
    WORK.mkdir(parents=True, exist_ok=True)

    small = small_charset(fp)
    big = big_charset()
    cases = []  # (名字, 码位表, 字号, x2, fmt, 字体)
    sizes = (args.size,) if args.size else (S1_SIZES + X2_SIZES)
    for size in sizes:
        for fmt in ("8888", "A8"):
            cases.append(("小字符集", small, size, size in X2_SIZES, fmt, font_path))
    if not args.size and big:
        cases.append(("原版成品字符集", big, 11, False, "8888", font_path))
        cases.append(("原版成品字符集", big, 16, True, "A8", font_path))

    # ★★ P17-3：带内嵌位图 strike 的系统字体（**x2=0**，见文件头那一节）
    strike_ran = 0
    missing_fonts = []
    for strike_font, strike_sizes in SYSTEM_FONT_CASES:
        if not strike_font.is_file():
            missing_fonts.append(str(strike_font))
            print("  !! 缺件：%s ⇒ 该字体的内嵌位图 strike 覆盖**未跑**" % strike_font)
            continue
        for size in strike_sizes:
            for fmt in ("8888", "A8"):
                cases.append(("内嵌位图 strike", small, size, False, fmt, strike_font))
                strike_ran += 1

    failures = 0
    for name, cps, size, x2, fmt, case_font in cases:
        tag = "%s[%s] size=%d x2=%d fmt=%s chars=%d" % (
            name, case_font.stem, size, 1 if x2 else 0, fmt, len(cps))
        chars_file = WORK / ("chars_%d.txt" % len(cps))
        write_charset(chars_file, cps)
        ours_prefix = WORK / ("ours_%s_%d_%s" % (case_font.stem, size, fmt))
        exp_dds = WORK / ("exp_%s_%d_%s.dds" % (case_font.stem, size, fmt))
        exp_ini = WORK / ("exp_%s_%d_%s.ini" % (case_font.stem, size, fmt))

        chars_str = "".join(chr(cp) for cp in cps)
        # ── 1.x 基准（调它的真函数，不是重写一遍逻辑）
        data, ini, w, h, max_y = fp.render_font(chars_str, str(case_font), size, x2, fmt)
        fp.write_dds(str(exp_dds), w, h, fmt, data)
        with open(exp_ini, "w", encoding="ascii") as f:
            f.write(ini)

        # ── 我们
        # ★ P17-30（04 F-06）：两个产物都已存在即拒 ⇒ 本步显式 `--force`（语义就是"要写产物"）
        proc = subprocess.run([str(CLI), "--font-render", str(case_font), str(size),
                               "1" if x2 else "0", fmt, str(chars_file), str(ours_prefix),
                               "--force"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = proc.stdout.decode("utf-8", "replace").strip()
        if proc.returncode != 0:
            failures += 1
            print("  ✗ %s：CLI 退出码 %d —— %s" % (tag, proc.returncode, out[:200]))
            continue

        got_dds = Path(str(ours_prefix) + ".dds").read_bytes()
        got_ini = Path(str(ours_prefix) + ".ini").read_bytes()
        want_dds = exp_dds.read_bytes()
        want_ini = exp_ini.read_bytes()

        # ★ P17-3：**定标字号**也要直接对拍（比"DDS 第几个字节不同"精确得多）。
        #   1.x `fit_render_size(font, size, x2)` 是它的定标真值；我们的 CLI 在 `FONTRENDER ok`
        #   那行里自报 `fitted=`。这条能单独抓住"MONO 候选被当成缺字形 ⇒ 提前 break"那个症状
        #   （simsun@11 修前 11 vs 1.x 的 17，而 DDS 只是"不空但对不上"）。
        want_fitted = fp.fit_render_size(str(case_font), size, x2)
        m_fitted = re.search(r"\bfitted=(\d+)", out)
        if m_fitted is None:
            failures += 1
            print("  ✗ %s：CLI 没自报 `fitted=`（拿不到定标字号，无法对拍）—— %s"
                  % (tag, out[:160]))
            continue
        got_fitted = int(m_fitted.group(1))
        if got_fitted != want_fitted:
            failures += 1
            print("  ✗ %s：定标字号不一致（1.x fit_render_size=%d / 我们 fitted=%d）"
                  % (tag, want_fitted, got_fitted))
            continue

        if got_dds == want_dds and got_ini == want_ini:
            print("  ✓ %s：DDS %d B / ini %d B 与 1.x 逐字节一致（tex %dx%d, max_y %d, sha %s）"
                  % (tag, len(want_dds), len(want_ini), w, h, max_y, sha8(want_dds)))
            continue
        failures += 1
        print("  ✗ %s：不一致（tex %dx%d, max_y %d）" % (tag, w, h, max_y))
        if got_dds != want_dds:
            off = first_diff(got_dds, want_dds)
            print("      DDS：我们 %d B / 1.x %d B，首个不同 @%s"
                  % (len(got_dds), len(want_dds), off if off >= 0 else "长度不同"))
        if got_ini != want_ini:
            off = first_diff(got_ini, want_ini)
            ctx = ""
            if off >= 0:
                ctx = " 我们 %r / 1.x %r" % (got_ini[max(0, off - 20):off + 20],
                                             want_ini[max(0, off - 20):off + 20])
            print("      ini：我们 %d B / 1.x %d B，首个不同 @%s%s"
                  % (len(got_ini), len(want_ini), off if off >= 0 else "长度不同", ctx))
        print("      CLI: %s" % out[:200])

    print("")
    print("GOLDEN-FONT-RENDER: %s (cases=%d failed=%d strike_cases=%d)"
          % ("FAIL" if failures else "PASS", len(cases), failures, strike_ran))
    if missing_fonts:
        print("  !! 未覆盖的内嵌位图 strike 夹具（缺件）：%s" % "、".join(missing_fonts))
    if strike_ran == 0:
        # ★ P17-3：一个 strike 夹具都没有 ⇒ **不许当通过**（否则这条锁会静默变成 0 条判据）
        print("  !! 带内嵌位图 strike 的系统字体一个都没找到 ⇒ 本组判据未覆盖")
        return 2
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
