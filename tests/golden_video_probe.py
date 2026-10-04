#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""视频参数解析对拍：2.x `parse_ffprobe_json` / `parse_encoder_list` vs **1.x 本体**。

做法（与 `golden_video_argv.py` / `probe_video_argv.py` 同族）：
  · 夹具是**真 ffprobe / 真 `ffmpeg -encoders` 的输出字节**（`tests/probe_video_probe.py --write`
    用真 exe 录下来；`tests/golden/video_probe/ffprobe_*.json` 每个都是一份真实样本）；
  · 期望值现算：把 1.x `apps/video_ogm_app.py` 里的 `VideoInfo/VideoStream/AudioStream` +
    `parse_video_info`（以及 `Converter._ensure_encoders`）`ast` 摘出来，桩掉 `subprocess.run`
    让它吃**同一份字节**，真调 1.x 的解析代码；
  · 我们的 CLI（`--video-probe` / `--video-encoders`）吃同一份字节，逐字段比。

所以"字段名/单位/字符串数字/图例行"这些坑都是 1.x 自己说了算，不是我照源码手写的。

用法：python tests/golden_video_probe.py [--cli <xrkit_cli.exe>] [--fixtures <夹具目录>]
退出码：0=一致 / 1=不一致 / 2=夹具或 CLI 缺失（外部前提）。
"""
from __future__ import annotations

import argparse
import ast
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
REF_APP = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\apps\video_ogm_app.py")
DEFAULT_FIXTURES = HERE / "golden" / "video_probe"

CLI = DEFAULT_CLI
FIXTURES = DEFAULT_FIXTURES


def load_reference_nodes():
    """把 1.x 的 VideoStream/AudioStream/VideoInfo/parse_video_info/Converter 摘出来。"""
    tree = ast.parse(REF_APP.read_text(encoding="utf-8"))
    wanted = {"VideoStream", "AudioStream", "VideoInfo", "Converter"}
    body = []
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name in wanted:
            body.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name == "parse_video_info":
            body.append(node)
    missing = sorted(wanted - {n.name for n in body if isinstance(n, ast.ClassDef)})
    if missing or not any(isinstance(n, ast.FunctionDef) for n in body):
        raise SystemExit("在 %s 里找不到 %s / parse_video_info" % (REF_APP, missing))
    return body


def make_namespace(probe_text: str, encoders_text: str) -> dict:
    class FakeResult:
        returncode = 0
        stderr = ""

        def __init__(self, stdout):
            self.stdout = stdout

    class FakeSubprocess:
        @staticmethod
        def run(cmd, **kw):
            if "-encoders" in cmd:
                return FakeResult(encoders_text)
            return FakeResult(probe_text)

    return {
        "subprocess": FakeSubprocess,
        "dataclass": __import__("dataclasses").dataclass,
        "field": __import__("dataclasses").field,
        "json": json,
        "os": __import__("os"),
        "sys": sys,
        "re": __import__("re"),
        "time": __import__("time"),
        "Optional": __import__("typing").Optional,
        "log_summary": lambda *a, **k: None,
        "log_detail": lambda *a, **k: None,
        "hidden_kwargs": lambda: {},
    }


def reference_fields(probe_text: str) -> dict:
    """1.x 的解析结论 → 与 CLI `PROBE` 行同形态的字符串表格。"""
    ns = make_namespace(probe_text, "")
    module = ast.Module(body=load_reference_nodes(), type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(REF_APP), "exec"), ns)  # noqa: S102
    # 1.x 在跑 ffprobe 之前会先 `os.path.isfile` 两道 ⇒ 造两个真存在的空文件给它，
    # 真正被喂进去的字节由上面的 `subprocess.run` 桩决定（就是那份夹具）。
    stub_dir = ROOT / "build" / "video_probe_tmp"
    stub_dir.mkdir(parents=True, exist_ok=True)
    stub_in = stub_dir / "in.avi"
    stub_probe = stub_dir / "ffprobe.exe"
    stub_in.write_bytes(b"")
    stub_probe.write_bytes(b"")
    info, err = ns["parse_video_info"](str(stub_in), str(stub_probe))
    if info is None:
        raise SystemExit("1.x parse_video_info 失败：%s" % err)
    video = getattr(info, "video", None)
    audio = getattr(info, "audio", None)
    fields = {
        "parsed": "1",
        "has_video": "1" if video is not None else "0",
        "has_audio": "1" if audio is not None else "0",
        "duration": "%.6f" % float(getattr(info, "duration", 0.0)),
        "size_mb": "%.6f" % float(getattr(info, "size_mb", 0.0)),
    }
    if video is not None:
        fields["video.codec"] = str(video.codec)
        fields["video.width"] = str(video.width)
        fields["video.height"] = str(video.height)
        fields["video.fps"] = "%.6f" % float(video.fps)
        fields["video.bitrate"] = str(video.bitrate)
    if audio is not None:
        fields["audio.codec"] = str(audio.codec)
        fields["audio.channels"] = str(audio.channels)
        fields["audio.sample_rate"] = str(audio.sample_rate)
        fields["audio.bitrate"] = str(audio.bitrate)
    return fields


def reference_encoders(encoders_text: str) -> list:
    """1.x `Converter._ensure_encoders` 的结论（它是 set ⇒ 比的时候排序）。"""
    ns = make_namespace("", encoders_text)
    module = ast.Module(body=load_reference_nodes(), type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(REF_APP), "exec"), ns)  # noqa: S102
    converter = ns["Converter"]("FFMPEG", "FFPROBE")
    converter._encoders_scanned = False
    converter._ensure_encoders()
    return sorted(converter._available_encoders)


def cli_fields(probe_path: Path) -> dict:
    proc = subprocess.run([str(CLI), "--video-probe", str(probe_path)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise SystemExit("--video-probe 退出码 %d：%s"
                         % (proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
    fields = {}
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if not line.startswith("PROBE\t"):
            continue
        _, key, value = line.split("\t", 2)
        fields[key] = value
    return fields


def cli_encoders(path: Path) -> list:
    proc = subprocess.run([str(CLI), "--video-encoders", str(path)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise SystemExit("--video-encoders 退出码 %d：%s"
                         % (proc.returncode, proc.stdout.decode("utf-8", "replace")[:200]))
    names = []
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("ENC\t"):
            names.append(line.split("\t", 1)[1])
    return names


def main() -> int:
    global CLI, FIXTURES
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=str(DEFAULT_CLI))
    parser.add_argument("--fixtures", default=str(DEFAULT_FIXTURES))
    args = parser.parse_args()
    CLI = Path(args.cli)
    FIXTURES = Path(args.fixtures)

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-VIDEO-PROBE: SKIP (cli-missing)")
        return 2
    fixtures = sorted(FIXTURES.glob("ffprobe_*.json"))
    encoders_fixture = FIXTURES / "ffmpeg_encoders.txt"
    if not fixtures or not encoders_fixture.is_file() or not REF_APP.is_file():
        print("!! 缺夹具（python tests/probe_video_probe.py --write）或缺 1.x 参照 %s" % REF_APP)
        print("GOLDEN-VIDEO-PROBE: SKIP (fixture-missing)")
        return 2

    failed = 0
    total_keys = 0
    for probe_path in fixtures:
        probe_text = probe_path.read_text(encoding="utf-8", newline="")
        want = reference_fields(probe_text)
        got = cli_fields(probe_path)
        total_keys += len(want)
        diffs = 0
        for key in sorted(want):
            if key not in got:
                print("[%s.%s] 我们没输出这个字段" % (probe_path.name, key))
                diffs += 1
            elif got[key] != want[key]:
                print("[%s.%s] 不一致：1.x=%r 我们=%r" % (probe_path.name, key, want[key], got[key]))
                diffs += 1
        extra = sorted(set(got) - set(want))
        if extra:
            print("注意：%s 多输出了 %s（1.x 参照里没有）" % (probe_path.name, extra))
        if diffs:
            failed += diffs
        else:
            print("[%s] ok %d 字段（video=%s %sx%s fps=%s / audio=%s %sch %sHz）"
                  % (probe_path.name, len(want), want.get("video.codec", "-"),
                     want.get("video.width", "-"), want.get("video.height", "-"),
                     want.get("video.fps", "-"), want.get("audio.codec", "-"),
                     want.get("audio.channels", "-"), want.get("audio.sample_rate", "-")))

    # 反面：坏 JSON 必须**如实**落成 parsed=0（不是猜一半、不是崩）
    broken = ROOT / "build" / "video_probe_broken.json"
    broken.parent.mkdir(parents=True, exist_ok=True)
    broken.write_text("{ this is not json", encoding="utf-8")
    bad = cli_fields(broken)
    if bad.get("parsed") == "0" and bad.get("has_video") == "0" and bad.get("has_audio") == "0":
        print("[broken] ok 坏 JSON ⇒ parsed=0 / 无流")
    else:
        print("[broken] 坏 JSON 没被如实拒绝：%s" % bad)
        failed += 1
    broken.unlink(missing_ok=True)

    encoders_text = encoders_fixture.read_text(encoding="utf-8", newline="")
    want_enc = reference_encoders(encoders_text)
    got_enc = sorted(cli_encoders(encoders_fixture))
    if want_enc == got_enc:
        print("[encoders] ok %d 个（含图例行那类垃圾，1.x 收什么我们收什么）" % len(want_enc))
    else:
        print("[encoders] 不一致：1.x 独有 %s；我们独有 %s"
              % (sorted(set(want_enc) - set(got_enc)), sorted(set(got_enc) - set(want_enc))))
        failed += 1

    print("")
    print("GOLDEN-VIDEO-PROBE: %s (fixtures=%d probe_keys=%d encoders=%d failed=%d)"
          % ("FAIL" if failed else "PASS", len(fixtures), total_keys, len(want_enc), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
