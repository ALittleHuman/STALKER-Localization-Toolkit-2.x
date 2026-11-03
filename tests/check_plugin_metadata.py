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
    checks = [
        ("PLUGIN name=a_provider provides=net.http_get/1", "字面量 provides 可见"),
        ("PLUGIN name=b_consumer", "每个顶层 .py 都有一行 PLUGIN"),
        ("requires=net.http_get/1", "字面量 requires 可见"),
        ("PLUGIN name=c_dynamic provides=? ", "动态构造 ⇒ 输出 ?（不猜）"),
        ("note=PLUGIN_INFO 不是字面量", "动态构造给出可读原因"),
        ("PLUGIN name=d_imports provides=- requires=- imports=ctypes,os",
         "imports 静态可见（不执行插件）"),
    ]
    for needle, label in checks:
        if needle in text:
            ok(label)
        else:
            fail("%s（输出里没有「%s」）" % (label, needle))
    if PROBLEMS:
        print("PLUGIN-METADATA: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PLUGIN-METADATA: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
