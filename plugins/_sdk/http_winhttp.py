# -*- coding: utf-8 -*-
"""零依赖 HTTP 参考实现（ctypes + 系统 winhttp.dll）—— 插件用，**不随包强绑**。

为什么不是 urllib：2.0 起不随包 `_ssl.pyd`，`http.client` 顶层 `import ssl` 会直接失败
（连 http://127.0.0.1 也不行）。插件要联网就抄这份，或自备 `_ssl.pyd` + OpenSSL。

用法：
    from http_winhttp import http_get
    text = http_get("https://example.com/version.txt", timeout=5)

契约（与 docs/2.0.0-跨插件服务契约.md 对齐）：
  · 同步阻塞，调用者线程执行（同进程、无隔离、无超时保证）；
  · 走系统代理（AUTOMATIC_PROXY）与系统证书校验，**不关校验**；
  · 允许 http→https 与 https→https 重定向，禁止 https→http 降级；
  · 总预算由调用者给（WinHTTP 的四个超时是分项，不是总超时）。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes

_winhttp = ctypes.WinDLL("winhttp", use_last_error=True)

WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4
WINHTTP_FLAG_SECURE = 0x00800000
WINHTTP_OPTION_REDIRECT_POLICY = 110
WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP = 1


def http_get(url: str, timeout_ms: int = 5000, max_bytes: int = 1 << 20) -> str:
    """GET url，返回 UTF-8 文本（上限 max_bytes，防超大响应打满内存）。"""
    session = _winhttp.WinHttpOpen("xrkit/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   None, None, 0)
    if not session:
        raise OSError("WinHttpOpen 失败：%d" % ctypes.get_last_error())
    try:
        # 四项超时都设成同一个预算（resolve/connect/send/receive）
        for opt in (1, 2, 3, 4):
            _winhttp.WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms)
        policy = wintypes.DWORD(WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP)
        _winhttp.WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
                                  ctypes.byref(policy), ctypes.sizeof(policy))

        # 拆分 scheme/host/path（够用即可；不做 URL 规范化）
        secure = url.lower().startswith("https://")
        rest = url.split("://", 1)[1]
        host, _, path = rest.partition("/")
        path = "/" + path
        port = 443 if secure else 80
        if ":" in host:
            host, _, p = host.partition(":")
            port = int(p)

        conn = _winhttp.WinHttpConnect(session, host, port, 0)
        if not conn:
            raise OSError("WinHttpConnect 失败：%d" % ctypes.get_last_error())
        try:
            req = _winhttp.WinHttpOpenRequest(conn, "GET", path, None, None, None,
                                              WINHTTP_FLAG_SECURE if secure else 0)
            if not req:
                raise OSError("WinHttpOpenRequest 失败：%d" % ctypes.get_last_error())
            try:
                if not _winhttp.WinHttpSendRequest(req, None, 0, None, 0, 0, 0):
                    raise OSError("WinHttpSendRequest 失败：%d" % ctypes.get_last_error())
                if not _winhttp.WinHttpReceiveResponse(req, None):
                    raise OSError("WinHttpReceiveResponse 失败：%d" % ctypes.get_last_error())
                chunks, total = [], 0
                buf = ctypes.create_string_buffer(8192)
                read = wintypes.DWORD(0)
                while total < max_bytes:
                    if not _winhttp.WinHttpReadData(req, buf, len(buf), ctypes.byref(read)):
                        raise OSError("WinHttpReadData 失败：%d" % ctypes.get_last_error())
                    if read.value == 0:
                        break
                    chunks.append(buf.raw[: read.value])
                    total += read.value
                return b"".join(chunks).decode("utf-8", "replace")
            finally:
                _winhttp.WinHttpCloseHandle(req)
        finally:
            _winhttp.WinHttpCloseHandle(conn)
    finally:
        _winhttp.WinHttpCloseHandle(session)
