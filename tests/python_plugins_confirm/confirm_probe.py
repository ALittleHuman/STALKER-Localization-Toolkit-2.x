# -*- coding: utf-8 -*-
"""P17-122 判据用的探针插件：**注入一个确认器**，看引擎真问不问。

口径（与判据一致）：
· `--py-plugin-call … confirm_probe.py {"action":"with_confirm", …}` ⇒ 注册确认器后跑转换
  ⇒ 引擎**必须问到**（返回值里 `asked` 为真），且按答走（`outcome` 非 needs_confirm）；
· `action":"without_confirm"` ⇒ **不注册**（并显式卸载，保证回到"没有窗口可弹"）
  ⇒ 引擎必须返回 `needs_confirm=True`（**不许**猜、不许静默按 auto 继续）。
"""
ASKED = []


def _answer(verdict):
    def cb(request):
        ASKED.append(request)
        return (verdict, "")
    return cb


def register(api):
    """加载链要求导出 `register(api)`；本插件是**探针**（判据走 headless），不注册界面。"""
    return None


def headless(api, args):
    engine = api.engine
    action = args.get("action", "without_confirm")
    if action == "with_confirm":
        ok = engine.set_encoding_confirm(_answer("confirmed"))
    else:
        engine.set_encoding_confirm(None)       # 显式卸载 ⇒ 回到"没有窗口可弹"
        ok = False
    out = engine.text_convert_batch(
        {"source": args["source"], "output_dir": args["out_dir"],
         "source_encoding": "auto", "target_encoding": "utf-8"},
        {"cancel": lambda: False, "message": lambda line: None,
         "progress": lambda done, total: None})
    return {"registered": bool(ok), "asked": len(ASKED),
            "asked_question": (ASKED[0].get("question", "") if ASKED else ""),
            "needs_confirm": bool(out.get("needs_confirm")),
            "exit_code": out.get("exit_code"), "success": out.get("success"),
            "failed": out.get("failed")}
