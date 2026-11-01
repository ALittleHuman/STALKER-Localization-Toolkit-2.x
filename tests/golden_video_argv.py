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

    # ★★ P-782：**有意分歧**（1.x 的死路，2.0 明令不同）。基线是 1.x 实调产物，
    #   不改它；在这里登记"2.0 的期望"，并说明理由。判据照样逐段比。
    #   · mpeg4_ref_no_bitrate_src_noaudio：1.x 输出 `-c:v libxvid`，而基线里这条
    #     `ok=false`、`message="输出文件不存在或为空"` —— **1.x 当场产不出文件**（不是"可能不可播"）；
    #     2.0 只出 Theora（P-782）。
    #   · last_resort_only_mpeg4：只有 mpeg4、没有 libtheora ⇒ 2.0 **硬失败**（argv 为空）。
    DIVERGENT_ARGV = {
        "mpeg4_ref_no_bitrate_src_noaudio": [
            "FFMPEG", "-y", "-i", "IN.avi", "-c:v", "libtheora", "-q:v", "7", "-pix_fmt",
            "yuv420p", "-g", "30", "-an", "-f", "ogg", "OUT.ogm",
        ],
        # 硬失败：argv 为空（PLAN 行第二格为空 ⇒ 切成 []）
        "last_resort_only_mpeg4": [],
    }
    DIVERGENT_SUMMARY = {
        # 与上面 argv 一致（面板要报"真的写进命令行"的值）
        "mpeg4_ref_no_bitrate_src_noaudio": "libtheora|scale=0|fps=0|vbr=0|abr=0|ar=0|ac=0|an=1|probe_warn=0|enc_warn=1|err=0",
        # 硬失败 ⇒ argv 清空、页面显示 plan.error；但**面板值保留**（"本会怎么写"，
        #   便于解释原因）⇒ 码率/音频这些值仍是引擎算出来的
        "last_resort_only_mpeg4": "|scale=0|fps=0|vbr=800000|abr=128000|ar=44100|ac=2|an=0|probe_warn=0|enc_warn=0|err=1",
    }
    for name, argv in DIVERGENT_ARGV.items():
        want[name] = argv
    # ★★ P17-101（审计 `02 F-09`）：**告警位的期望值来自基线的"输入事实"**，
    #   不是反推 argv、更不是照我们的实现写 —— 1.x 的判据是 `if src_info is None:`
    #   （`video_ogm_app.py:310`，只看探针成败、**不看有没有参考**）⇒
    #   期望 `probe_warn` = `0 if src_parsed else 1`。
    want_src_parsed = {row["case"]: bool(row.get("src_parsed", True)) for row in baseline["cases"]}
    want_ref_codec = {row["case"]: row.get("ref_codec") for row in baseline["cases"]}

    proc = subprocess.run([str(CLI), "--video-plan"], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        print("!! --video-plan 退出码 %d：%s"
              % (proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
        print("GOLDEN-VIDEO-ARGV: FAIL (cli-error)")
        return 1

    got = {}
    summaries = {}
    errors = {}
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("PLAN\t"):
            _, name, joined = line.split("\t", 2)
            # P-782：硬失败时 PLAN 第二格为空 ⇒ 是 []，不是 [""]
            got[name] = joined.split("\x1f") if joined else []
        elif line.startswith("PLANSUM\t"):
            _, name, text = line.split("\t", 2)
            summaries[name] = text
        elif line.startswith("PLANERR\t"):
            _, name, reason = line.split("\t", 2)
            errors[name] = reason

    # ★ P-786：硬失败**必须**有理由行；非 error 用例**不许**有理由行。
    #   只比"argv 为空"不够 —— 别的早退路径也可能让 argv 空（那就是假绿）。
    EXPECT_ERROR = {"last_resort_only_mpeg4"}

    compared = 0
    failed = 0
    for name in want:
        has_err = name in errors
        if name in EXPECT_ERROR:
            if not has_err:
                failed += 1
                print("[%s] ★ 期望硬失败（PLANERR），但没输出理由行" % name)
            elif "libtheora" not in errors[name]:
                failed += 1
                print("[%s] ★ PLANERR 理由没提 libtheora（无法归因）：%s" % (name, errors[name]))
        elif has_err:
            failed += 1
            print("[%s] ★ 非 error 用例却输出了 PLANERR：%s" % (name, errors[name]))

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
        failed += 1   # ★ P-783：基线里没有的用例 ⇒ 基线过期或夹具漂了，必须红
        print("[★] 我们多输出了基线里没有的用例 %s（基线过期？）" % extra)

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
        # ★ P-787：enc_warn 的期望从**基线记的 ref_codec** 现算（不是查我们的实现）。
        # ★ P-785：err=1 的用例里面板值仅供参考；判据只断言 err 位本身。
        ref_codec = want_ref_codec.get(name)
        if ref_codec is None:
            raise SystemExit("基线过期：缺 ref_codec（重跑 tests/probe_video_argv.py --write）")
        # ★ P-787/P-789：`enc_warn` 是 **2.0 的新定义**（参考属于 1.x `CODEC_MAP` 会走 libxvid
        #   的那一族：mpeg4/xvid/msmpeg4v3，而 2.0 恒出 Theora）。**1.x 没有这个位**，
        #   所以期望不可能从 1.x argv 反推（它那条 fallback 用例自己就选了 theora ⇒ 反推得 0，
        #   与 2.0 定义冲突）⇒ 只能用"基线 ref_codec + 2.0 定义"算。
        #   ※ 这份三元组与 transcode.cpp 的 kMap 是**同源两份**（P-789 已记；加新 codec 两处同改）。
        ENC_WARN_CASES = {"mpeg4_ref_no_bitrate_src_noaudio", "mpeg4_ref_fallback_noxvid"}
        expected_from_codec = ref_codec in ("mpeg4", "xvid", "msmpeg4v3")
        if expected_from_codec != (name in ENC_WARN_CASES):
            raise SystemExit("[%s] 基线 ref_codec=%r 与告警集合不一致（基线或规则漂了）"
                             % (name, ref_codec))
        enc_warn_expected = 1 if name in ENC_WARN_CASES else 0
        err_expected = 1 if name in EXPECT_ERROR else 0
        return ("%s|scale=%d|fps=%d|vbr=%s|abr=%s|ar=%s|ac=%s|an=%d|probe_warn=%d"
                "|enc_warn=%d|err=%d"
                % (encoder, 1 if "scale=" in vf else 0, 1 if "fps=" in vf else 0,
                   video_bitrate, audio_bitrate, sample_rate, channels,
                   1 if "-an" in argv else 0, probe_warn_expected,
                   enc_warn_expected, err_expected))

    for name, argv in want.items():
        if name not in summaries:
            print("[%s] 我们的 CLI 没输出 PLANSUM" % name)
            failed += 1
            continue
        expected = DIVERGENT_SUMMARY.get(name) or summary_from_argv(name, argv)
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
