#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# NON-PRODUCT-FIXTURE: 本文件是**插件 API 能力验证夹具**（非产品）——产品实现是 C++ 内置六页；不许当第二真源（P17-81）
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
           "files": [], "entries": [], "only_sizes": list(options.get("only_sizes", []))}
    if not out["ok"]:
        return out
    out_dir = options.get("out_dir") or ""
    entries, files = collect_manifest_entries(out_dir)
    out["files"] = files
    out["entries"] = entries
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
    if action == "pipeline":
        return run_pipeline(api, {
            "source_dir": str(args.get("source_dir", "")),
            "out_dir": str(args.get("out_dir", "")),
            "font_path": str(args.get("font_path", "")),
            "game": str(args.get("game", "SoC")),
            "lang": str(args.get("lang", "chs")),
            "prefix": str(args.get("prefix", "")),
            "id_prefix": str(args.get("id_prefix", "")),
            "clean": bool(args.get("clean", True)),
            "offset": int(args.get("offset", 0)),
            "suffix": str(args.get("suffix", "")),
            "only_sizes": [int(x) for x in (args.get("only_sizes") or [])],
        })
    if action in ("install", "restore"):
        out_dir = str(args.get("out_dir", ""))
        entries = args.get("entries") or load_entries_from_manifest(
            os.path.join(out_dir, "manifest.json"))
        target_root = str(args.get("target_root", ""))
        backup_root = str(args.get("backup_root", "") or os.path.join(out_dir, "backup"))
        if action == "install":
            return install_package(api, entries, target_root, backup_root)
        return restore_package(api, entries, target_root, backup_root)
    if action == "thread_probe":
        # ★★ P17-107（审计 `05 PLG-05`）：**"重活能不能搬进 worker"的取证入口**。
        #   它把六段重活各在**主线程**与**任务壳 worker**里跑一次，报告两边的
        #   `ui.on_ui_thread()` 取值与引擎调用成败 ⇒ "能不能搬"是**量出来的**，不是猜的。
        #   （无界面进程里没有任务壳 ⇒ 这条会如实报 unavailable，不假装。）
        return _thread_probe(api)
    return {"ok": False, "error": "未知动作：" + action}


# 六段重活（口径 A 的"能不能搬"清单；名字与界面按钮一一对应）
HEAVY_STEPS = (
    ("identify", lambda api: identify(api, "")),
    ("apply", lambda api: apply(api, "", "")),
    ("font_pack", lambda api: font_pack_regions(api, {"game": "SoC", "lang": "chs"})),
    ("pipeline", lambda api: run_pipeline(api, {"source_dir": "", "out_dir": ""})),
    ("install", lambda api: install_package(api, [], "")),
    ("restore", lambda api: restore_package(api, [], "")),
)


def _thread_probe(api):
    """逐段重活：先在主线程跑一次（看 `on_ui_thread`），再进 worker 跑一次。

    ★ 这里**故意用空参数**调引擎：目的不是"跑出正确结果"，而是**量出
    "这段引擎调用在哪个线程上执行、以及它能不能离开主线程"**（结果正确性由各自的
    `check_*` 判据管，不归这条管）。空参数只会让引擎很快返回一个错误 ——
    **"能不能被调用"这件事照样被证明**。
    """
    lines = []
    main_thread = bool(api.ui.on_ui_thread())
    lines.append("THREADPROBE main on_ui_thread=%s" % main_thread)
    for name, fn in HEAVY_STEPS:
        try:
            fn(api)
            called = "ok"
        except Exception as exc:                      # noqa: BLE001
            called = "raised:" + type(exc).__name__
        lines.append("THREADPROBE step=%s where=main called=%s" % (name, called))

    panel = api.ui.task_panel(None, True)
    if panel is None:
        lines.append("THREADPROBE worker=unavailable reason=无任务壳（无界面进程）")
        return {"ok": True, "lines": lines, "worker_available": False}

    seen = {}

    def on_message(text):
        lines.append("THREADPROBE worker-message=" + str(text).replace("\n", " | "))

    api.ui.task_on_message(panel, on_message)

    def worker_fn(progress, message):
        on_ui = bool(api.ui.on_ui_thread())
        seen["on_ui"] = on_ui
        message("worker on_ui_thread=%s" % on_ui)
        for name, fn in HEAVY_STEPS:
            try:
                fn(api)
                called = "ok"
            except Exception as exc:                  # noqa: BLE001
                called = "raised:" + type(exc).__name__
            message("worker step=%s called=%s" % (name, called))
        return True

    accepted = bool(api.ui.task_run(panel, "线程取证", worker_fn))
    lines.append("THREADPROBE worker=accepted accepts=%s" % accepted)
    return {"ok": True, "lines": lines, "worker_available": True}


# ══════════════════════════════════════════════════════════════════════════
# 三点五、面板 ③：自适应出汉化包（扫源 ⇒ 提取 ⇒ 字号自适应出包 ⇒ 统一清单 ⇒ 装/还原）
# ══════════════════════════════════════════════════════════════════════════
def kind_sub_dir(kind):
    """源目录类型 → 落盘子目录（与内置页/文本页**同一口径**：gameplay 之外都归 scripts）。"""
    return "gameplay" if kind == "gameplay" else "scripts"


def run_pipeline(api, options):
    """跑一遍产线（**唯一一份**：界面按钮与无界面入口都走它）。

    ① `text_extract_kind` 判源目录类型（gameplay / scripts）；
    ② `text_extract_dir` 把源文本提取/改写成目标树（落 `<out>/<类型>/`，xml 落 `<out>/`）；
    ③ `font_pack` 出字库（**字号自适应在引擎里**；`offset` / `only_sizes` 由调用方给）；
    ④ 把 `<out>` 下**全部产物**（改写的文本树 + 字库 + xml）登记成**一份清单**并原子写出 ——
       这就是规格说的"统一产物模型"（字库 + ltx 改写 + xml 译文 + 清单/哈希），
       之后 `pkg_install`/`pkg_restore` 直接吃这份清单。
    """
    out = {"ok": False, "error": "", "kind": None, "extract": None, "pack": None,
           "manifest": None, "entries": [], "files": []}
    source = options.get("source_dir") or ""
    out_dir = options.get("out_dir") or ""
    if not source or not out_dir:
        out["error"] = "源目录与输出目录都必须给"
        return out
    kind_result = api.engine.text_extract_kind(source)
    out["kind"] = kind_result
    if not kind_result or not kind_result.get("ok"):
        out["error"] = ("识别源目录类型失败："
                        + str((kind_result or {}).get("error", "宿主没有 text 能力")))
        return out
    target_dir = os.path.join(out_dir, kind_sub_dir(str(kind_result.get("kind", ""))))
    extract = api.engine.text_extract_dir(source, target_dir, out_dir,
                                          {"prefix": str(options.get("prefix", "")),
                                           "id_prefix": str(options.get("id_prefix", "")),
                                           "clean": bool(options.get("clean", True))})
    out["extract"] = extract
    if not extract or not extract.get("ok"):
        out["error"] = "文本提取失败：" + str((extract or {}).get("error", "宿主没有 text 能力"))
        return out
    packed = font_pack_regions(api, {
        "game": str(options.get("game", "SoC")), "lang": str(options.get("lang", "chs")),
        "xml_dir": out_dir, "font_path": str(options.get("font_path", "")),
        "out_dir": out_dir, "offset": int(options.get("offset", 0)),
        "suffix": str(options.get("suffix", "")), "full_cyrillic": True,
        "only_sizes": [int(x) for x in (options.get("only_sizes") or [])],
    })
    out["pack"] = packed
    if not packed.get("ok"):
        out["error"] = "出包失败：" + str((packed.get("result") or {}).get("error", ""))
        return out
    out["manifest"] = packed.get("manifest")
    out["entries"] = packed.get("entries", [])
    out["files"] = packed.get("files", [])
    out["ok"] = True
    return out


def install_package(api, entries, target_root, backup_root=""):
    """装进游戏根（引擎侧**先备份被覆盖的原件** + 逐条台账；覆盖必须可见）。"""
    if not target_root:
        return {"ok": False, "error": "没有指定游戏根", "installed": 0, "overwritten": 0,
                "backup_dir": "", "failures": []}
    if not entries:
        return {"ok": False, "error": "没有可装的条目（先跑一遍产线）", "installed": 0,
                "overwritten": 0, "backup_dir": "", "failures": []}
    return api.engine.pkg_install(target_root, entries, backup_root)


def restore_package(api, entries, target_root, backup_root=""):
    """还原：有备份 ⇒ 逐字节回；装了新文件 ⇒ 删掉（回退必须真的回到原样）。"""
    if not target_root:
        return {"ok": False, "error": "没有指定游戏根", "restored": 0, "removed": 0,
                "missing": [], "failures": []}
    rels = [str(item.get("rel", "")) for item in (entries or []) if item.get("rel")]
    if not rels:
        return {"ok": False, "error": "没有可还原的条目", "restored": 0, "removed": 0,
                "missing": [], "failures": []}
    return api.engine.pkg_restore(target_root, backup_root, rels)


def format_pipeline(out):
    if out is None:
        return "（宿主没有提供引擎能力）"
    if not out.get("ok"):
        return "结果: **没跑成**\n原因: %s" % out.get("error", "")
    extract = out.get("extract") or {}
    packed = out.get("pack") or {}
    result = packed.get("result") or {}
    manifest = out.get("manifest") or {}
    return "\n".join([
        "结果: 已出包 → %s" % result.get("out_gamedata", ""),
        "提取: 类型 %s · %s 个文件 · %s 条字符串 · %s 个字符"
        % (extract.get("kind", "?"), extract.get("files", 0), extract.get("strings", 0),
           extract.get("chars", 0)),
        "字库: %s 个档位（自适应由引擎负责） / XML 改写 %s 个"
        % (result.get("fonts_written", 0), result.get("xml_written", 0)),
        "统一清单: %s 条 → manifest.json（%s）"
        % (len(out.get("entries", [])), "好" if manifest.get("ok") else "**没写成**"),
        "产物 %d 个文件" % len(out.get("files", [])),
    ])


def format_install(out):
    if out is None:
        return "（宿主没有提供引擎能力）"
    if not out.get("ok"):
        return "结果: **没装上**\n原因: %s\n失败 %s 条" % (out.get("error", ""),
                                                       len(out.get("failures", []) or []))
    return ("结果: 已装 %s 个（其中**覆盖** %s 个）→ 备份在 %s"
            % (out.get("installed", 0), out.get("overwritten", 0), out.get("backup_dir", "")))


def format_restore(out):
    if out is None:
        return "（宿主没有提供引擎能力）"
    if not out.get("ok"):
        return "结果: **没还原干净**\n原因: %s\n失败 %s 条" % (out.get("error", ""),
                                                           len(out.get("failures", []) or []))
    return ("结果: 逐字节还原 %s 个 / 删掉新装的 %s 个"
            % (out.get("restored", 0), out.get("removed", 0)))


# ══════════════════════════════════════════════════════════════════════════
# 五、界面：一页多标签
# ══════════════════════════════════════════════════════════════════════════
def _run_off_ui_thread(api, panel, title, show, heavy):
    """把**引擎重活**丢到任务壳的 worker 线程上（审计 `05 PLG-05`，★ 用户 2026-10-12 拍板口径 A）。

    为什么（修前的事实）：本页三块面板的按钮回调**直接在主线程**调 `api.engine.*`
    （`patch_identify` / `patch_apply` / `font_pack` / `text_extract_dir` / `pkg_install` /
    `pkg_restore`）—— 那些是**长任务**：跑起来界面就死住（拖不动、日志不刷、取消点不动）。

    通道（**现有通道就够用，产品零改动**）—— 与 `plugins/convert_page.py` **同一套写法**：
      · `api.ui.task_run(panel, title, worker)`：worker 在**工作线程**里跑，拿
        `progress(0~100)` / `message(text)` 两个 sink；
      · ⚠ **worker 里不许碰任何 `api.ui.*` 控件方法**（宿主用 `on_ui_thread()` 挡掉，
        静默变 no-op —— `convert_page.py` 就踩过这个坑，见那边的注释）；
      · ⇒ `gather()` **在主线程**把控件值取好交进来；worker 只做**引擎调用 + 纯拼串**，
        结果经 `message(...)` 回主线程，由 `_on_message` 落控件。

    ★ 口径 A 的边界（**哪些搬不进去**，如实记在批报告里）：
      · 控件构造 / 取值 / `set_text` / `ask_*` 对话框 / 拖入回调 / `pick_font` 的回调 ——
        **必须**留在主线程（它们本身就是"和界面交互"）；
      · 只有"**引擎执行段**"进 worker。
    """
    def _on_message(text):
        # 回主线程（壳保证排队）⇒ 这里可以碰控件
        if show is not None:
            show(str(text))

    if panel is not None:
        api.ui.task_on_message(panel, _on_message)

    def worker(progress, message):
        # ★ 这里在**工作线程**上：不许碰 ui 控件（取值已经在主线程 gather 过了）
        # ★★ 判据用的**取证位**（P17-107，审计 `05 PLG-05`）：把"这段真的在 worker 上跑"
        #   记下来 —— 只写"非 UI 线程"时的那一行，所以 marker 里**出现**它就等于
        #   "重活确实离开了主线程"（不是靠读代码猜）。marker 路径由 `XRKIT_TOOLCHAIN_MARKER`
        #   环境变量给（不设 ⇒ 不写文件、行为不变）。
        marker = os.environ.get("XRKIT_TOOLCHAIN_MARKER", "")
        if marker:
            try:
                with open(marker, "a", encoding="utf-8") as handle:
                    handle.write("heavy title=%s on_ui_thread=%s\n"
                                 % (title, str(bool(api.ui.on_ui_thread()))))
            except OSError:
                pass
        progress(5)
        text = heavy(message)
        progress(100)
        if text is not None:
            message(str(text))

    if panel is None:
        # 没有任务壳（**无界面进程**）⇒ 如实退回同步执行，**不假装**跑了 worker。
        #   （这条路径由 `--py-plugin-call` 的无界面调用走到；判据只对"有壳"那条下结论。）
        text = heavy(lambda _t: None)
        if show_holder["fn"] is not None and text is not None:
            show_holder["fn"](str(text))
        return False
    if not api.ui.task_run(panel, title, worker):
        if show_holder["fn"] is not None:
            show_holder["fn"]("已有任务在跑，请等它结束或先取消")
        return False
    return True


def _build_patch_tab(api, host, tab):
    ui = api.ui
    ui.label(tab, "引擎 exe（识别＝看它是什么形态；生成＝从原版出一份补丁副本）")
    target_entry = ui.entry(tab, host.pref_get(SECTION, "target", ""), 60)
    result_view = ui.text_view(tab, 18, True)
    # ★ 任务壳（口径 A）：本页三块面板各有自己的壳，长活一律经它进 worker 线程
    panel = ui.task_panel(tab, True)

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
        # ★ 口径 A：识别是长任务（要读整个 exe）⇒ 进 worker；控件取值在主线程先取好
        def heavy(_message):
            return format_identify(identify(api, target))

        _run_off_ui_thread(api, panel, "二进制补丁：识别", show, heavy)
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
        # 对话框与路径都在主线程取好；**真正写盘那一段**（长活）进 worker
        def heavy(_message):
            return format_apply(apply(api, target, out))

        _run_off_ui_thread(api, panel, "二进制补丁：生成", show, heavy)
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
    panel = ui.task_panel(tab, True)   # ★ 口径 A：任务壳（长活进 worker）
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
        # ★ 口径 A：出包是长活（扫 xml + 逐档做字库）⇒ 进 worker
        def heavy(_message):
            return format_regions(font_pack_regions(api, options))

        _run_off_ui_thread(api, panel, "分区字体：出包", lambda t: ui.set_text(status, t), heavy)
        ui.log("分区字体：出包 " + options["out_dir"])

    ui.button(tab, "按所选档位出包…", run_pack)


def load_entries_from_manifest(path):
    """从清单里读回条目（TSV：`# …` 注释 + 每行 `rel\tsha256\tsource`）。

    有了它，"装进游戏 / 还原"不必依赖"刚跑过一遍产线"——重启后照样能装能退。
    """
    entries = []
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                line = line.rstrip("\r\n")
                if not line.strip() or line.startswith("#"):
                    continue
                parts = line.split("\t")
                if len(parts) >= 2:
                    entries.append({"rel": parts[0], "sha256": parts[1],
                                    "source_path": parts[2] if len(parts) > 2 else ""})
    except OSError:
        return []
    return entries


def _build_pipeline_tab(api, host, tab):
    ui = api.ui
    panel = ui.task_panel(tab, True)   # ★ 口径 A：任务壳（长活进 worker）
    ui.label(tab, "自适应出汉化包：源文本目录 →（类型判定 + 提取）→ 字号自适应出包 → "
                  "统一清单；可装进游戏（先备份），也能一键还原")
    src_entry = ui.entry(tab, host.pref_get(SECTION, "source_dir", ""), 60)
    out_entry = ui.entry(tab, host.pref_get(SECTION, "out_dir2", ""), 60)
    font_entry = ui.entry(tab, host.pref_get("font", "path", ""), 60)
    game_entry = ui.entry(tab, host.pref_get(SECTION, "game_root", ""), 60)
    status = ui.text_view(tab, 16, True)
    state = {"entries": []}

    def show(text):
        ui.set_text(status, text)

    def current_entries():
        """先用手上这份；没有就从输出目录的清单里读回来（重启后也能装/退）。"""
        if state["entries"]:
            return state["entries"]
        return load_entries_from_manifest(
            os.path.join(ui.get_text(out_entry).strip(), "manifest.json"))

    def backup_root():
        return os.path.join(ui.get_text(out_entry).strip(), "backup")

    def pick_font():
        def chosen(path):
            if path:
                ui.set_text(font_entry, path)
                host.pref_set("font", "path", path)

        ui.pick_font(ui.get_text(font_entry).strip(), chosen)

    ui.button(tab, "选字体…", pick_font)

    def run():
        options = {"source_dir": ui.get_text(src_entry).strip(),
                   "out_dir": ui.get_text(out_entry).strip(),
                   "font_path": ui.get_text(font_entry).strip()}
        host.pref_set(SECTION, "source_dir", options["source_dir"])
        host.pref_set(SECTION, "out_dir2", options["out_dir"])
        # ★ 口径 A：整条产线（提取 + 出包 + 清单）是最长的那一段 ⇒ 进 worker
        def heavy(_message):
            result = run_pipeline(api, options)
            state["entries"] = result.get("entries", []) or []
            return format_pipeline(result)

        _run_off_ui_thread(api, panel, "自适应出汉化包：跑一遍", show, heavy)
        ui.log("自适应出汉化包：跑一遍 " + options["out_dir"])

    def install():
        root = ui.get_text(game_entry).strip()
        host.pref_set(SECTION, "game_root", root)
        entries = current_entries()
        backup = backup_root()
        # ★ 口径 A：装包要逐条写盘 + 备份 ⇒ 进 worker（控件取值已在主线程取好）
        def heavy(_message):
            return format_install(install_package(api, entries, root, backup))

        _run_off_ui_thread(api, panel, "自适应出汉化包：装进游戏", show, heavy)
        ui.log("自适应出汉化包：装进 " + root)

    def restore():
        root = ui.get_text(game_entry).strip()
        entries = current_entries()
        backup = backup_root()
        def heavy(_message):
            return format_restore(restore_package(api, entries, root, backup))

        _run_off_ui_thread(api, panel, "自适应出汉化包：还原", show, heavy)
        ui.log("自适应出汉化包：还原 " + root)

    ui.button(tab, "跑一遍（提取 + 出包 + 清单）", run)
    ui.button(tab, "装进游戏（先备份）", install)
    ui.button(tab, "还原", restore)


def register(api):
    def build():
        ui = api.ui
        host = api.host
        tabs = ui.tabs(None)
        _build_patch_tab(api, host, ui.tabs_add_page(tabs, "自动二进制补丁"))
        _build_font_tab(api, host, ui.tabs_add_page(tabs, "分区字体"))
        _build_pipeline_tab(api, host, ui.tabs_add_page(tabs, "自适应出汉化包"))
        return None

    # ★ **新增页**（不覆盖内置页）⇒ replaces 留空；三件同页见各自的标签。
    api.register_tool("toolchain_py", build, 6, "", "汉化产线（插件版）", "汉化产线")
    api.log("汉化产线（插件版）：已注册（新增页；面板：自动二进制补丁 / 分区字体 / 自适应出汉化包）")
