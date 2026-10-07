#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""页面级消息标签必须是"有父、进布局"的控件（审计 2026-10-06 补充 F3 的第 1 条）。

审计原文（F3，标 S3/S4，但**效应等同用户可见缺陷**，所以要修）：
  · `xml_page.cpp:166` / `text_page.cpp:111` / `convert_page.cpp:241` 都写
    `status_ = new QLabel(task_->status())` —— `TaskShell::status()` 返回的是 **QString**
    （控件访问器叫 `status_label()`）⇒ `QLabel(const QString&, QWidget* = nullptr)` 造出一个
    **无父**标签 ⇒ 它是**顶层窗口**、**从不进布局**（全仓 `addWidget(status_)` 零命中）
    ⇒ 全页消息（空目录 / 导出成功失败 / 复制 id / 还没选源）**用户完全看不见**，
    且无人拥有（泄漏）。

判据：
  ① 静态：三页（+ 视频页）的 `status_` 赋值之后**必须**把它加进布局；且全仓不许再出现
     `new QLabel(task_->status())` 这种"把 QString 当控件"的写法；
  ② 分析器**自证**：喂反例（就是修前那一行）必须报红。

行为面（真的画出来、真的可见）在 `src/shell/smoke.cpp` 里，随 shell 自检跑。
退出码：0 = 全绿；1 = 有判据不成立。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _cpp_source import strip_comments  # noqa: E402  （共用"去注释"实现，同类只维护一套）

ROOT = Path(__file__).resolve().parent.parent
PAGES = ["xml_page.cpp", "text_page.cpp", "convert_page.cpp", "video_page.cpp"]
PAGES_DIR = ROOT / "src" / "shell" / "pages"

FAILURES: list[str] = []


def say(line: str) -> None:
    print(line, flush=True)


def check(ok: bool, label: str, detail: str = "") -> None:
    if ok:
        say("  \u2713 %s" % label)
    else:
        FAILURES.append(label)
        say("  \u2717 %s%s" % (label, ("\n      " + detail) if detail else ""))


# ── 分析器（纯文本 → 问题清单；下面会被喂反例自证）────────────────────────────

STATUS_ASSIGN = re.compile(r"status_\s*=\s*new\s+QLabel[^;]*;")


def label_wiring_problems(source: str) -> list[str]:
    problems: list[str] = []
    # ① 把 QString 当控件（修前那一行）：`new QLabel(task_->status())`
    if re.search(r"new\s+QLabel\s*\(\s*task_->status\(\)", source):
        problems.append("有 `new QLabel(task_->status())`：`status()` 返回 QString，"
                        "造出来的标签**无父**（顶层窗口、从不进布局）")
    # ② 每个 `status_ = new QLabel...;`（含 `= task_->status_label()` 这种壳内复用）之后
    #    必须出现把它加进布局/指定父的语句。按**每一处**分别计数（别写成"文件里出现过"）。
    for match in STATUS_ASSIGN.finditer(source):
        tail = source[match.end():match.end() + 400]
        wired = ("addWidget(status_" in tail) or ("status_->setParent(" in tail)
        if not wired:
            line = source[:match.start()].count("\n") + 1
            problems.append("第 %d 行的 `status_ = new QLabel...` 之后没有 "
                            "`addWidget(status_...)` / `setParent(...)` ⇒ 它不是页面里的控件"
                            % line)
    return problems


def main() -> int:
    say("PAGE-STATUS 页面级消息标签必须「有父、进布局」（F3）")
    total_assigns = 0
    for name in PAGES:
        path = PAGES_DIR / name
        if not path.is_file():
            check(False, "源文件存在：src/shell/pages/%s" % name)
            continue
        source = strip_comments(path.read_text(encoding="utf-8"))
        count = len(STATUS_ASSIGN.findall(source))
        total_assigns += count
        if count == 0:
            # video_page 的 status_ 不是 `new QLabel` 这一支也要有布局接线；这里只要求
            # "至少有一处并且接进布局"，缺了就是判据挂空 ⇒ 明确报红。
            check(False, "%s：找不到 `status_ = new QLabel...`（判据别挂空）" % name)
            continue
        problems = label_wiring_problems(source)
        check(not problems, "%s：%d 处页面级消息标签都接进了布局" % (name, count),
              "\n      ".join(problems))

    # ★ 自证：修前那一行必须被判红（否则这条锁只是"文件里出现过"式的假判据）
    bad = ("    status_ = new QLabel(task_->status());\n"
           "    status_->setObjectName(QStringLiteral(\"xml_status\"));\n")
    check(bool(label_wiring_problems(bad)),
          "自证：修前那种 `new QLabel(task_->status())` 必须被判红",
          str(label_wiring_problems(bad)))
    bad2 = "    status_ = new QLabel;\n    status_->setObjectName(QStringLiteral(\"x\"));\n"
    check(bool(label_wiring_problems(bad2)),
          "自证：造了标签却不进布局必须被判红", str(label_wiring_problems(bad2)))

    if FAILURES:
        say("PAGE-STATUS: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("PAGE-STATUS: PASS（%d 处标签）" % total_assigns)
    return 0


if __name__ == "__main__":
    sys.exit(main())
