# -*- coding: utf-8 -*-
"""包内 ffmpeg 的**产物断言**（DSH 2026-11-02 ③：堵住"悄悄回滚到 83 MB"的假绿）。

为什么必须有这条：
  `check_ffmpeg_min.py` 在缺 `build/ffmpeg_min/ffmpeg.exe` 时 skip（返回 2）；
  而 `package.ps1` 找不到自建构建就**回退**到旧的全量 exe。
  ⇒ 换台没编过的机器：门禁全绿，包体积悄悄回到 175 MB。

断言：
  1. 包内 `bin\\ffmpeg.exe` 存在且 **≤ 8 MB**（这是"自建 minimal 真被用上"的判据）；
  2. 包内**没有** `bin\\ffprobe.exe`（P-798）；
  3. `third_party\\ffmpeg\\{LICENSE.txt, BUILD.txt}` 在（LGPL 合规件），
     且 BUILD.txt 里写明了版本与 `--disable-ffprobe`。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from package_select import resolve_current_package  # noqa: E402

MAX_MB = 8.0
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 包内 ffmpeg 断言（体积 / 无 ffprobe / 合规件）==")
    pkg, _marker, _index, reason = resolve_current_package()
    print("   [选包] %s ⇒ %s" % (reason, pkg if pkg is not None else "<无>"))
    if pkg is None:
        fail("找不到当前包（package_select 返回空）")
        return 1

    ff = pkg / "bin" / "ffmpeg.exe"
    if not ff.is_file():
        fail("包内缺 bin\\ffmpeg.exe（视频页会直接不可用）")
    else:
        mb = ff.stat().st_size / 1048576
        if mb <= MAX_MB:
            ok("bin\\ffmpeg.exe = %.2f MB（≤ %.0f MB）" % (mb, MAX_MB))
        else:
            fail("bin\\ffmpeg.exe = %.2f MB > %.0f MB —— 打包回退到了全量 exe？"
                 % (mb, MAX_MB))

    for banned in ("download_ffmpeg.py",):
        if (pkg / "bin" / banned).exists() or (pkg / banned).exists():
            fail("包内出现 %s（自动安装已退役）" % banned)
        else:
            ok("包内无 %s" % banned)

    if (pkg / "bin" / "ffprobe.exe").exists():
        fail("包内出现 bin\\ffprobe.exe（P-798 已定为不随包）")
    else:
        ok("包内无 ffprobe.exe")

    lic = pkg / "third_party" / "ffmpeg" / "LICENSE.txt"
    build = pkg / "third_party" / "ffmpeg" / "BUILD.txt"
    if lic.is_file() and "LESSER GENERAL PUBLIC LICENSE" in lic.read_text(
            encoding="utf-8", errors="replace"):
        ok("third_party\\ffmpeg\\LICENSE.txt（LGPL 全文）")
    else:
        fail("缺 LGPL 许可全文 third_party\\ffmpeg\\LICENSE.txt")
    if build.is_file():
        text = build.read_text(encoding="utf-8", errors="replace")
        if "n7.1.5" in text and "--disable-ffprobe" in text:
            ok("third_party\\ffmpeg\\BUILD.txt（版本 + configure 行）")
        else:
            fail("BUILD.txt 没写清版本/configure 行（许可说明不合格）")
    else:
        fail("缺 third_party\\ffmpeg\\BUILD.txt")

    if PROBLEMS:
        print("PKG-FFMPEG: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PKG-FFMPEG: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
