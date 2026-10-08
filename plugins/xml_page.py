# -*- coding: utf-8 -*-
"""**第四页迁移：XML 对比**（原 C++ `xml_page` 的插件版）。

口径逐条照 C++ 页（别自己加戏）：
  · 两个文件夹 + 各自编码下拉；**模式下拉**（显示名/内部键都来自 `api.engine.xml_modes()`，
    内部键才是取值 —— 显示名不许当键）；**筛选下拉**（来自模式的 filters 声明）；
  · 结果表**多列**（列声明来自模式：`rel`/`status`/`lines_a`… 与页面同一份声明）——
    多列走门面的 `tree`（列要点：`tree_set_columns` 换列、`tree_set_root` 填行）；
  · 每行"状态"文案与 C++ 页同一套（一致 / 不一致 / 仅 A / 仅 B + 不可信后缀）；
  · 明细**由引擎渲染**（`xml_inline_detail` / `full_detail` / `full_text_detail`）——
    插件不自己拼一份，免得两处口径分叉；
  · 统计行（一致/不一致/仅 A/仅 B）+ CSV 导出（`xml_render_csv` / `xml_write_csv`，BOM + CRLF）+
    「复制 ID」（`ui.copy_text`）；
  · 任务壳**有取消**：取消在**文件边界**生效（引擎那一份实现里轮询），停下时表里都是完整行。

注册方式：**新 id `xml_py` + 显式 `replaces="xml"`**，标题仍旧名。
"""

PLUGIN_INFO = {"name": "XML 校对（插件版）", "version": "1.0"}
PLUGIN_API = 1


def columns_of(mode):
    """模式声明 → `[[title, width], …]`（门面 `tree`/`tree_set_columns` 要的形状）。"""
    return [[column["heading"], column["width"]] for column in mode["columns"]]


def status_text(kind, result, name_a, name_b):
    """一行状态文案（与 C++ 页 `row_status_text` 同一套；不可信要缀上）。"""
    if kind == "same":
        text = "一致"
    elif kind == "only_a":
        text = "仅 " + name_a
    elif kind == "only_b":
        text = "仅 " + name_b
    else:
        text = "不一致"
    if result.get("untrusted_a") or result.get("untrusted_b"):
        text += "（不可信）"
    return text


def cell_text(row, column, name_a, name_b):
    """一列一格（与 C++ 页 `cell_text` 同口径）—— 结果表与 CSV **共用这一份**。"""
    result = row
    key = column["key"]
    if key == "rel":
        return result.get("rel", "")
    if key == "status":
        return status_text(row.get("kind", ""), result, name_a, name_b)
    if key in ("lines_a", "lines_b", "ids_a", "ids_b"):
        return str(result.get(key, 0))
    if key == "diff_count":
        return str(len(result.get("only_a", [])) + len(result.get("only_b", [])) +
                   len(result.get("text_diff", [])))
    if key == "detail":
        return result.get("_inline_detail", "")
    return ""


def run_compare(api, args):
    """**唯一一份**对比逻辑：界面 worker 与无界面入口都走它。"""
    engine = api.engine
    options = {
        "dir_a": args.get("dir_a", ""),
        "dir_b": args.get("dir_b", ""),
        "enc_a": args.get("enc_a", "auto"),
        "enc_b": args.get("enc_b", "auto"),
        "mode_key": args.get("mode_key", "stats"),
    }
    # JSON 带不了回调：`cancel: true` ⇒ "确定性地第一次问就说停"（门禁验取消语义用它）
    cancel = args.get("cancel")
    if callable(cancel):
        options["cancel"] = cancel
    elif cancel:
        options["cancel"] = lambda: True
    return engine.xml_compare_dirs(options)


def headless(api, args):
    """**无界面入口**：把结果按 CLI 参照的**行格式**打出来（门禁据此逐行对拍）+ 原始报告。"""
    report = run_compare(api, args)
    lines = []
    for row in report["rows"]:
        lines.append("XMLDIRS row rel=%s kind=%s lines_a=%d lines_b=%d ids_a=%d ids_b=%d "
                     "count_diff=%d text_diff=%d only_a=%d only_b=%d consistent=%d untrusted=%d"
                     % (row["rel"], row["kind"], row["lines_a"], row["lines_b"], row["ids_a"],
                        row["ids_b"], len(row["count_diff"]), len(row["text_diff"]),
                        len(row["only_a"]), len(row["only_b"]), 1 if row["consistent"] else 0,
                        1 if (row["untrusted_a"] or row["untrusted_b"]) else 0))
    lines.append("XMLDIRS ok total=%d rows=%d cancelled=%d"
                 % (report["total"], len(report["rows"]), 1 if report["cancelled"] else 0))
    return {"lines": lines, "report": report}


def register(api):
    ui = api.ui
    engine = api.engine
    host = api.host

    modes = engine.xml_modes() or []
    mode_names = [mode["name"] for mode in modes]
    mode_by_name = {mode["name"]: mode for mode in modes}
    encodings = engine.text_encodings()["manual"]
    source_choices = ["auto"] + list(encodings)

    def build():
        pref = "xml"
        dir_a_entry = ui.entry(None, host.pref_get(pref, "dir_a", ""), 60)
        dir_b_entry = ui.entry(None, host.pref_get(pref, "dir_b", ""), 60)
        enc_a_combo = ui.combo(None, source_choices)
        enc_b_combo = ui.combo(None, source_choices)
        mode_combo = ui.combo(None, mode_names, lambda _text: on_mode_change())
        state = {"mode": modes[0] if modes else {"columns": [], "filters": [], "key": "stats"}}
        filter_combo = ui.combo(None, [f["label"] for f in state["mode"]["filters"]] or ["全部"],
                                lambda _text: on_filter_change())
        tree = ui.tree(None, columns_of(state["mode"]))
        detail_head = ui.label(None, "")
        detail_text = ui.text_view(None, 10, True)
        stats = ui.label(None, "就绪")
        log = ui.log_view(None, 4)
        panel = ui.task_panel(None, True)
        rows_state = {"rows": []}

        def name_a():
            text = ui.get_text(dir_a_entry).strip().rstrip("/\\")
            return text.replace("\\", "/").rsplit("/", 1)[-1] if text else "A"

        def name_b():
            text = ui.get_text(dir_b_entry).strip().rstrip("/\\")
            return text.replace("\\", "/").rsplit("/", 1)[-1] if text else "B"

        def current_filter():
            label = ui.combo_value(filter_combo)
            for spec in state["mode"]["filters"]:
                if spec["label"] == label:
                    return spec
            return None

        def visible_rows():
            spec = current_filter()
            if spec is None:
                return rows_state["rows"]
            return [row for row in rows_state["rows"]
                    if api.engine.xml_row_matches(spec, row["kind"], row)]

        def refresh_table():
            rows = visible_rows()
            na, nb = name_a(), name_b()
            nodes = []
            for index, row in enumerate(rows):
                cells = [cell_text(row, column, na, nb) for column in state["mode"]["columns"]]
                nodes.append({"key": "row-%d" % index, "cells": cells})
            ui.tree_set_root(tree, nodes)

        def refresh_stats():
            total = len(rows_state["rows"])
            same = sum(1 for row in rows_state["rows"] if row["kind"] == "same")
            only_a = sum(1 for row in rows_state["rows"] if row["kind"] == "only_a")
            only_b = sum(1 for row in rows_state["rows"] if row["kind"] == "only_b")
            diff = sum(1 for row in rows_state["rows"] if row["kind"] == "diff")
            untrusted = sum(1 for row in rows_state["rows"]
                            if row.get("untrusted_a") or row.get("untrusted_b"))
            ui.set_text(stats, "共 %d 个文件：一致 %d / 不一致 %d / 仅 %s %d / 仅 %s %d"
                        "%s" % (total, same, diff, name_a(), only_a, name_b(), only_b,
                                "（其中不可信 %d）" % untrusted if untrusted else ""))

        def on_mode_change(_text):
            mode = mode_by_name.get(ui.combo_value(mode_combo))
            if mode is None:
                return
            state["mode"] = mode
            ui.tree_set_columns(tree, columns_of(mode))
            labels = [spec["label"] for spec in mode["filters"]] or ["全部"]
            ui.combo_set_items(filter_combo, labels)   # ★ 换选项（第 0 项成为当前项）
            rows_state["rows"] = []
            refresh_table()
            refresh_stats()

        def on_filter_change(_text):
            refresh_table()

        def pick_dir(entry, which):
            picked = ui.ask_directory("选择文件夹 %s" % which)
            if picked:
                ui.set_text(entry, picked)
                host.pref_set(pref, "dir_" + which.lower(), picked)

        def start():
            args = {
                "dir_a": ui.get_text(dir_a_entry).strip(),
                "dir_b": ui.get_text(dir_b_entry).strip(),
                "enc_a": ui.combo_value(enc_a_combo),
                "enc_b": ui.combo_value(enc_b_combo),
                "mode_key": state["mode"]["key"],
            }
            if not args["dir_a"] or not args["dir_b"]:
                ui.set_text(stats, "请先选两个文件夹")
                return

            def worker(progress, message):
                # ★★ 线程契约：worker 在**工作线程**里跑 ⇒ **只许**调 progress/message 两个 sink，
                #   绝不碰控件（碰了就是偶发崩溃；界面刷新交给下面的回主线程回调）。
                progress(0)
                args_with_cancel = dict(args)
                args_with_cancel["cancel"] = ui.task_cancel_requested
                report = run_compare(api, args_with_cancel)
                rows_state["rows"] = report["rows"]
                # 明细由**引擎**渲染（插件只负责摆上界面；这一步是纯数据，可以在工作线程里做）
                na, nb = name_a(), name_b()
                for row in rows_state["rows"]:
                    row["_inline_detail"] = engine.xml_inline_detail(row, na, nb)
                if report["cancelled"]:
                    message("已取消：在文件边界停下（已比较 %d 个文件；表里每行都是完整结果）"
                            % len(report["rows"]))
                else:
                    message("对比完成：%d 个文件（模式 %s）"
                            % (len(report["rows"]), state["mode"]["name"]))
                progress(100)

            if not ui.task_run(panel, "XML 对比", worker):
                ui.set_text(stats, "已有任务在跑，请等它结束或先取消")

        def clear_all():
            rows_state["rows"] = []
            refresh_table()
            refresh_stats()
            ui.set_text(detail_head, "")
            ui.set_text(detail_text, "")

        def on_select(key):
            index = -1
            if key.startswith("row-"):
                try:
                    index = int(key[4:])
                except ValueError:
                    index = -1
            rows = visible_rows()
            if index < 0 or index >= len(rows):
                return
            row = rows[index]
            na, nb = name_a(), name_b()
            ui.set_text(detail_head, "%s（%s）" % (row["rel"],
                                                status_text(row["kind"], row, na, nb)))
            text = (engine.xml_full_text_detail(row, na, nb)
                    if row.get("mode") == "text" else engine.xml_full_detail(row, na, nb))
            ui.set_text(detail_text, text)

        def export_csv():
            path = ui.ask_save_file("导出统计")
            if not path:
                return
            rows = visible_rows()
            na, nb = name_a(), name_b()
            report = {
                "mode_name": state["mode"]["name"],
                "folder_a": ui.get_text(dir_a_entry).strip(),
                "folder_b": ui.get_text(dir_b_entry).strip(),
                "name_a": na, "name_b": nb,
                "enc_a": ui.combo_value(enc_a_combo), "enc_b": ui.combo_value(enc_b_combo),
                "filter_label": ui.combo_value(filter_combo),
                "generated_at": "",
                "columns": [column["heading"] for column in state["mode"]["columns"]],
                "rows": [[cell_text(row, column, na, nb) for column in state["mode"]["columns"]]
                         for row in rows],
                "consistent": sum(1 for row in rows if row["kind"] == "same"),
                "inconsistent": sum(1 for row in rows if row["kind"] != "same"),
                "only_a": sum(1 for row in rows if row["kind"] == "only_a"),
                "only_b": sum(1 for row in rows if row["kind"] == "only_b"),
                "encodings_used": "",
            }
            wrote = engine.xml_write_csv(path, report)
            ui.set_text(stats, "统计已导出：" + path if wrote["ok"]
                        else "导出失败：" + (wrote["error"] or "(无理由)"))

        def copy_selected_id():
            key = ui.tree_selected_key(tree)
            index = -1
            if key.startswith("row-"):
                try:
                    index = int(key[4:])
                except ValueError:
                    index = -1
            rows = visible_rows()
            if index < 0 or index >= len(rows):
                ui.set_text(stats, "请先在结果表里选一行")
                return
            row = rows[index]
            ids = list(row.get("only_a", [])) + list(row.get("only_b", []))
            for cells in row.get("count_diff", []) + row.get("text_diff", []):
                if cells:
                    ids.append(cells[0])
            if not ids:
                ui.set_text(stats, "这一行没有可复制的 ID（一致的行没有差异 ID）")
                return
            # ★★ P16-0：**必须看返回值**。修前不看、紧接着显示"已复制 ID：…"——
            #   而 `copy_text` 那时恒返回 False（门面登记缺口）⇒ 界面对用户**谎报成功**。
            #   零静默红线：失败要说出来，并且把 ID 原文留给用户手动复制。
            if ui.copy_text(ids[0]):
                ui.set_text(stats, "已复制 ID：" + ids[0])
            else:
                ui.set_text(stats, "复制失败（宿主没提供该能力）：" + ids[0])

        ui.label(None, "文件夹 A / B：")
        ui.button(None, "选择 A…", lambda: pick_dir(dir_a_entry, "A"))
        ui.button(None, "选择 B…", lambda: pick_dir(dir_b_entry, "B"))
        ui.label(None, "模式 / 筛选：")
        ui.label(None, "明细（引擎渲染）：")
        ui.button(None, "开始比较", start)
        ui.button(None, "清除", clear_all)
        ui.button(None, "复制 ID", copy_selected_id)
        ui.button(None, "导出统计", export_csv)
        # ★★ P12c-2：任务壳的**回主线程**消息回调 —— 界面刷新只在这里做（worker 里不碰控件）
        def on_task_message(text):
            if text.startswith("已取消") or text.startswith("对比完成"):
                refresh_table()
                refresh_stats()
            ui.set_text(stats, ui.get_text(stats) + " ｜ " + text)

        ui.task_on_message(panel, on_task_message)
        # ★ 选中一行 ⇒ 明细（**引擎渲染**）；回调要在控件都造好之后挂
        ui.tree_on_activate(tree, on_select)
        refresh_stats()

    api.register_tool("xml_py", build, 3, "xml", "XML 校对（插件版）", "XML 校对")
    api.log("XML 校对（插件版）：已注册，replaces=xml")
