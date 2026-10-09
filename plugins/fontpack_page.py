# -*- coding: utf-8 -*-
"""**样板页**：汉化包生成（原 C++ `fontpack_page` 的插件版）。

它是"一切皆插件"的第一个真实样板 —— 也是**反向定义**插件 API 的那一页：
  · 控件面 `api.ui.*`（字段 / 下拉 / 勾选 / 按钮 / 任务壳 / 日志视图 / 目录选择 / 读控件值）；
  · 引擎面 `api.engine.font_pack(...)`（**与内置页同一个引擎实现** ⇒ 产物逐字节一致）；
  · 宿主面 `api.host.*`（偏好读写：记住上次的 XML 目录 / 字体 / 输出目录）。

注册方式：**新 id `fontpack_py` + 显式 `replaces="fontpack"`**（规格 §五：覆盖必须显式；
被替换的内置页只隐藏、可一键还原），标签仍显示原名的"汉化包生成"。

口径（逐条照 C++ 页，别自己加戏）：
  · 字号档位 `offset`：原尺寸 0 / +1 / +2（引擎里 `render_size = 槽位 + offset`）；
  · 语言可选自定义；后缀"无/_chs/自定义"；
  · **本页没有取消按钮**（用户 2026-10-06：「封包和字库的取消……本质是完全没有的，
    故直接删除即可」）⇒ `task_panel(with_cancel=False)`；
  · 字体四态用引擎的 `font_state_marks()` / `font_check()`（不抄中文）。

本版**没做**（如实登记）：无（P8a 起字库选择器走宿主的 `api.ui.pick_font` —— 与内置页**同一个**
界面：系统字体列表 + 四态标记 + 预览 + 缩放 + 「浏览其他目录…」）。
"""

import os

PLUGIN_INFO = {"name": "汉化包生成（插件版）", "version": "1.0"}
PLUGIN_API = 1

SECTION = "font"
GAMES = ["SoC", "ClS", "CoP"]
LANGS = ["chs", "cht", "eng", "rus"]
SIZES = [0, 1, 2]
SUFFIXES = ["", "_chs"]


def generate(api, options):
    """出包（**唯一一份**）—— 界面按钮与无界面入口都走它。

    返回引擎给的那个 dict（`ok/error/out_gamedata/fonts_written/xml_written/warnings`）。
    ★ 界面那条路在 worker 线程里调它（引擎调用允许工作线程），
      无界面那条路（`headless`）在主线程里调它 —— 两条路**同一段代码**。
    """
    return api.engine.font_pack(options)


def headless(api, args):
    """**无界面入口**（`xrkit_cli --py-plugin-call plugins <json>`）—— 门禁判"产物逐字节一致"用。

    契约：`args` 是 JSON 解析出来的 dict（键同 `api.engine.font_pack` 的 options）；
    返回 `generate()` 的结果（会被 `json.dumps` 成一行交回）。
    ★ 这条路上 `api.ui.*` 不可用（没有界面）—— 所以这里**不碰任何控件**。
    """
    options = {
        "game": str(args.get("game", "SoC")),
        "lang": str(args.get("lang", "chs")),
        "xml_dir": str(args.get("xml_dir", "")),
        "font_path": str(args.get("font_path", "")),
        "out_dir": str(args.get("out_dir", "")),
        "offset": int(args.get("offset", 0)),
        "suffix": str(args.get("suffix", "")),
        "full_cyrillic": bool(args.get("full_cyrillic", True)),
        "only_sizes": list(args.get("only_sizes", [])),
    }
    return generate(api, options)


def register(api):
    ui = api.ui
    host = api.host
    engine = api.engine

    def build():
        # ── 界面（顺序即排版；宿主负责排版，插件只声明）────────────────────────
        ui.label(None, "文本 XML 目录（只扫顶层）")
        xml_entry = ui.entry(None, host.pref_get(SECTION, "xml_dir", ""), 60)

        def pick_xml():
            picked = ui.ask_directory("选择文本 XML 目录")
            if picked:
                ui.set_text(xml_entry, picked)
                host.pref_set(SECTION, "xml_dir", picked)

        ui.button(None, "浏览…", pick_xml)

        ui.label(None, "字体文件（.ttf/.ttc/.otf）")
        font_entry = ui.entry(None, host.pref_get(SECTION, "path", ""), 60)
        state_label = ui.label(None, "（未选字体）")

        def refresh_font_state():
            path = ui.get_text(font_entry).strip()
            if not path:
                ui.set_text(state_label, "（未选字体）")
                return
            ui.set_text(state_label, engine.font_check(path)["mark"])

        def pick_font():
            # ★★ P8a：用**宿主的字库选择器**（系统字体 + 四态标记 + 预览 + 缩放；与内置页**同一个**
            #   界面）—— 这就是用户那条下限「观感也不许比现状差」；回调是**异步**的（非模态）。
            def chosen(path):
                if not path:
                    return
                ui.set_text(font_entry, path)
                host.pref_set(SECTION, "path", path)
                refresh_font_state()

            if not ui.pick_font(ui.get_text(font_entry).strip(), chosen):
                # 宿主没这个能力 ⇒ 退回系统文件框（按钮点了不能没反应）
                picked = ui.ask_open_file("选择字体（.ttf/.ttc/.otf）")
                if picked:
                    chosen(picked)

        ui.button(None, "选择字体…", pick_font)
        refresh_font_state()

        ui.label(None, "输出目录（产物是 <输出>/gamedata）")
        out_entry = ui.entry(None, host.pref_get(SECTION, "out_dir", ""), 60)

        def pick_out():
            picked = ui.ask_directory("选择输出目录")
            if picked:
                ui.set_text(out_entry, picked)
                host.pref_set(SECTION, "out_dir", picked)

        ui.button(None, "浏览…", pick_out)

        game_combo = ui.combo(None, GAMES)
        lang_combo = ui.combo(None, LANGS + ["自定义"])
        size_combo = ui.combo(None, ["原尺寸", "+1", "+2"])
        suffix_combo = ui.combo(None, ["无", "_chs", "自定义"])
        lang_entry = ui.entry(None, "", 16)
        suffix_entry = ui.entry(None, "", 16)
        cyrillic = ui.checkbox(None, "包含全西里尔字母", None)

        panel = ui.task_panel(None, False)   # ★ 用户拍板：本页**没有**取消
        result_label = ui.label(None, "就绪")
        log = ui.log_view(None, 6)

        def combo_at(combo, choices):
            """读下拉当前项（`combo_value`）；读不到就退回第 0 项（不猜、不崩）。"""
            current = ui.combo_value(combo)
            return choices.index(current) if current in choices else 0

        def lang_value():
            index = combo_at(lang_combo, LANGS + ["自定义"])
            return ui.get_text(lang_entry).strip() if index == len(LANGS) else LANGS[index]

        def suffix_value():
            index = combo_at(suffix_combo, ["无", "_chs", "自定义"])
            return ui.get_text(suffix_entry).strip() if index == len(SIZES) + 1 else SUFFIXES[index]

        def on_generate():
            xml_dir = ui.get_text(xml_entry).strip()
            font_path = ui.get_text(font_entry).strip()
            out_dir = ui.get_text(out_entry).strip()
            if not (xml_dir and font_path and out_dir):
                ui.set_text(result_label, "请把三个路径都填上")
                return
            host.pref_set(SECTION, "xml_dir", xml_dir)
            host.pref_set(SECTION, "path", font_path)
            host.pref_set(SECTION, "out_dir", out_dir)
            options = {
                "game": GAMES[combo_at(game_combo, GAMES)],
                "lang": lang_value(),
                "xml_dir": xml_dir,
                "font_path": font_path,
                "out_dir": out_dir,
                "offset": SIZES[combo_at(size_combo, ["原尺寸", "+1", "+2"])],
                "suffix": suffix_value(),
                "full_cyrillic": ui.checkbox_state(cyrillic),
            }

            # ★★ P17-1（线程与取消契约，审计 03 F-01 / 05 PLG-01）：**worker 不许碰控件**。
            #   日志视图只在**回主线程的回调**里追加；worker 只调两个 sink（`progress`/`message`）。
            #   修前 worker 直接 `ui.log_view_append(log, …)` —— 被线程闸门静默拒掉 ⇒
            #   worker 阶段日志栏永远空白（只有 `message` 那一行到得了状态栏）。
            ui.task_on_message(panel, lambda text: ui.log_view_append(log, text))

            def worker(progress, message):
                progress(0)
                result = generate(api, options)
                if not result["ok"]:
                    message("失败：" + (result["error"] or "(无理由)"))
                    progress(100)
                    return
                for warning in result["warnings"]:
                    message("告警：" + warning)
                text = "完成：字体 %d 个、XML %d 个" % (result["fonts_written"],
                                                     result["xml_written"])
                if result["warnings"]:
                    text += "（%d 条告警）" % len(result["warnings"])
                message(text)
                progress(100)

            if not ui.task_run(panel, "生成汉化包", worker):
                ui.set_text(result_label, "任务没起来（宿主拒绝）")

        ui.button(None, "生成", on_generate)

    api.register_tool("fontpack_py", build, 0, "fontpack", "汉化包生成（插件版）", "汉化包生成")
    api.log("汉化包生成（插件版）：已注册，replaces=fontpack")
