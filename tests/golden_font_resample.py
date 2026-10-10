#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重采样**孤立**锁（M1-7 第⑥步）：我们的 Pillow-LANCZOS 等价实现 vs `PIL.Image.resize`。

为什么单独锁这一层：x2 档（16/17/18）的产物只差这一步，而它是"看着像、实则全错"的高危点
（定点系数、边界裁剪、两趟顺序、`(int)` 与 `floor` 的差别、取整口径，差一点就整片像素不同）。
在这里逐字节对拍，比在图集产物上排障便宜一个数量级。

用例覆盖：整数倍降采样（x2 档实际用法：源 = 2×目标）、非整数缩放、放大、单行/单列、
全零、全满、随机噪声、条纹/渐变（含负瓣振铃敏感的高频图案）。

    python tests/golden_font_resample.py
"""

from __future__ import annotations

import random
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_font_resample"


def make_cases():
    """(名字, 宽, 高, 像素 bytes, 目标宽, 目标高)"""
    rng = random.Random(20260924)
    out = []

    def img(w, h, fn):
        return bytes(fn(x, y) for y in range(h) for x in range(w))

    shapes = [(1, 1), (2, 3), (7, 5), (16, 16), (21, 17), (31, 25), (40, 40), (64, 33), (100, 7)]
    for w, h in shapes:
        out.append(("噪点 %dx%d" % (w, h), w, h,
                    bytes(rng.randrange(256) for _ in range(w * h)), w, h))
        out.append(("全零 %dx%d" % (w, h), w, h, bytes(w * h), w, h))
        out.append(("全满 %dx%d" % (w, h), w, h, b"\xff" * (w * h), w, h))
        out.append(("渐变 %dx%d" % (w, h), w, h,
                    img(w, h, lambda x, y: (x * 255) // max(1, w - 1)), w, h))
        out.append(("条纹 %dx%d" % (w, h), w, h,
                    img(w, h, lambda x, y: 255 if (x + y) % 2 else 0), w, h))
        out.append(("噪声 %dx%d" % (w, h), w, h,
                    bytes(rng.randrange(256) for _ in range(w * h)), w, h))

    # 目标尺寸：整数倍降采样（x2 档的真实用法）、非整数、放大、只缩一个方向
    pairs = [
        ((40, 40), (20, 20)), ((31, 25), (15, 12)), ((64, 33), (32, 16)),
        ((100, 7), (50, 3)), ((21, 17), (10, 8)), ((16, 16), (8, 8)),
        ((33, 33), (17, 16)), ((40, 40), (13, 27)), ((16, 16), (25, 9)),
        ((7, 5), (3, 2)), ((100, 7), (7, 100)), ((64, 33), (64, 33)),
        ((40, 40), (1, 1)), ((40, 40), (39, 39)), ((2, 3), (5, 7)),
        ((255, 3), (17, 2)),
    ]
    for (w, h), (dw, dh) in pairs:
        out.append(("降采样 %dx%d→%dx%d" % (w, h, dw, dh), w, h,
                    bytes(rng.randrange(256) for _ in range(w * h)), dw, dh))
        out.append(("图案 %dx%d→%dx%d" % (w, h, dw, dh), w, h,
                    img(w, h, lambda x, y: (x * 7 + y * 13) % 256), dw, dh))
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-FONT-RESAMPLE: SKIP (cli-missing)")
        return 2

    WORK.mkdir(parents=True, exist_ok=True)
    cases = make_cases()
    failures = 0
    for name, sw, sh, px, dw, dh in cases:
        src = WORK / "src.raw"
        dst = WORK / "dst.raw"
        src.write_bytes(px)
        # ★ P17-29（04 F-06）：本步在同一路径上**循环复用** dst.raw ⇒ 必须显式 `--force`
        #   （守卫口径：输出已存在即拒，除非 --force；这一处语义本来就是"要覆盖"）。
        proc = subprocess.run([str(CLI), "--resample-dump", str(src), str(sw), str(sh),
                               str(dw), str(dh), str(dst), "--force"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            failures += 1
            print("  ✗ %s：CLI 退出码 %d —— %s"
                  % (name, proc.returncode, proc.stdout.decode("utf-8", "replace")[:160]))
            continue
        ours = dst.read_bytes()
        want = Image.frombytes("L", (sw, sh), px).resize((dw, dh), Image.LANCZOS).tobytes()
        if ours == want:
            continue
        failures += 1
        n = min(len(ours), len(want))
        off = next((i for i in range(n) if ours[i] != want[i]), -1)
        print("  ✗ %s：我们 %d B / Pillow %d B，首个不同 @%s（我们 %s / Pillow %s）"
              % (name, len(ours), len(want), off,
                 ours[off] if off >= 0 else "-", want[off] if off >= 0 else "-"))
        diff = sum(1 for a, b in zip(ours, want) if a != b)
        print("      不同像素 %d / %d" % (diff, len(want)))

    src.unlink(missing_ok=True)
    dst.unlink(missing_ok=True)
    print("")
    print("GOLDEN-FONT-RESAMPLE: %s (cases=%d failed=%d)"
          % ("FAIL" if failures else "PASS", len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
