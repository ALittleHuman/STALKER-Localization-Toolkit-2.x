# -*- coding: utf-8 -*-
"""`--py-plugins` 的 provides/requires/imports 输出（ast 静态读，不 import 插件）。

判据：
  · 字面量 PLUGIN_INFO 的 provides / requires 逐项可见；
  · **动态构造** ⇒ 输出 `?`（不猜）；
  · 每个顶层 .py 都有一行 PLUGIN（不静默漏掉）；
  · 不 import 插件也能拿到（这条由"动态 PLUGIN_INFO 不执行也能看到 ?"间接证明）。
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
FIXTURES = ROOT / "tests" / "python_plugins_metadata"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== --py-plugins 元数据（provides/requires/imports，ast 静态读）==")
    if not CLI.is_file():
        print("PLUGIN-METADATA skip 缺 CLI")
        return 2
    p = subprocess.run([str(CLI), "--py-plugins", str(FIXTURES)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", "replace")
    lines = [ln for ln in text.splitlines() if ln.startswith("PLUGIN name=")]

    def line_for(name: str) -> str:
        for ln in lines:
            if ("name=%s " % name) in (ln + " "):
                return ln
        return ""

    checks = [
        ("provides=net.http_get/1" in line_for("net_http"),
         "字面量 provides 可见", "net_http 的 PLUGIN 行"),
        (bool(line_for("consumer")), "每个顶层 .py 都有一行 PLUGIN", "consumer"),
        ("requires=net.http_get/1" in text, "字面量 requires 可见", ""),
        ("provides=?" in line_for("c_dynamic"), "动态构造 ⇒ 输出 ?（不猜）", "c_dynamic"),
        ("note=PLUGIN_INFO 不是字面量" in line_for("c_dynamic"),
         "动态构造给出可读原因", "c_dynamic"),
        ("imports=ctypes,os" in line_for("imports_demo"),
         "imports 静态可见（不执行插件）", "imports_demo"),
        # ★ 2026-11-09：清单结构化字段（管理器读这些；与 --json 同源）
        ("id=" in line_for("net_http") and "depends=" in line_for("net_http"),
         "清单字段 id/depends 可见", line_for("net_http")[:80]),
    ]
    for cond, label, detail in checks:
        if cond:
            ok(label + (("（%s）" % detail) if detail and not cond else ""))
        else:
            fail("%s（%s）" % (label, detail or "输出里没有对应行"))
    if PROBLEMS:
        print("PLUGIN-METADATA: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PLUGIN-METADATA: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
