# -*- coding: utf-8 -*-
"""L2 等价性：`ffmpeg -i` stderr 解析 ⇄ ffprobe JSON 解析（同一组录制样本）。

样本在 `tests/golden/video_probe_text/`：每组同时有 `.ffprobe.json`（事实基准）与
`.ffmpeg_i.txt`（降级路径的输入）。

判据分两类：
  · 数值白名单（实测收敛）：fps ≤0.01 / 流级 bit_rate ≤1000 / duration ≤0.02；
  · **结构判据**（不吃容差）：`-b:v` 有没有必须一致 —— 视频流码率"缺失"与"0"语义不同；
    缺失时 plan_transcode 走 `-q:v 7`，有码率时走 `-b:v`，这是 argv 的结构性差异。

诚实标注：这是**解析层**等价，不含 `plan_transcode` 的决策位（fps=/vbr=）对比 ——
那一步在页面接线后由 argv golden 覆盖（nofprobe 组）。
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
SAMPLES = HERE / "golden" / "video_probe_text"
MANIFEST = SAMPLES / "manifest.json"

FPS_TOL = 0.01
BITRATE_TOL = 1000
DUR_TOL = 0.02


def frac(text: str) -> float:
    try:
        num, den = text.split("/")
        den = float(den)
        return float(num) / den if den else 0.0
    except Exception:  # noqa: BLE001
        return 0.0


def expected_from_json(obj: dict) -> dict:
    streams = obj.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
    out = {"has_video": video is not None, "has_audio": audio is not None}
    if video:
        avg = frac(video.get("avg_frame_rate") or "0/0")
        r = frac(video.get("r_frame_rate") or "0/0")
        out.update({
            "codec": video.get("codec_name") or "",
            "w": int(video.get("width") or 0),
            "h": int(video.get("height") or 0),
            "fps": avg if avg > 0 else r,
            "vbr": int(video.get("bit_rate") or 0),
            "has_vbr": video.get("bit_rate") is not None,
        })
    if audio:
        out.update({
            "acodec": audio.get("codec_name") or "",
            "sr": int(audio.get("sample_rate") or 0),
            "ch": int(audio.get("channels") or 0),
            "abr": int(audio.get("bit_rate") or 0),
            "has_abr": audio.get("bit_rate") is not None,
        })
    out["dur"] = float(obj.get("format", {}).get("duration") or 0.0)
    return out


def parse_output(line: str) -> dict:
    fields = {}
    for token in line.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.is_file():
        print("VIDEO-PROBE-TEXT skip 缺 CLI 构建产物")
        return 2
    if not MANIFEST.is_file():
        print("VIDEO-PROBE-TEXT skip 缺 manifest")
        return 2

    cases = json.loads(MANIFEST.read_text(encoding="utf-8"))["cases"]
    problems = []
    compared = 0
    for row in cases:
        name = row["case"]
        stderr_file = SAMPLES / (name + ".ffmpeg_i.txt")
        json_file = SAMPLES / (name + ".ffprobe.json")
        if not stderr_file.is_file() or not json_file.is_file():
            problems.append("%s：缺样本文件" % name)
            continue
        expected = expected_from_json(json.loads(json_file.read_text(encoding="utf-8")))
        proc = subprocess.run([str(CLI), "--video-probe-text", str(stderr_file), "1048576"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = proc.stdout.decode("utf-8", errors="replace")
        line = next((ln for ln in text.splitlines() if ln.startswith("PROBETEXT")), "")
        got = parse_output(line) if line else {}
        if not got:
            problems.append("%s：CLI 没输出 PROBETEXT（rc=%d）" % (name, proc.returncode))
            continue
        compared += 1
        checks = []
        # 结构判据：先看"有没有"
        checks.append(("parsed", got.get("parsed") == "1"))
        checks.append(("has_video", got.get("has_video") == ("1" if expected["has_video"] else "0")))
        checks.append(("has_audio", got.get("has_audio") == ("1" if expected["has_audio"] else "0")))
        if expected["has_video"]:
            checks.append(("codec", got.get("codec") == expected["codec"]))
            checks.append(("w", int(got.get("w", 0)) == expected["w"]))
            checks.append(("h", int(got.get("h", 0)) == expected["h"]))
            # ★ 结构：码率"缺失"与"0"不是一回事（缺 ⇒ -q:v 7；有 ⇒ -b:v）
            checks.append(("has_vbr", got.get("has_vbr") == ("1" if expected["has_vbr"] else "0")))
            if expected["has_vbr"]:
                checks.append(("vbr≈", abs(int(got.get("vbr", 0)) - expected["vbr"]) <= BITRATE_TOL))
            # ★ VFR：avg 与 r 不同时，必须取 avg（否则 tbr 陷阱）
            checks.append(("fps≈", abs(float(got.get("fps", 0)) - expected["fps"]) <= FPS_TOL))
        if expected["has_audio"]:
            checks.append(("acodec", got.get("acodec") == expected["acodec"]))
            checks.append(("sr", int(got.get("sr", 0)) == expected["sr"]))
            checks.append(("ch", int(got.get("ch", 0)) == expected["ch"]))
            if expected.get("has_abr"):
                checks.append(("abr≈", abs(int(got.get("abr", 0)) - expected["abr"]) <= BITRATE_TOL))
        checks.append(("dur≈", abs(float(got.get("dur", 0)) - expected["dur"]) <= DUR_TOL))

        failed = [label for label, ok in checks if not ok]
        if failed:
            problems.append("%s：%s 不一致（got=%s）" % (name, ",".join(failed), line))
            print("  ✗ %s" % name)
        else:
            print("  ✓ %s（fps=%s has_vbr=%s dur=%s）"
                  % (name, got.get("fps"), got.get("has_vbr"), got.get("dur")))

    # VFR 专项：avg≠r 的样本必须证明没被 tbr 抢走
    vfr = "vfr_h264_concat_30_10"
    if (SAMPLES / (vfr + ".ffprobe.json")).is_file():
        obj = json.loads((SAMPLES / (vfr + ".ffprobe.json")).read_text(encoding="utf-8"))
        video = next(s for s in obj["streams"] if s.get("codec_type") == "video")
        if frac(video["avg_frame_rate"]) == frac(video["r_frame_rate"]):
            problems.append("%s：样本本身 avg==r，测不到 tbr 陷阱（重新录）" % vfr)
        else:
            print("  ✓ VFR 样本 avg=%s r=%s（足够触发 tbr 陷阱）"
                  % (video["avg_frame_rate"], video["r_frame_rate"]))

    if problems:
        print("\nVIDEO-PROBE-TEXT: FAIL（%d 处）" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("\nVIDEO-PROBE-TEXT: PASS（compared=%d）" % compared)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
