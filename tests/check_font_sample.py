# -*- coding: utf-8 -*-
"""文本样张（`--font-sample`）判据：预览的**唯一一份**光栅口径。

口径（P3 预览；用户 2026-10-26）：
  · 插件没有光栅能力 ⇒ 预览必须走引擎（与字库产物同一个 `raster_glyph`）；
  · 输出 PPM(P6)（Qt `QPixmap::load` 原生支持；不引 PNG/zlib 依赖）；
  · **缺字只按"字体里没有这个码位"算**（`index==0`）；空格没墨迹**不算缺**（修前算成假红）；
  · 打不开的字体 ⇒ 退 1 并说出理由（零静默）。

外部前提：系统字体 `arial.ttf`（无中文）与 `NotoSansSC-VF.ttf`（有中文）；缺 ⇒ 退 2。
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
CJK = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
LATIN = Path(r"C:\Windows\Fonts\arial.ttf")

TEXT = "你好，汉化 Привет"


def fail(msg: str) -> None:
    print("  ✗ " + msg)


def ok(msg: str) -> None:
    print("  ✓ " + msg)


def run_sample(font: Path, text: str, px: int, out: Path) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--font-sample", str(font), text, str(px), str(out)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def ppm_info(path: Path) -> tuple[int, int, bytes]:
    raw = path.read_bytes()
    assert raw.startswith(b"P6\n"), "不是 P6 PPM"
    parts = raw.split(b"\n", 3)
    width, height = (int(x) for x in parts[1].split())
    return width, height, parts[3]


def main() -> int:
    print("== 文本样张（--font-sample）：PPM + 缺字口径 + 零静默 ==")
    if not CLI.exists():
        print("!! 缺 %s" % CLI)
        return 1
    missing = [p for p in (CJK, LATIN) if not p.is_file()]
    if missing:
        print("!! 缺系统字体：%s" % ", ".join(str(p) for p in missing))
        print("FONT-SAMPLE: SKIP (fonts-missing)")
        return 2

    problems = 0
    with tempfile.TemporaryDirectory(prefix="font_sample_") as tmp:
        tmp = Path(tmp)

        # ① 有中文字体：缺字 0，PPM 宽高/像素对得上
        out = tmp / "cjk.ppm"
        rc, stdout = run_sample(CJK, TEXT, 28, out)
        if rc == 0 and "missing=0" in stdout:
            ok("有中文字体：rc=0、missing=0（空格没被算成缺字）")
        else:
            problems += 1
            fail("有中文字体结果不对：rc=%d\n%s" % (rc, stdout[-400:]))
        if out.is_file():
            w, h, pixels = ppm_info(out)
            if w > 0 and h > 0 and len(pixels) == w * h * 3:
                ok("PPM 结构合法：%dx%d、像素字节数对" % (w, h))
            else:
                problems += 1
                fail("PPM 结构不对：%dx%d len=%d" % (w, h, len(pixels)))
            ink = sum(1 for b in pixels if b > 0x20)
            if ink > 100:
                ok("画布上真有墨迹（%d 个像素不是背景）" % ink)
            else:
                problems += 1
                fail("画布几乎没有墨迹（%d）—— 字体没被用上？" % ink)
        else:
            problems += 1
            fail("没写出 PPM")

        # ② 无中文字体：中文必须计入缺字（2 个汉字）
        out2 = tmp / "latin.ppm"
        rc2, stdout2 = run_sample(LATIN, "你好", 28, out2)
        if rc2 == 0 and "missing=2" in stdout2:
            ok("无中文字体：missing=2（缺字如实入账）")
        else:
            problems += 1
            fail("无中文字体的缺字计数不对：rc=%d\n%s" % (rc2, stdout2[-300:]))

        # ②b 缺字必须**可行动**（P2-6：只给数字不合格）
        if "SAMPLE advice=" in stdout2 and "U+4F60" in stdout2:
            ok("缺字可行动：列出具体码位（U+4F60…）+ 换字体/补字形建议")
        else:
            problems += 1
            fail("缺字只给了数字，没有码位/建议：\n%s" % stdout2[-300:])

        # ③ 打不开的字体 ⇒ 退 1 且说理由（零静默）
        rc3, stdout3 = run_sample(tmp / "nope.ttf", "你好", 28, tmp / "nope.ppm")
        if rc3 == 1 and "SAMPLE error reason=" in stdout3:
            ok("打不开的字体：退 1 且给出理由")
        else:
            problems += 1
            fail("坏字体没被拒：rc=%d\n%s" % (rc3, stdout3[-300:]))

        # ④ 用量条件：字号 0/负数不许崩（走同一入口）
        rc4, stdout4 = run_sample(CJK, TEXT, 0, tmp / "zero.ppm")
        if rc4 == 1 and "SAMPLE error" in stdout4:
            ok("字号 0：退 1 且说理由（不崩）")
        else:
            problems += 1
            fail("字号 0 的行为不对：rc=%d" % rc4)

    if problems:
        print("FONT-SAMPLE: FAIL（%d 处）" % problems)
        return 1
    print("FONT-SAMPLE: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
