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
import shutil
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

    # ── ★★ P9a：convert 页那一组（第二页迁移用的引擎面）──────────────────────
    encodings = engine.text_encodings()
    mark("enc_manual_count:%d" % len(encodings["manual"]))
    mark("enc_manual_head:%s" % ",".join(encodings["manual"][:3]))
    mark("enc_target_only:%s" % ",".join(encodings["target_only"]))
    mark("duration_3661:%s" % engine.text_format_duration(3661))

    work = os.path.join(api.plugin_dir, "convert_work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    original = ('<?xml version="1.0" encoding="windows-1251"?>\n<string_table>\n'
                '\t<string id="a"><text>Привет мир</text></string>\n</string_table>\n'
                ).encode("cp1251")
    source = os.path.join(work, "a.xml")
    with open(source, "wb") as handle:
        handle.write(original)

    out_dir = os.path.join(work, "out")
    messages = []
    outcome = engine.text_convert_batch(
        {"source": work, "output_dir": out_dir, "extensions": [".xml"], "recursive": True,
         "backup": True, "source_encoding": "windows-1251", "target_encoding": "utf-8"},
        {"message": messages.append})
    converted = os.path.join(out_dir, "a.xml")
    converted_bytes = b""
    if os.path.isfile(converted):
        with open(converted, "rb") as handle:
            converted_bytes = handle.read()
    expected = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
                '\t<string id="a"><text>Привет мир</text></string>\n</string_table>\n'
                ).encode("utf-8")
    mark("convert_selected:%d" % outcome["selected"])
    mark("convert_success:%d" % outcome["success"])
    mark("convert_exit:%d" % outcome["exit_code"])
    mark("convert_exists:%d" % (1 if os.path.isfile(converted) else 0))
    mark("convert_bytes_ok:%d" % (1 if converted_bytes == expected else 0))
    mark("convert_messages:%d" % (1 if messages else 0))

    # 备份三件：原地转换（`output_dir=""`）+ 勾了备份 ⇒ 生成 `backup/a.xml.bak`
    inplace = engine.text_convert_batch(
        {"source": work, "output_dir": "", "extensions": [".xml"], "recursive": True,
         "backup": True, "source_encoding": "windows-1251", "target_encoding": "utf-8"})
    backup = os.path.join(work, "backup", "a.xml.bak")
    mark("backup_count:%d" % inplace["backed_up"])
    mark("backup_exists:%d" % (1 if os.path.isfile(backup) else 0))
    restored = engine.text_restore_backups(
        {"source_root": work, "recursive": True, "extensions": [".xml"]})
    mark("restore_ok:%d" % restored["ok"])
    mark("restore_failed:%d" % restored["failed"])
    with open(source, "rb") as handle:
        back = handle.read()
    mark("restore_bytes_ok:%d" % (1 if back == original else 0))
    cleared = engine.text_clear_backups(
        {"source_root": work, "recursive": True, "extensions": [".xml"]})
    mark("clear_ok:%d" % cleared["ok"])
    mark("clear_left:%d" % (1 if os.path.isfile(backup) else 0))

    stats_path = os.path.join(work, "stats.csv")
    wrote = engine.text_write_stats_csv(
        stats_path, {"generated_at": "2026-10-08 00:00:00", "source": work, "single_file": False,
                     "output_dir": out_dir, "source_enc": "windows-1251", "target_enc": "utf-8",
                     "candidates": 1, "processed": 1, "success": 1, "skipped": 0, "failed": 0,
                     "duration": "0:00:01"})
    mark("stats_ok:%d" % (1 if wrote["ok"] else 0))
    head = b""
    if os.path.isfile(stats_path):
        with open(stats_path, "rb") as handle:
            head = handle.read(3)
    mark("stats_bom:%d" % (1 if head == b"\xef\xbb\xbf" else 0))

    mark("done")
    # ★ 夹具自清：`convert_work` 不留痕（插件目录里只该留 marker —— 门禁按行断言它）
    shutil.rmtree(work, ignore_errors=True)
