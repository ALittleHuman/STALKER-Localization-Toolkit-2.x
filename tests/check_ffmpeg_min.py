# -*- coding: utf-8 -*-
"""自建 minimal ffmpeg 的验收（P-798 之后体积的主解）。

只验 `build/ffmpeg_min/ffmpeg.exe`（staging），**不碰** dist 里的 exe。
缺这个文件时 skip（返回 2）—— 它是构建产物，不该让没编过的人红。
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIN = ROOT / "build" / "ffmpeg_min" / "ffmpeg.exe"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
OLD = ROOT / "dist" / "2.0.0" / "BETA" / "84" / "bin" / "ffmpeg.exe"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(exe, args, timeout=120):
    p = subprocess.run([str(exe)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def main() -> int:
    print("== minimal ffmpeg 验收（staging）==")
    if not MIN.is_file():
        print("FFMPEG-MIN skip 缺 build/ffmpeg_min/ffmpeg.exe（先跑 scripts/build_ffmpeg_min.sh）")
        return 2
    size_mb = MIN.stat().st_size / 1048576
    if 3.0 <= size_mb <= 25.0:
        ok("体积 %.2f MB（3–25 MB）" % size_mb)
    else:
        fail("体积 %.2f MB 不在 3–25 MB 区间" % size_mb)

    rc, out, err = run(MIN, ["-hide_banner", "-version"])
    text = out + err
    if rc == 0 and "ffmpeg version 7." in text:
        ok("`-version` = 7.x（%s）" % text.splitlines()[0].strip())
    else:
        fail("`-version` 不是 7.x：%s" % text.splitlines()[:1])

    # 编码器：走引擎的 parse_encoder_list（经插件 headless），不 grep 关键字
    enc_txt = ROOT / "build" / "logs" / "_ffmin_encoders.txt"
    rc2, out2, err2 = run(MIN, ["-hide_banner", "-encoders"])
    enc_txt.parent.mkdir(parents=True, exist_ok=True)
    enc_txt.write_text(out2, encoding="utf-8")
    if CLI.is_file():
        payload = json.dumps({"mode": "encoders", "text_path": str(enc_txt)}, ensure_ascii=False)
        p = subprocess.run([str(CLI), "--py-plugin-call", str(ROOT / "plugins"),
                            "--plugin", "video_page.py", payload],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        got = p.stdout.decode("utf-8", "replace")
        names = []
        for line in got.splitlines():
            if " result=" not in line:
                continue
            try:
                payload_json = json.loads(line.split(" result=", 1)[1])
            except ValueError:
                continue
            for item in payload_json.get("lines", []):
                if item.startswith("ENC\t"):
                    names.append(item.split("\t", 1)[1].strip())
        if "libtheora" in names and "libvorbis" in names:
            ok("parse_encoder_list 含 libtheora + libvorbis（%d 个）" % len(names))
        else:
            fail("parse_encoder_list 缺 theora/vorbis：%s" % names[:10])
    else:
        fail("缺 CLI，无法用 parse_encoder_list 验收")

    for flag, needle in (("-muxers", "ogg"), ("-demuxers", "matroska"), ("-decoders", "theora")):
        rc3, out3, err3 = run(MIN, ["-hide_banner", flag])
        if needle in (out3 + err3):
            ok("%s 含 %s" % (flag, needle))
        else:
            fail("%s 缺 %s" % (flag, needle))

    # 空目录独立运行（防 libwinpthread-1.dll 之类）
    with tempfile.TemporaryDirectory() as td:
        solo = Path(td) / "ffmpeg.exe"
        shutil.copy2(MIN, solo)
        rc4, out4, err4 = run(solo, ["-hide_banner", "-version"])
        if rc4 == 0 and "ffmpeg version 7." in (out4 + err4):
            ok("空目录独立运行（无同目录 DLL）")
        else:
            fail("空目录跑不起来（缺 DLL？）：rc=%d" % rc4)

    # 真转：用旧全量生成输入，再用 minimal 转 OGM，并用引擎解析读回
    if OLD.is_file():
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "in.mp4"
            rc5, _, _ = run(OLD, ["-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                                  "testsrc=size=320x240:rate=25", "-f", "lavfi", "-i",
                                  "sine=frequency=440", "-t", "1", "-c:v", "mpeg4", "-c:a", "aac",
                                  str(src)])
            out_ogm = Path(td) / "out.ogm"
            rc6, _, err6 = run(MIN, ["-hide_banner", "-y", "-i", str(src),
                                     "-c:v", "libtheora", "-q:v", "7", "-pix_fmt", "yuv420p",
                                     "-g", "30", "-c:a", "libvorbis", "-b:a", "128k",
                                     "-f", "ogg", str(out_ogm)])
            if rc5 == 0 and rc6 == 0 and out_ogm.is_file() and out_ogm.stat().st_size > 0:
                probe = Path(td) / "probe.txt"
                run(MIN, ["-hide_banner", "-nostdin", "-i", str(out_ogm)])
                # 单独再跑一次拿原始 stderr 落文件
                with open(probe, "wb") as fh:
                    subprocess.run([str(MIN), "-hide_banner", "-nostdin", "-i", str(out_ogm)],
                                   stdout=subprocess.DEVNULL, stderr=fh)
                if CLI.is_file():
                    p = subprocess.run([str(CLI), "--video-probe-text", str(probe), "30000"],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    got = p.stdout.decode("utf-8", "replace")
                    if "parsed=1" in got and "codec=theora" in got and "acodec=vorbis" in got:
                        ok("MP4→OGM 真转并用引擎读回 theora/vorbis")
                    else:
                        fail("读回不对：%s" % got.strip()[:160])
                else:
                    fail("缺 CLI，读回判据跳过")
            else:
                fail("MP4→OGM 真转失败 rc=%d size=%s" % (rc6, out_ogm.stat().st_size if out_ogm.exists() else 0))
    else:
        print("  · 缺旧全量 ffmpeg（BETA.84），真转判据跳过（信息）")

    if PROBLEMS:
        print("FFMPEG-MIN: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FFMPEG-MIN: PASS（%.2f MB）" % size_mb)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
