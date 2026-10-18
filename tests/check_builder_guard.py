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
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
HUB = ROOT / "src" / "shell" / "hub.cpp"

CALL = "built = reg->builder(container);"

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

    print("== S-02：插件页 builder 的 C++ 异常护栏（静态锁）==")
    if not src_path.is_file():
        print("BUILDER-GUARD skip 缺 %s" % src_path)
        return 2
    lines = src_path.read_text(encoding="utf-8", errors="replace").splitlines()

    # ★ 注释行里"引用旧代码"不算调用点（H-01/H-04 的教训：文档/注释引用代码不该锁住自己）
    hits = [i for i, l in enumerate(lines)
            if CALL in l and not l.lstrip().startswith("//")]
    want(len(hits) == 1, "A1 只有**一处** builder 调用点（多处＝护栏可能只包了一半）",
         "命中 %d 处" % len(hits))
    if len(hits) != 1:
        print("-" * 66)
        print("BUILDER-GUARD fail %d 项不成立" % len(PROBLEMS))
        return 1
    at = hits[0]
    # 免责：注释行里出现同样的字样不算调用点
    if lines[at].lstrip().startswith("//"):
        want(False, "A1b 命中行是注释、不是真的调用点", lines[at].strip()[:60])
        print("-" * 66)
        print("BUILDER-GUARD fail %d 项不成立" % len(PROBLEMS))
        return 1

    back = "\n".join(lines[max(0, at - 3):at])
    want("try {" in back, "A2 调用点上方 3 行内必须有 `try {`（裸调用＝异常直接穿出去）",
         back.strip().splitlines()[-1][:60] if back.strip() else "空")
    fwd = "\n".join(lines[at:at + 8])
    want("catch (const std::exception&" in fwd, "A3 调用点下方 8 行内必须接 `catch (const std::exception&`")
    want("catch (...)" in fwd, "A4 还要有 `catch (...)`（非 std::exception 的 C++ 异常也别穿出去）")
    want("e.what()" in fwd, "A5 异常文本要留下（`what()`）—— 零静默：用户得知道是哪个插件、为什么")

    text = "\n".join(lines)
    want("const bool builder_threw = !builder_exception.empty();" in text,
         "B1 有显式的 `builder_threw` 标志（不是靠 `built == nullptr` 猜）")
    idx = [i for i, l in enumerate(lines) if "anything_built =" in l and "==" not in l]
    ab = "\n".join(lines[idx[0]:idx[0] + 3]) if idx else ""
    want(bool(ab) and "!builder_threw" in ab,
         "B2 **抛过异常 ⇒ 一票否决**：`anything_built` 里必须有 `!builder_threw`"
         "（抛在半途的 builder 会留下半残页面）",
         ab.strip().splitlines()[-1][:70] if ab.strip() else "没找到赋值")
    tail = "\n".join(lines[at:at + 60])
    want("make_placeholder" in tail, "B3 异常之后走**占位页**（与「没造出控件」同一条善后）")
    want("log_line" in tail and "异常" in tail, "B4 异常之后**留痕**（log_line 说明抛了异常）")

    print("-" * 66)
    if PROBLEMS:
        print("BUILDER-GUARD fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BUILDER-GUARD ok %d 项（builder 调用在 try 里、两类 catch 都在、异常文本留痕、"
          "抛过就一票否决走占位页；★ 行为锁待 panel-builder 夹具）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
