# -*- coding: utf-8 -*-
"""联网总开关判据：enabled=0 时本地 HTTP 请求数必须为 0；enabled=1 时请求成功。"""

from __future__ import annotations

import http.server
import os
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-shell" / "tools" / "cli" /
                                           "xrkit_cli.exe")))
if not CLI.exists():
    print("SKIP cli-missing")
    raise SystemExit(2)

requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        global requests
        requests += 1
        body = b"toggle-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def run_get(url: str) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--http-get", url], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/x" % server.server_address[1]
    ltx = CLI.parent / "xrkit_user.ltx"
    backup = CLI.parent / "xrkit_user.ltx.bak"
    if ltx.exists():
        ltx.replace(backup)
    try:
        ltx.write_text("[network]\nenabled=0\n", encoding="utf-8")
        rc, out = run_get(url)
        off_ok = rc != 0 and requests == 0 and "联网功能已关闭" in out
        print(("OK " if off_ok else "FAIL ") + "enabled=0：拒绝请求且服务端收到 0 次")
        if not off_ok:
            print("  rc=%d requests=%d out=%s" % (rc, requests, out[-300:]))

        before = requests
        ltx.write_text("[network]\nenabled=1\n", encoding="utf-8")
        rc, out = run_get(url)
        on_ok = rc == 0 and requests == before + 1 and "HTTP ok" in out
        print(("OK " if on_ok else "FAIL ") + "enabled=1：请求成功且服务端收到 1 次")
        if not on_ok:
            print("  rc=%d requests=%d out=%s" % (rc, requests, out[-300:]))
        return 0 if off_ok and on_ok else 1
    finally:
        server.shutdown()
        if backup.exists():
            backup.replace(ltx)
        elif ltx.exists():
            ltx.unlink()


if __name__ == "__main__":
    raise SystemExit(main())
