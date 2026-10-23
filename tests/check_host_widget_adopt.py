# -*- coding: utf-8 -*-
"""**宿主容器必须登记句柄**：`container = new QWidget` 在 `hub.cpp` 里一律禁止（改用 `make_host_widget()`）。

为什么要这条锁（2026-10-23 的实证）
-----------------------------------
`Hub::sync_pages()` / `render_plugin_slots()` 会给插件 builder 一个**根容器**，门面的
`parent=None` 就落到它上面（`resolve_parent()` → `current_panel_root()`）。修前那个容器是
`new QWidget`、**从不登记**进门面句柄表；而 `unwrap()` 对"已登记但已死"的地址返回 nullptr
⇒ 地址一旦复用某个已销毁门面控件的地址，**活容器被判死** ⇒ 该页所有控件无父（`place_null`）
⇒ 页看起来"根本没建出来"。

实测（`tools/p5_probe.py`，10 次）：`root_stale=1` 出现 6 次，其中 1 次直接红 8 条
（P17-82×4 / P16-3b-2 / P16-3c / P4×2）。修法＝容器登记成句柄（`adopt_host_widget`）。
现在统一走工厂 `make_host_widget()`（= `new QWidget` + `adopt_host_widget`），
**这个脚本防的就是"下次换个页又直接 new"**。

判据（窄而硬）
--------------
A1 `hub.cpp` 里**不许**出现 `container = new QWidget`（页容器与面板容器都算）。
A2 两处根容器都必须来自 `make_host_widget()`（恰好 2 处，少一处＝有人绕回裸 new）。
A3 工厂本体在 `widget_facade_qt.cpp` 里**真的**调了 `adopt_host_widget`（不是空壳）。

自检（反例必红）
  · 把 A1 的样本换回 `auto* container = new QWidget;` ⇒ A1 红
  · 把 A2 的样本换成 `new QWidget()` ⇒ A2 红
  · 把 A3 的样本换成"只 new 不 adopt" ⇒ A3 红
  · 干净样本 ⇒ 三条都绿

退出码：0=绿 / 1=红 / 2=缺输入。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUB = ROOT / "src" / "shell" / "hub.cpp"
FACADE = ROOT / "src" / "shell" / "widget_facade_qt.cpp"

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

BARE_NEW = re.compile(r"\bcontainer\s*=\s*new\s+QWidget\b")
FACTORY = re.compile(r"\bcontainer\s*=\s*make_host_widget\s*\(\s*\)")
PROBLEMS: list[str] = []
CASES = 0


def want(ok: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    print("   %s %s%s" % ("\u2713" if ok else "\u2717", label, ("（%s）" % detail) if detail else ""))
    if not ok:
        PROBLEMS.append(label)
    return ok


def strip_comments(text: str) -> str:
    """注释里的示例代码不算（本仓注释密度极高，且注释里就写着"修前是裸 new"）。"""
    out, i, n = [], 0, len(text)
    in_line = in_block = False
    while i < n:
        ch = text[i]
        if in_line:
            if ch == "\n":
                in_line = False
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        if text.startswith("//", i):
            in_line = True
            out.append("  ")
            i += 2
            continue
        if text.startswith("/*", i):
            in_block = True
            out.append("  ")
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def audit(hub_src: str, facade_src: str) -> list:
    problems = []
    hub = strip_comments(hub_src)
    bare = BARE_NEW.findall(hub)
    if bare:
        problems.append("hub.cpp 里还有 %d 处 `container = new QWidget`（必须走 make_host_widget）" % len(bare))
    made = FACTORY.findall(hub)
    if len(made) != 2:
        problems.append("hub.cpp 里 make_host_widget() 应恰好 2 处（页容器 + 面板容器），实得 %d" % len(made))
    facade = strip_comments(facade_src)
    m = re.search(r"QWidget\*\s*make_host_widget\s*\(\s*\)\s*\{(.*?)\n\}", facade, re.S)
    if m is None:
        problems.append("widget_facade_qt.cpp 里找不到 make_host_widget() 的定义")
    elif "adopt_host_widget(" not in m.group(1):
        problems.append("make_host_widget() 没有调 adopt_host_widget（空壳工厂 ⇒ 地址复用照旧）")
    return problems


def selfcheck() -> bool:
    good_hub = """
auto* container = make_host_widget();
auto* container2 = make_host_widget();          // 名字不同也算一处 -> 见下方说明
// 注释里写 container = new QWidget; 不算
"""
    # 上面第二行名字不是 container ⇒ 只有 1 处命中；用正规样本
    good_hub = """
auto* container = make_host_widget();
...
auto* container = make_host_widget();
// 注释：修前是 container = new QWidget;
"""
    bad_hub = """
auto* container = new QWidget;
auto* container = make_host_widget();
"""
    good_facade = """
QWidget* make_host_widget() {
    auto* widget = new QWidget;
    adopt_host_widget(widget);
    return widget;
}
"""
    bad_facade = """
QWidget* make_host_widget() {
    auto* widget = new QWidget;
    return widget;
}
"""
    r1 = audit(good_hub, good_facade)
    r2 = audit(bad_hub, good_facade)
    r3 = audit(good_hub, bad_facade)
    ok = (not r1) and r2 and r3
    print("   %s 自检：干净样本绿 / 裸 new 红 / 空壳工厂红"
          % ("\u2713" if ok else "\u2717"))
    return ok


def main() -> int:
    print("== 宿主容器必须登记句柄（2026-10-23 P5 地址复用实证的回归锁）==")
    if not HUB.is_file() or not FACADE.is_file():
        print("HOST-WIDGET-ADOPT skip 缺输入：%s / %s" % (HUB, FACADE))
        return 2
    if not selfcheck():
        print("HOST-WIDGET-ADOPT fail 反例自证不通过（本锁自己不可信）")
        return 1
    problems = audit(HUB.read_text(encoding="utf-8", errors="replace"),
                     FACADE.read_text(encoding="utf-8", errors="replace"))
    if problems:
        for item in problems:
            print("   \u2717 %s" % item)
        print("HOST-WIDGET-ADOPT fail %d 项不成立" % len(problems))
        return 1
    print("HOST-WIDGET-ADOPT ok hub.cpp 的两种根容器都走 make_host_widget()（＝登记句柄）；"
          "工厂本体真的调了 adopt_host_widget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
