# -*- coding: utf-8 -*-
"""判据 S-02：**插件页 builder 必须有 C++ 异常护栏**（第三方只读审计 S-02）。

修前事实（审计原话 + 代码核）
----------------------------
`src/shell/hub.cpp::render_plugin_slots` 里是一句裸的 `built = reg->builder(container);`
—— C++ 插件（或任何会抛的实现）在 builder 里抛异常时，异常会**穿进 Qt 的页面构造路径**，
一路冒到 `QApplication::exec`；Qt 不接异常 ⇒ **进程终止**（与 S-03 的 `init()` 同一族）。

修法（与 S-03 同一套口径）
--------------------------
拦下 → **留痕（点名插件与 `what()`）→ 走"没造出控件"那条善后**（占位页顶替）。
★ **抛过异常就一票否决**：抛在半途的 builder 可能已经往根里塞了几个控件
（`placed->count() > 0`），但那张页面**半残** —— 拿它当真比占位页更糟。
★ 只拦 **C++ 异常**；**不拦 SEH**（访问违例要 /EHa 或 `__try`，会把"插件里崩了"变成
"静默吞掉"，属口径变更，单独裁定）。

本判据是**静态锁**（行为锁需要一个"panel builder 会抛"的 DLL 夹具 ＋ 在 smoke 里造 Hub，
已登记为下一步）：断言护栏的**结构**在场，且**不许**退化成裸调用 —— 红证就是把它退回去。

退出码：0=绿 / 1=红 / 2=缺输入。`--hub-src` 只为取证（拿 `git show <修前>:src/shell/hub.cpp` 跑红证）。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
HUB = ROOT / "src" / "shell" / "hub.cpp"

# ★★ 用户 2026-10-19/20 要求固化：**找调用点用正则**，别写字面片段。
#   我第一版写 `.builder(container)` 命中 0 处（实际是 `->builder(`）⇒ 判据直接红，
#   白跑一轮。这条也适用于其它"找调用点"的静态锁。
CALL_RE = re.compile(r"(->|\.)builder\s*\(")

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--hub-src", default=str(HUB))
    args = ap.parse_args()
    src_path = Path(args.hub_src)

    print("== S-02 / 决策 2：插件页 **与面板** builder 的 C++ 异常护栏（静态锁）==")
    if not src_path.is_file():
        print("BUILDER-GUARD skip 缺 %s" % src_path)
        return 2
    lines = src_path.read_text(encoding="utf-8", errors="replace").splitlines()

    # ★ 注释行里"引用旧代码"不算调用点（H-01/H-04 的教训：文档/注释引用代码不该锁住自己）
    hits = [i for i, l in enumerate(lines)
            if CALL_RE.search(l) and not l.lstrip().startswith("//")]
    # ★★ 决策 2（用户 2026-10-18/19）：**两个** builder 调用点都要有护栏 ——
    #   页（`reg->builder`）与**面板**（`panel->builder`）。修前只锁了页那一个，
    #   面板是裸调用：插件面板 builder 抛异常会直接穿出 Hub（整页崩，且不留痕）。
    want(len(hits) == 2, "A1 恰好**两处** builder 调用点（页 ＋ 面板；少一处＝护栏只包了一半）",
         "命中 %d 处：%s" % (len(hits), [lines[i].strip()[:40] for i in hits]))
    if len(hits) != 2:
        print("-" * 66)
        print("BUILDER-GUARD fail %d 项不成立" % len(PROBLEMS))
        return 1

    for k, at in enumerate(hits, start=1):
        tag = "页" if "reg->builder" in lines[at] else "面板"
        back = "\n".join(lines[max(0, at - 4):at])
        want("try {" in back,
             "A2[%d/%s] 调用点上方 4 行内必须有 `try {`（裸调用＝异常直接穿出去）" % (k, tag),
             back.strip().splitlines()[-1][:60] if back.strip() else "空")
        fwd = "\n".join(lines[at:at + 10])
        want("catch (const std::exception&" in fwd,
             "A3[%d/%s] 下方 10 行内接 `catch (const std::exception&`" % (k, tag))
        want("catch (...)" in fwd,
             "A4[%d/%s] 还要有 `catch (...)`（非 std::exception 的也别穿出去）" % (k, tag))
        want("what()" in fwd,
             "A5[%d/%s] 异常文本要留下（`what()`）—— 零静默" % (k, tag))

    text = "\n".join(lines)
    want("const bool builder_threw = !builder_exception.empty();" in text,
         "B1 页：显式 `builder_threw` 标志（不是靠 `built == nullptr` 猜）")
    want("bool panel_threw = false;" in text and "if (panel_threw)" in text,
         "B2 面板：显式 `panel_threw` 标志 ＋ 据此**一票否决**（不拿半残面板）")
    idx = [i for i, l in enumerate(lines) if "anything_built =" in l and "==" not in l]
    ab = "\n".join(lines[idx[0]:idx[0] + 3]) if idx else ""
    want(bool(ab) and "!builder_threw" in ab,
         "B3 页：`anything_built` 里必须有 `!builder_threw`", ab.strip()[:60] if ab.strip() else "没找到")
    page_at = next((i for i in hits if "reg->builder" in lines[i]), None)
    panel_at = next((i for i in hits if "panel->builder" in lines[i]), None)
    if page_at is not None:
        tail = "\n".join(lines[page_at:page_at + 60])
        want("make_placeholder" in tail, "B4 页：异常之后走**占位页**")
        want("log_line" in tail and "异常" in tail, "B5 页：异常之后**留痕**")
    if panel_at is not None:
        tailp = "\n".join(lines[panel_at:panel_at + 30])
        want("deleteLater" in tailp and "continue;" in tailp,
             "B6 面板：异常之后 `deleteLater` ＋ `continue`（整块丢弃）")
        want("log_line" in tailp and "异常" in tailp and "panel->title" in tailp,
             "B7 面板：留痕要点名 `panel->title` 与 `panel->plugin`")

    print("-" * 66)
    if PROBLEMS:
        print("BUILDER-GUARD fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BUILDER-GUARD ok %d 项（页 ＋ 面板两个 builder 都在 try 里、两类 catch 都在、"
          "异常文本留痕、抛过就整块丢弃）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
