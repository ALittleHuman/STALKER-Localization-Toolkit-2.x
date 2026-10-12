# -*- coding: utf-8 -*-
"""无任务壳那条同步路的回归锁（复审计 [High] 的复现/验证）。

背景：`plugins/toolchain_page.py` 的 `_run_off_ui_thread` 里，`panel is None` 那一支
（无界面进程 ⇒ 如实退回同步执行）原来写着上一版残留的 `show_holder`（签名里只有 `show`）
⇒ 重复点长任务按钮就 `NameError` 崩。

判据做法：用**假 api**（`task_panel` 返回 None）**真调一遍**那条路，断言：
  · 返回值 False（"没跑 worker"要如实说）；
  · heavy 被调 **1 次**；
  · 结果**如实落了回调**（这正是原来崩掉的那一句）。
退出码 0=绿 / 1=红 / 2=缺前提。★ 它是"名字解析"之外的第二道：**真执行**。

为什么要有它：`check_toolchain_worker.py` 第一版只做 AST 字符串检查
（"`task_run` 这串在不在"）—— 那种判据**抓不到"名字根本不存在"**，所以 [High] 从那里过去了。
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "plugins" / "toolchain_page.py"


class FakeUi:
    """只实现这条路会碰到的方法；`task_panel` 返回 None ＝ **模拟无任务壳**。"""

    def on_ui_thread(self) -> bool:
        return True

    def task_panel(self, parent, with_cancel):
        return None

    def task_on_message(self, panel, handler) -> bool:
        return False

    def task_run(self, panel, title, worker) -> bool:
        return False


class FakeApi:
    def __init__(self) -> None:
        self.ui = FakeUi()


def main() -> int:
    print("== P17-108 无任务壳那条同步路的回归锁（复审计 [High]）==")
    if not PAGE.is_file():
        print("!! 缺夹具页：%s" % PAGE)
        print("SYNC-PATH skip 缺夹具页")
        return 2
    spec = importlib.util.spec_from_file_location("toolchain_page_under_test", PAGE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    shown: list = []
    calls: list = []
    try:
        rc = module._run_off_ui_thread(
            FakeApi(), None, "回归锁",
            lambda text: shown.append(text),
            lambda message: (calls.append(1), "结果文本")[1])
    except NameError as exc:
        print("   ✗ 那条路抛 NameError：%s" % exc)
        print("SYNC-PATH fail 无壳的同步路又崩了（就是复审计 [High] 的形状）")
        return 1

    ok = (rc is False and len(calls) == 1 and shown == ["结果文本"])
    print("   %s rc=%s heavy 调用=%d show 收到=%r"
          % ("✓" if ok else "✗", rc, len(calls), shown))
    if not ok:
        print("SYNC-PATH fail 无壳那条路的结果没如实落回调")
        return 1
    print("SYNC-PATH ok 无任务壳那条路不再 NameError，且结果如实落回调")
    return 0


if __name__ == "__main__":
    sys.exit(main())
