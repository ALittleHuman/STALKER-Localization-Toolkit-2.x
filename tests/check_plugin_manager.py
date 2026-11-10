# -*- coding: utf-8 -*-
"""判据：**独立进程的插件管理器**（本体崩了它也能开）＋禁用清单生效。

覆盖：
  1. `xrkit_plugin_manager.exe --scan-json`：纯静态扫描 —— 夹具插件**顶层就 raise**，
     不 import 也必须被列出来（证明管理器不执行插件）；
  2. `--disable/--enable` 写 `plugins.disabled`；宿主 `--py-plugins` 在 import **之前**跳过：
     禁用后 `skipped_disabled=1`、`errors=0`；重新启用后坏插件才会报加载错误；
  3. 静态锁：本体用 `QProcess::startDetached` 唤起独立 exe，且启动时看 `previous_crashed_`；
     管理器用 `--py-plugins-scan`（不 import）。

退出码：0=全绿 / 1=有断言不成立 / 2=缺外部前提（manager/CLI 缺失）。
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                         "cli" / "xrkit_cli.exe")))
MANAGER = Path(os.environ.get("XRKIT_PLUGIN_MANAGER",
                              str(ROOT / "build" / "msvc-shell" / "src" / "shell" /
                                  "xrkit_plugin_manager.exe")))
PROBLEMS: list[str] = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label)
        return True
    print("   \u2717 " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append(label + (("（%s）" % detail) if detail else ""))
    return False


def run(program: Path, argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(program)] + argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 独立插件管理器：静态扫描 + 禁用清单 ==")
    if not CLI.is_file():
        print("!! 缺 CLI：%s" % CLI)
        print("PLUGIN-MANAGER: SKIP (cli-missing)")
        return 2
    if not MANAGER.is_file():
        print("!! 缺管理器：%s" % MANAGER)
        print("PLUGIN-MANAGER: SKIP (manager-missing)")
        return 2

    work = Path(tempfile.mkdtemp(prefix="xrkit_pm_"))
    try:
        plugins = work / "plugins"
        plugins.mkdir()
        (plugins / "boom.py").write_text(
            "PLUGIN_INFO = {\"id\": \"demo.boom\", \"version\": \"1.0\"}\n"
            "raise RuntimeError('import 就崩（扫描必须不执行它）')\n",
            encoding="utf-8")
        (plugins / "good.py").write_text(
            "PLUGIN_INFO = {\"id\": \"demo.good\", \"version\": \"1.0\"}\n"
            "def register(api):\n    return None\n",
            encoding="utf-8")

        rc, out = run(MANAGER, ["--scan-json", "--plugins", str(plugins)])
        want(rc == 0, "管理器静态扫描退出码 0", "rc=%d\n%s" % (rc, out[-300:]))
        json_line = [ln for ln in out.splitlines() if ln.startswith("PYPLUGINSCAN-JSON ")]
        want(bool(json_line), "扫描输出结构化 JSON")
        files = set()
        if json_line:
            try:
                items = json.loads(json_line[0][len("PYPLUGINSCAN-JSON "):])
                files = {str(it.get("file")) for it in items}
            except Exception as exc:  # noqa: BLE001
                want(False, "扫描 JSON 可解析", repr(exc))
            else:
                want(True, "扫描 JSON 可解析", "%d 条" % len(items))
        want("boom.py" in files, "顶层会崩的插件也被静态列出（证明没 import）", sorted(files))

        rc, out = run(CLI, ["--py-plugins", str(plugins)])
        want("errors=1" in out, "未禁用时：坏插件 import 报错（对照）", out.splitlines()[:1])

        rc, out = run(MANAGER, ["--disable", "boom.py", "--id", "demo.boom",
                                "--plugins", str(plugins)])
        want(rc == 0, "管理器 --disable 写入禁用清单", "rc=%d\n%s" % (rc, out[-200:]))
        rc, out = run(CLI, ["--py-plugins", str(plugins)])
        want("skipped_disabled=1" in out, "宿主按禁用清单跳过（import 之前）", out.splitlines()[:2])
        want("loaded=1" in out and "errors=0" in out,
             "禁用后：好插件照常加载、坏插件根本不执行", out.splitlines()[:2])

        rc, out = run(MANAGER, ["--enable", "boom.py", "--plugins", str(plugins)])
        want(rc == 0, "管理器 --enable 移除禁用项", "rc=%d\n%s" % (rc, out[-200:]))
        rc, out = run(CLI, ["--py-plugins", str(plugins)])
        want("errors=1" in out, "重新启用后坏插件才会被 import（对照）", out.splitlines()[:2])

        hub_src = (ROOT / "src" / "shell" / "hub.cpp").read_text(encoding="utf-8")
        want("QProcess::startDetached" in hub_src and "launch_plugin_manager" in hub_src,
             "本体用独立进程唤起管理器（QProcess::startDetached）")
        want("previous_crashed_" in hub_src and "launch_plugin_manager" in hub_src,
             "本体在「上次未正常退出」时唤起管理器")
        mgr_src = (ROOT / "tools" / "plugin_manager" / "main.cpp").read_text(encoding="utf-8")
        want("--py-plugins-scan" in mgr_src, "管理器只做静态扫描（--py-plugins-scan）")
        want("--disable" in mgr_src and "--enable" in mgr_src, "管理器有启停入口")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("PLUGIN-MANAGER fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("PLUGIN-MANAGER ok %d 项（独立进程、不 import、禁用清单生效）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
