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
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
