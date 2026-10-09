# -*- coding: utf-8 -*-
"""**第二页迁移：编码转换**（原 C++ `convert_page` 的插件版）。

口径逐条照 C++ 页（别自己加戏）：
  · 源/目标编码：源侧 = `auto` + 引擎那张共用表；目标侧 = 共用表 + **ascii**（只在目标侧）；
  · 扩展名（默认 `.xml`）/ 递归（默认开）/ 备份（默认开）/ 排除模式；
  · ★**输出目录非空 ⇒ 「备份」自动取消勾选并禁用**（备份只对"原地转换"有意义，1.x `on_output_dir_change`）；
  · 扫描台账那一行（**零静默**：递归几层 / 扫到多少 / 各自为什么被刷掉）来自引擎的计数，不自己重算；
  · 任务壳：**有取消**（1.x 会先确认 ⇒ 走宿主 `task_panel(with_cancel=True)`）；
    「暂停/继续」是**页面自己的状态机** —— 靠 `hooks["gate"]` 卡住引擎（返回 False ⇒ 引擎停下）；
  · 六颗按钮：开始转换 / 暂停(继续) / 恢复备份 / 清空备份 / 打开输出 / 导出统计；
  · 状态文案与 C++ 页同一套（"请先选源路径" / "请输入文件扩展名，例如 .xml,.txt" / 完成后的计数）。

注册方式：**新 id `convert_py` + 显式 `replaces="convert"`**，标题仍是原名的"编码转换"。
"""

PLUGIN_INFO = {"name": "编码转换（插件版）", "version": "1.0"}
PLUGIN_API = 1

DEFAULT_EXT = ".xml"


# ── 无界面入口也在用的那份逻辑 ────────────────────────────────────────────────
def batch_options(args):
    """把（界面的 / 无界面的）参数整理成引擎要的 options dict（**唯一一份**）。"""
    exts = args.get("extensions") or [DEFAULT_EXT]
    return {
        "source": args.get("source", ""),
        "output_dir": args.get("output_dir", ""),
        "extensions": list(exts),
        "recursive": bool(args.get("recursive", True)),
        "backup": bool(args.get("backup", True)),
        "exclude_patterns": args.get("exclude_patterns", ""),
        "source_encoding": args.get("source_encoding", "auto"),
        "target_encoding": args.get("target_encoding", "utf-8"),
    }


def backup_options(args):
    exts = args.get("extensions") or [DEFAULT_EXT]
    return {
        "source_root": args.get("source_root", args.get("source", "")),
        "recursive": bool(args.get("recursive", True)),
        "extensions": list(exts),
    }


def scan_line(outcome):
    """扫描台账那一行（计数**全部来自引擎**，这里只排版）。"""
    if outcome["single_file"]:
        return "扫描：单个文件"
    text = "扫描：递归 %d 层，目录 %d 个，文件 %d 个 ⇒ 选中 %d" % (
        outcome["depth"], outcome["dirs"], outcome["files_seen"], outcome["selected"])
    text += "，后缀不符 %d" % outcome["skipped_ext"]
    text += "，命中排除 %d" % outcome["skipped_excluded"]
    text += "，备份区 %d" % outcome["skipped_backup"]
    if not outcome["skipped_depth"] == 0:
        text += "，非递归挡住 %d" % outcome["skipped_depth"]
    return text


def result_line(outcome):
    """完成后的那一行（口径同 C++ 页：失败/取消/全成三种说法）。"""
    qualifier = ""
    if outcome["low_confidence"] or outcome["override_decl"] or outcome["used_mismatch"]:
        qualifier = "（低置信 %d / 声明被否决 %d / 判定与实用不一致 %d）" % (
            outcome["low_confidence"], outcome["override_decl"], outcome["used_mismatch"])
    counts = "成功 %d / 跳过 %d / 失败 %d" % (outcome["success"], outcome["skipped"],
                                            outcome["failed"])
    if outcome["exit_code"] != 0:
        return "完成，但有失败：" + counts + qualifier
    if outcome["cancelled"]:
        return outcome["stop_reason"]
    return "全部完成：" + counts + qualifier


def headless(api, args):
    """**无界面入口**（`xrkit_cli --py-plugin-call plugins/convert_page.py <json>`）。

    门禁判"插件产物与引擎参照逐字节一致"走这条（不需要界面）。
    """
    engine = api.engine
    action = args.get("action", "convert")
    if action == "restore":
        return engine.text_restore_backups(backup_options(args), {})
    if action == "clear":
        return engine.text_clear_backups(backup_options(args), {})
    return engine.text_convert_batch(batch_options(args), {})


def register(api):
    ui = api.ui
    engine = api.engine
    host = api.host

    encodings = engine.text_encodings()
    source_choices = ["auto"] + list(encodings["manual"])
    target_choices = list(encodings["manual"]) + list(encodings["target_only"])

    def build():
        pref = "convert"
        source_entry = ui.entry(None, host.pref_get(pref, "source", ""), 60)
        output_entry = ui.entry(None, host.pref_get(pref, "output", ""), 60)
        ext_entry = ui.entry(None, host.pref_get(pref, "exts", DEFAULT_EXT), 16)
        exclude_entry = ui.entry(None, host.pref_get(pref, "exclude", ""), 40)
        source_combo = ui.combo(None, source_choices)
        target_combo = ui.combo(None, target_choices)
        recursive_box = ui.checkbox(None, "递归子目录", None)
        backup_box = ui.checkbox(None, "备份到 源目录\\backup", None)
        if not ui.set_checked(recursive_box, True):
            pass  # 老宿主没有写勾选态 ⇒ 默认未勾选（能力缺失要能被看见，不假装）
        ui.set_checked(backup_box, True)

        status = ui.label(None, "就绪")
        log = ui.log_view(None, 5)
        panel = ui.task_panel(None, True)          # ★ 本页**有**取消（1.x 会先确认）
        paused = [False]
        last_outcome = [None]   # 上一次转换的结果（导出统计要用）
        pause_button = ui.button(None, "暂停", None)

        def say(text):
            ui.set_text(status, text)
            ui.log_view_append(log, text)

        # ★★ P17-1（线程与取消契约，审计 03 F-01 / 05 PLG-01）：**worker 不许碰控件**。
        #   界面的两处落点（状态标签 / 日志视图）一律在**回主线程的回调**里改：
        #   worker 只调两个 sink（`progress`/`message`）＋跨线程只读白名单
        #   （`ui.task_cancel_requested` —— 引擎的 cancel hook 在 worker 线程里被同步轮询）。
        #   修前 worker 直接 `ui.set_text(status, …)` / `ui.log_view_append(log, …)`，
        #   而门面的线程闸门把它们**静默**变成 no-op ⇒ worker 阶段的状态与日志永远是空的。
        def on_task_message(text):
            ui.log_view_append(log, text)
            ui.set_text(status, text)

        ui.task_on_message(panel, on_task_message)

        def sync_backup_enabled():
            in_place = not ui.get_text(output_entry).strip()
            ui.set_enabled(backup_box, in_place)
            if not in_place:
                ui.set_checked(backup_box, False)

        def toggle_pause():
            paused[0] = not paused[0]
            ui.set_text(pause_button, "继续" if paused[0] else "暂停")

        def wait_if_paused():
            # 暂停时在这里等（返回 False 会让引擎**停下**，所以等完必须返回 True）
            import time
            while paused[0]:
                time.sleep(0.01)
            return True

        def pick_source():
            picked = ui.ask_directory("选择源目录")
            if picked:
                ui.set_text(source_entry, picked)
                host.pref_set(pref, "source", picked)

        def pick_output():
            picked = ui.ask_directory("选择输出目录")
            if picked:
                ui.set_text(output_entry, picked)
                host.pref_set(pref, "output", picked)

        def options_from_ui():
            return {
                "source": ui.get_text(source_entry).strip(),
                "output_dir": ui.get_text(output_entry).strip(),
                "extensions": [e.strip() for e in ui.get_text(ext_entry).split(",") if e.strip()],
                "recursive": ui.checkbox_state(recursive_box),
                "backup": ui.checkbox_state(backup_box),
                "exclude_patterns": ui.get_text(exclude_entry).strip(),
                "source_encoding": ui.combo_value(source_combo),
                "target_encoding": ui.combo_value(target_combo),
            }

        def remember(options):
            host.pref_set(pref, "source", options["source"])
            host.pref_set(pref, "output", options["output_dir"])
            host.pref_set(pref, "exts", ",".join(options["extensions"]) or DEFAULT_EXT)
            host.pref_set(pref, "exclude", options["exclude_patterns"])

        def start():
            options = options_from_ui()
            if not options["source"]:
                say("请先选源路径")
                return
            if not options["extensions"] and not options["source"].lower().endswith(DEFAULT_EXT):
                say("请输入文件扩展名，例如 .xml,.txt")
                return
            remember(options)

            def worker(progress, message):
                progress(0)

                def on_message(text):
                    message(text)

                outcome = engine.text_convert_batch(
                    batch_options(options),
                    {"cancel": lambda: ui.task_cancel_requested(panel),
                     "gate": wait_if_paused,
                     "message": on_message,
                     "progress": lambda done, total: progress(100 if total <= 0 else int(done * 100 / total))})
                last_outcome[0] = outcome
                # ★ P17-1：状态与日志一律经 `message`（主线程回调落控件）—— worker 不碰控件。
                #   收尾文案的顺序照旧：扫描台账 → 停止理由 → 结论（`message` 与壳同队列、保序），
                #   于是状态栏最终仍是 `result_line(outcome)`（与 C++ 页同一口径）。
                message(scan_line(outcome))
                if outcome["stop_reason"]:
                    message(outcome["stop_reason"])
                message(result_line(outcome))
                progress(100)

            if not ui.task_run(panel, "编码转换", worker):
                say("已有任务在跑，请等它结束或先取消")

        def run_restore():
            options = options_from_ui()
            if not options["source"]:
                say("请先选源目录（备份在 源目录\\backup 下）")
                return

            def worker(progress, message):
                progress(0)
                result = engine.text_restore_backups(
                    backup_options(options),
                    {"cancel": lambda: ui.task_cancel_requested(panel),
                     "gate": wait_if_paused,
                     "message": message})
                if result["dir_missing"]:
                    message("未找到 backup 目录，无备份可恢复：" + options["source"])
                elif result["total"] == 0:
                    message("未找到可恢复的 .bak 备份文件（扩展名：" +
                            options["extensions"][0] + " 等）")
                elif result["cancelled"]:
                    message(result["stop_reason"])
                else:
                    message("恢复结束：成功 %d / 失败 %d / 候选 %d" % (result["ok"],
                                                              result["failed"], result["total"]))
                for line in result["errors"]:
                    message("[恢复] " + line)
                # ★ P17-1：收尾文案经 `message` 落状态栏（worker 不碰控件）；顺序照旧放最后
                message("恢复完成：成功 %d / 候选 %d" % (result["ok"], result["total"]))
                progress(100)

            if not ui.task_run(panel, "恢复备份", worker):
                say("已有任务在跑，请等它结束或先取消")

        def run_clear():
            options = options_from_ui()
            if not options["source"]:
                say("请先选源目录（备份在 源目录\\backup 下）")
                return

            def worker(progress, message):
                progress(0)
                result = engine.text_clear_backups(
                    backup_options(options),
                    {"cancel": lambda: ui.task_cancel_requested(panel), "message": message})
                if result["dir_missing"]:
                    message("未找到 backup 目录，无需清空")
                else:
                    message("清空结束：删除 %d / 失败 %d / 候选 %d" % (result["ok"],
                                                             result["failed"], result["total"]))
                for line in result["errors"]:
                    message("[清空] " + line)
                # ★ P17-1：同上 —— 收尾文案经 `message`（worker 不碰控件）
                message("清空完成：删除 %d / 候选 %d" % (result["ok"], result["total"]))
                progress(100)

            if not ui.task_run(panel, "清空备份", worker):
                say("已有任务在跑，请等它结束或先取消")

        def open_output():
            target = ui.get_text(output_entry).strip() or ui.get_text(source_entry).strip()
            if not target:
                say("输出目录无效：（空）")
                return
            if ui.open_path(target):
                say("已打开：" + target)
            else:
                say("打开失败：%s（可能缺少文件管理器关联或权限不足，请手动打开）" % target)

        def export_stats():
            path = ui.ask_save_file("导出统计")
            if not path:
                return
            outcome = last_outcome[0]
            if outcome is None:
                say("还没有可导出的统计（先跑一次转换）")
                return
            options = options_from_ui()
            stats = {
                "generated_at": "",
                "source": options["source"],
                "single_file": False,
                "output_dir": options["output_dir"],
                "source_enc": options["source_encoding"],
                "target_enc": options["target_encoding"],
                "candidates": outcome["candidates"],
                "processed": outcome["processed"],
                "success": outcome["success"],
                "skipped": outcome["skipped"],
                "failed": outcome["failed"],
                "duration": outcome["duration"],
            }
            wrote = engine.text_write_stats_csv(path, stats)
            if wrote["ok"]:
                say("统计已导出：" + path)
            else:
                say("导出失败：" + (wrote["error"] or "(无理由)"))

        ui.label(None, "源路径：")
        ui.button(None, "选择源目录…", pick_source)
        ui.label(None, "输出目录（留空 = 原地转换）：")
        ui.button(None, "选择输出目录…", pick_output)
        ui.label(None, "源编码 / 目标编码：")
        ui.label(None, "扩展名（逗号分隔）：")
        ui.label(None, "排除模式（逗号分隔）：")
        ui.button(None, "开始转换", start)
        ui.button(None, "恢复备份", run_restore)
        ui.button(None, "清空备份", run_clear)
        ui.button(None, "打开输出", open_output)
        ui.button(None, "导出统计", export_stats)
        sync_backup_enabled()

    api.register_tool("convert_py", build, 1, "convert", "编码转换（插件版）", "编码转换")
    api.log("编码转换（插件版）：已注册，replaces=convert")
