#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""字库**栅格一致性**锁（M1-7 第①步）：我们的 FreeType 栅格 vs Pillow 的 `getmask(mode="L")`。

为什么这是 M1-7 的第一道锁：1.x 的字库产物是 **Pillow 12.3.0**（捆绑 FreeType **2.14.3**）做的，
与本工程 vendored 的 FreeType 版本一致。只要栅格能逐字节复现，S=1 档（11/13/15，**不做重采样**）
的产物就有希望逐字节一致；栅格都对不上就没必要往下写渲染管线。

实测结论（2026-09-24，字号 11/17/25 各 58 个字符）：
  · **墨迹像素逐个一致**（连"末行全零"这种边界也一致）；
  · Pillow 的 mask 画布口径 = **宽 = advance(像素)、高 = max(墨迹行数, bitmap_top)**，
    墨迹落在 `(bitmap_left, 0)`；
  · `mask.getbbox()` 会裁掉**首尾全零的行/列** —— 所以 1.x 注释里的"设计内缩 mb[0]"
    就是 `bitmap_left`（对全角标点恒为 0 的 `font.getbbox` 不是同一回事）。

★ 2026-09-27 扩样（Hana 第二次审计 §四-1 的收尾）：**只测 msyh 是这条判据的盲区** —— msyh 没有
  内嵌位图 strike，所以"我们读不了 1bpp MONO 位图"这个大坑在门禁里**一直没被覆盖**（真机实测
  simsun@12 直接空位图）。现在默认把**系统字体里带 strike 的 simsun/simhei** 也纳入：
  **存在才跑、不存在显式 note 跳过**（不因环境差异判红；SKIP 必须可见）。

    python tests/golden_font_raster.py                 # 默认字体 + 系统字体（存在才跑）
    python tests/golden_font_raster.py --size 11       # 只跑默认字体的某个字号
    python tests/golden_font_raster.py --font <路径>   # 只跑指定字体（单目标模式）
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

from PIL import ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_raster"

DEFAULT_FONT = Path(r"E:\Software\Games\STALKER\Localization\Tools\otherTools"
                    r"\CN_Pack_Generator\msyh.ttf")
SIZES = (11, 13, 15, 17)  # 覆盖 S=1 与 x2 两档

# 系统字体里**带内嵌位图 strike** 的那几个（字号取实测命中的 strike 尺寸）
SYSTEM_FONTS = (
    ("simsun", Path(r"C:\Windows\Fonts\simsun.ttc"), (12, 16)),
    ("simhei", Path(r"C:\Windows\Fonts\simhei.ttf"), (16,)),
)


def read_container(path: Path):
    data = path.read_bytes()
    magic, size, count = struct.unpack_from("<III", data, 0)
    if magic != 0x504C5947:
        raise RuntimeError("容器 magic 不符：%s" % hex(magic))
    pos = 12
    out = []
    for _ in range(count):
        cp, left, top, adv, asc, desc, h, w, pitch, ln = struct.unpack_from(
            "<Iiiiiiiiii", data, pos)
        pos += 40
        out.append({"cp": cp, "left": left, "top": top, "adv": adv, "asc": asc, "desc": desc,
                    "h": h, "w": w, "pitch": pitch, "data": data[pos:pos + ln]})
        pos += ln
    return size, out


def pillow_mask(font: ImageFont.FreeTypeFont, ch: str):
    mask = font.getmask(ch, mode="L")
    w, h = mask.size
    raw = bytes(mask)
    stride = len(raw) // h if h else 0
    tight = b"".join(raw[r * stride:r * stride + w] for r in range(h)) if h else b""
    return w, h, tight, mask.getbbox()


def compare_font(font_path: Path, sizes, failures: int) -> tuple[int, int]:
    """逐字号比一个字体，返回 (compared_sizes, failures)。"""
    compared = 0
    for size in sizes:
        dump = WORK / ("ours_%s_%d.bin" % (font_path.stem, size))
        proc = subprocess.run([str(CLI), "--glyph-dump", str(font_path), str(size), str(dump)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            print("  ✗ [%s] 字号 %d：CLI 失败 %s"
                  % (font_path.name, size, proc.stdout.decode("utf-8", "replace")[:160]))
            failures += 1
            continue
        _, ours = read_container(dump)
        font = ImageFont.truetype(str(font_path), size)
        good = 0
        bad = []
        for g in ours:
            ch = chr(g["cp"])
            w, h, tight, mb = pillow_mask(font, ch)
            # ① 画布口径：宽 = advance 像素；高 = max(墨迹行数, bitmap_top)
            want_w = g["adv"] // 64
            want_h = max(g["h"], g["top"])
            if (w, h) != (want_w, want_h):
                bad.append("%r 画布口径：Pillow %dx%d / 规则 %dx%d"
                           % (ch, w, h, want_w, want_h))
                continue
            # ② 位图内容：把我们的 tight 墨迹按 (bitmap_left, 0) 贴进画布，逐字节比
            canvas = bytearray(w * h)
            for r in range(min(g["h"], h)):
                for c in range(g["w"]):
                    x = g["left"] + c
                    if 0 <= x < w:
                        canvas[r * w + x] = g["data"][r * g["w"] + c]
            if bytes(canvas) != tight:
                n = min(len(canvas), len(tight))
                first = next((i for i in range(n) if canvas[i] != tight[i]), -1)
                bad.append("%r 位图不同（首个不同 @%d：我们 %s / Pillow %s）"
                           % (ch, first, canvas[first] if first >= 0 else "-",
                              tight[first] if first >= 0 else "-"))
                continue
            good += 1
        if bad:
            failures += 1
            print("  ✗ [%s] 字号 %d：%d/%d 一致" % (font_path.name, size, good, len(ours)))
            for item in bad[:6]:
                print("      %s" % item)
        else:
            print("  ✓ [%s] 字号 %d：%d 个字符的栅格与 Pillow 逐字节一致"
                  % (font_path.name, size, good))
        compared += 1
    return compared, failures


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="字库栅格一致性锁")
    parser.add_argument("--font", default="")
    parser.add_argument("--size", type=int, default=0, help="只跑某个字号")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-RASTER: SKIP (cli-missing)")
        return 2

    WORK.mkdir(parents=True, exist_ok=True)
    failures = 0
    compared = 0
    single_mode = bool(args.font) or bool(args.size)

    targets = []
    main_font = Path(args.font) if args.font else DEFAULT_FONT
    main_sizes = (args.size,) if args.size else SIZES
    targets.append((main_font, main_sizes))
    if not single_mode:
        for label, path, sizes in SYSTEM_FONTS:
            if path.is_file():
                targets.append((path, sizes))
            else:
                print("  note %s：系统无此字体（%s）⇒ 跳过（带内嵌位图 strike 的字体是 §四-1 的暴露面）"
                      % (label, path))

    for font_path, sizes in targets:
        if not font_path.is_file():
            print("!! 找不到字体 %s" % font_path)
            if font_path == main_font:
                print("GOLDEN-FONT-RASTER: SKIP (font-missing)")
                return 2
            continue
        c, failures = compare_font(font_path, sizes, failures)
        compared += c

    print("")
    print("GOLDEN-FONT-RASTER: %s (sizes=%d failed=%d)"
          % ("FAIL" if failures else "PASS", compared, failures))
    return 1 if failures else (0 if compared else 2)


if __name__ == "__main__":
    sys.exit(main())
