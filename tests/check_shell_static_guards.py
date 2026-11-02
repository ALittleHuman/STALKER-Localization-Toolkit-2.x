# -*- coding: utf-8 -*-
"""外壳静态锁（2026-11-01 两次真实事故的收口）。

三条都是"编译器不报、行为怪"的族：
  A `resolve_ffmpeg()` 只许在 `status_text()` / `ensure_ffmpeg()` 里出现 ——
    在别处直接调它 ⇒ 忘了 ensure ⇒ paths 全空 ⇒ 表现成"找不到 ffmpeg"，不报错（P-356 静默退化）；
  B `ensure_ffmpeg()` 体里不许调它自己 —— 自递归 ⇒ 栈溢出 0xC00000FD（本轮真发生）；
  C `//` 注释行不许以反斜杠结尾 —— 会把下一行代码续进注释（本轮真发生：`left` 未声明）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE_CPP = ROOT / "src" / "shell" / "pages" / "video_page.cpp"
SCAN_DIRS = [ROOT / "src" / "shell", ROOT / "src" / "engine", ROOT / "tools" / "cli"]
PROBLEMS = []


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def ok(msg):
    print("  \u2713 " + msg)


def main() -> int:
    text = PAGE_CPP.read_text(encoding="utf-8", errors="replace")
    print("== 外壳静态锁：resolve 调用点 / 自递归 / 注释续行 ==")

    # A：resolve_ffmpeg() 调用点只许在 status_text / ensure_ffmpeg
    lines = text.splitlines()
    offenders = []
    current_fn = ""
    for line in lines:
        if re.match(r"^[A-Za-z_:<> ]+\s+VideoPage::\w+", line):
            m = re.search(r"VideoPage::(\w+)", line)
            current_fn = m.group(1) if m else ""
        if "resolve_ffmpeg()" in line and "VideoPage::resolve_ffmpeg" not in line:
            if current_fn not in ("status_text", "ensure_ffmpeg"):
                offenders.append((current_fn, line.strip()))
    if not offenders:
        ok("A `resolve_ffmpeg()` 只出现在 status_text / ensure_ffmpeg")
    else:
        for fn, line in offenders:
            fail("A 在 %s 里直接调 resolve_ffmpeg()（忘了 ensure ⇒ paths 可能全空）：%s" % (fn, line))

    # B：ensure_ffmpeg 体里不许自递归
    m = re.search(r"FfmpegPaths VideoPage::ensure_ffmpeg\(\) \{(.*?)\n\}", text, re.S)
    if not m:
        fail("B 找不到 ensure_ffmpeg() 的实现")
    elif "ensure_ffmpeg()" in m.group(1):
        fail("B ensure_ffmpeg() 体内调用了自身（自递归 ⇒ 栈溢出）")
    else:
        ok("B ensure_ffmpeg() 不自递归")

    # C：`//` 注释行不得以反斜杠结尾
    bad = []
    for base in SCAN_DIRS:
        for path in base.rglob("*"):
            if path.suffix not in (".cpp", ".h"):
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for idx, line in enumerate(lines, 1):
                if not re.search(r"//.*\\$", line):
                    continue
                nxt = lines[idx].strip() if idx < len(lines) else ""
                # 下一行仍是注释 ⇒ 续行无害；下一行是代码 ⇒ 会把代码吞进注释（本轮真事故）
                if nxt and not nxt.startswith("//"):
                    bad.append("%s:%d" % (path.relative_to(ROOT), idx))
    if not bad:
        ok("C 没有以反斜杠结尾的 `//` 注释（会续行吞掉下一行代码）")
    else:
        fail("C 注释续行风险：%s" % ", ".join(bad[:6]))

    if PROBLEMS:
        print("SHELL-STATIC-GUARDS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("SHELL-STATIC-GUARDS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
