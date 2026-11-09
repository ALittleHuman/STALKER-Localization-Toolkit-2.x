# -*- coding: utf-8 -*-
"""判据：**联网插件（单文件）** 端到端可用（离线；只用 127.0.0.1 本地 HTTP）。

口径（2026-11-09）：
  · `plugins_net/net_http.py` 是**单文件自包含**插件（内嵌 WinHTTP/ctypes，不依赖 `_sdk` 相对路径）；
  · 清单：稳定 `id` / `version` / `repo`（仅元数据）/ `provides`；服务 `net.http_get/1`；
  · 加载：`--py-plugins --json` 能读到清单；`--py-plugin-call` 加载 2 个插件、0 错误；
  · 调用：本地 HTTP server → 服务返回 `status=ok/body/code=200`；
  · 坏 scheme：`ftp://` ⇒ `status=error` 且原因可见（不把异常抛给消费者）；
  · **不随包**：本判据只读仓库里的 `plugins_net/`，不要求它在发行包里（host-offline 另判）。

退出码：0=全绿 / 1=有断言不成立 / 2=CLI 缺失（外部前提）。
"""

from __future__ import annotations

import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                         "cli" / "xrkit_cli.exe")))
PLUGIN = ROOT / "plugins_net" / "net_http.py"
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


def run_cli(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


class _Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 (http.server 约定)
        body = b"HELLO-FROM-LOCAL-SERVER"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # 静音（判据只看结果）
        pass


def main() -> int:
    print("== 联网插件（单文件）：本地 HTTP → 加载 → 服务调用 ==")
    if not CLI.is_file():
        print("!! 找不到 CLI：%s" % CLI)
        print("NET-PLUGIN: SKIP (cli-missing)")
        return 2
    if not PLUGIN.is_file():
        print("!! 找不到插件：%s" % PLUGIN)
        print("NET-PLUGIN: SKIP (plugin-missing)")
        return 2

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/hello.txt" % port
    work = Path(tempfile.mkdtemp(prefix="xrkit_net_plugin_"))
    try:
        plugins = work / "plugins"
        plugins.mkdir()
        shutil.copy2(PLUGIN, plugins / "net_http.py")
        (plugins / "probe_net.py").write_text(
            "# 判据夹具：只调服务，不自己联网\n"
            "PLUGIN_INFO = {\"id\": \"test.net_probe\", \"version\": \"1.0.0\"}\n"
            "\n"
            "def register(api):\n"
            "    return None\n"
            "\n"
            "def headless(api, args):\n"
            "    return api.services.call(\"net.http_get/1\", args[\"url\"],\n"
            "                             args.get(\"timeout\", 5))\n",
            encoding="utf-8")

        rc, out = run_cli(["--py-plugins", str(plugins), "--json"])
        want(rc == 0, "加载退出码 0", "rc=%d" % rc)
        summary = [ln for ln in out.splitlines() if ln.startswith("PYPLUGINS files=")]
        want(bool(summary) and "files=2" in summary[0] and "loaded=2" in summary[0]
             and "errors=0" in summary[0],
             "两个插件都加载、零错误", summary[0] if summary else "(无汇总行)")
        json_lines = [ln for ln in out.splitlines() if ln.startswith("PYPLUGINS-JSON ")]
        want(bool(json_lines), "有结构化清单（--json）")
        if json_lines:
            try:
                items = json.loads(json_lines[0][len("PYPLUGINS-JSON "):])
            except Exception as exc:  # noqa: BLE001
                items = []
                want(False, "JSON 可解析", repr(exc))
            else:
                want(True, "JSON 可解析", "%d 条" % len(items))
            by_id = {str(it.get("id")): it for it in items}
            plugin = by_id.get("xrkit.net_http", {})
            want(bool(plugin), "清单 id=xrkit.net_http", sorted(by_id))
            want(plugin.get("version") == "1.0.0", "清单 version=1.0.0",
                 str(plugin.get("version")))
            want(bool(plugin.get("repo")), "清单带 repo（仅元数据）", str(plugin.get("repo")))

        def call_service(payload: dict) -> dict:
            rc2, out2 = run_cli(["--py-plugin-call", str(plugins),
                                 "--plugin", "probe_net.py", json.dumps(payload)])
            line = [ln for ln in out2.splitlines()
                    if ln.startswith("PYPLUGINCALL plugin=probe_net.py result=")]
            if not line:
                return {"_raw": out2, "_rc": rc2}
            try:
                return json.loads(line[0].split("result=", 1)[1])
            except Exception:  # noqa: BLE001
                return {"_raw": out2, "_rc": rc2}

        result = call_service({"url": url, "timeout": 5})
        want(result.get("status") == "ok", "服务调用 status=ok", str(result)[:200])
        want(result.get("body") == "HELLO-FROM-LOCAL-SERVER",
             "正文回读一致", str(result.get("body"))[:80])
        want(result.get("code") == 200, "HTTP 状态码=200", str(result.get("code")))
        want(result.get("bytes", 0) > 0, "bytes>0", str(result.get("bytes")))

        bad = call_service({"url": "ftp://127.0.0.1/x"})
        want(bad.get("status") == "error", "坏 scheme ⇒ status=error", str(bad)[:160])
        want("http/https" in str(bad.get("error")), "坏 scheme 原因可见（只允许 http/https）",
             str(bad.get("error"))[:160])

        # ★ 消费者示例的 depends：没有联网插件时应被**自动禁用**（清单前置真的生效）。
        consumer = ROOT / "plugins_net" / "z_consumer_example.py"
        if consumer.is_file():
            only_consumer = work / "plugins_consumer"
            only_consumer.mkdir()
            shutil.copy2(consumer, only_consumer / "z_consumer_example.py")
            rc3, out3 = run_cli(["--py-plugins", str(only_consumer)])
            summary3 = [ln for ln in out3.splitlines()
                        if ln.startswith("PYPLUGINS files=")]
            disabled = [ln for ln in out3.splitlines()
                        if ln.startswith("PYPLUGINS disabled plugin=")]
            want(bool(summary3) and "loaded=0" in summary3[0] and "errors=0" in summary3[0],
                 "消费者示例：只有禁用、没有加载错误（rc=%d）" % rc3,
                 summary3[0] if summary3 else "(无汇总行)")
            want(len(disabled) == 1 and "xrkit.net_http" in disabled[0],
                 "消费者示例：缺 net_http 时被自动禁用并点名前置",
                 disabled[0] if disabled else "(无 disabled 行)")
    finally:
        server.shutdown()
        shutil.rmtree(work, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("NET-PLUGIN fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("   · " + item)
        return 1
    print("NET-PLUGIN ok %d 项（单文件插件，离线本地 HTTP 端到端）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
