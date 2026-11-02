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

    # D：线程判定必须"先判空再比"。
    #   禁区一：裸 `instance()->thread()`（无 app 时空指针解引用）—— 一律红；
    #   禁区二：`app->thread()` —— 只有当**同一行或往上 6 行内**出现过该变量的判空
    #   （`app == nullptr` / `app != nullptr` / `if (!app` / `if (app`）才放行。
    #   （DSH：原白名单只要出现 "app->thread()" 就放行，`auto* app = instance(); app->thread()`
    #    会静默通过。）
    def null_checked(lines, idx, var="app"):
        window = lines[max(0, idx - 6): idx + 1]
        needles = ("%s == nullptr" % var, "%s != nullptr" % var,
                   "if (!%s" % var, "if (%s" % var)
        return any(any(n in line for n in needles) for line in window)

    bare = []
    for base in (ROOT / "src" / "shell", ROOT / "src" / "engine"):
        for path in base.rglob("*.cpp"):
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            in_block_comment = False
            for idx, line in enumerate(lines):
                stripped = line.lstrip()
                if in_block_comment:
                    if "*/" in line:
                        in_block_comment = False
                    continue   # 块注释里提到不算
                if stripped.startswith("/*"):
                    if "*/" not in line:
                        in_block_comment = True
                    continue
                if stripped.startswith("//"):
                    continue
                if "instance()->thread()" in line:
                    bare.append("%s:%d" % (path.relative_to(ROOT), idx + 1))
                elif "app->thread()" in line and not null_checked(lines, idx):
                    bare.append("%s:%d" % (path.relative_to(ROOT), idx + 1))
    if not bare:
        ok("D 线程判定都先判空（无裸 instance()->thread()、app->thread() 有判空窗口）")
    else:
        fail("D 裸线程判定：%s" % ", ".join(bare[:6]))

    # E：TextPage 是**测试专用**（HA/DSH 2026-11-02 裁决）：注释必须写死，工厂不许注册它。
    text_h = (ROOT / "src" / "shell" / "pages" / "text_page.h")
    factory = (ROOT / "src" / "shell" / "pages" / "page_factory.cpp")
    if text_h.is_file() and "测试专用，不是产品页" in text_h.read_text(encoding="utf-8", errors="replace"):
        ok("E TextPage 标了「测试专用，不是产品页」")
    else:
        fail("E TextPage 没标测试专用（注释会腐烂，将来会被当产品页接回）")
    if factory.is_file():
        body = factory.read_text(encoding="utf-8", errors="replace")
        if "TextPage" not in body:
            ok("E page_factory 没有注册 TextPage（不会造第二份提取入口）")
        else:
            fail("E page_factory 里出现了 TextPage（测试页被接回产品）")

    if PROBLEMS:
        print("SHELL-STATIC-GUARDS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("SHELL-STATIC-GUARDS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
