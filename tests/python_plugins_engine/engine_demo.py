# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（**引擎调用门面**，方案 3 的第一批能力）。

它验的是"插件真的能通过门面调引擎、拿到的是 Python 友好的值"：
  · `api.engine.font_size_slots()` —— 字号槽位表（引擎的 `slot_count/slot_size`）；
  · `api.engine.font_state_marks()` —— 四态文案（**从引擎拿**，插件不抄中文）；
  · `api.engine.font_check(path)` —— 真字体的四态检查（字体路径由 `XRKIT_ENGINE_FONT` 给；
    没给就记 `check_skipped`，由门禁按"外部前提"记账，**不假装通过**）；
  · ★**线程纪律的另一半**：`api.engine.*` **允许在工作线程上调用**（与 `api.ui.*` 相反）——
    这里真起一个线程调一次，证明重活可以放进任务壳的 worker 里。

标记落点：`<plugin_dir>/engine_marker.txt`（门禁按行断言）。
"""

import os
import threading

PLUGIN_INFO = {"name": "引擎门面夹具", "version": "1.0"}


def register(api):
    def mark(text):
        with open(os.path.join(api.plugin_dir, "engine_marker.txt"), "a", encoding="utf-8") as handle:
            handle.write(text + "\n")

    engine = api.engine
    slots = engine.font_size_slots()
    mark("slots:%s" % ",".join(str(size) for size in slots))
    mark("slot_count:%d" % len(slots))

    marks = engine.font_state_marks()
    mark("marks_ok:%s" % marks["ok"])
    mark("marks_japanese:%s" % marks["japanese"])
    mark("marks_missing:%s" % marks["missing"])

    font = os.environ.get("XRKIT_ENGINE_FONT", "")
    if font:
        check = engine.font_check(font)
        mark("check_state:%s" % check["state"])
        mark("check_mark:%s" % check["mark"])
        mark("check_missing_count:%d" % len(check["missing"]))
    else:
        mark("check_skipped:no-font")

    # ★ 引擎调用**允许**在工作线程上（`api.ui.*` 不允许）—— 真起一个线程验一次
    def worker():
        inner = engine.font_size_slots()
        mark("thread_slots:%d" % len(inner))

    thread = threading.Thread(target=worker)
    thread.start()
    thread.join()
    mark("done")
