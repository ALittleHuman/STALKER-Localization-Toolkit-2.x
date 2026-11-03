# -*- coding: utf-8 -*-
"""跨插件服务（api.services）的判据：宿主自带契约，五条规则里可静态/离线验的四条。

夹具：`tests/python_plugins_services/`
  00_provider  注册 net.echo
  01_consumer  调用 net.echo（**跨插件**，不是同一个文件）
  02_dup       重复注册 ⇒ 必须报"不静默覆盖"
  03_missing   调用未注册 ⇒ 必须报"服务不可用（没有提供者）：<名字>"
  04_noname    名字不带命名空间 ⇒ 必须被拒
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
FIXTURES = ROOT / "tests" / "python_plugins_services"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 跨插件服务 api.services（provide/call/重复/缺失/命名空间）==")
    if not CLI.is_file():
        print("PLUGIN-SERVICES skip 缺 CLI")
        return 2
    p = subprocess.run([str(CLI), "--py-plugins", str(FIXTURES)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", "replace")

    checks = [
        ("SVC-PROVIDED net.echo", "提供者注册成功、available() 看得到"),
        ("SVC-CALL echo:hi", "**跨插件**调用成功（消费者拿到提供者返回值）"),
        ("服务已被注册（不静默覆盖）：net.echo", "重复注册被拒（不静默覆盖）"),
        ("服务不可用（没有提供者）：net.nope", "调用未注册服务给出明确异常（含服务名）"),
        ("必须带命名空间", "不带命名空间的服务名被拒"),
    ]
    for needle, label in checks:
        if needle in text:
            ok(label)
        else:
            fail("%s（输出里没有「%s」）" % (label, needle))

    if PROBLEMS:
        print("PLUGIN-SERVICES: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PLUGIN-SERVICES: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
