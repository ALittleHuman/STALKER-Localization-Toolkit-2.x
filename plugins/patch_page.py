#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""xrEngine「1 或 3 字节 UTF-8」字符判定补丁 —— **插件页**（P16-1c）。

为什么是"插件 + 引擎通用机器"
-----------------------------
判定/守卫/写盘那套**机器**在引擎里（`engine::patch`，走 `api.engine.patch_*` 门面）；
本页只做两件事：① **给表**（`PATCH_SITES`，单一构建的特化数据）；② **画界面/报账**。
表由插件持有的理由（用户 2026-10-09 过目的设计笔记 §二）：1.x 表的原设计意图就是
"收录一处就能补一处、引擎代码一行不用改"，而表是**单一构建**的（换个模组/换次编译就作废）
⇒ 按本仓红线（不许拿特化数据当语义判据）不焊进内核。

判定纪律（用户红线）：结论只来自**逐项字节内容**（expect/patched/周边锚/thunk 完整性/
空闲区是否全零），**绝不用"差了多少字节"** —— 实测 v3 与 v4 的差异字节数完全相同（都是 88）。
`reference_diff_bytes` 只作报告参考值。

三种操作
--------
* **识别** —— 报结论（原版 / v4(正确) / v2(有缺陷) / v1 或 v3(有缺陷) / 未知）+ 逐项命中矩阵；
* **生成** —— 从**原版** exe 生成补丁副本；写前跑完全部守卫，任何一条不满足 ⇒ **不写**并逐条报原因；
  输入**永不改动**；输出已存在要先确认覆盖；
* **清空** —— 只清界面上的结果框，不碰文件。

无界面入口（`--py-plugin-call … --plugin patch_page.py <json>`）：
    {"action": "identify", "path": "<exe>"}  /  {"action": "apply", "src": …, "out": …, "force": bool}
"""

PLUGIN_API = 1

PLUGIN_INFO = {
    "name": "引擎二进制补丁（实验性）",
    "version": "0.2",
    "description": "识别 / 校验 / 生成 OGSR 系 x64 引擎的『1 或 3 字节 UTF-8』字符判定补丁；"
                   "偏移表针对单一引擎构建，属实验性功能。",
}

SECTION = "patch"

# ══════════════════════════════════════════════════════════════════════════
# 站点表 PATCH_SITES —— 本插件持有的**唯一事实来源**（收录一处就能补一处）
#
# 全部偏移都是**文件偏移**（不是 RVA）；本构建里 RVA = 文件偏移 + 0xC00。
#   .text: VA=0x1000  RawPtr=0x400  RawSize=0x00ED5400  → VA-RawPtr=0xC00
# kind: overwrite（整段换）/ call（等长改写成 `call helper`）/ helper（helper 机码落位）/
#       preserve（必须逐字节不变）
# ★ 判定只看"逐项字节内容"，不看差了多少字节。
# ══════════════════════════════════════════════════════════════════════════
PATCH_SITES = (
    {"id": "imm.cbMultiByte", "kind": "overwrite", "offset": 0x76C7C7, "expect_hex": "02", "patched_hex": "03", "before_hex": "24 28 01 00 00 00 4C 8B C7 48 89 44 24 20 41 B9", "after_hex": "00 00 00 33 D2 B9 E9 FD 00 00 FF 15 08 9E 76 00", "note": "cbMultiByte 立即数 02→03"},
    {"id": "call.1", "kind": "call", "offset": 0x76C931, "expect_hex": "41 8B C6 0F 95 C0 48 FF C0", "patched_hex": "E8 28 8E 76 00 90 90 90 90", "before_hex": "41 0F 2F C3 0F 97 C1 45 33 F6 45 84 C9 88 4D B0", "after_hex": "48 2B F8 84 C9 75 09 48 3B F7 0F 85 4C 02 00 00", "helper": "A", "note": "setne eax → call helper A"},
    {"id": "call.2", "kind": "call", "offset": 0x76C992, "expect_hex": "0F 95 C1 4C 8B C6 48 FF C1", "patched_hex": "E8 D0 8D 76 00 4C 8B C6 90", "before_hex": "54 24 68 33 DB 45 84 C9 8B CB 0F 57 C0 0F 57 C9", "after_hex": "4C 2B C2 4C 03 C1 48 89 4D 98 48 C7 C1 FF FF FF", "tail_hex": "4C 8B C6", "helper": "B", "note": "setne ecx → call helper B（保留原 `mov r8,rsi`）"},
    {"id": "call.3", "kind": "call", "offset": 0x76CBAA, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 AF 8B 76 00 90", "before_hex": "49 8B C6 4C 8B 74 24 48 45 84 C9 BA 00 00 00 00", "after_hex": "48 03 F0 48 3B F7 0F 82 94 FB FF FF 4C 8B 74 24", "helper": "A", "note": "setne eax → call helper A"},
    {"id": "call.4", "kind": "call", "offset": 0x12AC2D, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 3E AB DA 00 90", "before_hex": "B6 CD 80 E1 08 74 04 0F B6 57 01 84 C9 49 8B C3", "after_hex": "48 03 F8 EB 03 80 C2 FF 41 88 28 49 83 C0 0E 49", "helper": "C", "note": "setne eax → call helper C（读入口 ZF）"},
    {"id": "call.5", "kind": "call", "offset": 0x16A740, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 2B B0 D6 00 90", "before_hex": "8B 74 24 70 E9 43 FF FF FF A8 80 B8 00 00 00 00", "helper": "C", "note": "setne eax → call helper C；后窗在文件里出现 5 次（不唯一），只挂前窗"},
    {"id": "call.6", "kind": "call", "offset": 0x16A7A7, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 C4 AF D6 00 90", "helper": "C", "note": "setne eax → call helper C；本段被复制 4 份，锚不唯一 → 只靠绝对偏移"},
    {"id": "call.7", "kind": "call", "offset": 0x16A80E, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 5D AF D6 00 90", "helper": "C", "note": "setne eax → call helper C；锚不唯一 → 只靠绝对偏移"},
    {"id": "call.8", "kind": "call", "offset": 0x16A875, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 F6 AE D6 00 90", "helper": "C", "note": "setne eax → call helper C；锚不唯一 → 只靠绝对偏移"},
    {"id": "call.9", "kind": "call", "offset": 0x16A905, "expect_hex": "0F 95 C0 48 FF C0", "patched_hex": "E8 66 AE D6 00 90", "helper": "C", "note": "setne eax → call helper C；锚不唯一 → 只靠绝对偏移"},
    {"id": "helper.A", "kind": "helper", "offset": 0xED575E, "expect_hex": "00 00 00 00 00 00 00 00 00", "patched_hex": "41 0F B6 C1 8D 44 00 01 C3", "before_hex": "CC CC 48 8D 0D 49 3B 51 00 48 FF 25 F2 10 00 00", "note": "movzx eax,r9b ; lea eax,[rax+rax+1] ; ret（1→1，2→3）"},
    {"id": "helper.B", "kind": "helper", "offset": 0xED5767, "expect_hex": "00 00 00 00 00 00 00 00 00", "patched_hex": "41 0F B6 C9 8D 4C 09 01 C3", "note": "movzx ecx,r9b ; lea ecx,[rcx+rcx+1] ; ret"},
    {"id": "helper.C", "kind": "helper", "offset": 0xED5770, "expect_hex": "00 00 00 00 00 00 00 00 00 00 00", "patched_hex": "0F 95 C0 0F B6 C0 8D 44 00 01 C3", "note": "setne al ; movzx eax,al ; lea eax,[rax+rax+1] ; ret（读入口 ZF）"},
    {"id": "keep.thunk", "kind": "preserve", "offset": 0xED5757, "expect_hex": "48 FF 25 F2 10 00 00", "patched_hex": "48 FF 25 F2 10 00 00", "before_hex": "48 FF 25 02 11 00 00 CC CC 48 8D 0D 49 3B 51 00", "note": "import thunk；v1/v2/v3 就是覆盖了它才在退出时崩"},
)

# 挂载点常量（引擎侧同名同义）
RVA_DELTA = 0xC00
DEFECT_SHIFT = -2


def identify(api, path):
    """识别（判定半）：结论 + 逐项命中矩阵 + thunk/空闲区 + 参考差异数。"""
    if not path:
        return {"ok": False, "error": "没有目标文件", "verdict_key": "", "verdict_text": "",
                "items": [], "counts": {}, "thunk_intact": False, "cave_all_zero": False,
                "reference_diff_bytes": 0}
    return api.engine.patch_identify(path, PATCH_SITES)


def apply(api, src, out, force=False):
    """生成（写盘半）：写前全守卫 + 原子写；不匹配不许写、输入永不动。"""
    if not src:
        return {"ok": False, "reason": "没有目标文件", "out_path": "", "notes": [],
                "written_bytes": 0}
    if not out:
        return {"ok": False, "reason": "没有指定输出文件", "out_path": "", "notes": [],
                "written_bytes": 0}
    return api.engine.patch_apply(src, out, PATCH_SITES, bool(force))


def format_identify(result):
    """把识别结果排成给人看的几段（界面/日志共用同一份渲染）。"""
    if result is None:
        return "（宿主没有提供 patch 能力）"
    if result.get("error"):
        return "识别失败：" + str(result["error"])
    lines = ["结论: %s —— %s" % (result.get("verdict_text", "?"), result.get("detail", ""))]
    counts = result.get("counts", {}) or {}
    lines.append("汇总: 原版 %s / 终态 %s / 保持未变 %s / 被动过 %s / 既非原版也非终态 %s"
                 % (counts.get("orig", 0), counts.get("patched", 0), counts.get("kept", 0),
                    counts.get("broken", 0), counts.get("other", 0)))
    lines.append("thunk: %s    空闲区全零: %s    文件大小: %s"
                 % ("逐字节未变" if result.get("thunk_intact") else "**被动过**",
                    "是" if result.get("cave_all_zero") else "否",
                    result.get("file_size", 0)))
    lines.append("（参考：与『本构建原版』在目录覆盖范围内共有 %s 个字节不同；**判定不看它**）"
                 % result.get("reference_diff_bytes", 0))
    lines.append("")
    lines.append("逐项判态：")
    for item in result.get("items", []) or []:
        lines.append("  %-16s 0x%08X  %s%s" % (item.get("id", "?"), item.get("offset", 0),
                                               item.get("state", "?"),
                                               ("（" + item["context_problem"] + "）")
                                               if item.get("context_problem") else ""))
    return "\n".join(lines)


def format_apply(result):
    """把生成结果排成给人看的几段。"""
    if result is None:
        return "（宿主没有提供 patch 能力）"
    lines = list(result.get("notes", []) or [])
    if result.get("ok"):
        lines.append("")
        lines.append("结果: 已写出 %s（**输入未被改动**）" % result.get("out_path", ""))
        lines.append("写入自检: 逐项目标终态逐字节吻合、preserve 项未变、改动集合 == 目录声明"
                     "（本次写入 %s 个字节；参考值，判定不看它）"
                     % result.get("written_bytes", 0))
    else:
        lines.append("")
        lines.append("结果: **未写出任何文件**")
        lines.append("原因: %s" % result.get("reason", ""))
    return "\n".join(lines)


def headless(api, args):
    """**无界面入口**（`--py-plugin-call … --plugin patch_page.py <json>`）。"""
    action = str(args.get("action", "identify"))
    if action == "identify":
        return identify(api, args.get("path", ""))
    if action == "apply":
        return apply(api, args.get("src", ""), args.get("out", ""),
                     bool(args.get("force", False)))
    return {"ok": False, "error": "未知动作：" + action}


def register(api):
    ui = api.ui
    host = api.host

    def build():
        ui.label(None, "引擎 exe（识别＝看它是什么形态；生成＝从原版出一份补丁副本）")
        target_entry = ui.entry(None, host.pref_get(SECTION, "target", ""), 60)
        result_view = ui.text_view(None, 18, True)

        def show(text):
            ui.set_text(result_view, text)

        def current_target():
            return ui.get_text(target_entry).strip()

        def do_identify(path=""):
            target = path or current_target()
            if not target:
                picked = ui.ask_open_file("选择 xrEngine.exe")
                if not picked:
                    return
                target = picked
            ui.set_text(target_entry, target)
            host.pref_set(SECTION, "target", target)
            show(format_identify(identify(api, target)))
            ui.log("二进制补丁：识别 " + target)

        def do_apply():
            target = current_target()
            if not target:
                show("请先选择目标 exe（识别 或 拖入）")
                return
            # ★ 先识别：不是"未打补丁的原版"就**不写**（引擎侧的守卫链还会再拦一次）
            before = identify(api, target)
            if before is None or before.get("error"):
                show("识别失败：" + str((before or {}).get("error", "宿主没有提供 patch 能力")))
                return
            if before.get("verdict_key") != "orig":
                show("这份不是本构建**原版**（结论：%s）—— 生成只从原版出发，不改它。\n\n%s"
                     % (before.get("verdict_text", "?"), format_identify(before)))
                return
            out = ui.ask_save_file("补丁副本保存到…")
            if not out:
                return
            show(format_apply(apply(api, target, out)))
            ui.log("二进制补丁：生成 " + out)

        ui.button(None, "识别", do_identify)
        ui.button(None, "生成…", do_apply)
        ui.button(None, "清空", lambda: show(""))
        zone = ui.drop_zone(None, "把 xrEngine.exe 拖到这里（等价于识别）")

        def on_drop(paths):
            # 拖入 = 识别（与 1.x 面板同义）；只认第一个路径
            if paths:
                do_identify(str(paths[0]))

        ui.drop_zone_on_drop(zone, on_drop)
        return None

    api.register_tool("patch_py", build, 6, "", "引擎二进制补丁（实验性）", "二进制补丁")
    api.log("二进制补丁（插件版）：已注册（新增页，不覆盖内置页）")
