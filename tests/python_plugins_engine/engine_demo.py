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

    # ── ★★ P11b：xml 目录对比（第四页真正跑起来要的那一块）────────────────────
    dir_a = os.path.join(work, "cmp_a")
    dir_b = os.path.join(work, "cmp_b")
    os.makedirs(os.path.join(dir_a, "sub"), exist_ok=True)
    os.makedirs(os.path.join(dir_b, "sub"), exist_ok=True)
    same = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
            '\t<string id="a"><text>same</text></string>\n</string_table>\n')
    diff_a = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="b"><text>two</text></string>\n</string_table>\n')
    diff_b = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="a"><text>one again</text></string>\n'
              '\t<string id="b"><text>two changed</text></string>\n</string_table>\n')
    for root_dir in (dir_a, dir_b):
        with open(os.path.join(root_dir, "same.xml"), "w", encoding="utf-8") as handle:
            handle.write(same)
        with open(os.path.join(root_dir, "sub", "nested.xml"), "w", encoding="utf-8") as handle:
            handle.write(same)
    with open(os.path.join(dir_a, "diff.xml"), "w", encoding="utf-8") as handle:
        handle.write(diff_a)
    with open(os.path.join(dir_b, "diff.xml"), "w", encoding="utf-8") as handle:
        handle.write(diff_b)
    with open(os.path.join(dir_a, "only_a.xml"), "w", encoding="utf-8") as handle:
        handle.write(same)
    with open(os.path.join(dir_b, "only_b.xml"), "w", encoding="utf-8") as handle:
        handle.write(same)

    report = engine.xml_compare_dirs({"dir_a": dir_a, "dir_b": dir_b, "mode_key": "stats",
                                      "enc_a": "auto", "enc_b": "auto"})
    mark("dirs_total:%d" % report["total"])
    mark("dirs_rows:%d" % len(report["rows"]))
    kinds = sorted(row["kind"] for row in report["rows"])
    mark("dirs_kinds:%s" % ",".join(kinds))
    by_rel = {row["rel"]: row for row in report["rows"]}
    mark("dirs_same_kind:%s" % by_rel.get("same.xml", {}).get("kind"))
    mark("dirs_nested_kind:%s" % by_rel.get("sub/nested.xml", {}).get("kind"))
    mark("dirs_diff_count:%d" % len(by_rel.get("diff.xml", {}).get("count_diff", [])))
    mark("dirs_only_a_kind:%s" % by_rel.get("only_a.xml", {}).get("kind"))
    mark("dirs_only_b_kind:%s" % by_rel.get("only_b.xml", {}).get("kind"))
    diff_row = by_rel.get("diff.xml", {})
    mark("dirs_match_diff:%d"
         % (1 if engine.xml_row_matches({"label": "只看不一致", "row_kinds": ["diff"]},
                                        diff_row.get("kind", ""), diff_row) else 0))
    same_row = by_rel.get("same.xml", {})
    mark("dirs_match_same:%d"
         % (1 if engine.xml_row_matches({"label": "只看不一致", "row_kinds": ["diff"]},
                                        same_row.get("kind", ""), same_row) else 0))

    # ★ 取消谓词必须是**可调用对象**（传 True 会被静默忽略：那不是回调）
    cancelled = engine.xml_compare_dirs({"dir_a": dir_a, "dir_b": dir_b, "mode_key": "stats",
                                         "cancel": (lambda: True)})
    mark("dirs_cancel_flag:%d" % (1 if cancelled["cancelled"] else 0))
    mark("dirs_cancel_rows:%d" % len(cancelled["rows"]))

    # ── ★★ P12a：video 页那一组（第五页迁移用的引擎面）────────────────────────
    probe_json = ('{"streams":[{"codec_type":"video","codec_name":"h264","width":1280,'
                  '"height":720,"avg_frame_rate":"25/1","bit_rate":"1500000"},'
                  '{"codec_type":"audio","codec_name":"vorbis","sample_rate":"44100",'
                  '"channels":2,"bit_rate":"128000"}],"format":{"duration":"120.5",'
                  '"size":"10485760"}}')
    media = engine.video_parse_ffprobe(probe_json)
    mark("video_parsed:%d" % (1 if media["parsed"] else 0))
    mark("video_has_video:%d" % (1 if media["has_video"] else 0))
    mark("video_has_audio:%d" % (1 if media["has_audio"] else 0))
    mark("video_width:%d" % media["video"]["width"])
    mark("video_height:%d" % media["video"]["height"])
    mark("video_fps_positive:%d" % (1 if media["video"]["fps"] > 0 else 0))
    mark("video_codec:%s" % media["video"]["codec"])
    mark("video_size_mb_positive:%d" % (1 if media["size_mb"] > 0 else 0))

    encoders_text = ("Encoders:\n"
                     " V..... libx264              H.264 / AVC (codec h264)\n"
                     " V..... mpeg4                MPEG-4 part 2\n"
                     " A..... libvorbis            Vorbis\n")
    encoders = engine.video_parse_encoders(encoders_text)
    mark("video_encoders:%s" % ",".join(encoders))

    source = {"parsed": True, "has_video": True, "has_audio": True,
              "video": {"codec": "h264", "width": 1920, "height": 1080, "fps": 30.0,
                        "bitrate": 4000000},
              "audio": {"codec": "vorbis", "sample_rate": 44100, "channels": 2,
                        "bitrate": 128000},
              "duration": 60.0, "size_mb": 40.0}
    reference = {"parsed": True, "has_video": True, "has_audio": True,
                 "video": {"codec": "mpeg4", "width": 640, "height": 480, "fps": 25.0,
                           "bitrate": 900000},
                 "audio": {"codec": "vorbis", "sample_rate": 22050, "channels": 1,
                           "bitrate": 64000},
                 "duration": 60.0, "size_mb": 10.0}
    request = {"ffmpeg": "ffmpeg", "input": "in.avi", "output": "out.ogm",
               "has_reference": True, "reference_path": "ref.ogm",
               "reference": reference, "source": source,
               "available_encoders": encoders}
    plan = engine.video_plan_transcode(request)
    mark("video_argv_nonempty:%d" % (1 if plan["argv"] else 0))
    mark("video_argv_has_input:%d" % (1 if "-i" in plan["argv"] else 0))
    mark("video_argv_has_output:%d" % (1 if plan["argv"][-1] == "out.ogm" else 0))
    mark("video_encoder:%s" % plan["video_encoder"])
    mark("video_scale_applied:%d" % (1 if plan["scale_applied"] else 0))
    mark("video_fps_applied:%d" % (1 if plan["fps_applied"] else 0))
    mark("video_audio_fields:%d"
         % (1 if plan["audio_sample_rate"] > 0 and plan["audio_channels"] > 0 else 0))

    same_as_source = engine.video_check_output_conflict(
        {"input": "in.avi", "output": "in.avi", "source": source})
    mark("video_conflict_source:%s" % same_as_source["conflict"])
    mark("video_conflict_reason_nonempty:%d" % (1 if same_as_source["reason"] else 0))
    same_as_reference = engine.video_check_output_conflict(
        {"input": "in.avi", "output": "ref.ogm", "has_reference": True,
         "reference_path": "ref.ogm", "reference": reference, "source": source})
    mark("video_conflict_reference:%s" % same_as_reference["conflict"])
    clean = engine.video_check_output_conflict(
        {"input": "in.avi", "output": "brand_new.ogm", "source": source})
    mark("video_conflict_none:%s" % clean["conflict"])

    sample = engine.video_progress_from_line("frame= 100 time=00:00:10.00 bitrate=1k", 100.0)
    mark("video_progress_matched:%d" % (1 if sample["matched"] else 0))
    mark("video_progress_percent_10:%d" % (1 if 9.5 <= sample["percent"] <= 10.5 else 0))
    capped = engine.video_progress_from_line("frame= 1 time=00:00:10.00", 0.2)
    mark("video_progress_capped:%d" % (1 if capped["percent"] <= 99.5 else 0))
    missing = engine.video_progress_from_line("no time here", 100.0)
    mark("video_progress_unmatched:%d" % (1 if not missing["matched"] else 0))

    mark("video_major_version:%d" % engine.video_major_version("ffmpeg version 6.1.1 Copyright"))
    # 引擎口径（读码得到）：**只**拒 major==8（8.x 的 theora 对 STALKER OGM 有 bug），
    #   不是"拒太老" —— 我第一版猜错了，夹具 marker 当场把它照出来。
    mark("video_version_rejected_8:%d"
         % (1 if engine.video_version_rejected("ffmpeg version 8.0") else 0))
    mark("video_version_ok_6:%d"
         % (1 if not engine.video_version_rejected("ffmpeg version 6.1.1") else 0))

    # ── ★★ P12b：宿主服务「跑进程」（第五页视频转码要用；语义唯一一份在引擎）────
    import time as _time

    echo_lines = []
    echo = api.host.run_process({"program": "cmd", "args": ["/c", "echo hello"],
                                 "on_stdout": echo_lines.append})
    mark("proc_echo_started:%d" % (1 if echo["started"] else 0))
    mark("proc_echo_exit:%d" % echo["exit_code"])
    mark("proc_echo_line:%d" % (1 if any("hello" in line for line in echo_lines) else 0))
    mark("proc_echo_normal:%d" % (1 if echo["normal_exit"] else 0))

    failed = api.host.run_process({"program": "cmd", "args": ["/c", "exit 3"]})
    mark("proc_exit3_code:%d" % failed["exit_code"])
    mark("proc_exit3_normal:%d" % (1 if failed["normal_exit"] else 0))

    err_lines = []
    stderr_run = api.host.run_process({"program": "cmd",
                                       "args": ["/c", "echo problem 1>&2"],
                                       "on_stderr": err_lines.append})
    mark("proc_stderr_tail:%d" % (1 if "problem" in stderr_run["stderr_tail"] else 0))
    mark("proc_stderr_lines:%d" % (1 if stderr_run["stderr_lines"] > 0 else 0))

    # 取消：孙进程要活 20 秒；谓词第一次问就说停 ⇒
    #   ① 立刻回来；② **整棵树**真停（取消后 `tasklist` 里不该还有 ping.exe）
    started_at = _time.time()
    cancelled = api.host.run_process({"program": "cmd",
                                      "args": ["/c", "ping -n 20 127.0.0.1"],
                                      "should_stop": (lambda: True)})
    elapsed = _time.time() - started_at
    mark("proc_cancel_flag:%d" % (1 if cancelled["cancelled"] else 0))
    mark("proc_cancel_timeout:%d" % (1 if cancelled["cancel_timeout"] else 0))
    mark("proc_cancel_fast:%d" % (1 if elapsed < 3.0 else 0))

    listing = []
    api.host.run_process({"program": "cmd",
                          "args": ["/c", "tasklist /FI \"IMAGENAME eq ping.exe\" /NH"],
                          "on_stdout": listing.append})
    orphan = any("ping.exe" in line for line in listing)
    mark("proc_cancel_orphan:%d" % (1 if orphan else 0))

    missing = api.host.run_process({"program": "definitely_no_such_program_xyz"})
    mark("proc_missing_started:%d" % (1 if missing["started"] else 0))
    mark("proc_missing_error:%d" % (1 if missing["error"] else 0))

    # ── ★★ P14-0a：fs 面第一批（纯函数那组）───────────────────────────────────
    formats = engine.fs_formats()
    mark("fs_formats_count:%d" % len(formats))
    keys = [row["key"] for row in formats]
    mark("fs_has_auto:%d" % (1 if any(row["is_auto"] for row in formats) else 0))
    mark("fs_has_sqfs:%d" % (1 if any(row["is_sqfs"] for row in formats) else 0))
    mark("fs_format_count_positive:%d" % (1 if len(formats) > 3 else 0))

    xdb = engine.fs_format_to_db("xdb")
    mark("fs_xdb_is_db:%d" % (1 if xdb["is_db"] else 0))
    mark("fs_xdb_key:%s" % xdb["db_key"])
    mark("fs_xdb_name_nonempty:%d" % (1 if xdb["db_name"] else 0))
    mark("fs_auto_is_db:%d" % (1 if engine.fs_format_to_db("auto")["is_db"] else 0))
    mark("fs_auto_is_auto:%d" % (1 if engine.fs_format_is_auto("auto") else 0))
    mark("fs_sqfs_is_sqfs:%d" % (1 if engine.fs_format_is_sqfs("sqfs") else 0))

    mark("fs_is_package_db0:%d" % (1 if engine.fs_is_package_name("a.db0") else 0))
    mark("fs_is_package_txt:%d" % (1 if engine.fs_is_package_name("readme.txt") else 0))
    mark("fs_pack_ext_sqfs:%s" % engine.fs_pack_ext_for("sqfs"))
    mark("fs_pack_ext_xdb:%s" % engine.fs_pack_ext_for("xdb"))

    shelled = engine.fs_preview_layout("ai.db", [
        {"path": "ai/1.txt", "is_dir": False, "size": 3},
        {"path": "ai/sub", "is_dir": True, "size": 0},
    ])
    mark("fs_preview_rows:%d" % len(shelled))
    # 文档明写的保证：**自带壳的包**（条目已经带 `ai/…`）⇒ 路径原样（不叠第二层）
    shelled_paths = [row["path"] for row in shelled]
    mark("fs_preview_shelled_exact:%d"
         % (1 if shelled_paths == ["ai/1.txt", "ai/sub"] else 0))

    safe_bad = engine.fs_safe_out_path("out", "../evil.txt")
    mark("fs_safe_bad_ok:%d" % (1 if safe_bad["ok"] else 0))
    mark("fs_safe_bad_reason:%d" % (1 if safe_bad["reason"] else 0))
    safe_ok = engine.fs_safe_out_path("out", "a/b.txt")
    mark("fs_safe_ok_ok:%d" % (1 if safe_ok["ok"] else 0))

    target = engine.fs_unpack_target_dir("root", "pkg/ai.db", ["ai/1.txt"])
    mark("fs_unpack_self_shell:%d" % (1 if target in ("root", "root/") else 0))
    plan = engine.fs_plan_unpack_targets("root", [
        {"package": "pkg/ai.db", "entries": ["ai/1.txt"]},
        {"package": "pkg/plain.db", "entries": ["xr/a.txt"]},
    ])
    mark("fs_plan_rows:%d" % len(plan))

    pack = engine.fs_resolve_pack_target("", "base", "sqfs")
    mark("fs_pack_target_ext:%d" % (1 if pack["path"].lower().endswith(".sqfs") else 0))

    # ── ★★ P14-0b：fs 面第二批（条目与合并树）───────────────────────────────
    loose_dir = os.path.join(work, "loose")
    os.makedirs(os.path.join(loose_dir, "sub"), exist_ok=True)
    with open(os.path.join(loose_dir, "one.txt"), "w", encoding="utf-8") as handle:
        handle.write("1")
    with open(os.path.join(loose_dir, "sub", "two.txt"), "w", encoding="utf-8") as handle:
        handle.write("22")
    loose = engine.fs_loose_entries(loose_dir)
    mark("loose_count:%d" % len(loose["entries"]))
    mark("loose_error_empty:%d" % (1 if not loose["error"] else 0))
    mark("loose_has_sub:%d" % (1 if any(r["is_dir"] for r in loose["entries"]) else 0))

    # 文档口径：按 **basename** 升序（同名按原顺序，稳定）—— 不是按整路径
    inputs = ["b/x.db", "a/y.db", "b/a.db"]
    order = engine.fs_sort_sources_by_basename(inputs)
    mark("tree_sort_basename:%d"
         % (1 if [inputs[i] for i in order] == ["b/a.db", "b/x.db", "a/y.db"] else 0))

    sources = [
        {"source": "a.db", "entries": [
            {"path": "xr/1.txt", "is_dir": False, "size": 10},
            {"path": "xr/2.txt", "is_dir": False, "size": 20},
            {"path": "only_a.txt", "is_dir": False, "size": 1}]},
        {"source": "b.db", "entries": [
            {"path": "xr/1.txt", "is_dir": False, "size": 11},
            {"path": "b_only.txt", "is_dir": False, "size": 2}]},
    ]
    handle_id = engine.fs_tree_build(sources)
    mark("tree_handle_positive:%d" % (1 if handle_id > 0 else 0))
    mark("tree_sources:%s" % ",".join(engine.fs_tree_sources(handle_id)))
    rows = engine.fs_tree_flatten(handle_id)
    by_path = {row["path"]: row for row in rows}
    mark("tree_has_files:%d" % (1 if all(p in by_path for p in
                                         ("xr/1.txt", "xr/2.txt", "only_a.txt", "b_only.txt")) else 0))
    mark("tree_later_source_wins:%d" % (1 if by_path.get("xr/1.txt", {}).get("source_index") == 1
                                        else 0))
    mark("tree_conflict_positive:%d" % (1 if engine.fs_tree_conflict_count(handle_id) >= 1 else 0))
    mark("tree_checked_zero:%d" % (1 if engine.fs_tree_checked_file_count(handle_id) == 0 else 0))

    before = engine.fs_tree_checked_file_count(handle_id)
    changed = engine.fs_tree_toggle_source(handle_id, 1, True)
    after = engine.fs_tree_checked_file_count(handle_id)
    mark("tree_toggle_changed_positive:%d" % (1 if changed > 0 else 0))
    mark("tree_toggle_raised:%d" % (1 if after > before else 0))
    snapshot = engine.fs_tree_checked_paths(handle_id)
    mark("tree_snapshot_nonempty:%d" % (1 if snapshot else 0))
    engine.fs_tree_toggle_all(handle_id, False)
    mark("tree_cleared:%d" % (1 if engine.fs_tree_checked_file_count(handle_id) == 0 else 0))
    applied = engine.fs_tree_apply_checked_paths(handle_id, snapshot)
    restored = engine.fs_tree_checked_file_count(handle_id)
    mark("tree_roundtrip:%d" % (1 if applied == len(snapshot) and restored == after else 0))
    mark("tree_toggle_dir_ok:%d"
         % (1 if (engine.fs_tree_toggle_dir(handle_id, "xr", False) or True) else 0))
    state = engine.fs_tree_source_state(handle_id, 1)
    mark("tree_source_state_files:%d" % (1 if state["files"] > 0 else 0))
    found = engine.fs_tree_find(handle_id, "xr/1.txt")
    mark("tree_find_found:%d" % (1 if found["found"] else 0))
    mark("tree_find_missing:%d" % (1 if not engine.fs_tree_find(handle_id, "nope.txt")["found"]
                                   else 0))
    engine.fs_tree_free(handle_id)
    mark("tree_freed:%d" % (1 if engine.fs_tree_flatten(handle_id) == [] else 0))

    # ── ★★ P14-0c：fs 面第三批（读条目/闸门/收集/解包）──────────────────────
    crypto_dir = engine.fs_decrypt_output_dir()
    mark("gate_dir_nonempty:%d" % (1 if crypto_dir else 0))

    gate_ok = engine.fs_gate_path(os.path.join(work, "loose", "one.txt"))
    mark("gate_plain_ok:%d" % (1 if gate_ok["ok"] else 0))
    mark("gate_plain_same_path:%d"
         % (1 if gate_ok["path"].replace("\\", "/").endswith("loose/one.txt") else 0))

    golden_db = os.path.join("tests", "golden", "db", "synth_xdb.db")
    listed = engine.fs_package_entries(golden_db, "xdb")
    mark("pkg_entries_positive:%d" % (1 if len(listed["entries"]) > 0 else 0))
    mark("pkg_error_empty:%d" % (1 if not listed["error"] else 0))
    missing = engine.fs_package_entries(os.path.join(work, "nope.db"), "xdb")
    mark("pkg_missing_error:%d" % (1 if missing["error"] else 0))

    collect = engine.fs_collect_pack_files(os.path.join(work, "loose"),
                                           [os.path.join(work, "loose")])
    mark("pack_collect_items:%d" % len(collect["items"]))
    mark("pack_collect_sorted:%d"
         % (1 if [row["rel"] for row in collect["items"]]
              == sorted(row["rel"] for row in collect["items"]) else 0))

    unpack_root = os.path.join(work, "unpacked")
    extract = engine.fs_extract_package(golden_db, unpack_root, "all", None, "xdb")
    mark("extract_ok:%d" % (1 if extract["ok"] else 0))
    mark("extract_files_positive:%d" % (1 if extract["files_written"] > 0 else 0))
    mark("extract_bytes_positive:%d" % (1 if extract["bytes_written"] > 0 else 0))
    mark("extract_no_failures:%d" % (1 if extract["failure_count"] == 0 else 0))
    mark("extract_written_positive:%d" % (1 if extract["written_count"] > 0 else 0))
    second = engine.fs_extract_package(golden_db, unpack_root, "all", None, "xdb")
    mark("extract_overwrite_visible:%d" % (1 if second["overwritten_count"] > 0 else 0))

    mark("done")
    # ★ 夹具自清：`convert_work` 不留痕（插件目录里只该留 marker —— 门禁按行断言它）
    shutil.rmtree(work, ignore_errors=True)
