# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（**插件声明所需版本**，P7b）。

`PLUGIN_API = 999` ⇒ 比宿主（v1）新 ⇒ 加载器必须**当场拒绝**并留痕，
而且**一条注册都不能留下**（拒绝点放在 `register(api)` 之前）。
门禁 `[python-plugins]` 步会拿它判红：`loaded=0 errors=1`，理由里带"要求插件 API"。
"""

PLUGIN_INFO = {"name": "版本声明夹具", "version": "999"}
PLUGIN_API = 999     # ← 故意声明一个宿主还没有的版本

REGISTERED = []


def register(api):
    # 走到这里就说明"拒绝"没生效（本夹具**不该**跑到这一行）
    REGISTERED.append("register-called")
    api.register_command("too_new", "不该出现的命令", lambda when: None)
