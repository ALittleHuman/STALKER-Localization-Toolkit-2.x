# -*- coding: utf-8 -*-
"""P-739 / P-745 / P-770 的红锁（注入审计 session-ac61aa8a）。

这三条都是"一行级"缺陷，**修前必须红**：
  · P-739 `plugins/video_page.py`：版本闸门 `if not rejected(...): pass`（空壳 ⇒ 8.x 放行）；
  · P-745 `plugins/fontpack_page.py`：`index == len(SIZES) + 1` 永假 ⇒ `SUFFIXES[2]` IndexError；
  · P-770 `tools/cli/build_cmd.cpp`：回退失败后 `goto fail_archive` 回到失败块 ⇒ 自循环。

P-739/P-745 同时做**可导入性**检查：`python -m py_compile` 通过，防止"修好了但语法坏"。
诚实标注：这三条是**源码锚点 + 语法编译**，不是运行行为（要驱动插件 UI 需整套 facade 桩）。
"""
import py_compile
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBLEMS = []


def ok(msg):
    print("  \u2713 " + msg)


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def main() -> int:
    print("== 一行级缺陷红锁：P-739 / P-745 / P-770 ==")
    video = (ROOT / "plugins" / "video_page.py").read_text(encoding="utf-8", errors="replace")
    fontpack = (ROOT / "plugins" / "fontpack_page.py").read_text(encoding="utf-8", errors="replace")
    build = (ROOT / "tools" / "cli" / "build_cmd.cpp").read_text(encoding="utf-8", errors="replace")

    # ★ P-739 剩余两步（复核指出的）：输入链必须通、未知版本必须 fail-closed
    # ★★ 2026-11-01（复核要求）：改成**行为判据** —— 直接问程序"探测到的主版本是多少"。
    #   死代码（探测写在 return 之后）会让 major=-1 ⇒ 这条必红。
    cli = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
    # 找一个**真的带 ffmpeg** 的目录：优先最新打包产物，其次仓库 bin/
    probe_dir = None
    dist = ROOT / "dist" / "2.0.0"
    for cand in sorted(dist.glob("BETA/*/bin/ffmpeg.exe"), reverse=True) if dist.is_dir() else []:
        probe_dir = cand.parent.parent
        break
    if probe_dir is None and (ROOT / "bin" / "ffmpeg.exe").is_file():
        probe_dir = ROOT
    if not cli.is_file() or probe_dir is None:
        fail("P-739：缺 xrkit_cli.exe 或带 ffmpeg 的目录，无法做行为判据")
    else:
        proc = subprocess.run([str(cli), "--video-ffmpeg-probe", str(probe_dir)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = proc.stdout.decode("utf-8", errors="replace")
        major = -2
        for token in text.split():
            if token.startswith("version_major="):
                try:
                    major = int(token.split("=", 1)[1])
                except ValueError:
                    pass
        if proc.returncode == 0 and major > 0:
            ok("P-739 行为：真 ffmpeg 的 version_major=%d（探测不是死代码）" % major)
        else:
            fail("P-739 行为：探测拿不到版本（major=%d，rc=%d）——死代码或找不到 ffmpeg"
                 % (major, proc.returncode))

    if "if not engine.video_version_rejected(" not in video and "video_version_rejected(version_text)" in video:
        ok("P-739：版本闸门不再是空壳（rejected 决定是否继续）")
    else:
        fail("P-739：版本闸门仍是 `if not rejected(...): pass`")
    if "len(SIZES) + 1" not in fontpack:
        ok("P-745：suffix_value 不再用 len(SIZES)+1 判自定义")
    else:
        fail("P-745：`len(SIZES) + 1` 还在（自定义永远走错分支）")
    # 只许"阶段失败 ⇒ 归档"的 goto；回退失败那条必须消失。
    # 注意：注释里会提到 `goto fail_archive`（说明历史），所以只认**代码行**。
    import re as _re
    rollback_region = build[build.find("回退失败") : build.find("回退失败") + 600]
    code_goto = _re.search(r"(?m)^\s*goto fail_archive;", rollback_region)
    if code_goto is None:
        ok("P-770：回退失败后不再 goto（自循环消除；注释里的历史说明不算）")
    else:
        fail("P-770：回退失败后仍有 goto fail_archive")

    # ★ P-782 纯函数反向锁：OGM 只认 Theora —— 不依赖 ffmpeg、不依赖 golden，
    #   一行断言把"回到 mpeg4/libxvid 回退"的路堵死。
    src = (ROOT / "src" / "engine" / "video" / "transcode.cpp").read_text(
        encoding="utf-8", errors="replace")
    pick_start = src.find("std::string pick_video_encoder(")
    pick_end = src.find("\n}", pick_start)
    pick_body = src[pick_start:pick_end] if pick_start >= 0 else ""
    # 只看**代码行**（注释里写"libxvid / mpeg4"是在解释历史，不算回退）
    code_lines = [ln for ln in pick_body.splitlines() if not ln.strip().startswith("//")]
    code_body = "\n".join(code_lines)
    if 'contains(available, "libtheora")' in code_body and "libxvid" not in code_body and \
            "mpeg4" not in code_body:
        ok("P-782 纯函数锁：pick_video_encoder 只认 libtheora（无 libxvid/mpeg4 回退）")
    else:
        fail("P-782 纯函数锁：pick_video_encoder 仍可能回退到 libxvid/mpeg4")

    for rel in ("plugins/video_page.py", "plugins/fontpack_page.py"):
        try:
            py_compile.compile(str(ROOT / rel), doraise=True)
            ok("%s 语法编译通过" % rel)
        except Exception as exc:  # noqa: BLE001
            fail("%s 编译失败：%r" % (rel, exc))

    if PROBLEMS:
        print("PLUGIN-ONELINERS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PLUGIN-ONELINERS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
