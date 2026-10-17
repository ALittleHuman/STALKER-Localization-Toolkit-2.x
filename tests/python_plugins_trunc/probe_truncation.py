# -*- coding: utf-8 -*-
"""P17-134 判据用的探针插件：问引擎要散装条目表，把**截断信号**带回来。

★ 它存在的理由：`plugins/fs_page.py` 的 `collect_entries` 是**产品侧**消费点，
  但判据要在**无 GUI、无插件页**的场合也能判 ⇒ 用一个最小探针走**同一条链**：
  Python → `py_host` dict → `EngineFacade::fs_loose_entries` → `fs::loose_entries`。
"""
PLUGIN_API = 1
PLUGIN_INFO = {"name": "截断链路探针（判据用）", "version": "1.0"}


def register(api):
    """加载链要求导出 `register(api)`；本插件是**探针**（判据走 headless），不注册界面。"""
    return None


def headless(api, args):
    got = api.engine.fs_loose_entries(args["root"])
    if got is None:
        return {"available": False}
    return {
        "available": True,
        "count": len(got["entries"]),
        "error": got.get("error", ""),
        # ★ 关键：这两个键**必须来自 py_host 的 dict**；缺了就是"信号在门面/Python 处丢了"
        "has_key": "truncated" in got,
        "truncated": bool(got.get("truncated", False)),
        "reason": got.get("truncated_reason", ""),
    }
