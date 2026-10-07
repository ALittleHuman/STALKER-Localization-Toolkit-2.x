# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（控件门面 **重型 L2 第一批**）。

演示 P4 的四件宿主组件（都在宿主里渲染/滚动/线程化，插件只给数据与回调）：
  · `api.ui.list` —— 虚拟化行列表（`list_set_row_text(provider)` **只为可见行**取文本）；
  · `api.ui.log_view` —— 插件**自己**的日志区（一次一行追加、可清空）；
  · `api.ui.drop_zone` —— 拖入区域（宿主判定"本地文件才接"）；
  · `api.ui.task_panel` + `task_run` —— 宿主任务壳：worker 在**工作线程**里跑，收
    `progress(0~100)` / `message(text)` 两个 sink；★worker 里**不许**调别的 `ui.*`
    （会被 `on_ui_thread` 拒掉，`ui.label(...) is None` 就是判据）。

标记落点：`<api.plugin_dir>/l2_marker.txt`（自检用临时目录 ⇒ 不污染仓库）。
"""

import os
import time

PLUGIN_INFO = {"name": "控件门面 L2 夹具", "version": "1.0"}


def register(api):
    def mark(text):
        with open(os.path.join(api.plugin_dir, "l2_marker.txt"), "a", encoding="utf-8") as handle:
            handle.write(text + "\n")

    def build():
        ui = api.ui
        calls = {"n": 0}
        rows = ui.list(None, 18)

        def provider(row):
            calls["n"] += 1
            mark("row:%d" % row)
            return "第 %d 行" % row

        ui.list_set_row_count(rows, 200)
        ui.list_set_row_text(rows, provider)
        mark("provider_calls:%d" % calls["n"])          # ← 应当是个小数字（可见行 + overscan）
        mark("rowtext199:%s" % ui.list_row_text(rows, 199))   # ← 按需读任意行
        ui.list_select_row(rows, 7)
        mark("selected:%d" % ui.list_selected_row(rows))
        mark("content_height:%d" % ui.list_content_height(rows))

        view = ui.log_view(None, 4)
        ui.log_view_append(view, "第一行")
        ui.log_view_append(view, "第二行")
        mark("viewtext:%s" % ui.log_view_text(view).replace("\n", "|"))
        ui.log_view_clear(view)
        mark("after_clear:[%s]" % ui.log_view_text(view))

        zone = ui.drop_zone(None, "把目录拖进来")
        ui.drop_zone_on_drop(zone, lambda paths: mark("drop:" + ",".join(sorted(paths))))

        panel = ui.task_panel(None, True)

        def worker(progress, message):
            mark("worker_thread_ui:%s" % (ui.label(None, "不该出现") is None))
            message("开始干活")
            progress(50)
            time.sleep(0.05)
            progress(100)
            mark("worker_done")

        mark("run:%d" % int(ui.task_run(panel, "夹具任务", worker)))

    if not api.register_tool("门面L2夹具页", build, 6):
        raise RuntimeError("register_tool 失败")
    api.log("控件门面 L2 夹具：register 完成")
