# -*- coding: utf-8 -*-
"""联网闸门 / 能力表夹具：由 check_plugin_net_gate.py 驱动。

打印固定前缀行，供判据断言：
  PROBE-NET get ok=... denied=... err=...
  PROBE-NET caps {...}
  PROBE-NET supports <key>=True/False
"""

import json
import os

PLUGIN_INFO = {"id": "probe.net", "name": "probe net", "version": "1.0"}
PLUGIN_API = 1


def register(api):
    url = os.environ.get("XRKIT_PROBE_NET_URL", "")
    if url:
        result = api.host.http_get(url, 10, 1 << 16)
        api.log("PROBE-NET get ok=%s status=%s denied=%s err=%s"
                % (bool(result.get("ok")), result.get("status"),
                   bool(result.get("denied")), result.get("error")))
    try:
        caps = api.host.capabilities()
        api.log("PROBE-NET caps " + json.dumps(caps, sort_keys=True,
                                               ensure_ascii=False))
    except Exception as exc:  # noqa: BLE001
        api.log("PROBE-NET caps-err=%s: %s" % (type(exc).__name__, exc))
    for key in ("host.http.concurrent", "host.download.sha256",
                "host.download.cross_source_resume", "host.capabilities"):
        try:
            api.log("PROBE-NET supports %s=%s" % (key, api.supports(key)))
        except Exception as exc:  # noqa: BLE001
            api.log("PROBE-NET supports-err %s: %s" % (key, exc))
