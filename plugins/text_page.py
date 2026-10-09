# -*- coding: utf-8 -*-
"""**第三页迁移：文本提取/改写**（原 C++ `text_page` 的插件版）。

口径逐条照 C++ 页（别自己加戏）：
  · 源目录必须是 **gameplay / scripts 文件夹本身**（判定与理由都来自引擎：`text_extract_kind` /
    `text_validate_extract` —— 页面不自己重写那句话）；
  · 「文本前缀」默认取引擎的默认 ID 前缀；「ID 前缀」留空＝引擎按 prefix+类型自动生成
    （**对拍要显式给固定值**，因为 1.x 那边 ts = 时间戳）；
  · 「先清空输出目录里的旧产物」默认**勾上**（1.x 同）；
  · 统计框三行：gameplay / scripts / 总计（数字**只来自引擎**；总计＝两行相加，不重数）；
  · 结果区是**行式、能滚**的日志（C++ 页用自绘虚拟列表；见报告里"观感不等价"那条如实登记）；
  · 任务壳**有取消**；取消后**必须说清停在哪、已产出多少**（零静默）。
注册方式：**新 id `text_py` + 显式 `replaces="text"`**，标题仍旧名。
"""

PLUGIN_INFO = {"name": "文本提取（插件版）", "version": "1.0"}
PLUGIN_API = 1


def kind_sub_dir(kind):
    """引擎的 kind → 目标子目录名（与 C++ 页/CLI 同一口径）。"""
    return "gameplay" if kind == "gameplay" else "scripts"


def extract(api, args):
    """**唯一一份**提取逻辑：界面 worker 与无界面入口都走它。"""
    engine = api.engine
    source = args.get("source_dir", "")
    target = args.get("target_dir", "")
    request = {"source_dir": source, "target_dir": target,
               "prefix": args.get("prefix", "")}
    problem = engine.text_validate_extract(request)
    if problem:
        # 引擎给的理由原样交回（页面不重写、也不"顺手清一下"什么）
        return {"ok": False, "error": problem, "kind": "", "files": 0, "strings": 0, "chars": 0,
                "skipped": [], "repl_skipped": [], "write_failures": 0, "texts_xml_name": "",
                "elapsed_ms": 0.0, "cancelled": False}

    kind_result = engine.text_extract_kind(source)
    if not kind_result["ok"]:
        return {"ok": False, "error": kind_result["error"], "kind": "", "files": 0, "strings": 0,
                "chars": 0, "skipped": [], "repl_skipped": [], "write_failures": 0,
                "texts_xml_name": "", "elapsed_ms": 0.0, "cancelled": False}

    options = {"prefix": args.get("prefix", ""), "id_prefix": args.get("id_prefix", ""),
               "clean": bool(args.get("clean", True))}
    cancel = args.get("cancel")
    if cancel is not None:
        # ★★ P17-1：两条入口的语义**分开**（修前这一行把 GUI 的真回调也换成了"第一次问就说停"，
        #   ⇒ GUI 的取消谓词恒为 True，提取在第一个文件边界就停）。
        #   · 无界面判据（JSON）带不了回调 ⇒ 传进来的是真值标记 ⇒ 用**确定性**谓词验取消语义；
        #   · GUI worker 传的是真回调 ⇒ **原样透给引擎**（它自己会在工作线程里轮询取消位）。
        options["cancel"] = cancel if callable(cancel) else (lambda: True)
    out_dir = args.get("target_dir", "")
    return engine.text_extract_dir(source, out_dir.rstrip("/\\") + "/" +
                                   kind_sub_dir(kind_result["kind"]),
                                   out_dir, options)


def headless(api, args):
    """**无界面入口**（`--py-plugin-call … --plugin text_page.py <json>`）。"""
    return extract(api, args)


def register(api):
    ui = api.ui
    engine = api.engine
    host = api.host

    def build():
        pref = "text"
        default_prefix = engine.text_default_id_prefix()
        source_entry = ui.entry(None, host.pref_get(pref, "source", ""), 60)
        target_entry = ui.entry(None, host.pref_get(pref, "target", ""), 60)
        prefix_entry = ui.entry(None, host.pref_get(pref, "prefix", default_prefix), 12)
        id_prefix_entry = ui.entry(None, host.pref_get(pref, "id_prefix", ""), 40)
        clean_box = ui.checkbox(None, "先清空输出目录里的旧产物", None)
        ui.set_checked(clean_box, True)
        stats_label = ui.label(None, stats_text(None, None))
        status = ui.label(None, "就绪")
        log = ui.log_view(None, 8)
        panel = ui.task_panel(None, True)          # ★ 本页**有**取消
        last = {"gameplay": None, "scripts": None}

        def say(text):
            ui.set_text(status, text)
            ui.log_view_append(log, text)

        # ★★ P17-1（线程与取消契约，审计 03 F-01 / 05 PLG-01）：**worker 不许碰控件**。
        #   统计标签与日志视图一律在**回主线程的回调**里改；worker 只调两个 sink ＋
        #   跨线程只读白名单（`ui.task_cancel_requested`）。
        #   修前 worker 直接 `ui.set_text(stats_label, …)` —— 被线程闸门静默拒掉 ⇒ 统计栏
        #   在 worker 阶段永远不动（用户看到的是"提取完了统计还是旧的"）。
        #   数字仍然只来自引擎：`last` 由 worker 写、回调只做排版（`stats_text`）。
        def on_task_message(text):
            ui.log_view_append(log, text)
            ui.set_text(stats_label, stats_text(last["gameplay"], last["scripts"]))

        ui.task_on_message(panel, on_task_message)

        def pick_source():
            picked = ui.ask_directory("选择 gameplay / scripts 文件夹")
            if picked:
                ui.set_text(source_entry, picked)
                host.pref_set(pref, "source", picked)

        def pick_target():
            picked = ui.ask_directory("选择输出目录")
            if picked:
                ui.set_text(target_entry, picked)
                host.pref_set(pref, "target", picked)

        def start():
            args = {
                "source_dir": ui.get_text(source_entry).strip(),
                "target_dir": ui.get_text(target_entry).strip(),
                "prefix": ui.get_text(prefix_entry).strip() or default_prefix,
                "id_prefix": ui.get_text(id_prefix_entry).strip(),
                "clean": ui.checkbox_state(clean_box),
            }
            host.pref_set(pref, "source", args["source_dir"])
            host.pref_set(pref, "target", args["target_dir"])
            host.pref_set(pref, "prefix", args["prefix"])
            host.pref_set(pref, "id_prefix", args["id_prefix"])

            def worker(progress, message):
                progress(0)
                args_with_cancel = dict(args)
                # ★ P17-1：`task_cancel_requested` 在跨线程只读白名单里（取消谓词在
                #   worker 线程里被引擎同步轮询）—— 修前它在 worker 里恒为 None ⇒ 取消关不掉。
                args_with_cancel["cancel"] = lambda: ui.task_cancel_requested(panel)
                result = extract(api, args_with_cancel)
                if result["kind"]:
                    last[result["kind"]] = result
                progress(100)
                if result["cancelled"]:
                    # ①零静默：取消必须**说清停在哪、已产出多少**（绝不报成"完成"）
                    message("已取消：在文件边界停下（已提取 %d 个文件 / %d 字符串 / %d 字符；"
                            "已写出的产物都是完整的）" % (result["files"], result["strings"],
                                                   result["chars"]))
                    return
                if not result["ok"]:
                    # ★ S1-1：失败也**先把台账逐条报出来**（产物没落盘时这是唯一能自救的线索）
                    for line in result["skipped"]:
                        message("跳过 " + line)
                    message("提取失败：" + result["error"])
                    return
                message("files=%d strings=%d chars=%d xml=%s"
                        % (result["files"], result["strings"], result["chars"],
                           result["texts_xml_name"]))
                for line in result["skipped"]:
                    message("跳过 " + line)
                for line in result["repl_skipped"]:
                    # ★ 保护性替换：因保护而没替换的命中逐条可见（零静默）
                    message("未替换 " + line)

            if not ui.task_run(panel, "文本提取", worker):
                say("已有任务在跑，请等它结束或先取消")

        ui.label(None, "源目录（gameplay / scripts 文件夹本身）：")
        ui.button(None, "选择源目录…", pick_source)
        ui.label(None, "输出目录（改写后的源文件与文本 XML 都写这里）：")
        ui.button(None, "选择输出目录…", pick_target)
        ui.label(None, "文本前缀 / ID 前缀：")
        ui.button(None, "开始提取", start)

    api.register_tool("text_py", build, 2, "text", "文本提取（插件版）", "文本提取")
    api.log("文本提取（插件版）：已注册，replaces=text")


def stats_text(gameplay, scripts):
    """三行统计（数字只来自引擎；总计＝两行相加，**不重数**）。"""
    def line(title, stats):
        if stats is None:
            stats = {"files": 0, "strings": 0, "chars": 0}
        return "%s：%d 字符串 · %d 文件 · %d 字符" % (title, stats["strings"], stats["files"],
                                                stats["chars"])
    total = {"files": (gameplay or {}).get("files", 0) + (scripts or {}).get("files", 0),
             "strings": (gameplay or {}).get("strings", 0) + (scripts or {}).get("strings", 0),
             "chars": (gameplay or {}).get("chars", 0) + (scripts or {}).get("chars", 0)}
    return line("gameplay", gameplay) + "\n" + line("scripts", scripts) + "\n" + line("总计", total)
