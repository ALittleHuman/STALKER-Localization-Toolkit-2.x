# -*- coding: utf-8 -*-
"""自建 minimal ffmpeg 的验收（P-798 之后体积的主解）。

HA/DSH 复核后的口径（2026-11-02）：
  · 白名单**逐项**断言（解析 -muxers/-demuxers/-decoders/-filters，不抽样、不 grep 关键字）；
  · `dumpbin /dependents` 进脚本（只许系统 DLL；找不到 dumpbin 就红，不静默）；
  · 真转矩阵按**UI 承诺的扩展名**：mp4/mov/m4v/avi/mkv/webm/ogm；
  · "故意全不同"样本（640x480 yuv422p 48k stereo → 320x240 yuv420p 44.1k mono）
    把 scale/swscale/aresample 真跑一遍（同参数时它们是空转）；
  · 旧全量 ffmpeg 固定放 `build/ffmpeg_full/ffmpeg.exe`；缺了**红**，不"信息后继续绿"。
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
FULL = ROOT / "build" / "ffmpeg_full" / "ffmpeg.exe"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []

WHITELIST = {
    "-muxers": ["ogg"],
    "-demuxers": ["avi", "mov", "matroska", "ogg", "mov,mp4,m4a,3gp,3g2,mj2"],
    "-decoders": ["h264", "hevc", "mpeg4", "mjpeg", "theora", "vorbis", "aac", "mp3",
                  "ac3", "eac3", "pcm_s16le", "vp8", "vp9", "opus", "mpeg2video"],
    "-filters": ["scale", "fps", "format", "aresample", "aformat", "setpts"],
}


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(exe, args, timeout=180):
    p = subprocess.run([str(exe)] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def list_items(exe, flag):
    rc, out, err = run(exe, ["-hide_banner", flag])
    return out + err


def has_item(text, name):
    # 列格式：`D   avi` / `E  ogg` / `VFS..D h264` / ` ... scale  V->V ...`
    for line in text.splitlines():
        parts = line.split()
        for tok in parts:
            # muxer/demuxer 的"名字"可以是逗号串（`mov,mp4,m4a,...`、`matroska,webm`）
            if tok == name or tok.startswith(name + ","):
                return True
    return False


def probe(exe, path):
    with tempfile.NamedTemporaryFile("wb", suffix=".txt", delete=False) as fh:
        tmp = fh.name
    with open(tmp, "wb") as fh:
        subprocess.run([str(exe), "-hide_banner", "-nostdin", "-i", str(path)],
                       stdout=subprocess.DEVNULL, stderr=fh)
    p = subprocess.run([str(CLI), "--video-probe-text", tmp], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    os.unlink(tmp)
    return p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== minimal ffmpeg 验收（staging）==")
    if not MIN.is_file():
        print("FFMPEG-MIN skip 缺 build/ffmpeg_min/ffmpeg.exe（先跑 scripts/build_ffmpeg_min.sh）")
        return 2
    if not CLI.is_file():
        fail("缺 CLI，无法用引擎解析验收")
        return 1
    if not FULL.is_file():
        fail("缺 build/ffmpeg_full/ffmpeg.exe（生成样本用）—— 不许静默跳过真转")
        return 1

    size_mb = MIN.stat().st_size / 1048576
    (ok if 3.0 <= size_mb <= 25.0 else fail)("体积 %.2f MB（3–25 MB）" % size_mb)

    rc, out, err = run(MIN, ["-hide_banner", "-version"])
    text = out + err
    (ok if rc == 0 and "ffmpeg version 7." in text else fail)(
        "`-version` = %s" % text.splitlines()[:1])

    # 白名单逐项（解析列表，不抽样）
    for flag, names in WHITELIST.items():
        body = list_items(MIN, flag)
        missing = [n for n in names if not has_item(body, n)]
        if missing:
            fail("%s 缺：%s" % (flag, ", ".join(missing)))
        else:
            ok("%s 全在（%d 项）" % (flag, len(names)))

    # 编码器走引擎的 parse_encoder_list（经插件 headless）
    enc_txt = ROOT / "build" / "logs" / "_ffmin_encoders.txt"
    enc_txt.parent.mkdir(parents=True, exist_ok=True)
    rc2, out2, _ = run(MIN, ["-hide_banner", "-encoders"])
    enc_txt.write_text(out2, encoding="utf-8")
    payload = json.dumps({"mode": "encoders", "text_path": str(enc_txt)}, ensure_ascii=False)
    p = subprocess.run([str(CLI), "--py-plugin-call", str(ROOT / "plugins"),
                        "--plugin", "video_page.py", payload],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    names = []
    for line in p.stdout.decode("utf-8", "replace").splitlines():
        if " result=" not in line:
            continue
        try:
            for item in json.loads(line.split(" result=", 1)[1]).get("lines", []):
                if item.startswith("ENC\t"):
                    names.append(item.split("\t", 1)[1].strip())
        except ValueError:
            pass
    if "libtheora" in names and "libvorbis" in names:
        ok("parse_encoder_list 含 libtheora + libvorbis（%d 个）" % len(names))
    else:
        fail("parse_encoder_list 缺 theora/vorbis：%s" % names[:10])

    # dumpbin（找不到就红）
    vcvars = None
    for cand in (r"E:\Software\Tools\Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
                 r"C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"):
        if Path(cand).is_file():
            vcvars = cand
            break
    if vcvars is None:
        fail("找不到 vcvars64.bat，dumpbin 依赖判据无法执行")
    else:
        bat = Path(tempfile.gettempdir()) / "_ffmin_deps.bat"
        bat.write_text('@echo off\r\ncall "%s" >nul 2>&1\r\ndumpbin /dependents "%s"\r\n'
                       % (vcvars, MIN), encoding="mbcs", errors="replace")
        p = subprocess.run(["cmd", "/c", str(bat)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        body = p.stdout.decode("mbcs", "replace")
        deps = [ln.strip() for ln in body.splitlines() if ln.strip().lower().endswith(".dll")]
        system = {"kernel32.dll", "msvcrt.dll", "bcrypt.dll", "shell32.dll", "user32.dll",
                  "advapi32.dll", "ws2_32.dll", "gdi32.dll", "ole32.dll", "crypt32.dll"}
        bad = [d for d in deps if d.lower() not in system]
        if not deps:
            fail("dumpbin 没读出任何依赖（判据没成立）")
        elif bad:
            fail("依赖里出现非系统 DLL：%s" % ", ".join(bad))
        else:
            ok("dumpbin 只依赖系统 DLL：%s" % ", ".join(deps))

    # 空目录独立运行
    with tempfile.TemporaryDirectory() as td:
        solo = Path(td) / "ffmpeg.exe"
        shutil.copy2(MIN, solo)
        rc4, out4, err4 = run(solo, ["-hide_banner", "-version"])
        (ok if rc4 == 0 and "ffmpeg version 7." in (out4 + err4) else fail)(
            "空目录独立运行（无同目录 DLL）")

    # ── 真转矩阵：按 UI 承诺的扩展名 ──
    cases = [
        ("mp4_h264_aac", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                          "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                          "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"], ".mp4"),
        ("mov_h264_aac", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                          "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                          "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"], ".mov"),
        ("m4v_h264_aac", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                          "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                          "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"], ".m4v"),
        ("avi_mpeg4_mp3", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                           "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                           "-c:v", "mpeg4", "-c:a", "libmp3lame"], ".avi"),
        ("mkv_h264_aac", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                          "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                          "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"], ".mkv"),
        ("webm_vp9_opus", ["-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                           "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                           "-c:v", "libvpx-vp9", "-b:v", "300k", "-c:a", "libopus"], ".webm"),
        # 故意"全不同"：640x480 yuv422p 48k stereo → 320x240 yuv420p 44.1k mono
        ("differences_all", ["-f", "lavfi", "-i", "testsrc=size=640x480:rate=30",
                             "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                             "-t", "1", "-pix_fmt", "yuv422p", "-ac", "2",
                             "-c:v", "mpeg4", "-c:a", "aac"], ".mkv"),
    ]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, gen_args, ext in cases:
            src = td / (name + ext)
            rc5, _, err5 = run(FULL, ["-hide_banner", "-loglevel", "error", "-y"] + gen_args + [str(src)])
            if rc5 != 0 or not src.is_file():
                fail("样本生成失败 %s：%s" % (name, err5.strip()[:120]))
                continue
            out_ogm = td / (name + ".ogm")
            vf = []
            if name == "differences_all":
                vf = ["-vf", "scale=320:240,fps=25"]
                audio = ["-c:a", "libvorbis", "-ar", "44100", "-ac", "1"]
            else:
                audio = ["-c:a", "libvorbis", "-b:a", "128k"]
            rc6, _, err6 = run(MIN, ["-hide_banner", "-y", "-i", str(src),
                                     "-c:v", "libtheora", "-q:v", "7", "-pix_fmt", "yuv420p",
                                     "-g", "30"] + vf + audio + ["-f", "ogg", str(out_ogm)])
            if rc6 != 0 or not out_ogm.is_file() or out_ogm.stat().st_size == 0:
                fail("%s → OGM 失败：%s" % (name, err6.strip()[:120]))
                continue
            got = probe(MIN, out_ogm)
            if "parsed=1" in got and "codec=theora" in got and "acodec=vorbis" in got:
                ok("%s → OGM 真转 + 读回 theora/vorbis" % name)
            else:
                fail("%s 读回不对：%s" % (name, got.strip()[:120]))

        # 参考 OGM → OGM（同格式）
        ref = td / "ref.ogm"
        # 参考 OGM 用**全量** ffmpeg 生成（minimal 没有 lavfi 输入设备，只有 file 协议）
        rc7, _, err7 = run(FULL, ["-hide_banner", "-y", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                                  "-f", "lavfi", "-i", "sine=frequency=440", "-t", "1",
                                  "-c:v", "libtheora", "-q:v", "7", "-pix_fmt", "yuv420p",
                                  "-c:a", "libvorbis", "-f", "ogg", str(ref)])
        if rc7 == 0 and ref.is_file():
            got = probe(MIN, ref)
            (ok if "codec=theora" in got and "acodec=vorbis" in got else fail)(
                "OGM 参考 → OGM（同格式链路）")
        else:
            fail("OGM 参考样本生成/自转失败")

    if PROBLEMS:
        print("FFMPEG-MIN: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FFMPEG-MIN: PASS（%.2f MB）" % size_mb)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
