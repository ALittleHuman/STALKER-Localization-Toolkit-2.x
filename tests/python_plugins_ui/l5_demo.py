# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（控件门面 **重型 L2 第二批**：滚动视口 / 可分栏）。

演示 P5 的两件"框架件"（六页每一页都要）：
  · `api.ui.viewport` —— 宿主造「容器 + 视口 + 画布」，**句柄＝画布**（插件往它里面塞控件）。
    条子只由宿主造；`viewport_sync()` 在任何内容/尺寸变化后调一次。
    视口里的长列表会自动把"内容总高"交给视口（宿主接线，插件不用管）。
  · `api.ui.split` —— 可分栏容器：`split_add_pane` 返回**该格容器**，插件往里面塞控件；
    每格可设上限、可完全收起。

标记落点：`<api.plugin_dir>/l5_marker.txt`（自检用临时目录 ⇒ 不污染仓库）。
"""

import os

PLUGIN_INFO = {"name": "控件门面 L2b 夹具", "version": "1.0"}


def register(api):
    def mark(text):
        with open(os.path.join(api.plugin_dir, "l5_marker.txt"), "a", encoding="utf-8") as handle:
            handle.write(text + "\n")

    def build():
        ui = api.ui
        body = ui.viewport(None, 1200)
        for index in range(6):
            ui.label(body, "长内容第 %d 行" % index)
        ui.viewport_sync(body)
        mark("vp_can_scroll:%d" % int(ui.viewport_can_scroll(body)))
        mark("vp_top:%d" % ui.viewport_scroll_top(body))

        body2 = ui.viewport(None, 0)
        rows = ui.list(body2, 18)
        ui.list_set_row_count(rows, 200)
        ui.viewport_sync(body2)
        mark("list_vp_can_scroll:%d" % int(ui.viewport_can_scroll(body2)))

        split = ui.split(None, True)
        for index in range(3):
            cell = ui.split_add_pane(split, 40)
            ui.label(cell, "格 %d" % index)
        mark("panes:%d" % ui.split_pane_count(split))
        ui.split_set_max_size(split, 0, 180)
        ui.split_set_collapsed(split, 2, True)
        mark("collapsed2:%d" % int(ui.split_is_collapsed(split, 2)))
        mark("sash0:%d" % ui.split_sash_pos(split, 0))

    if not api.register_tool("门面L2b夹具页", build, 7):
        raise RuntimeError("register_tool 失败")
    api.log("控件门面 L2b 夹具：register 完成")
