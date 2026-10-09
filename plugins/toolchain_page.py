#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""汉化产线（插件版）—— **三个紧耦合插件同页共存**（P16-1c）。

规格 §七.7 把这三件并成一组：**自适应出汉化包 / 分区字体 / 自动二进制补丁**，
共享一个"统一产物模型"（字库 + ltx 改写 + xml 译文 + 二进制补丁 + 安装/还原 + 清单/台账）。
本页用 `ui.tabs` 分标签，三件各占一个：

  ① **自动二进制补丁**（已有）：目标框 + 识别/生成/清空 + 只读结果框 + 拖放区；
     站点表由本页持有（单一构建的特化数据），能力经 `api.engine.patch_*` 门面调用。
  ② **分区字体**（本批新增）：10 档字号槽位**分区**选择 + 出包 + **清单/台账**（`pkg_write_manifest`）。
  ③ **自适应出汉化包**（下一批）：扫源 ⇒ 编码 ⇒ 文本 ⇒ 字号自适应 ⇒ 出包 ⇒ 清单/台账。

为什么是"插件 + 引擎通用机器"
-----------------------------
判定/守卫/写盘/出包那套**机器**都在引擎里；本页只做两件事：**给数据**（站点表 / 档位选择）
与**画界面、报账**。这也是规格 §二"内核留 C++、插件只描述做什么"的落法。

判定纪律（用户红线）
--------------------
二进制补丁的结论只来自**逐项字节内容**，**绝不看"差了多少字节"**（`reference_diff_bytes` 只作参考）；
"档位"这类选项也一律以**产物内容**为判据，不看计数自报。
"""

import hashlib
import os

PLUGIN_API = 1

PLUGIN_INFO = {
    "name": "汉化产线（插件版）",
    "version": "0.3",
    "description": "三件同页：自动二进制补丁（引擎 UTF-8 1/3 字节判定）/ 分区字体（10 档槽位）/ "
                   "自适应出汉化包（下一批）。产物统一走清单（pkg_*）。",
    "experimental": True,
}

SECTION = "toolchain"

# ══════════════════════════════════════════════════════════════════════════
# 一、二进制补丁：站点表（本插件持有的**唯一事实来源**）
#
# 全部偏移都是**文件偏移**（不是 RVA）；本构建里 RVA = 文件偏移 + 0xC00。
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


# ══════════════════════════════════════════════════════════════════════════
# 二、面板 ①：自动二进制补丁
# ══════════════════════════════════════════════════════════════════════════
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
    if result is None:
        return "（宿主没有提供 patch 能力）"
    if result.get("error"):
        return "识别失败：" + str(result["error"])
    counts = result.get("counts", {}) or {}
    lines = ["结论: %s —— %s" % (result.get("verdict_text", "?"), result.get("detail", "")),
             "汇总: 原版 %s / 终态 %s / 保持未变 %s / 被动过 %s / 既非原版也非终态 %s"
             % (counts.get("orig", 0), counts.get("patched", 0), counts.get("kept", 0),
                counts.get("broken", 0), counts.get("other", 0)),
             "thunk: %s    空闲区全零: %s    文件大小: %s"
             % ("逐字节未变" if result.get("thunk_intact") else "**被动过**",
                "是" if result.get("cave_all_zero") else "否", result.get("file_size", 0)),
             "（参考：与『本构建原版』在目录覆盖范围内共有 %s 个字节不同；**判定不看它**）"
             % result.get("reference_diff_bytes", 0),
             "",
             "逐项判态："]
    for item in result.get("items", []) or []:
        lines.append("  %-16s 0x%08X  %s%s" % (item.get("id", "?"), item.get("offset", 0),
                                               item.get("state", "?"),
                                               ("（" + item["context_problem"] + "）")
                                               if item.get("context_problem") else ""))
    return "\n".join(lines)


def format_apply(result):
    if result is None:
        return "（宿主没有提供 patch 能力）"
    lines = list(result.get("notes", []) or [])
    lines.append("")
    if result.get("ok"):
        lines.append("结果: 已写出 %s（**输入未被改动**）" % result.get("out_path", ""))
        lines.append("写入自检: 逐项目标终态逐字节吻合、preserve 项未变、改动集合 == 目录声明"
                     "（本次写入 %s 个字节；参考值，判定不看它）" % result.get("written_bytes", 0))
    else:
        lines.append("结果: **未写出任何文件**")
        lines.append("原因: %s" % result.get("reason", ""))
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════════════
# 三、面板 ②：分区字体（10 档字号槽位分区 + 出包 + 清单）
# ══════════════════════════════════════════════════════════════════════════
def collect_manifest_entries(root):
    """把出包产出的文件逐条登记（rel 相对 root、sha256 现算；清单自身不登记）。"""
    entries, rels = [], []
    for base, _dirs, names in os.walk(root):
        for name in sorted(names):
            path = os.path.join(base, name)
            rel = os.path.relpath(path, root).replace("\\", "/")
            if rel == "manifest.json":
                continue
            with open(path, "rb") as handle:
                digest = hashlib.sha256(handle.read()).hexdigest()
            entries.append({"rel": rel, "source_path": path, "sha256": digest})
            rels.append(rel)
    entries.sort(key=lambda item: item["rel"])
    return entries, sorted(rels)


def font_pack_regions(api, options):
    """出包 + 写清单（**唯一一份**：界面按钮与无界面入口都走它）。

    `options["only_sizes"]` 就是"分区"：空 ⇒ 引擎按全部 10 档出；非空 ⇒ 只出这些档。
    """
    result = api.engine.font_pack(options)
    out = {"ok": bool(result and result.get("ok")), "result": result, "manifest": None,
           "files": [], "only_sizes": list(options.get("only_sizes", []))}
    if not out["ok"]:
        return out
    out_dir = options.get("out_dir") or ""
    entries, files = collect_manifest_entries(out_dir)
    out["files"] = files
    if entries:
        out["manifest"] = api.engine.pkg_write_manifest(
            os.path.join(out_dir, "manifest.json"), entries)
    return out


def format_regions(out):
    if out is None:
        return "（宿主没有提供 font 能力）"
    result = out.get("result") or {}
    if not out.get("ok"):
        return "结果: **未出包**\n原因: %s" % result.get("error", "")
    lines = ["结果: 已出包 → %s" % result.get("out_gamedata", ""),
             "本次档位: %s" % (out.get("only_sizes") or "（空＝全部档位）"),
             "字库 %s 个 / XML 改写 %s 个" % (result.get("fonts_written", 0),
                                             result.get("xml_written", 0)),
             "产物 %d 个文件；清单 %s" % (len(out.get("files", [])),
                                       ("好（ok=%s）" % (out.get("manifest") or {}).get("ok"))
                                       if out.get("manifest") else "**没写成**")]
    for warning in result.get("warnings", []) or []:
        lines.append("警告: %s" % warning)
    return "\n".join(lines)


# ══════════════════════════════════════════════════════════════════════════
# 四、无界面入口（判据用它；`--py-plugin-call … --plugin toolchain_page.py <json>`）
# ══════════════════════════════════════════════════════════════════════════
def headless(api, args):
    action = str(args.get("action", "identify"))
    if action == "identify":
        return identify(api, args.get("path", ""))
    if action == "apply":
        return apply(api, args.get("src", ""), args.get("out", ""),
                     bool(args.get("force", False)))
    if action == "font_regions":
        options = {
            "game": str(args.get("game", "SoC")),
            "lang": str(args.get("lang", "chs")),
            "xml_dir": str(args.get("xml_dir", "")),
            "font_path": str(args.get("font_path", "")),
            "out_dir": str(args.get("out_dir", "")),
            "offset": int(args.get("offset", 0)),
            "suffix": str(args.get("suffix", "")),
            "full_cyrillic": bool(args.get("full_cyrillic", True)),
            "only_sizes": [int(x) for x in (args.get("only_sizes") or [])],
        }
        return font_pack_regions(api, options)
    return {"ok": False, "error": "未知动作：" + action}


# ══════════════════════════════════════════════════════════════════════════
# 五、界面：一页多标签（本批交付前两个；"自适应出汉化包"下一批）
# ══════════════════════════════════════════════════════════════════════════
def _build_patch_tab(api, host, tab):
    ui = api.ui
    ui.label(tab, "引擎 exe（识别＝看它是什么形态；生成＝从原版出一份补丁副本）")
    target_entry = ui.entry(tab, host.pref_get(SECTION, "target", ""), 60)
    result_view = ui.text_view(tab, 18, True)

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

    ui.button(tab, "识别", do_identify)
    ui.button(tab, "生成…", do_apply)
    ui.button(tab, "清空", lambda: show(""))
    zone = ui.drop_zone(tab, "把 xrEngine.exe 拖到这里（等价于识别）")

    def on_drop(paths):
        # 拖入 = 识别（与 1.x 面板同义）；只认第一个路径
        if paths:
            do_identify(str(paths[0]))

    ui.drop_zone_on_drop(zone, on_drop)


def _build_font_tab(api, host, tab):
    ui = api.ui
    slots = api.engine.font_size_slots() or []
    ui.label(tab, "分区字体：勾选要生成的**字号档位**（一个都不勾＝全部 %d 档）" % len(slots))
    boxes = []
    for slot in slots:
        boxes.append((slot, ui.checkbox(tab, "档 %d" % slot, None)))

    ui.label(tab, "文本 XML 目录（只扫顶层）")
    xml_entry = ui.entry(tab, host.pref_get("font", "xml_dir", ""), 60)
    ui.label(tab, "字体文件（.ttf/.ttc/.otf）")
    font_entry = ui.entry(tab, host.pref_get("font", "path", ""), 60)
    ui.label(tab, "输出目录")
    out_entry = ui.entry(tab, host.pref_get(SECTION, "out_dir", ""), 60)
    status = ui.text_view(tab, 12, True)

    def pick_font():
        def chosen(path):
            if path:
                ui.set_text(font_entry, path)
                host.pref_set("font", "path", path)

        ui.pick_font(ui.get_text(font_entry).strip(), chosen)

    ui.button(tab, "选字体…", pick_font)

    def run_pack():
        chosen_slots = [slot for slot, box in boxes if ui.checkbox_state(box)]
        options = {
            "game": "SoC", "lang": "chs",
            "xml_dir": ui.get_text(xml_entry).strip(),
            "font_path": ui.get_text(font_entry).strip(),
            "out_dir": ui.get_text(out_entry).strip(),
            "offset": 0, "suffix": "", "full_cyrillic": True,
            "only_sizes": chosen_slots,
        }
        host.pref_set("font", "xml_dir", options["xml_dir"])
        host.pref_set(SECTION, "out_dir", options["out_dir"])
        ui.set_text(status, format_regions(font_pack_regions(api, options)))
        ui.log("分区字体：出包 " + options["out_dir"])

    ui.button(tab, "按所选档位出包…", run_pack)


def register(api):
    def build():
        ui = api.ui
        host = api.host
        tabs = ui.tabs(None)
        _build_patch_tab(api, host, ui.tabs_add_page(tabs, "自动二进制补丁"))
        _build_font_tab(api, host, ui.tabs_add_page(tabs, "分区字体"))
        return None

    # ★ **新增页**（不覆盖内置页）⇒ replaces 留空；三件同页见各自的标签。
    api.register_tool("toolchain_py", build, 6, "", "汉化产线（插件版）", "汉化产线")
    api.log("汉化产线（插件版）：已注册（新增页；面板：自动二进制补丁 / 分区字体）")
