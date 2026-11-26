# -*- coding: utf-8 -*-
"""BETA.270 判据：插件联网闸门 + 能力表。

三条：
  1. 联网开着 → 请求照常成功（服务端收到 1 次）；
  2. 联网关着 + 询问开（默认）→ **无界面（CLI）下必须拒绝且一个字节都不发**；
  3. 联网关着 + 询问关 → 同样拒绝（不再询问、也不放行）；
  同时核对 `api.host.capabilities()` 的关键承诺与 `api.supports()` 新键。
"""

from __future__ import annotations

import http.server
import json
import os
import re
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("XRKIT_CLI",
                          str(ROOT / "build" / "msvc-shell" / "tools" / "cli" /
                              "xrkit_cli.exe")))
if not CLI.exists():
    print("SKIP cli-missing")
    raise SystemExit(2)

FIXTURE = ROOT / "tests" / "python_plugins_net"
requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        global requests
        requests += 1
        body = b"net-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def run_fixture(url: str) -> str:
    env = dict(os.environ)
    env["XRKIT_PROBE_NET_URL"] = url
    env["PYTHONUTF8"] = "1"
    proc = subprocess.run([str(CLI), "--py-plugins", str(FIXTURE)],
                          stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          env=env, timeout=120)
    return proc.stdout.decode("utf-8", "replace")


def main() -> int:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/x" % server.server_address[1]

    ltx = CLI.parent / "xrkit_user.ltx"
    backup = CLI.parent / "xrkit_user.ltx.bak"
    if ltx.exists():
        ltx.replace(backup)
    results = []
    try:
        # ── 1 联网开着：照常 ─────────────────────────────────────
        ltx.write_text("[network]\nenabled=1\n", encoding="utf-8")
        before = requests
        out = run_fixture(url)
        got = re.search(r"PROBE-NET get ok=(\w+) status=(\d+) denied=(\w+)", out)
        ok1 = (got is not None and got.group(1) == "True"
               and requests == before + 1)
        print(("OK " if ok1 else "FAIL ") + "联网开：请求成功（服务端 +1）")
        if not ok1:
            print("  out=%s" % out[-600:])
        results.append(ok1)

        # 能力表 / supports（同一进程里打印过）
        caps_line = [ln for ln in out.splitlines() if "PROBE-NET caps " in ln]
        caps = {}
        if caps_line:
            try:
                caps = json.loads(caps_line[-1].split("PROBE-NET caps ", 1)[1])
            except Exception:  # noqa: BLE001
                caps = {}
        ok2 = (caps.get("http", {}).get("gil") == "released"
               and caps.get("http", {}).get("network_switch") == "authoritative"
               and caps.get("download", {}).get("sha256") is True
               and caps.get("download", {}).get("cross_source_resume") is True)
        print(("OK " if ok2 else "FAIL ") + "capabilities(): gil/switch/sha/跨源续传")
        if not ok2:
            print("  caps=%s" % caps_line[-1:] or "")
        results.append(ok2)
        ok3 = ("PROBE-NET supports host.http.concurrent=True" in out
               and "PROBE-NET supports host.capabilities=True" in out)
        print(("OK " if ok3 else "FAIL ") + "supports() 新键为 True")
        results.append(ok3)

        # ── 2 联网关 + 询问开：无界面必须拒绝且不发 ──────────────
        ltx.write_text("[network]\nenabled=0\nplugin_prompt=1\n", encoding="utf-8")
        before = requests
        out = run_fixture(url)
        got = re.search(r"PROBE-NET get ok=(\w+) status=(\d+) denied=(\w+) err=(.*)", out)
        denied = (got is not None and got.group(1) == "False"
                  and got.group(3) == "True"
                  and "未获授权" in got.group(4)
                  and requests == before)
        print(("OK " if denied else "FAIL ") + "联网关+询问开：拒绝且零请求")
        if not denied:
            print("  out=%s" % out[-600:])
        results.append(denied)

        # ── 3 联网关 + 询问关：直接拒绝 ─────────────────────────
        ltx.write_text("[network]\nenabled=0\nplugin_prompt=0\n", encoding="utf-8")
        before = requests
        out = run_fixture(url)
        denied2 = ("PROBE-NET get ok=False" in out and requests == before)
        print(("OK " if denied2 else "FAIL ") + "联网关+询问关：直接拒绝且零请求")
        results.append(denied2)
    finally:
        server.shutdown()
        server.server_close()
        if backup.exists():
            backup.replace(ltx)
        elif ltx.exists():
            ltx.unlink()
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
