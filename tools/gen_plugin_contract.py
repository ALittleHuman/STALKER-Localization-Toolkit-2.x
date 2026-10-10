#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 1.x 源码里**按 AST 读**出插件契约表 → `src/engine/plugin/contract_tables.inc`。

为什么必须生成而不是手抄（对照表 ⑫12.1/12.3/12.4）：
  · 槽位表（HOSTS/LOCATIONS/AREA_*）是"唯一事实来源"，手抄一份就会与 1.x 漂移；
  · `api.ui` 的 **16 方法**（9 造控件 + 7 改控件/取输入）**名字与参数名就是第三方契约**，
    契约清单必须从源码读，**读不到就报错、不许静默降级**（否则"少了一个方法"会变成
    "插件调用时才发现"）。

    python tools/gen_plugin_contract.py            # 生成
    python tools/gen_plugin_contract.py --check    # 只校验磁盘上的 .inc（门禁用）
"""

from __future__ import annotations

import os
import argparse
import ast
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
# ★★ 2026-10-10（P17-23，审计 04 F-08）：**不要写死本机的 1.x 路径** ——
#   优先读环境变量 `XRKIT_TOOLKIT_1X`（换机器/换检出位置不用改源码），没有才回落到本机既有位置。
TOOLKIT_1X = Path(os.environ.get("XRKIT_TOOLKIT_1X")
                  or r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
PLUGINS_PY = TOOLKIT_1X / "toolkit_plugins.py"
PLUGIN_UI_PY = TOOLKIT_1X / "toolkit_plugin_ui.py"
OUT = ROOT / "src" / "engine" / "plugin" / "contract_tables.inc"

EXPECTED_UI_METHODS = 16  # 12.3：9 造控件 + 7 改控件/取输入


def fail(msg: str) -> None:
    raise SystemExit("生成插件契约表失败：%s" % msg)


def literal(node: ast.AST):
    try:
        return ast.literal_eval(node)
    except Exception as exc:  # noqa: BLE001
        fail("读不出字面量（%s）：%s" % (exc, ast.dump(node)[:120]))


def read_hosts(tree: ast.Module) -> list[tuple[str, str]]:
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "HOSTS" for t in node.targets):
            data = literal(node.value)
            if not isinstance(data, dict) or not data:
                fail("HOSTS 不是非空 dict")
            return [(str(k), str(v)) for k, v in data.items()]
    fail("源文件里找不到 HOSTS")


def read_dict(tree: ast.Module, name: str) -> list[tuple[str, str]]:
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == name for t in node.targets):
            data = literal(node.value)
            if not isinstance(data, dict):
                fail("%s 不是 dict" % name)
            return [(str(k), str(v)) for k, v in data.items()]
    fail("源文件里找不到 %s" % name)


def read_areas(tree: ast.Module) -> list[tuple[str, str]]:
    out: list[tuple[str, str]] = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and target.id.startswith("AREA_"):
                out.append((target.id, str(literal(node.value))))
    if not out:
        fail("源文件里找不到任何 AREA_* 槽位常量")
    return out


def read_ui_methods(tree: ast.Module) -> list[str]:
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "UI_METHODS" for t in node.targets):
            data = literal(node.value)
            names = [str(x) for x in data]
            if len(names) != EXPECTED_UI_METHODS:
                fail("UI_METHODS 是 %d 个，契约要求 %d 个（12.3）"
                     % (len(names), EXPECTED_UI_METHODS))
            return names
    fail("源文件里找不到 UI_METHODS")


def read_ui_signatures(tree: ast.Module, names: list[str]) -> list[tuple[str, str]]:
    """后端类（`TkUiFactory`）里这 16 个方法的**参数名**（按声明顺序，去掉 self）。"""
    klass = None
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == "TkUiFactory":
            klass = node
            break
    if klass is None:
        fail("源文件里找不到 TkUiFactory（后端类）")
    found: dict[str, str] = {}
    for item in klass.body:
        if not isinstance(item, ast.FunctionDef):
            continue
        args = [a.arg for a in item.args.args if a.arg != "self"]
        # 关键字参数也要算进契约（有默认值的同样暴露给插件）
        args += [a.arg for a in item.args.kwonlyargs]
        found[item.name] = ", ".join(args)
    missing = [n for n in names if n not in found]
    if missing:
        fail("后端类里缺这些契约方法（**不许静默降级**）：%s" % ", ".join(missing))
    return [(n, found[n]) for n in names]


def cstr(text: str) -> str:
    out = text.replace("\\", "\\\\").replace('"', '\\"')
    out = out.replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return '"%s"' % out


def build() -> tuple[bytes, dict]:
    if not PLUGINS_PY.is_file() or not PLUGIN_UI_PY.is_file():
        fail("1.x 源码不在（%s / %s）" % (PLUGINS_PY, PLUGIN_UI_PY))
    plugins = ast.parse(PLUGINS_PY.read_text(encoding="utf-8"))
    ui = ast.parse(PLUGIN_UI_PY.read_text(encoding="utf-8"))

    hosts = read_hosts(plugins)
    locations = read_dict(plugins, "LOCATIONS")
    areas = read_areas(plugins)
    methods = read_ui_methods(ui)
    signatures = read_ui_signatures(ui, methods)

    lines: list[str] = []
    lines.append("// 生成物：xrkit::plugin 的契约表 —— **不许手改**。")
    lines.append("// 由 tools/gen_plugin_contract.py 从 1.x 源码按 AST 读出；")
    lines.append("// `--check` 在门禁里逐字节比对（不一致＝FAIL，不是 SKIP）。")
    lines.append("// 生成源：toolkit_plugins.py（HOSTS/LOCATIONS/AREA_*）与")
    lines.append("//         toolkit_plugin_ui.py（UI_METHODS + 后端类的方法签名）。")
    lines.append("//")
    lines.append("// ★ 本文件**不带 namespace 包裹**：由 includer 决定放进哪个命名空间")
    lines.append("//   （与 font/adv_tables.inc 等生成物的惯例一致）。")
    lines.append("")
    lines.append("inline constexpr SlotSpec kHosts[] = {")
    for key, label in hosts:
        lines.append("    {%s, %s}," % (cstr(key), cstr(label)))
    lines.append("};")
    lines.append("inline constexpr int kHostCount = %d;" % len(hosts))
    lines.append("")
    lines.append("inline constexpr SlotSpec kLocations[] = {")
    for key, label in locations:
        lines.append("    {%s, %s}," % (cstr(key), cstr(label)))
    lines.append("};")
    lines.append("inline constexpr int kLocationCount = %d;" % len(locations))
    lines.append("")
    lines.append("inline constexpr SlotSpec kAreas[] = {")
    for name, value in areas:
        lines.append("    {%s, %s},  // %s" % (cstr(value), cstr(value), name))
    lines.append("};")
    lines.append("inline constexpr int kAreaCount = %d;" % len(areas))
    lines.append("")
    lines.append("// api.ui 的契约：名字 + **参数名**（第三方就是照这两样写的）")
    lines.append("inline constexpr UiMethodSpec kUiMethods[] = {")
    for name, params in signatures:
        lines.append("    {%s, %s}," % (cstr(name), cstr(params)))
    lines.append("};")
    lines.append("inline constexpr int kUiMethodCount = %d;" % len(signatures))
    lines.append("")
    counts = {"hosts": len(hosts), "locations": len(locations), "areas": len(areas),
              "ui_methods": len(signatures)}
    return "\n".join(lines).encode("utf-8"), counts


def main() -> int:
    parser = argparse.ArgumentParser(description="生成插件契约表")
    parser.add_argument("--check", action="store_true", help="只校验磁盘上的 .inc")
    args = parser.parse_args()
    want, counts = build()
    if args.check:
        if not OUT.is_file():
            fail("产物不存在：%s" % OUT.relative_to(ROOT))
        got = OUT.read_bytes()
        if got != want:
            fail("磁盘上的 %s 与源码不一致（%d 字节 vs %d 字节）—— 重新生成"
                 % (OUT.relative_to(ROOT), len(got), len(want)))
        print("插件契约表与 1.x 源码一致（hosts=%d locations=%d areas=%d ui_methods=%d）"
              % (counts["hosts"], counts["locations"], counts["areas"], counts["ui_methods"]))
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(want)  # ★ bytes + LF：文本模式会把 \n 翻成 \r\n
    print("已写 %s（%d 字节）" % (OUT.relative_to(ROOT), len(want)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
