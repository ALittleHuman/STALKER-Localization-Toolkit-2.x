# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（控件门面 P3）——用 `api.ui.*` 造一个**插件页**。

它是"控件门面（L1/L2）真的能用"这条判据的夹具：
  · `register_tool(name, build)` 注册一个 Hub 页；`build()` **什么都不返回**，
    只往宿主给的"当前面板根"里塞控件 —— 这正是 L1 的写法（宿主负责排版，`place()`）；
  · 点按钮时：把**输入框的当前文本**写进标记文件（证明 `get_text` 真的读到了控件），
    再把标签文本改掉（证明 `set_text`）、并 `ui.log()` 往宿主日志栏写一行；
  · 下拉/勾选框的回调也各写一行（证明 L1 的回调通道通）。

标记落点：`<api.plugin_dir>/ui_marker.txt`（夹具目录由宿主给，自检用临时目录 ⇒ 不污染仓库）。
"""

import os

PLUGIN_INFO = {"name": "控件门面夹具", "version": "1.0"}


def register(api):
    def mark(text):
        # 落到**插件自己的目录**里（自检用临时目录，因此不会污染仓库）
        with open(os.path.join(api.plugin_dir, "ui_marker.txt"), "a", encoding="utf-8") as handle:
            handle.write(text + "\n")

    def build():
        ui = api.ui
        label = ui.label(None, "控件门面夹具：标签")
        entry = ui.entry(None, "初值", 24)
        ui.combo(None, ["甲", "乙"], lambda value: mark("combo:" + value))
        ui.checkbox(None, "勾我", lambda on: mark("check:%d" % int(on)))
        ui.text_view(None, 4, True)

        def on_click():
            mark("click:" + ui.get_text(entry))
            ui.set_text(label, "点过了")
            ui.log("控件门面夹具：按钮被点了")

        ui.button(None, "夹具按钮", on_click)
        mark("built")

    if not api.register_tool("控件夹具页", build, 5):
        raise RuntimeError("register_tool 失败（宿主拒绝了注册）")
    api.log("控件门面夹具：register 完成")
