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
    shutil.rmtree(work, ignore_errors=True)   # 开场先清（判据只看本轮产物）
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

    # ── ★★ P10a：text 页那一组（第三页迁移用的引擎面）────────────────────────
    mark("id_prefix_default:%s" % engine.text_default_id_prefix())
    bad_source = os.path.join(work, "not_gameplay")
    os.makedirs(bad_source, exist_ok=True)
    mark("validate_bad:%d" % (1 if engine.text_validate_extract(
        {"source_dir": bad_source, "target_dir": out_dir, "prefix": "x"}) else 0))
    mark("validate_missing:%d" % (1 if engine.text_validate_extract(
        {"source_dir": os.path.join(work, "nope"), "target_dir": out_dir, "prefix": "x"}) else 0))
    kind_bad = engine.text_extract_kind(bad_source)
    mark("kind_bad_ok:%d" % (1 if kind_bad["ok"] else 0))
    mark("kind_bad_error:%d" % (1 if kind_bad["error"] else 0))

    scripts_dir = os.path.join(work, "scripts")
    os.makedirs(scripts_dir, exist_ok=True)
    with open(os.path.join(scripts_dir, "a.script"), "w", encoding="cp1251") as handle:
        handle.write('local s = "Привет, сталкер!"\n')
    with open(os.path.join(scripts_dir, "d.ltx"), "w", encoding="cp1251") as handle:
        handle.write('[wpn_akm]\ninv_name = "АКМ"\n; inv_name = Не должно попасть\n')
    kind = engine.text_extract_kind(scripts_dir)
    mark("kind_ok:%s" % kind["kind"])
    mark("validate_good:%d" % (1 if not engine.text_validate_extract(
        {"source_dir": scripts_dir, "target_dir": out_dir, "prefix": "p10a"}) else 0))

    text_out = os.path.join(work, "text_out")
    sub = "scripts" if kind["kind"] == "scripts" else "gameplay"
    # ★ 第二个参数是「该类型的目标子目录的**完整路径**」（引擎口径；传相对名会按
    #   当前工作目录解析 —— 本批实踩：写成 "scripts" 就把仓库自己的 scripts\ 清了）
    stats = engine.text_extract_dir(
        scripts_dir, os.path.join(text_out, sub), text_out,
        {"prefix": "p10a", "id_prefix": "sgtat_p10a_gp_0", "clean": True})
    mark("extract_ok:%d" % (1 if stats["ok"] else 0))
    mark("extract_kind:%s" % stats["kind"])
    mark("extract_files_positive:%d" % (1 if stats["files"] > 0 else 0))
    mark("extract_strings_positive:%d" % (1 if stats["strings"] > 0 else 0))
    mark("extract_xml_name:%s" % stats["texts_xml_name"])
    texts_xml = os.path.join(text_out, stats["texts_xml_name"])
    mark("extract_xml_exists:%d" % (1 if os.path.isfile(texts_xml) else 0))
    rewritten = os.path.join(text_out, sub, "a.script")
    rewritten_text = ""
    if os.path.isfile(rewritten):
        with open(rewritten, "rb") as handle:
            rewritten_text = handle.read().decode("utf-8", "replace")
    mark("extract_rewrote:%d" % (1 if "translate_string" in rewritten_text else 0))
    ltx_out = os.path.join(text_out, sub, "d.ltx")
    ltx_text = ""
    if os.path.isfile(ltx_out):
        with open(ltx_out, "rb") as handle:
            ltx_text = handle.read().decode("utf-8", "replace")
    # ★ 保护性替换那条纪律：注释行里的命中**不许**被改写
    mark("extract_comment_kept:%d" % (1 if "Не должно попасть" in ltx_text else 0))

    # 取消谓词：**第一次问就说停** ⇒ cancelled=True 且 ok=True（取消不是失败）
    cancelled = engine.text_extract_dir(
        scripts_dir, os.path.join(text_out, sub), text_out,
        {"prefix": "p10a", "id_prefix": "sgtat_p10a_gp_0", "clean": False,
         "cancel": lambda: True})
    mark("extract_cancel_flag:%d" % (1 if cancelled["cancelled"] else 0))
    mark("extract_cancel_ok:%d" % (1 if cancelled["ok"] else 0))

    tree = []
    for root_dir, _dirs, files in os.walk(text_out):
        for name in sorted(files):
            rel = os.path.relpath(os.path.join(root_dir, name), text_out).replace("\\", "/")
            tree.append(rel)
    mark("extract_out_tree:%s" % ",".join(sorted(tree)))

    # ── ★★ P11a：xml 页那一组（第四页迁移用的引擎面）────────────────────────
    modes = engine.xml_modes()
    mark("xml_mode_count:%d" % len(modes))
    keys = [mode["key"] for mode in modes]
    mark("xml_mode_keys:%s" % ",".join(keys))
    found_stats = engine.xml_find_mode("stats")
    mark("xml_find_stats:%d" % (1 if found_stats["found"] else 0))
    mark("xml_find_stats_key:%s" % found_stats["mode"]["key"])
    mark("xml_find_text_columns:%d" % len(engine.xml_find_mode("text")["mode"]["columns"]))
    mark("xml_find_unknown:%d" % (1 if engine.xml_find_mode("no-such-mode")["found"] else 0))

    xml_src = os.path.join(work, "xml_in")
    os.makedirs(xml_src, exist_ok=True)
    cp1251_path = os.path.join(xml_src, "side1251.xml")
    with open(cp1251_path, "wb") as handle:
        handle.write(('<?xml version="1.0" encoding="windows-1251"?>\n<string_table>\n'
                      '\t<string id="a"><text>Привет мир</text></string>\n'
                      '</string_table>\n').encode("cp1251"))
    read_back = engine.text_read_bytes(cp1251_path, "windows-1251")
    mark("read_ok:%d" % (1 if read_back["ok"] else 0))
    mark("read_has_text:%d" % (1 if "Привет мир" in read_back["text"] else 0))
    mark("read_encoding:%s" % read_back["used_encoding"])

    text_a = ('<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="b"><text>two</text></string>\n'
              '</string_table>\n')
    text_b = ('<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="a"><text>one again</text></string>\n'
              '\t<string id="b"><text>two changed</text></string>\n'
              '</string_table>\n')
    stats = engine.xml_compare_stats("x.xml", text_a, text_b, "utf-8", "utf-8")
    mark("stats_mode:%s" % stats["mode"])
    mark("stats_consistent:%d" % (1 if stats["consistent"] else 0))
    mark("stats_count_diff:%d" % len(stats["count_diff"]))
    text_cmp = engine.xml_compare_text("x.xml", text_a, text_b, "utf-8", "utf-8")
    mark("text_mode:%s" % text_cmp["mode"])
    mark("text_diff_rows:%d" % len(text_cmp["text_diff"]))
    side = engine.xml_only_one_side("only_in_b.xml", "b")
    mark("one_side_flag:%d" % (1 if side["only_one_side"] else 0))
    mark("one_side_side:%s" % side["only_side"])
    mark("inline_detail_nonempty:%d"
         % (1 if engine.xml_inline_detail(text_cmp, "A", "B") else 0))
    mark("full_detail_nonempty:%d" % (1 if engine.xml_full_detail(stats, "A", "B") else 0))
    mark("full_text_detail_nonempty:%d"
         % (1 if engine.xml_full_text_detail(text_cmp, "A", "B") else 0))

    report = {"mode_name": "ID 文本对比", "folder_a": "a", "folder_b": "b",
              "name_a": "A", "name_b": "B", "enc_a": "utf-8", "enc_b": "utf-8",
              "filter_label": "全部", "generated_at": "2026-10-08 00:00:00",
              "columns": ["rel", "status", "detail"],
              "rows": [["x.xml", "不一致", "a: 次数不同"]],
              "consistent": 0, "inconsistent": 1, "only_a": 0, "only_b": 0,
              "encodings_used": "a=utf-8 b=utf-8"}
    csv_text = engine.xml_render_csv(report)
    mark("csv_bom:%d" % (1 if csv_text.startswith("\ufeff") else 0))
    mark("csv_has_row:%d" % (1 if "x.xml" in csv_text else 0))
    csv_path = os.path.join(work, "report.csv")
    wrote = engine.xml_write_csv(csv_path, report)
    mark("csv_write_ok:%d" % (1 if wrote["ok"] else 0))
    written = b""
    if os.path.isfile(csv_path):
        with open(csv_path, "rb") as handle:
            written = handle.read()
    mark("csv_roundtrip:%d" % (1 if written == csv_text.encode("utf-8") else 0))

    mark("done")
    # ★ 夹具自清：`convert_work` 不留痕（插件目录里只该留 marker —— 门禁按行断言它）
    shutil.rmtree(work, ignore_errors=True)
