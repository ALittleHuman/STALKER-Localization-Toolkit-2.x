"""H4 导入守卫的门禁用例（--py-plugins 实跑）。

判据：
  · 目录插件 `from . import helper` 能加载（dd_multi）；
  · 声明了 `depends` 的跨插件 import 能加载（bb_consumer import aa_base）；
  · 未声明的跨插件 import 被拒（cc_undeclared 报"未声明"）；
  · 缺前置的插件被禁用（ee_missing_dep）；
  · 运行时跨插件 import 被拒（ff_runtime 的 handler 捕获后写日志）。
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
FIXTURES = ROOT / "tests" / "python_plugins_import_gate"

PROBLEMS: list[str] = []


def check(cond: bool, label: str, detail: str = "") -> None:
    if cond:
        print("   ✓ " + label)
    else:
        print("   ✗ " + label + (("（%s）" % detail) if detail else ""))
        PROBLEMS.append(label)


def main() -> int:
    if not CLI.is_file():
        print("PLUGIN-IMPORT-GATE skip：缺 %s" % CLI)
        return 2
    if not FIXTURES.is_dir():
        print("PLUGIN-IMPORT-GATE skip：缺 %s" % FIXTURES)
        return 2
    proc = subprocess.run([str(CLI), "--py-plugins", str(FIXTURES)],
                          capture_output=True, timeout=300)
    out = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    print("== --py-plugins（H4 导入守卫）==")
    print(out)
    check("PYPLUGINS files=6" in out, "读到 6 个入口")
    check("loaded=4" in out, "4 个插件注册成功（aa/bb/dd/ff）")
    check("errors=1" in out, "未声明的 import 让 cc 失败一次")
    check("PYPLUGINS error plugin=cc_undeclared.py" in out
          and "PLUGIN_INFO.depends" in out, "cc 被拒理由=未声明依赖")
    check("PYPLUGINS disabled=1" in out and "ee_missing_dep.py" in out
          and "gate.absent" in out, "ee 因缺前置被禁用")
    check("dd_multi helper=multi-ok" in out, "目录插件相对 import 可用")
    check("bb_consumer imported=base-ok" in out, "声明依赖后跨插件 import 可用")
    check("runtime-blocked" in out, "运行时跨插件 import 被拒（handler 捕获）")
    check("should not load" not in out, "被拒/被禁用的插件没有执行 register")
    print("-" * 66)
    if PROBLEMS:
        print("PLUGIN-IMPORT-GATE fail %d 项" % len(PROBLEMS))
        return 1
    print("PLUGIN-IMPORT-GATE ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
