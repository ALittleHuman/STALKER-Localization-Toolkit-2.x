"""视频转码 argv 差分基线（判据本体）—— **参照就是 1.x 源码本身**。

做法（与 XML 校对那条同族）：`ast.parse` 1.x `apps/video_ogm_app.py`，把含 `convert()` 的
**ClassDef 整个摘出来**，`exec` 到一个自带桩的命名空间（`subprocess.Popen` 只记录 argv、
`parse_video_info` 由用例喂、`log_summary`/`log_detail` 只打印），然后**真调 1.x 的
`convert()`**，录下它构造的 ffmpeg 命令行。

这样基线是"1.x 自己算出来的"，不是我照着源码手写的（转述误差为 0）。输出写
`tests/golden/video_argv.json`，供 2.x 的 `engine::video` 实现逐项对拍。

用法：python tests/probe_video_argv.py [--write]
"""
from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from pathlib import Path
from typing import Optional

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REF_APP = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\apps\video_ogm_app.py")
BASELINE = HERE / "golden" / "video_argv.json"


class VideoInfo:  # 桩：只放 convert() 会读的字段
    def __init__(self, video=None, audio=None, duration=0.0):
        self.video = video
        self.audio = audio
        self.duration = duration


class Stream:
    def __init__(self, **kw):
        self.__dict__.update(kw)


def load_reference_class():
    src = REF_APP.read_text(encoding="utf-8")
    tree = ast.parse(src)

    def has_convert(node):
        return any(isinstance(x, ast.FunctionDef) and x.name == "convert" for x in node.body)

    cls = None
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and has_convert(node):
            cls = node
            break
    if cls is None:
        raise SystemExit("在 %s 里找不到含 convert() 的类" % REF_APP)

    recorded = {"cmd": None}

    class FakeProc:
        def __init__(self, cmd, **kw):
            recorded["cmd"] = [str(c) for c in cmd]
            self.stderr = []
            self.returncode = 0

        def poll(self):
            return 0

        def terminate(self):
            pass

        def wait(self, timeout=None):
            return 0

    class FakeSubprocess:
        Popen = staticmethod(lambda cmd, **kw: FakeProc(cmd, **kw))
        PIPE = -1
        DEVNULL = -3
        STDOUT = -2

        @staticmethod
        def run(cmd, **kw):
            class R:
                stdout = " V....D libtheora           libtheora\n V....D libxvid             libxvid\n"

            return R()

    ns = {
        "subprocess": FakeSubprocess,
        "re": re,
        "Optional": Optional,
        "VideoInfo": VideoInfo,
        "parse_video_info": None,  # 每个用例现设
        "log_summary": lambda *a, **k: None,
        "log_detail": lambda *a, **k: None,
        "hidden_kwargs": lambda: {},
        "os": __import__("os"),
        "sys": sys,
        "time": __import__("time"),
    }
    module = ast.Module(body=[cls], type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(REF_APP), "exec"), ns)  # noqa: S102
    return ns[cls.name], recorded


def cases():
    """(名字, 参考信息, 源信息 or None(ffprobe 失败), 可用编码器)"""
    ref_theora = VideoInfo(video=Stream(codec="theora", width=640, height=480, fps=25.0,
                                        bitrate=800000),
                           audio=Stream(sample_rate=44100, channels=2, bitrate=128000))
    ref_mpeg4 = VideoInfo(video=Stream(codec="mpeg4", width=640, height=480, fps=25.0,
                                       bitrate=0),
                          audio=Stream(sample_rate=0, channels=0, bitrate=0))
    ref_other = VideoInfo(video=Stream(codec="h264", width=0, height=0, fps=0.0, bitrate=0),
                          audio=None)
    src_same = VideoInfo(video=Stream(codec="theora", width=640, height=480, fps=25.0, bitrate=0),
                         audio=Stream(sample_rate=48000, channels=2, bitrate=160000),
                         duration=10.0)
    src_diff = VideoInfo(video=Stream(codec="theora", width=320, height=240, fps=30.0, bitrate=0),
                         audio=Stream(sample_rate=48000, channels=2, bitrate=160000),
                         duration=10.0)
    src_noaudio = VideoInfo(video=Stream(codec="theora", width=640, height=480, fps=25.0,
                                         bitrate=0), audio=None, duration=10.0)
    allenc = {"libtheora", "libxvid", "mpeg4"}
    noxvid = {"libtheora", "mpeg4"}
    onlympeg = {"mpeg4"}
    # ★★ 2026-10-10（P17-9，审计 02 F-03 Medium）：**非整数参考帧率**（影视化片头的 23.976/29.97）。
    #   1.x 是 `vf_parts.append(f"fps={ref_fps}")`（`video_ogm_app.py:302`）＝ Python 的
    #   `repr(float)`（**最短往返**表示）⇒ `23.976023976023978`；我们修前用 `%.6g` 只给
    #   `23.976` ⇒ `-vf fps=` 数值与参考 argv **不等**，而且帧率滤镜数值不同会改产物的时基。
    #   基准由 `--write` 真调 1.x 的规划器生成，所以这两条是"1.x 真行为"而不是手算值。
    ref_fps_23976 = VideoInfo(video=Stream(codec="theora", width=640, height=480,
                                           fps=24000.0 / 1001.0, bitrate=800000),
                              audio=Stream(sample_rate=44100, channels=2, bitrate=128000))
    ref_fps_2997 = VideoInfo(video=Stream(codec="theora", width=640, height=480,
                                          fps=30000.0 / 1001.0, bitrate=800000),
                             audio=Stream(sample_rate=44100, channels=2, bitrate=128000))
    return [
        ("theora_ref_bitrate_src_same_audio", ref_theora, src_same, allenc),
        ("theora_ref_no_bitrate_src_diff_noaudio", ref_other, src_diff, allenc),
        ("mpeg4_ref_no_bitrate_src_noaudio", ref_mpeg4, src_noaudio, allenc),
        ("mpeg4_ref_fallback_noxvid", ref_mpeg4, src_same, noxvid),
        ("last_resort_only_mpeg4", ref_theora, src_same, onlympeg),
        ("ffprobe_failed_keeps_audio", ref_theora, None, allenc),
        ("no_reference_at_all", None, src_same, allenc),
        ("ref_fps_24000_1001_src_same", ref_fps_23976, src_same, allenc),
        ("ref_fps_30000_1001_src_same", ref_fps_2997, src_same, allenc),
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true", help="写基线 tests/golden/video_argv.json")
    args = parser.parse_args()

    cls, recorded = load_reference_class()
    rows = []
    for name, ref, src, encoders in cases():
        obj = cls(ffmpeg_exe="FFMPEG", ffprobe_exe="FFPROBE")
        obj._available_encoders = set(encoders)
        obj._encoders_scanned = True
        obj._cancelled = False
        # parse_video_info 桩：每个用例现设（None 表示 ffprobe 解析失败）
        obj.__dict__["_case_src"] = src
        import builtins
        builtins.__dict__["__xrkit_src"] = src

        def fake_parse(path, probe, _src=src):
            return (_src, "" if _src is not None else "probe failed")

        # 直接换掉模块级函数（exec 出来的方法在它的 __globals__ 里找这个名字）
        cls.convert.__globals__["parse_video_info"] = fake_parse
        recorded["cmd"] = None
        ok, msg = obj.convert("IN.avi", "OUT.ogm", reference_info=ref)
        rows.append({
            "case": name,
            "encoders": sorted(encoders),
            "ok": ok,
            "argv": recorded["cmd"],
            "message": msg,
        })
        print("%-38s ok=%s  %s" % (name, ok, " ".join(recorded["cmd"] or [])))

    if args.write:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(json.dumps({
            "note": "基线由 1.x apps/video_ogm_app.py 的 convert() 本体 exec 出来后**实调**得到"
                    "（桩只替换 Popen/parse_video_info/日志）——不是我照着源码手写的",
            "reference": str(REF_APP),
            "cases": rows,
        }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print("已写基线 → %s" % BASELINE.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
