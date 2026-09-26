#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""视频转码 argv 对拍：2.x `engine::video::plan_transcode` vs 1.x 实调录下的基线。

基线 `tests/golden/video_argv.json` 由 `tests/probe_video_argv.py --write` 生成 —— 它
**exec 1.x `convert()` 本体并实调**，所以期望值现算自 1.x 源码，不是我手写的。

退出码：0=全一致 / 1=不一致 / 2=基线或 CLI 缺失（按外部前提处理）。
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BASELINE = HERE / "golden" / "video_argv.json"


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-VIDEO-ARGV: SKIP (cli-missing)")
        return 2
    if not BASELINE.exists():
        print("!! 缺基线（先跑 python tests/probe_video_argv.py --write）")
        print("GOLDEN-VIDEO-ARGV: SKIP (baseline-missing)")
        return 2

    baseline = json.loads(BASELINE.read_text(encoding="utf-8"))
    want = {row["case"]: row["argv"] for row in baseline["cases"]}

    proc = subprocess.run([str(CLI), "--video-plan"], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print("!! --video-plan 退出码 %d：%s"
              % (proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
        print("GOLDEN-VIDEO-ARGV: FAIL (cli-error)")
        return 1

    got = {}
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if not line.startswith("PLAN\t"):
            continue
        _, name, joined = line.split("\t", 2)
        got[name] = joined.split("\x1f")

    compared = 0
    failed = 0
    for name, expected in want.items():
        if name not in got:
            print("[%s] 我们的 CLI 没有输出这个用例" % name)
            failed += 1
            continue
        compared += 1
        if got[name] != expected:
            failed += 1
            print("[%s] argv 不一致" % name)
            print("   1.x : %s" % " ".join(expected))
            print("   我们: %s" % " ".join(got[name]))
            for i, (a, b) in enumerate(zip(expected, got[name])):
                if a != b:
                    print("   首个不同 @%d：1.x=%r 我们=%r" % (i, a, b))
                    break
        else:
            print("[%s] ok %d 段" % (name, len(expected)))

    extra = sorted(set(got) - set(want))
    if extra:
        print("注意：我们多输出了 %s（基线里没有）" % extra)

    verdict = "FAIL" if failed else "PASS"
    print("")
    print("GOLDEN-VIDEO-ARGV: %s (compared=%d failed=%d)" % (verdict, compared, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
