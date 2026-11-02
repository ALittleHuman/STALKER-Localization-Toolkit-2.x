# -*- coding: utf-8 -*-
"""`stk_ui` 16 原语的**函数级**静态锁（HA 口径，2026-11-01）。

为什么是函数级、不是文件级
------------------------
`widget_facade_qt.cpp` 里 `:961/:1434` 有**合法的**直接 `new QLabel`（语义能力自己的容器）
⇒ 文件级 grep「不许出现 new QLabel」会误红。真正的纪律是：
**9 个造控件方法（label/button/row/checkbox/entry/text/combo/drop_box/text_view）实现体内
必须经由 `table()-><同名>(`** —— 否则就是绕过原语层自己造控件（会产出第二份实现）。

另两条（alive 语义）：
- `facade_api.h` 的 `alive` 声明处必须写明「不可用于判存活」；
- `plugin_ui.cpp` 的 `ui_alive` 实现里同样写明（`reinterpret_cast` 恒真，除纯空指针）。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
QT = ROOT / "src" / "shell" / "widget_facade_qt.cpp"
API_H = ROOT / "include" / "xrkit" / "engine" / "plugin" / "facade_api.h"
PLUGIN_UI = ROOT / "src" / "shell" / "plugin_ui.cpp"
NAMES = ["label", "button", "row", "checkbox", "entry", "text", "combo", "drop_box", "text_view"]
PROBLEMS = []


def ok(msg):
    print("  \u2713 " + msg)


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def lambda_block(text: str, name: str) -> str:
    marker = "/* %s */" % name
    at = text.find(marker)
    if at < 0:
        return ""
    # 从 marker 之后的第一个 '{' 起，按括号配平取整块
    begin = text.find("{", at)
    if begin < 0:
        return ""
    depth = 0
    for i in range(begin, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[begin:i + 1]
    return ""


def main() -> int:
    print("== stk_ui 函数级静态锁（9 个造控件 + alive 语义）==")
    if not QT.is_file():
        print("UI-FACADE-PRIMITIVES skip 缺 widget_facade_qt.cpp")
        return 2
    text = QT.read_text(encoding="utf-8", errors="replace")
    for name in NAMES:
        block = lambda_block(text, name)
        if not block:
            fail("%s：找不到 /* %s */ 的实现块" % (name, name))
        elif ("table()->%s(" % name) not in block:
            fail("%s：实现体内没有 `table()->%s(` —— 绕过原语层自己造控件" % (name, name))
        else:
            ok("%s 经由 table()->%s(" % (name, name))

    api = API_H.read_text(encoding="utf-8", errors="replace")
    if "不可用于判存活" in api:
        ok("facade_api.h 的 alive 声明写明「不可用于判存活」")
    else:
        fail("facade_api.h 的 alive 声明没写语义边界（会被当成真存活探针）")

    ui = PLUGIN_UI.read_text(encoding="utf-8", errors="replace")
    if "不可用于判存活" in ui:
        ok("plugin_ui.cpp 的 ui_alive 写明「不可用于判存活」")
    else:
        fail("plugin_ui.cpp 的 ui_alive 没写语义边界（reinterpret_cast 恒真）")

    if PROBLEMS:
        print("UI-FACADE-PRIMITIVES: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("UI-FACADE-PRIMITIVES: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
