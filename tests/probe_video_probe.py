#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""录视频解析判据的夹具：**真的** `ffprobe` / `ffmpeg -encoders` 输出字节。

为什么要有这个脚本（而不是手写一段 JSON）：判据要拿**真输出**去对拍。手写样本只能
证明"我按我印象里的字段解析对了"，证明不了"真 ffprobe 的字节我能解析对"。

产物（入 `tests/golden/video_probe/`）：
  · `ffprobe_in_avi.json` —— `ffprobe -v quiet -print_format json -show_format -show_streams`
    对一个 1 秒 testsrc+sine 的 AVI 的输出（与 smoke 端到端造输入的那条命令同源）；
  · `ffmpeg_encoders.txt`  —— `ffmpeg -encoders` 的整段输出。

ffmpeg/ffprobe 的定位顺序（**不搜 PATH**，与产品口径一致）：
  ① `--ffmpeg <dir>`；② 环境变量 `XRKIT_FFMPEG_DIR`；③ 仓库同级的 1.x 工具目录。

用法：python tests/probe_video_probe.py [--write] [--ffmpeg <目录>]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT_DIR = HERE / "golden" / "video_probe"
REF_TOOLKIT = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")


def find_deps(explicit: str | None) -> tuple[Path, Path]:
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get("XRKIT_FFMPEG_DIR"):
        candidates.append(Path(os.environ["XRKIT_FFMPEG_DIR"]))
    candidates.append(REF_TOOLKIT)
    candidates.append(ROOT / "third_party" / "ffmpeg")
    for candidate in candidates:
        ffmpeg = candidate / "ffmpeg.exe"
        ffprobe = candidate / "ffprobe.exe"
        if ffmpeg.is_file() and ffprobe.is_file():
            return ffmpeg, ffprobe
    raise SystemExit("找不到 ffmpeg.exe / ffprobe.exe（用 --ffmpeg <目录> 指定；不搜 PATH）")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true", help="写夹具到 tests/golden/video_probe/")
    parser.add_argument("--ffmpeg", default=None, help="ffmpeg.exe 所在目录")
    args = parser.parse_args()

    ffmpeg, ffprobe = find_deps(args.ffmpeg)
    print("ffmpeg : %s" % ffmpeg)
    print("ffprobe: %s" % ffprobe)

    work = ROOT / "build" / "video_probe_tmp"
    work.mkdir(parents=True, exist_ok=True)
    sample = work / "in.avi"
    # 与 smoke 端到端造输入的那条命令同一个配方（1 秒 testsrc + 440Hz sine）
    gen = subprocess.run(
        [str(ffmpeg), "-y", "-f", "lavfi", "-i", "testsrc=d=1", "-f", "lavfi", "-i",
         "sine=f=440:d=1", "-shortest", str(sample)],
        capture_output=True, timeout=300,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if gen.returncode != 0 or not sample.is_file():
        print("!! 造样本失败：%s" % gen.stderr.decode("utf-8", "replace")[-400:])
        return 1

    probe = subprocess.run(
        [str(ffprobe), "-v", "quiet", "-print_format", "json", "-show_format", "-show_streams",
         str(sample)],
        capture_output=True, timeout=120,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if probe.returncode != 0:
        print("!! ffprobe 失败：%s" % probe.stderr.decode("utf-8", "replace")[-400:])
        return 1
    json_text = probe.stdout.decode("utf-8", "replace")

    enc = subprocess.run([str(ffmpeg), "-encoders"], capture_output=True, timeout=120,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if enc.returncode != 0:
        print("!! ffmpeg -encoders 失败")
        return 1
    enc_text = enc.stdout.decode("utf-8", "replace")

    print("ffprobe json  %d 字节 / 流 %d 个" % (len(json_text), json_text.count('"index"')))
    print("encoders      %d 行" % len(enc_text.splitlines()))
    if args.write:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        (OUT_DIR / "ffprobe_in_avi.json").write_text(json_text, encoding="utf-8", newline="")
        (OUT_DIR / "ffmpeg_encoders.txt").write_text(enc_text, encoding="utf-8", newline="")
        print("已写夹具 → %s" % OUT_DIR.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
