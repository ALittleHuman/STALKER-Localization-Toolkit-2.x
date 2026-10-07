# -*- coding: utf-8 -*-
"""坏夹具 ①：**注册到一半就抛异常**（插件改装 P2b）。

用途：证明"坏插件"那一侧的三件事同时成立 ——
  ① 宿主**不崩**（异常被接住、逐条留痕）；
  ② 它这次注册进表的东西**按 owner 全被回滚**（`Registry::residue_for` 必须为 0）——
     否则表里留着指向"半截插件"的回调（C ABI 那条 S1-2 的血账，同一套口径）；
  ③ 同目录里的**好插件照常加载**（坏的不许拖累别人）。
"""


def register(api):
    command_id = api.register_command("half", "半个命令", lambda: None)
    api.register_menu_item("fs", "toolbar", command_id, 0)
    api.register_option_entry("fs", "top_bar", "半截下拉值")
    raise RuntimeError("夹具：注册到一半就炸（用来验证按 owner 回滚）")
