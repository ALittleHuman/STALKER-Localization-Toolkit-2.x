# -*- coding: utf-8 -*-
"""F-152 行为回归：两个更新源，一个立即失败、一个慢成功；不得因已消费 future 崩溃。"""

from __future__ import annotations

import http.server
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get("XRKIT_APP", str(ROOT / "build" / "msvc-shell" / "src" / "shell" /
                                      "xrkit_app.exe")))
if not APP.exists():
    print("SKIP app-missing")
    raise SystemExit(2)

release = {
    "tag_name": "v2.1.0",
    "prerelease": False,
    "assets": [{
        "name": "app.zip",
        "browser_download_url": "http://127.0.0.1:9/app.zip",
        "digest": "sha256:" + "0" * 64,
    }],
}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        if self.path == "/bad":
            body = b"not-json"
            self.send_response(200)
        else:
            time.sleep(1.5)
            body = json.dumps(release).encode("utf-8")
            self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        pass


def main() -> int:
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    env = dict(os.environ)
    env["XRKIT_APP_UPDATE_URL"] = (
        "http://127.0.0.1:%d/bad,http://127.0.0.1:%d/good" % (port, port)
    )
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["XRKIT_NO_PLUGIN_MANAGER"] = "1"
    try:
        proc = subprocess.run([str(APP), "--host-update-check"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=30)
    finally:
        server.shutdown()
    out = proc.stdout.decode("utf-8", "replace")
    ok = proc.returncode == 0 and "version=v2.1.0" in out
    print(("OK " if ok else "FAIL ") + "F-152 多源：坏源先返回，慢源仍能成功（rc=%d）"
          % proc.returncode)
    if not ok:
        print(out[-800:])
        return 1

    # ★ 2026-11-22：**一个 Release 多个 zip** —— 必须按版本 build 选，且 digest 跟着同一个资产。
    multi = {
        "tag_name": "2.0.0.254",
        "prerelease": True,
        "assets": [
            {"name": "999.zip", "browser_download_url": "http://127.0.0.1:9/999.zip",
             "digest": "sha256:" + "a" * 64},
            {"name": "254.zip", "browser_download_url": "http://127.0.0.1:9/254.zip",
             "digest": "sha256:" + "b" * 64},
        ],
    }

    class MultiHandler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802
            body = json.dumps(multi).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    server2 = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MultiHandler)
    threading.Thread(target=server2.serve_forever, daemon=True).start()
    env2 = dict(os.environ)
    env2["XRKIT_APP_UPDATE_URL"] = "http://127.0.0.1:%d/" % server2.server_address[1]
    env2["QT_QPA_PLATFORM"] = "offscreen"
    env2["XRKIT_NO_PLUGIN_MANAGER"] = "1"
    try:
        proc2 = subprocess.run([str(APP), "--host-update-check"], env=env2,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    finally:
        server2.shutdown()
    out2 = proc2.stdout.decode("utf-8", "replace")
    ok2 = proc2.returncode == 0 and "254.zip" in out2 and "999.zip" not in out2
    print(("OK " if ok2 else "FAIL ") + "多 zip：按版本 build 选 254.zip（rc=%d）" % proc2.returncode)
    if not ok2:
        print(out2[-800:])
    return 0 if ok2 else 1


if __name__ == "__main__":
    sys.exit(main())
