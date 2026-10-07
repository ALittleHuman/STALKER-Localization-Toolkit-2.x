# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（插件改装 P2b）——**好**的那一枚。

它同时是"注册 ↔ 消费"两端的判据夹具：
  · 命令 handler 往**标记文件**里追加一行（路径 = 环境变量 `XRKIT_PY_MARK`，缺省落系统临时目录）
    —— 探针/自检据此证明"回调真的从 C++ 回到了 Python"；
  · `api.log()` 写一行 —— 证明插件日志通道通到宿主（`take_python_log_lines()`）；
  · `register()` 自己也会写一行 `register:<文件名>` —— 证明"注册期真的跑过"（不是只 import 了）。

契约（第一版 API 表）：插件文件导出 `register(api)`；`api` 由宿主给（`xrkit.Api`）。
"""

import os
import tempfile

PLUGIN_INFO = {"name": "夹具插件", "version": "1.0"}


def _mark(text):
    path = os.environ.get("XRKIT_PY_MARK") or os.path.join(
        tempfile.gettempdir(), "xrkit_py_plugin_marker.txt")
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(text + "\n")
    return path


def register(api):
    api.log("夹具插件：register 开始")
    _mark("register:" + api.plugin_file)

    def on_click():
        # ★ 这一行是"回调真的回到 Python"的**证据**（C++ 侧调 handler ⇒ 走回解释器）
        _mark("click")
        api.log("夹具插件：命令被调用")

    command_id = api.register_command("greet", "夹具问候", on_click)
    if command_id is None:
        raise RuntimeError("register_command 失败（宿主拒绝了注册）")
    if not api.register_menu_item("fs", "toolbar", command_id, 0):
        raise RuntimeError("register_menu_item 失败（host/location 不对？）")
    # 两类"数据面"注册点也各来一条：下拉条目 + 新建下拉
    api.register_option_entry("fs", "top_bar", "夹具下拉值")
    api.register_option("夹具下拉", ["甲", "乙"], None, "fs", "top_bar", None, 0)
