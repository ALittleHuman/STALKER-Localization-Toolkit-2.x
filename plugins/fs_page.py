# -*- coding: utf-8 -*-
"""**第六页迁移：文件系统**（原 C++ `fs_page` 的插件版）。

口径逐条照 C++ 页与用户历轮反馈（别自己加戏）：
  · ★**两标签**「解包 / 封包」—— 用户 2026-09-29 原话：「其实之前的设计可以，但是
    **上面解包和下面封包分两个标签**就没问题了。」（`ui.tabs` 为此专门补的门面能力）
  · 解包页＝**页级一行**（格式下拉 + 格式说明 + 「解包到」+ 选择…/打开；整行是拖放区）
    + **左右分栏**（左＝包列表三列树「包/大小/✕」，右＝条目树三列「包内条目/大小/来源」）；
  · 包列表：`加包`（右键菜单两模式：选包文件…/选文件夹…）· `全选`（**选中而非勾选**
    —— 用户 2026-10-01「全选包，功能是选中而非勾选。」）· `加载选中` · `取消加载` · `清空`
    + 计数；**行尾 ✕ = 摘源**（用户：「移除这个按钮也可以优化掉，在包的条目后面增加一个叉叉按钮。」）；
  · 条目树：**懒建** + **三态勾选**（勾/取消向下传播 + 向上重算）+ **搜索**（边打边筛）+
    `包内全选` / `包内全不选` + 计数；★用户 2026-09-29：「文件夹要双击才能打开，改成单击」
    「单个文件要点框才能勾选，改成点击条目就能勾选」⇒ 走 `tree_on_click`（宿主只报行 key，
    语义在这里：有子 ⇒ 展开/折叠；可勾 ⇒ 切勾选）；
  · 条目路径＝**解包产物的预览**（`fs_preview_layout`；用户 2026-10-03 原话：
    「这是一个解包的工具，而右边罗列的是包的内容，换句话说**就是解包产物的预览**。」）；
  · 解包：`解包选中` / `批量解包`；落点走引擎（`fs_plan_unpack_targets` / `fs_unpack_target_dir`）；
    ★**覆盖必须可见**（`overwritten_count` 要报出来 —— 用户报过"一次把 3.9 万个文件盖在模组目录"）；
  · 封包页＝格式（**跳过 auto**）+ 输出 + 选择… + 封包；`＋文件` / `＋目录` / `移除` / `清空` + 计数；
    ★**名字改写台账必须逐条显示**（cp1251 表示不了的字符会被顶替成 `?`；用户口径
    「不需要支持中文名，但**必须看得见**改了哪些」）；
  · ★**任务壳无取消**：用户 2026-10-06 原话「封包和字库的取消，在这里本质是完全没有的，
    故直接删除即可。」⇒ `task_panel(None, False)`（连取消按钮都不创建）；
  · 解密闸门：`fs_gate_path`（该读哪个路径）+ `fs_decrypt_output_dir`（落点两边同源）。

如实登记的**形态差异**（功能等价、形态不同，不假装一致）：
  ① 「加包」的两种模式走**右键菜单**（门面没有"左键弹菜单"的能力）⇒ 左键不弹菜单；
  ② 页级行与两个工具行的**拖放**由 `drop_zone` 容器接住（C++ 页是把过滤器挂在既有控件上）；
  ③ 封包待打包列表用 `ui.list`（**单选**；C++ 页是 `QListWidget` 多选）⇒「移除」按当前行；
  ④ 「解包选中/批量解包」放在页内一行（C++ 页在贴底操作栏）。

注册：**新 id `fs_py` + 显式 `replaces="fs"`**，标签仍显示原名「文件系统」。
"""

import os

PLUGIN_INFO = {"name": "文件系统（插件版）", "version": "1.0"}
PLUGIN_API = 1

PREF = "fs"


# ── 无界面入口也在用的那份逻辑（唯一一份）────────────────────────────────────
def list_sources(args):
    """把 args 里的 `packages` / `roots` 收成"源表"（包文件 + 散装目录）。"""
    engine = args["_engine"]
    sources = []
    for path in args.get("packages") or []:
        sources.append(("package", path))
    for root in args.get("roots") or []:
        if os.path.isdir(root):
            sources.append(("loose", root))
            for dirpath, _dirnames, filenames in os.walk(root):
                for name in sorted(filenames):
                    full = os.path.join(dirpath, name)
                    if engine.fs_is_package_name(full):
                        sources.append(("package", full))
        else:
            sources.append(("package", root))
    return sources


def collect_entries(engine, kind, path, db_fallback_key):
    """读一个源的条目表（包 ⇒ 过闸门；目录 ⇒ 散装）。返回 (entries, error)。"""
    gate = engine.fs_gate_path(path)
    if not gate["ok"]:
        return [], (gate["error"] or "解密闸门拒绝")
    if kind == "loose":
        got = engine.fs_loose_entries(gate["path"])
        return got["entries"], got["error"]
    got = engine.fs_package_entries(gate["path"], db_fallback_key)
    return got["entries"], got["error"]


def unpack_plan(engine, args):
    """**解包计划**（界面与无界面共用）：源 ⇒ 读条目 ⇒ `fs_plan_unpack_targets`。"""
    key = args.get("format_key", "auto")
    out_root = args.get("out_root", "")
    srcs = list_sources(dict(args, _engine=engine))
    plan_input = []
    errors = []
    for kind, path in srcs:
        if kind == "loose":
            continue                      # 散装目录不是"包"，不参与解包计划（同 C++ 页）
        entries, error = collect_entries(engine, kind, path, key)
        if error:
            errors.append("%s：%s" % (path, error))
            continue
        plan_input.append({"package": path, "entries": [row["path"] for row in entries]})
    if not out_root:
        front = plan_input[0]["package"] if plan_input else ""
        out_root = engine.fs_resolve_output_root(front, os.getcwd())
    plan = engine.fs_plan_unpack_targets(out_root, plan_input) if plan_input else []
    return plan, out_root, errors


def headless(api, args):
    """**无界面入口**（`xrkit_cli --py-plugin-call plugins/fs_page.py <json>`）。

    门禁 `[fs-plugin]` 的"插件产物 vs CLI 参照"走这条（不需要界面）。
    `action`：`unpack`（真解包）/ `pack`（真封包）/ `preview`（右栏预览表）/ `list`（条目表）。
    """
    engine = api.engine
    action = args.get("action", "unpack")
    if action == "unpack":
        plan, _root, errors = unpack_plan(engine, args)
        results = []
        for item in plan:
            got = engine.fs_extract_package(item["image"], item["target"],
                                            args.get("selection", "all"),
                                            args.get("only") or None,
                                            args.get("format_key", "auto"))
            results.append({"image": item["image"], "target": item["target"],
                            "ok": got["ok"], "error": got["error"],
                            "files": got["files_written"], "dirs": got["dirs_created"],
                            "bytes": got["bytes_written"],
                            "overwritten": got["overwritten_count"],
                            "failures": got["failure_count"]})
        return {"ok": True, "errors": errors, "results": results}
    if action == "pack":
        items = [{"path": row["path"], "is_dir": bool(row.get("is_dir")),
                  "source_path": row["source_path"]} for row in args.get("items") or []]
        key = args.get("format_key", "sqfs")
        out = args.get("out_path", "")
        if engine.fs_format_is_sqfs(key):
            return engine.fs_pack_squashfs(out, items)
        return engine.fs_pack_db(out, items, key)
    if action == "preview":
        entries, _error = collect_entries(engine, args.get("kind", "package"),
                                          args["path"], args.get("format_key", "auto"))
        layout = engine.fs_preview_layout(args["path"], entries)
        return {"entries": layout, "count": len(layout)}
    # list
    entries, error = collect_entries(engine, args.get("kind", "package"), args["path"],
                                     args.get("format_key", "auto"))
    return {"entries": entries, "error": error, "count": len(entries)}


def register(api):
    ui = api.ui
    engine = api.engine
    host = api.host

    formats = engine.fs_formats()
    all_choices = [(row["key"], row["label"]) for row in formats]
    pack_choices = [(row["key"], row["label"]) for row in formats if not row["is_auto"]]

    def build():
        # ★ 状态（界面唯一一份真值）
        sources = []          # [{"key","kind","path","entries":[..],"checked":[..]}]
        merged = [None]       # 当前合并树句柄（int）
        rows = []             # `fs_tree_flatten` 的快照（懒建要用）

        def say(log, status, text):
            ui.set_text(status, text)
            ui.log_view_append(log, text)

        tabs = ui.tabs(None)
        page_unpack = ui.tabs_add_page(tabs, "解包")
        page_pack = ui.tabs_add_page(tabs, "封包")
        if not tabs or not page_unpack or not page_pack:
            api.log("fs_py：宿主没有标签页能力 ⇒ 页面不完整（能力缺失要能被看见）")

        # ── 通用：状态标签 / 日志 / 任务壳（★无取消）────────────────────────
        status = ui.label(None, "就绪")
        log = ui.log_view(None, 6)
        panel = ui.task_panel(None, False)      # ★ 封包与字库页**无取消**
        ui.task_on_message(panel, lambda text: ui.log_view_append(log, text))

        # ══ 解包页 ══════════════════════════════════════════════════════════
        format_choices = ["%s：%s" % (key, label) for key, label in all_choices]
        format_keys = [key for key, _label in all_choices]
        format_combo = ui.combo(page_unpack, format_choices)
        hint = ui.label(page_unpack, "自动检测")
        out_entry = ui.entry(page_unpack, host.pref_get(PREF, "out_root", ""), 50)
        zone = ui.drop_zone(page_unpack, "把包或文件夹拖到这里（也可以拖到包列表）")

        def current_format_key():
            value = ui.combo_value(format_combo) or ""
            key = value.split("：", 1)[0]
            return key if key in format_keys else "auto"

        def sync_hint():
            key = current_format_key()
            label = dict(all_choices).get(key, "自动检测")
            ui.set_text(hint, "自动检测" if key == "auto" else label)

        def pick_out_root():
            picked = ui.ask_directory("选择解包输出目录")
            if picked:
                ui.set_text(out_entry, picked)
                host.pref_set(PREF, "out_root", picked)

        def open_out_root():
            target = ui.get_text(out_entry).strip() or engine.fs_decrypt_output_dir()
            say(log, status, ("已打开：" + target) if ui.open_path(target)
                else ("打开失败：" + target))

        # 左右分栏：左＝包列表，右＝条目树
        split = ui.split(page_unpack, True)
        left = ui.split_add_pane(split, 240)
        right = ui.split_add_pane(split, 360)

        package_count = ui.label(left, "包 0")
        package_tree = ui.tree(left, [{"title": "包", "width": 0},
                                      {"title": "大小", "width": 90},
                                      {"title": "", "width": 48}])
        entry_count = ui.label(right, "条目 0")
        search = ui.entry(right, "", 30)
        entry_tree = ui.tree(right, [{"title": "包内条目", "width": 0},
                                     {"title": "大小", "width": 110},
                                     {"title": "来源", "width": 220}])

        # ── 左右两棵树的数据刷新 ────────────────────────────────────────────
        def refresh_entry_tree():
            """条目树＝合并树的**产物预览**（懒建：根层只放第一段）。"""
            handle = merged[0]
            rows.clear()
            if handle:
                rows.extend(engine.fs_tree_flatten(handle))
            nodes = []
            seen = set()
            for row in rows:
                path = row["path"]
                if "/" in path:
                    head = path.split("/", 1)[0]
                    if head in seen:
                        continue
                    seen.add(head)
                    nodes.append({"key": head, "cells": [head + "/", "", ""],
                                  "has_children": True, "checkable": True})
                else:
                    nodes.append({"key": path, "cells": [row["name"],
                                                         str(row["size"]),
                                                         str(row["source_index"])],
                                  "has_children": row["is_dir"], "checkable": True})
            ui.tree_set_root(entry_tree, nodes)
            ui.set_text(entry_count, "条目 %d" % len(rows))

        def on_entry_expand(key):
            """懒建：按 key 的前缀把下一层节点算出来。"""
            prefix = key + "/"
            depth = prefix.count("/")
            nodes = []
            seen = set()
            for row in rows:
                path = row["path"]
                if not path.startswith(prefix):
                    continue
                rest = path[len(prefix):]
                if "/" in rest:
                    head = rest.split("/", 1)[0]
                    if head in seen:
                        continue
                    seen.add(head)
                    nodes.append({"key": prefix + head, "cells": [head + "/", "", ""],
                                  "has_children": True, "checkable": True})
                else:
                    nodes.append({"key": path, "cells": [row["name"], str(row["size"]),
                                                         str(row["source_index"])],
                                  "has_children": False, "checkable": True})
            if nodes:
                ui.tree_set_children(entry_tree, key, nodes)

        def refresh_package_tree():
            nodes = []
            for source in sources:
                checked = len([p for p in source["checked"]])
                total = len(source["entries"])
                label = source["path"]
                nodes.append({"key": source["key"],
                              "cells": [os.path.basename(label), "%d/%d" % (checked, total), ""],
                              "checkable": False, "has_action": True})
            ui.tree_set_root(package_tree, nodes)
            ui.set_text(package_count, "包 %d" % len(sources))

        def rebuild_merged():
            """合并树重建（★勾选态**快照 → 回灌**：不存就"用户勾过的全没了"）。"""
            snapshot = []
            for source in sources:
                source["checked"] = list(source["checked"])
                snapshot.extend(source["checked"])
            if merged[0]:
                engine.fs_tree_free(merged[0])
                merged[0] = None
            if not sources:
                refresh_entry_tree()
                return
            payload = [{"source": source["path"], "entries": source["entries"]}
                       for source in sources]
            merged[0] = engine.fs_tree_build(payload)
            if snapshot:
                engine.fs_tree_apply_checked_paths(merged[0], snapshot)
            refresh_entry_tree()

        def load_paths(paths):
            for path in paths:
                if os.path.isdir(path):
                    entries, error = collect_entries(engine, "loose", path, current_format_key())
                    if error:
                        say(log, status, "散装目录读不出：%s（%s）" % (path, error))
                    else:
                        sources.append({"key": "loose:" + path, "kind": "loose", "path": path,
                                        "entries": entries, "checked": []})
                    for dirpath, _dirs, names in os.walk(path):
                        for name in sorted(names):
                            full = os.path.join(dirpath, name)
                            if engine.fs_is_package_name(full):
                                add_package(full)
                else:
                    add_package(path)
            refresh_package_tree()
            rebuild_merged()
            say(log, status, "已加 %d 个源，共 %d 条" %
                (len(sources), sum(len(s["entries"]) for s in sources)))

        def add_package(path):
            entries, error = collect_entries(engine, "package", path, current_format_key())
            if error:
                say(log, status, "列不出条目：%s（%s）" % (path, error))
                return False
            sources.append({"key": "pkg:" + path, "kind": "package", "path": path,
                            "entries": entries, "checked": []})
            return True

        def remove_source(key):
            for index, source in enumerate(sources):
                if source["key"] == key:
                    del sources[index]
                    break
            else:
                return
            refresh_package_tree()
            rebuild_merged()
            say(log, status, "已摘源：" + key)

        def selected_sources():
            keys = set(ui.tree_selected_keys(package_tree) or [])
            return [source for source in sources if source["key"] in keys]

        def load_selected():
            for source in selected_sources():
                source["checked"] = [row["path"] for row in source["entries"]]
            if merged[0]:
                for source in selected_sources():
                    index = sources.index(source)
                    engine.fs_tree_toggle_source(merged[0], index, True)
            refresh_package_tree()
            say(log, status, "已加载选中 %d 个源" % len(selected_sources()))

        def unload_selected():
            for source in selected_sources():
                source["checked"] = []
            if merged[0]:
                for source in selected_sources():
                    engine.fs_tree_toggle_source(merged[0], sources.index(source), False)
            refresh_package_tree()
            say(log, status, "已取消加载 %d 个源" % len(selected_sources()))

        def select_all():
            # ★ 用户口径：「全选包，功能是**选中**而非勾选。」
            keys = [source["key"] for source in sources]
            for key in keys:
                ui.tree_select(package_tree, key)
            say(log, status, "已选中 %d 个源" % len(keys))

        def clear_all():
            sources.clear()
            if merged[0]:
                engine.fs_tree_free(merged[0])
                merged[0] = None
            refresh_package_tree()
            refresh_entry_tree()
            say(log, status, "已清空")

        def add_menu():
            # 形态差异①：门面没有"左键弹菜单" ⇒ 走右键菜单（两模式）
            ui.context_menu(package_tree, ["选包文件…", "选文件夹…"],
                            lambda label: pick_for_add(label))

        def pick_for_add(label):
            if label == "选文件夹…":
                picked = ui.ask_directory("选择要加载的文件夹")
                if picked:
                    load_paths([picked])
                return
            picked = ui.ask_open_file("选择要加载的包")
            if picked:
                load_paths([picked])

        add_button = ui.button(left, "加包（右键选模式）", add_menu)
        ui.button(left, "全选", select_all)
        ui.button(left, "加载选中", load_selected)
        ui.button(left, "取消加载", unload_selected)
        ui.button(left, "清空", clear_all)
        ui.button(right, "包内全选", lambda: check_all(True))
        ui.button(right, "包内全不选", lambda: check_all(False))

        def check_all(checked):
            handle = merged[0]
            if handle:
                engine.fs_tree_toggle_all(handle, checked)
            for source in sources:
                source["checked"] = [row["path"] for row in source["entries"]] if checked else []
            refresh_package_tree()
            say(log, status, ("包内全选" if checked else "包内全不选") +
                "：%d 条" % (len(engine.fs_tree_checked_paths(handle)) if handle else 0))

        def on_search(text):
            ui.tree_set_filter(entry_tree, text)
            ui.set_text(entry_count, "条目 %d（命中 %d）" %
                        (len(rows), ui.tree_filter_hits(entry_tree)))

        def on_entry_click(key):
            # ★ 用户口径：文件夹单击＝展开/折叠；文件点条目身体＝切勾选
            checked = ui.tree_checked(entry_tree, key)
            ui.tree_set_checked(entry_tree, key, not checked)
            sync_checked_from_tree()

        def on_entry_check(key, checked):
            sync_checked_from_tree()

        def sync_checked_from_tree():
            """把树上的勾选态写回"每个源的 checked"（用于包列表的计数与快照）。"""
            if not merged[0]:
                return
            chosen = set(engine.fs_tree_checked_paths(merged[0]))
            for source in sources:
                source["checked"] = [row["path"] for row in source["entries"]
                                     if row["path"] in chosen]
            refresh_package_tree()

        def start_unpack(only_selected):
            jobs = []
            key = current_format_key()
            out_root = ui.get_text(out_entry).strip()
            if not out_root:
                out_root = engine.fs_resolve_output_root(
                    sources[0]["path"] if sources else "", os.getcwd())
            plan_input = []
            for source in sources:
                if source["kind"] != "package":
                    continue
                entries = [row["path"] for row in source["entries"]]
                plan_input.append({"package": source["path"], "entries": entries})
            if not plan_input:
                say(log, status, "请先加包（左栏）")
                return
            plan = engine.fs_plan_unpack_targets(out_root, plan_input)
            only = None
            if only_selected:
                chosen = []
                for source in sources:
                    chosen.extend(source["checked"])
                if not chosen:
                    say(log, status, "解包选中：一个条目都没勾")
                    return
                only = chosen
            want = dict((item["image"], item) for item in plan)
            for row in plan_input:
                jobs.append(want[row["package"]])
            host.pref_set(PREF, "out_root", out_root)
            say(log, status, "解包 %d 个包 → %s" % (len(jobs), out_root))

            def worker(progress, message):
                progress(0)
                done = 0
                for index, item in enumerate(jobs):
                    got = engine.fs_extract_package(item["image"], item["target"],
                                                    "listed" if only else "all",
                                                    only, key)
                    lines = ["%s ⇒ %s：文件 %d / 目录 %d / 字节 %d" %
                             (os.path.basename(item["image"]), item["target"],
                              got["files_written"], got["dirs_created"], got["bytes_written"])]
                    if got["overwritten_count"]:
                        # ★ 覆盖必须可见（用户报过"3.9 万个文件被盖"）
                        lines.append("★ 覆盖了已存在的文件 %d 个（逐条见 CLI/日志）" %
                                     got["overwritten_count"])
                    if got["failure_count"]:
                        lines.append("失败 %d 项：%s" % (got["failure_count"],
                                                    got["failure_note"] or "(无理由)"))
                    if not got["ok"]:
                        lines.append("整包没跑起来：" + (got["error"] or "(无理由)"))
                    for line in lines:
                        message(line)
                    done += 1
                    progress(int(done * 100 / max(1, len(jobs))))
                message("解包结束：%d 个包" % len(jobs))
                progress(100)

            def on_message(text):
                ui.log_view_append(log, text)
                ui.set_text(status, text)

            ui.task_on_message(panel, on_message)
            if not ui.task_run(panel, "解包", worker):
                say(log, status, "已有任务在跑，请等它结束")

        # ══ 封包页 ══════════════════════════════════════════════════════════
        pack_choices_text = ["%s：%s" % (key, label) for key, label in pack_choices]
        pack_keys = [key for key, _label in pack_choices]
        pack_combo = ui.combo(page_pack, pack_choices_text)
        pack_out = ui.entry(page_pack, host.pref_get(PREF, "pack_out", ""), 50)
        pack_items = []          # [{"path","source_path","is_dir"}]
        pack_count = ui.label(page_pack, "待打包 0")
        pack_list = ui.list(page_pack, 20)

        def pack_key():
            value = ui.combo_value(pack_combo) or ""
            key = value.split("：", 1)[0]
            return key if key in pack_keys else (pack_keys[0] if pack_keys else "sqfs")

        def refresh_pack_list():
            ui.list_set_row_count(pack_list, len(pack_items))
            items = list(pack_items)

            def provider(row):
                return "%s  ←  %s" % (items[row]["path"], items[row]["source_path"]) \
                    if 0 <= row < len(items) else ""
            ui.list_set_row_text(pack_list, provider)
            ui.set_text(pack_count, "待打包 %d" % len(pack_items))

        def add_pack_files():
            picked = ui.ask_open_file("选择要打进去的文件")
            if not picked:
                return
            base = os.path.dirname(picked)
            got = engine.fs_collect_pack_files(base, [picked])
            for item in got["items"]:
                pack_items.append({"path": item["rel"], "source_path": item["source_path"],
                                   "is_dir": False})
            refresh_pack_list()
            say(log, status, "＋文件 %d（越界 %d / 包文件 %d）" %
                (len(got["items"]), got["skipped_outside_base"], got["skipped_package_files"]))

        def add_pack_dir():
            picked = ui.ask_directory("选择要打进去的目录（递归，自动排除包文件）")
            if not picked:
                return
            got = engine.fs_collect_pack_files(picked, [picked])
            for item in got["items"]:
                pack_items.append({"path": item["rel"], "source_path": item["source_path"],
                                   "is_dir": False})
            refresh_pack_list()
            say(log, status, "＋目录 %d 个文件（越界 %d / 包文件 %d）" %
                (len(got["items"]), got["skipped_outside_base"], got["skipped_package_files"]))

        def remove_pack_selected():
            row = ui.list_selected_row(pack_list)
            if row is None or row < 0 or row >= len(pack_items):
                say(log, status, "请先在待打包列表里点一行")
                return
            removed = pack_items.pop(row)
            refresh_pack_list()
            say(log, status, "已移除：" + removed["path"])

        def clear_pack():
            pack_items.clear()
            refresh_pack_list()
            say(log, status, "已清空待打包列表")

        def pick_pack_out():
            key = pack_key()
            suggestion = engine.fs_pack_ext_for(key)
            picked = ui.ask_save_file("封包输出文件（扩展名 %s）" % suggestion)
            if picked:
                ui.set_text(pack_out, picked)
                host.pref_set(PREF, "pack_out", picked)

        def start_pack():
            key = pack_key()
            out = ui.get_text(pack_out).strip()
            if not pack_items:
                say(log, status, "待打包列表是空的")
                return
            target = engine.fs_resolve_pack_target(out, "packed", key)
            if target["needs_frontend_dialog"] and not out:
                picked = ui.ask_save_file("封包输出文件")
                if not picked:
                    return
                out = picked
            out = out or target["path"]
            host.pref_set(PREF, "pack_out", out)
            items = list(pack_items)

            def worker(progress, message):
                progress(0)
                if engine.fs_format_is_sqfs(key):
                    got = engine.fs_pack_squashfs(out, items)
                    if got["ok"]:
                        message("封包完成（SquashFS）：%d 个文件、%d 字节 → %s" %
                                (len(items), got["file_size"], out))
                    else:
                        message("封包失败（SquashFS）：" + (got["error"] or "(无理由)"))
                else:
                    got = engine.fs_pack_db(out, items, key)
                    if got["ok"]:
                        message("封包完成（%s）：%d 个文件、%d 字节 → %s" %
                                (key, got["item_count"], got["file_size"], out))
                    else:
                        message("封包失败（%s）：%s" % (key, got["error"] or "(无理由)"))
                    # ★ 名字改写台账**必须逐条显示**（零静默）
                    if got["rewrites"]:
                        message("警告：%d 个条目名被改写（cp1251 表示不了的字符顶替成 '?'）" %
                                len(got["rewrites"]))
                        for row in got["rewrites"]:
                            message("  改名：%s → %s" % (row["source"], row["written"]))
                    else:
                        message("名字改写：无（%d 个条目名都能用 cp1251 原样写出）" %
                                got["item_count"])
                progress(100)

            def on_message(text):
                ui.log_view_append(log, text)
                ui.set_text(status, text)

            ui.task_on_message(panel, on_message)
            if not ui.task_run(panel, "封包", worker):
                say(log, status, "已有任务在跑，请等它结束")

        ui.button(page_pack, "＋文件", add_pack_files)
        ui.button(page_pack, "＋目录", add_pack_dir)
        ui.button(page_pack, "移除", remove_pack_selected)
        ui.button(page_pack, "清空", clear_pack)
        ui.button(page_pack, "选择输出…", pick_pack_out)
        ui.button(page_pack, "封包", start_pack)

        # ── 接线（回调 / 拖放 / 搜索 / 树语义）─────────────────────────────
        ui.entry_on_change(search, on_search)
        ui.tree_on_expand(entry_tree, on_entry_expand)
        ui.tree_on_check(entry_tree, on_entry_check)
        ui.tree_on_click(entry_tree, on_entry_click)
        ui.tree_on_action(package_tree, "✕", remove_source)
        ui.tree_on_click(package_tree, lambda key: ui.tree_set_expanded(
            package_tree, key, key not in (ui.tree_expanded_keys(package_tree) or [])))
        ui.tree_on_activate(package_tree, lambda key: load_selected())
        ui.drop_zone_on_drop(zone, load_paths)
        ui.pack(None, ui.button(page_unpack, "解包选中", lambda: start_unpack(True)), "top", "", False, 0, 0)
        ui.button(page_unpack, "批量解包", lambda: start_unpack(False))
        ui.button(page_unpack, "选择…", pick_out_root)
        ui.button(page_unpack, "打开", open_out_root)
        refresh_package_tree()
        refresh_entry_tree()
        refresh_pack_list()
        api.log("文件系统（插件版）：已注册，replaces=fs")

    api.register_tool("fs_py", build, 0, "fs", "文件系统（插件版）", "文件系统")
