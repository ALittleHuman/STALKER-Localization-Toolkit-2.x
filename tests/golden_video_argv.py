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
    # ★★ P17-101（审计 `02 F-09`）：**告警位的期望值来自基线的"输入事实"**，
    #   不是反推 argv、更不是照我们的实现写 —— 1.x 的判据是 `if src_info is None:`
    #   （`video_ogm_app.py:310`，只看探针成败、**不看有没有参考**）⇒
    #   期望 `probe_warn` = `0 if src_parsed else 1`。
    want_src_parsed = {row["case"]: bool(row.get("src_parsed", True)) for row in baseline["cases"]}

    proc = subprocess.run([str(CLI), "--video-plan"], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print("!! --video-plan 退出码 %d：%s"
              % (proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
        print("GOLDEN-VIDEO-ARGV: FAIL (cli-error)")
        return 1

    got = {}
    summaries = {}
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("PLAN\t"):
            _, name, joined = line.split("\t", 2)
            got[name] = joined.split("\x1f")
        elif line.startswith("PLANSUM\t"):
            _, name, text = line.split("\t", 2)
            summaries[name] = text

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

    # ── PLANSUM：界面「视频参数」面板读的那几个值的判据 ─────────────────────
    # 期望值**从 1.x 录下来的 argv 反推**（哪个开关真在命令行里 ⇒ 摘要该报什么），
    # 不是我照实现手写的 —— 否则"面板说的"和"真正交给 ffmpeg 的"能一起错。
    def summary_from_argv(name, argv):
        def value_after(flag):
            if flag in argv:
                index = argv.index(flag)
                if index + 1 < len(argv):
                    return argv[index + 1]
            return None

        vf = value_after("-vf") or ""
        encoder = value_after("-c:v") or ""
        video_bitrate = value_after("-b:v") or "0"
        audio_bitrate = value_after("-b:a") or "0"
        sample_rate = value_after("-ar") or "0"
        channels = value_after("-ac") or "0"
        # ★★ P17-101（审计 `02 F-09`）：`probe_warn` 的期望值**从基线的输入事实算**：
        #   1.x 是 `if src_info is None:` 那一支必告警 ⇒ 期望 = "这一格的源探针失败了吗"。
        #   （不反推 argv、不查我们的用例表 —— 那两种都是"拿自己当期望"。）
        probe_warn_expected = 0 if want_src_parsed.get(name, True) else 1
        return ("%s|scale=%d|fps=%d|vbr=%s|abr=%s|ar=%s|ac=%s|an=%d|probe_warn=%d"
                % (encoder, 1 if "scale=" in vf else 0, 1 if "fps=" in vf else 0,
                   video_bitrate, audio_bitrate, sample_rate, channels,
                   1 if "-an" in argv else 0, probe_warn_expected))

    for name, argv in want.items():
        if name not in summaries:
            print("[%s] 我们的 CLI 没输出 PLANSUM" % name)
            failed += 1
            continue
        expected = summary_from_argv(name, argv)
        if summaries[name] != expected:
            failed += 1
            print("[%s] PLANSUM 与 1.x argv 反推的不一致" % name)
            print("   1.x argv 反推: %s" % expected)
            print("   我们:          %s" % summaries[name])
        else:
            print("[%s] PLANSUM ok %s" % (name, summaries[name]))

    # ★★ P17-101（审计 `02 F-09`）：把那一格**单独点名**判一次。
    #   修前 `transcode.cpp:200-201` 是 `= src.parsed == false && request.has_reference`
    #   ⇒「无参考 + 探针失败」时告警位是 **0**（1.x 在 `src_info is None` 那一支**必告警**）
    #   ⇒ 这一条会红，而它就是审计那条判定的**唯一考点**。
    probe_case = "no_reference_and_probe_failed"
    if probe_case in summaries:
        if "probe_warn=1" not in summaries[probe_case]:
            failed += 1
            print("[%s] ★ 无参考 + 探针失败时**必须置告警位**（1.x 在 `src_info is None` 那一支必告警）；"
                  "实测 %s" % (probe_case, summaries[probe_case]))
        else:
            print("[%s] ★ 探针失败告警位 = 1（判据 02 F-09 的考点）" % probe_case)
    else:
        failed += 1
        print("[%s] ★ 基线里缺这一格（`probe_video_argv.py` 的用例表与 CLI 的用例表要对齐）"
              % probe_case)

    verdict = "FAIL" if failed else "PASS"
    print("")
    print("GOLDEN-VIDEO-ARGV: %s (compared=%d failed=%d)" % (verdict, compared, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
