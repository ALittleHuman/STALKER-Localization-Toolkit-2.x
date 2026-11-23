# -*- coding: utf-8 -*-
"""BETA.256 网络补强判据。

三条：
  1. 回环地址必须直连 —— 本机若配了 IE/WinINET 代理，127.0.0.1 走代理会 404/超时；
  2. 宿主门面在阻塞期必须放 GIL —— 3 路并发总耗时应 ≈ 单次，而不是 3×；
  3. run_shutdown 必须真的调 cancel_owner —— 卸载/重载后异步下载不能继续跑。
"""

from __future__ import annotations

import http.server
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("XRKIT_CLI",
                          str(ROOT / "build" / "msvc-shell" / "tools" / "cli" /
                              "xrkit_cli.exe")))
if not CLI.exists():
    print("SKIP cli-missing")
    raise SystemExit(2)

DELAY = 0.8          # 单次响应延迟：串行 3 路 ≈ 2.4s，并发 ≈ 0.8s
requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        global requests
        requests += 1
        time.sleep(DELAY)
        body = b"loopback-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


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
        ltx.write_text("[network]\nenabled=1\n", encoding="utf-8")

        # ── 1 回环直连 ────────────────────────────────────────────
        started = time.time()
        proc = subprocess.run([str(CLI), "--http-get", url],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        elapsed = time.time() - started
        out = proc.stdout.decode("utf-8", "replace")
        ok1 = proc.returncode == 0 and "HTTP ok" in out and elapsed < 5.0
        print(("OK " if ok1 else "FAIL ") + "回环 GET 直连成功（%.2fs）" % elapsed)
        if not ok1:
            print("  rc=%d out=%s" % (proc.returncode, out[-300:]))
        results.append(ok1)

        # ── 2 门面并发（GIL） ─────────────────────────────────────
        fixture = ROOT / "tests" / "python_plugins_http"
        before = requests
        env = dict(os.environ)
        env["XRKIT_PROBE_HTTP_URL"] = url
        proc = subprocess.run([str(CLI), "--py-plugins", str(fixture)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              env=env, timeout=180)
        out = proc.stdout.decode("utf-8", "replace")
        match = re.search(r"PROBE-HTTP total=([0-9.]+) ok=(\d+)", out)
        limit = DELAY * 2.0        # 串行是 3×，并发是 1×；取 2× 做分界
        ok2 = (match is not None and int(match.group(2)) == 3
               and float(match.group(1)) < limit
               and requests - before == 3)
        print(("OK " if ok2 else "FAIL ")
              + "3 路并发门面放 GIL（%s s，期望 < %.1fs）"
              % (match.group(1) if match else "?", limit))
        if match is None:
            print("  out=%s" % out[-500:])
        results.append(ok2)

        # ── 3 卸载取消异步下载（静态判据） ────────────────────────
        source = (ROOT / "src" / "engine" / "plugin" / "py_host.cpp").read_text(encoding="utf-8")
        start = source.find("void run_shutdown(")
        end = source.find("\nvoid unload_module(", start)
        body = source[start:end] if start >= 0 and end > start else ""
        ok3 = "cancel_owner" in body
        print(("OK " if ok3 else "FAIL ") + "run_shutdown 调用 cancel_owner")
        results.append(ok3)
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
