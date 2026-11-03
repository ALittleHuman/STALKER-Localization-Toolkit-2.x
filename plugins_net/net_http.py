# NON-PRODUCT-FIXTURE: **示例**（不随包；产品包保持"无联网面"）。
"""联网插件示例：提供 net.http_get/1 服务。

依赖：plugins/_sdk/http_winhttp.py（ctypes + 系统 winhttp；不需要 _ssl.pyd）。
其他插件只写 api.services.call("net.http_get/1", url, timeout=5)，不重复实现 HTTP。
"""
PLUGIN_INFO = {"name": "net_http", "provides": ["net.http_get/1"],}

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "plugins", "_sdk"))
from http_winhttp import http_get  # noqa: E402


def _handler(url, timeout=5):
    try:
        return {"status": "ok", "body": http_get(url, timeout * 1000), "error": ""}
    except Exception as exc:  # 零静默：失败文本原样带出去
        return {"status": "error", "body": "", "error": "%s: %s" % (type(exc).__name__, exc)}


def register(api):
    api.services.register("net.http_get/1", _handler)
