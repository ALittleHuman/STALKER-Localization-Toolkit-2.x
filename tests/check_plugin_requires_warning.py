# -*- coding: utf-8 -*-
"""判据：`PLUGIN_INFO.requires`（跨插件服务）**启动期可见告警**（契约 §7.5）。

口径（2026-11-10，P1-6）：
  · 加载完成后，逐插件检查 `requires` 里的服务是否已有提供者；
  · 缺提供者 ⇒ 一条 Warn（含插件名与服务名），但**不阻塞加载**（调用时还会再报一次）；
  · 有提供者 ⇒ 不许告警（判据必须能区分，不能恒红/恒绿）。

夹具：
  provider.py  register 里注册 `demo.svc/1`
  consumer.py  requires ["demo.svc/1"]       ⇒ 有提供者，不许告警
  missing.py   requires ["demo.absent/1"]    ⇒ 没提供者，必须告警

退出码：0=全绿 / 1=断言不成立 / 2=缺 CLI（外部前提）。
"""

from __future__ import annotations

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


def main() -> int:
    print("== 插件服务依赖 requires：启动期告警（有提供者不告警） ==")
    if not CLI.is_file():
        print("!! 缺 CLI：%s" % CLI)
        print("PLUGIN-REQUIRES: SKIP (cli-missing)")
        return 2
    work = Path(tempfile.mkdtemp(prefix="xrkit_req_"))
    try:
        plugins = work / "plugins"
        plugins.mkdir()
        (plugins / "provider.py").write_text(
            "PLUGIN_INFO = {\"id\": \"demo.provider\", \"version\": \"1.0\"}\n"
            "def _svc(*args):\n"
            "    return {\"status\": \"ok\"}\n"
            "def register(api):\n"
            "    api.services.register(\"demo.svc/1\", _svc)\n",
            encoding="utf-8")
        (plugins / "consumer.py").write_text(
            "PLUGIN_INFO = {\"id\": \"demo.consumer\", \"version\": \"1.0\",\n"
            "               \"requires\": [\"demo.svc/1\"]}\n"
            "def register(api):\n"
            "    return None\n",
            encoding="utf-8")
        (plugins / "missing.py").write_text(
            "PLUGIN_INFO = {\"id\": \"demo.missing\", \"version\": \"1.0\",\n"
            "               \"requires\": [\"demo.absent/1\"]}\n"
            "def register(api):\n"
            "    return None\n",
            encoding="utf-8")

        proc = subprocess.run([str(CLI), "--py-plugins", str(plugins)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = proc.stdout.decode("utf-8", "replace")
        want(proc.returncode == 0, "加载退出码 0", "rc=%d" % proc.returncode)
        summary = [ln for ln in out.splitlines() if ln.startswith("PYPLUGINS files=")]
        want(bool(summary) and "loaded=3" in summary[0] and "errors=0" in summary[0],
             "三个插件都加载（缺服务不阻塞加载）", summary[0] if summary else "(无汇总行)")
        want(bool(summary) and "service_warnings=1" in summary[0],
             "汇总行 service_warnings=1", summary[0] if summary else "")
        warns = [ln for ln in out.splitlines() if "warn service" in ln]
        want(len(warns) == 1, "只对缺提供者的插件告警（不恒红）", "\n".join(warns[:3]))
        joined = "\n".join(warns)
        want("plugin=missing.py" in joined and "service=demo.absent/1" in joined,
             "告警点名插件与服务", joined[:160])
        want("没有提供者" in joined, "告警文本可读（没有提供者）", joined[:160])
        want("plugin=consumer.py" not in joined,
             "有提供者的 requires 不告警", joined[:160])
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("PLUGIN-REQUIRES fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("PLUGIN-REQUIRES ok %d 项（requires 缺服务 ⇒ 启动期可见告警，有提供者不告警）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
