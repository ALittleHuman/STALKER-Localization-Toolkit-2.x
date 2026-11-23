# -*- coding: utf-8 -*-
"""门面并发夹具：3 路并发调 api.host.http_get，打印总耗时。

由 tests/check_network_loopback.py 用 XRKIT_PROBE_HTTP_URL 指向本机回环服务。
判据：GIL 在阻塞期放掉 ⇒ 总耗时 ≈ 单次；仍持 GIL ⇒ ≈ 3×单次。
"""

import os
import threading
import time

PLUGIN_INFO = {"id": "probe.http", "name": "probe http", "version": "1.0"}
PLUGIN_API = 1


def register(api):
    url = os.environ.get("XRKIT_PROBE_HTTP_URL", "")
    if not url:
        api.log("PROBE-HTTP skip: no XRKIT_PROBE_HTTP_URL")
        return
    spans = []

    def work():
        started = time.time()
        result = api.host.http_get(url, 10, 1 << 16)
        spans.append((time.time() - started, bool(result.get("ok"))))

    threads = [threading.Thread(target=work) for _ in range(3)]
    started = time.time()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    total = time.time() - started
    ok_count = sum(1 for _s, good in spans if good)
    api.log("PROBE-HTTP total=%.2f ok=%d spans=%s"
            % (total, ok_count, ",".join("%.2f" % s for s, _good in spans)))
