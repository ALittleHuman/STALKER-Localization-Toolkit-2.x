# -*- coding: utf-8 -*-
"""IX-Ray 语言包生成（**真插件**，不是产品内置页）—— 路线：工具稳定 → 插件 → 合并。

口径（用户 2026-10-26）：
  · 「指定」模式：用户给一个**能当模组打的整包**（例 `MODS\\~NotoSansSC`），
    工具识别它（四档/语言/槽位），以它为基底**只换字体**，其余文件逐字节保留；
  · 分区字体 = 汉化包生成的**高级功能**：勾选要生成的字号档位（空 = 全部）；
  · 识别走 `api.engine.langpack_scan`（与 CLI `--langpack-scan` 同一份引擎实现）；
  · 生成走**外部助手** `_lib/ixrgen.py`（纯 fonttools）：本页只当它是命令行工具，
    读回它的 `IXRGEN {...}` 报告（缺字/新增字形都进界面，不静默）。

v0.1 的边界（写清楚，免得当成缺口反复修）：
  · 生成目前是"**整包一个中文字体**"（逐槽位不同字体在规划的下一刀）；
  · 助手需要带 fonttools 的 Python：用环境变量 `XRKIT_IXR_PYTHON` 指定，
    没配就报"未配置"（**不假装能跑**）。
"""
from __future__ import annotations

import json
import os
import sys

PLUGIN_INFO = {
    "id": "ixr_langpack",
    "name": "IX-Ray 语言包生成（插件）",
    "version": "0.1",
    "description": "扫参照包 → 槽位表 → 分区（字号档）→ 生成可当模组打的整包",
}

SECTION = "ixr"


def _helper_python() -> str:
    """带 fonttools 的 Python：环境变量优先，否则退回当前解释器（可能没有 fonttools）。"""
    return os.environ.get("XRKIT_IXR_PYTHON", "") or sys.executable


def register(api):
    ui = api.ui
    host = api.host
    helper = os.path.join(api.plugin_dir, "_lib", "ixrgen.py")

    # ── 无头探针（判据用：不建 UI，只证明"插件能扫"这条链接通）───────────────
    probe = os.environ.get("XRKIT_IXR_PROBE", "")
    if probe:
        try:
            model = api.engine.langpack_scan(probe)
            lines = [
                "probe_profile:%s" % model["profile"],
                "probe_lang:%s" % model["lang"],
                "probe_slots:%d" % len(model["slots"]),
                "probe_ttf:%d" % model["ttf"],
                "probe_sections:%s" % ",".join(s["section"] for s in model["slots"][:3]),
            ]
            with open(os.path.join(api.plugin_dir, "ixr_marker.txt"), "w", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
        except Exception as exc:  # 零静默：探针失败也要留痕
            with open(os.path.join(api.plugin_dir, "ixr_marker.txt"), "w", encoding="utf-8") as handle:
                handle.write("probe_error:%r\n" % (exc,))

    def build(parent):
        state = {"scan": None, "sizes": {}, "plan": {}}

        ui.label(parent, "参照包（能当模组打的整包，例如 MODS\\~NotoSansSC）")
        pack_entry = ui.entry(parent, host.pref_get(SECTION, "pack", ""), 60)
        row1 = ui.row(parent)

        def pick_pack():
            picked = ui.ask_directory("选择参照语言包目录")
            if picked:
                ui.set_text(pack_entry, picked)
                host.pref_set(SECTION, "pack", picked)

        ui.button(row1, "选目录…", pick_pack)

        info = ui.label(parent, "（未扫描）")
        tree = ui.tree(parent, [["段", 22], ["字号", 6], ["字体", 16], ["继承", 12], ["计划字体", 20]])
        ui.label(parent, "分区（字号档；一个都不勾 = 不按档裁剪）")
        size_boxes = []
        for slot in api.engine.font_size_slots() or []:
            size_boxes.append((slot, ui.checkbox(parent, "档 %d" % slot, None)))

        log = ui.log_view(parent, 10)

        def refresh_tree():
            model = state["scan"]
            if not model:
                return
            nodes = []
            for slot in model["slots"]:
                planned = state["plan"].get(slot["section"], "")
                nodes.append({
                    "key": slot["section"],
                    "cells": [slot["section"], str(slot["size"] or "-"),
                              slot["name"] or "(位图)", slot["base"] or "-",
                              os.path.basename(planned) if planned else ""],
                })
            ui.tree_set_root(tree, nodes)

        def do_scan():
            root = ui.get_text(pack_entry).strip()
            if not root:
                ui.set_text(info, "先选参照包目录")
                return
            model = api.engine.langpack_scan(root)
            state["scan"] = model
            ui.set_text(info, "profile=%s lang=%s 段=%d ttf=%d dds=%d"
                         % (model["profile"], model["lang"], len(model["slots"]),
                            model["ttf"], model["dds"]))
            for line in model["evidence"]:
                ui.log_view_append(log, "依据：" + line)
            refresh_tree()

        ui.button(row1, "扫描", do_scan)

        def on_pick_slot_font():
            key = ui.tree_selected_key(tree)
            if not key:
                ui.log_view_append(log, "先在上面表里选一个段")
                return

            def chosen(path):
                if path:
                    state["plan"][key] = path
                    ui.log_view_append(log, "段 %s ⇒ %s" % (key, os.path.basename(path)))
                    refresh_tree()

            ui.pick_font(host.pref_get(SECTION, "font", ""), chosen)

        ui.button(parent, "给选中段选字体…", on_pick_slot_font)

        # ── 预览（P3）：引擎出样张 PPM → ui.image 显示；换新图先 destroy 旧控件 ──
        preview = {"widget": None}

        def do_preview():
            font = ""
            for path in state["plan"].values():
                font = path
                break
            if not font:
                ui.log_view_append(log, "先给一个段选字体（预览用它）")
                return
            out = os.path.join(api.plugin_dir, "_lib", "preview.ppm")
            sample = api.engine.font_sample(font, "原字体 vs 候选：你好 ABC Привет", 28, out)
            if not sample or not sample.get("ok"):
                ui.log_view_append(log, "预览失败：%s" % (sample or {}).get("error", "?"))
                return
            if preview["widget"] is not None:
                ui.destroy(preview["widget"])
            preview["widget"] = ui.image(parent, out, 0, 0)
            ui.log_view_append(log, "预览：%dx%d，缺字 %d"
                               % (sample["width"], sample["height"], sample["missing"]))

        ui.button(parent, "预览样张", do_preview)

        # ★★ 2026-10-27：**并排对比**（原字体 vs 候选）—— 两张样张各出一张 ui.image。
        #   "原字体" = 参照包里该段的 ttf；"候选" = 用户给该段选的字体（没选就用参照包字体）。
        compare = {"left": None, "right": None}

        def do_compare():
            model = state["scan"]
            if not model or not model.get("slots"):
                ui.log_view_append(log, "先扫描参照包")
                return
            slot = model["slots"][0]
            base = ""
            # 参照包内的 ttf：text_dir 的同级 fonts/<lang>/<name>.ttf
            name = slot.get("name") or ""
            pack_root = ui.get_text(pack_entry).strip()
            if name and pack_root:
                cand_path = os.path.join(pack_root, "gamedata", "fonts", model["lang"],
                                         name + ".ttf")
                if os.path.isfile(cand_path):
                    base = cand_path
            if not base:
                ui.log_view_append(log, "找不到参照包里的原始 ttf（位图包不支持对比预览）")
                return
            chosen = ""
            for path in state["plan"].values():
                chosen = path
                break
            out_l = os.path.join(api.plugin_dir, "_lib", "cmp_base.ppm")
            left = api.engine.font_sample(base, "原字体：你好 Привет", 28, out_l)
            if not left or not left.get("ok"):
                ui.log_view_append(log, "原字体样张失败：%s" % (left or {}).get("error", "?"))
                return
            if compare["left"] is not None:
                ui.destroy(compare["left"])
            compare["left"] = ui.image(parent, out_l, 0, 0)
            ui.log_view_append(log, "原字体：缺字 %d" % left["missing"])
            if chosen:
                out_r = os.path.join(api.plugin_dir, "_lib", "cmp_cand.ppm")
                right = api.engine.font_sample(chosen, "候选：你好 Привет", 28, out_r)
                if right and right.get("ok"):
                    if compare["right"] is not None:
                        ui.destroy(compare["right"])
                    compare["right"] = ui.image(parent, out_r, 0, 0)
                    ui.log_view_append(log, "候选：缺字 %d" % right["missing"])

        ui.button(parent, "并排对比（原字体 vs 候选）", do_compare)

        multi_check = ui.checkbox(parent, "多语言：每个语言各出一份（--all-langs）", None)
        multi_check_holder = {"box": multi_check}

        # ★★ 2026-10-29（交接区审计 P0-7）：插件以前**没有译文入口** ⇒ 生成的永远
        #   是"换了字体的俄语包"。这里补「译文 XML 目录」，并作为 --text-root 传下去。
        ui.label(parent, "译文 XML 目录（纯 XML + 改造产物；不填 = 只出字体包）")
        trans_entry = ui.entry(parent, host.pref_get(SECTION, "text_root", ""), 60)

        def pick_trans():
            picked = ui.ask_directory("选择译文 XML 目录")
            if picked:
                ui.set_text(trans_entry, picked)
                host.pref_set(SECTION, "text_root", picked)

        ui.button(parent, "选译文目录…", pick_trans)

        out_entry = ui.entry(parent, host.pref_get(SECTION, "out", ""), 60)
        ui.label(parent, "输出目录")

        def pick_out():
            picked = ui.ask_directory("输出目录")
            if picked:
                ui.set_text(out_entry, picked)
                host.pref_set(SECTION, "out", picked)

        ui.button(parent, "选输出目录…", pick_out)

        def generate():
            root = ui.get_text(pack_entry).strip()
            out = ui.get_text(out_entry).strip()
            if not root or not out:
                ui.log_view_append(log, "参照包/输出目录都要填")
                return
            sizes = [slot for slot, box in size_boxes if ui.checkbox_state(box)]
            if sizes:
                ui.log_view_append(log, "分区：%s" % ",".join(str(s) for s in sizes))
            font = ""
            for path in state["plan"].values():
                font = path
                break
            if not font:
                ui.log_view_append(log, "先在表里给至少一个段选字体（v0.1 整包一个字体）")
                return
            python = _helper_python()
            # ★ 逐槽位计划 + 分区档位：写一份计划 JSON 给助手（段 → 字体；sizes）
            plan_path = os.path.join(api.plugin_dir, "_lib", "last_plan.json")
            with open(plan_path, "w", encoding="utf-8") as handle:
                json.dump({"sections": dict(state["plan"]), "sizes": sizes}, handle,
                          ensure_ascii=False)
            args = [helper, "--pack", root, "--font", font, "--out", out, "--plan", plan_path]
            trans_root = ui.get_text(trans_entry).strip()
            if trans_root:
                args += ["--text-root", trans_root]
            else:
                ui.log_view_append(log, "⚠ 没填译文目录：产物只有字体、文本仍是原文（不是汉化包）")
            if sizes:
                args += ["--sizes", ",".join(str(s) for s in sizes)]
            if multi_check_holder["box"] is not None and ui.checkbox_state(multi_check_holder["box"]):
                args += ["--all-langs", "--split-output"]
            ui.log_view_append(log, "运行：" + " ".join([python] + args))
            # ★★ 2026-10-29（交接区审计 P0-10）：`run_process` 只收**一个 options 字典**；
            #   修前写成 `run_process(python, args, cb)` ⇒ 点「生成」直接 TypeError，
            #   助手从没被真正调用过（"插件能生成"这个说法在此之前是不成立的）。
            result = host.run_process({
                "program": python,
                "args": args,
                "on_stdout": lambda line: ui.log_view_append(log, line),
            })
            if not result.get("started"):
                ui.log_view_append(log, "起不来：%s" % result.get("stderr_tail", ""))
                return
            report = {}
            for line in (result.get("stdout_tail", "") or "").splitlines():
                if line.startswith("IXRGEN {"):
                    try:
                        report = json.loads(line[len("IXRGEN "):])
                    except Exception:
                        report = {}
            if report:
                ui.log_view_append(log, "结果：字体 %d / 新增字形 %d / 缺字 %d"
                                   % (len(report.get("fonts", [])), report.get("added", 0),
                                      report.get("missing", 0)))
            if result.get("exit_code") != 0:
                ui.log_view_append(log, "助手退出码=%s" % result.get("exit_code"))

        ui.button(parent, "生成", generate)
        ui.log_view_append(log, "v0.1：整包一个字体；逐槽位不同字体在下一刀。助手需 XRKIT_IXR_PYTHON。")

    api.register_tool("ixr_langpack", build, 7, "", "IX-Ray 语言包生成（插件）", "语言包生成")
