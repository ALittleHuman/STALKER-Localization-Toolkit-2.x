# NON-PRODUCT-FIXTURE: **联网插件（单文件，独立分发；不随包、不内置）**
"""联网插件：提供 ``net.http_get/1`` 服务（零依赖，ctypes + 系统 winhttp.dll）。

为什么单文件
------------
插件目录只扫**顶层** ``*.py``（``loader.cpp`` / ``py_host.cpp``），而 ``_sdk/`` 子目录
不会被加载。为了“把这一个文件丢进 plugins\\ 就能用”，WinHTTP 实现直接内嵌在本文件里，
不依赖 ``_sdk/http_winhttp.py`` 的相对路径（那份仍作为**参考实现**给别的插件抄）。

不随包 / 不内置
--------------
宿主与发行包都不带本文件（``tests/check_host_offline.py`` 断言包内无 ``plugins_net``）；
用户要联网就自己把本文件放进插件目录。宿主自己的“查看最新版本”不经过本服务。

服务契约（``net.http_get/1``）
-----------------------------
``result = api.services.call("net.http_get/1", url, timeout=5, max_bytes=1048576)``

    {
      "status": "ok" | "error",
      "body":   str,          # UTF-8 文本（错误时为空）
      "error":  str,          # 错误时 "类型: 说明"；成功为空
      "code":   int,          # HTTP 状态码（拿不到为 0）
      "url":    str,          # 请求的 url（原样）
      "bytes":  int,          # 实际读到的字节数
      "truncated": bool       # 是否因为 max_bytes 上限被截断
    }

边界：
  · 只允许 http/https；其他 scheme 直接返回 error（不抛给消费者）；
  · 同步阻塞、在调用者线程执行；同进程、无隔离、无超时保证（见跨插件服务契约）；
  · 走系统代理与系统证书校验，**不关校验**；禁止 https→http 降级；
  · 超时按秒给（1..120），max_bytes 上限 16 MiB（默认 1 MiB）。
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from urllib.parse import urlsplit

PLUGIN_INFO = {
    "id": "xrkit.net_http",
    "name": "联网（WinHTTP）",
    "version": "1.0.0",
    # ★ repo 只作元数据（插件管理器展示/更新来源），不参与依赖判定。
    "repo": "https://github.com/ALittleHuman/STALKER-Localization-Toolkit-2.x",
    "provides": ["xrkit.net"],
}

# ── WinHTTP（ctypes）：原型必须声明 ────────────────────────────────────────────
# 不声明 restype/argtypes 时 ctypes 默认把返回值当 32 位 int ⇒ 64 位下 HINTERNET
# 被截断，WinHttpConnect 直接 ERROR_INVALID_HANDLE(6)。本文件按已验证的原型写死。
_winhttp = ctypes.WinDLL("winhttp", use_last_error=True)

_HANDLE = wintypes.HANDLE
_DWORD = wintypes.DWORD
_LPCWSTR = wintypes.LPCWSTR

_winhttp.WinHttpOpen.restype = _HANDLE
_winhttp.WinHttpOpen.argtypes = [_LPCWSTR, _DWORD, _LPCWSTR, _LPCWSTR, _DWORD]
_winhttp.WinHttpSetTimeouts.restype = wintypes.BOOL
_winhttp.WinHttpSetTimeouts.argtypes = [_HANDLE, ctypes.c_int, ctypes.c_int,
                                        ctypes.c_int, ctypes.c_int]
_winhttp.WinHttpSetOption.restype = wintypes.BOOL
_winhttp.WinHttpSetOption.argtypes = [_HANDLE, _DWORD, ctypes.c_void_p, _DWORD]
_winhttp.WinHttpConnect.restype = _HANDLE
_winhttp.WinHttpConnect.argtypes = [_HANDLE, _LPCWSTR, wintypes.WORD, _DWORD]
_winhttp.WinHttpOpenRequest.restype = _HANDLE
_winhttp.WinHttpOpenRequest.argtypes = [_HANDLE, _LPCWSTR, _LPCWSTR, _LPCWSTR, _LPCWSTR,
                                        ctypes.POINTER(_LPCWSTR), _DWORD]
_winhttp.WinHttpSendRequest.restype = wintypes.BOOL
_winhttp.WinHttpSendRequest.argtypes = [_HANDLE, _LPCWSTR, _DWORD, ctypes.c_void_p, _DWORD,
                                        _DWORD, ctypes.c_size_t]
_winhttp.WinHttpReceiveResponse.restype = wintypes.BOOL
_winhttp.WinHttpReceiveResponse.argtypes = [_HANDLE, ctypes.c_void_p]
_winhttp.WinHttpQueryHeaders.restype = wintypes.BOOL
_winhttp.WinHttpQueryHeaders.argtypes = [_HANDLE, _DWORD, _LPCWSTR, ctypes.c_void_p,
                                         ctypes.POINTER(_DWORD), ctypes.c_void_p]
_winhttp.WinHttpReadData.restype = wintypes.BOOL
_winhttp.WinHttpReadData.argtypes = [_HANDLE, ctypes.c_void_p, _DWORD,
                                     ctypes.POINTER(_DWORD)]
_winhttp.WinHttpCloseHandle.restype = wintypes.BOOL
_winhttp.WinHttpCloseHandle.argtypes = [_HANDLE]

WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4
WINHTTP_FLAG_SECURE = 0x00800000
WINHTTP_OPTION_REDIRECT_POLICY = 110
WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP = 1
WINHTTP_QUERY_STATUS_CODE = 19
WINHTTP_QUERY_FLAG_NUMBER = 0x20000000


def _http_get(url: str, timeout_ms: int, max_bytes: int):
    """返回 (text, status_code, raw_bytes, truncated)。调用方保证 scheme 合法。"""
    parts = urlsplit(url)
    secure = parts.scheme.lower() == "https"
    host = parts.hostname or ""
    port = parts.port or (443 if secure else 80)
    path = parts.path or "/"
    if parts.query:
        path += "?" + parts.query

    session = _winhttp.WinHttpOpen("xrkit/2.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   None, None, 0)
    if not session:
        raise OSError("WinHttpOpen 失败：%d" % ctypes.get_last_error())
    try:
        # 四项超时都设成同一个预算（resolve/connect/send/receive）
        _winhttp.WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms)
        policy = wintypes.DWORD(WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP)
        _winhttp.WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
                                  ctypes.byref(policy), ctypes.sizeof(policy))

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

                code = wintypes.DWORD(0)
                size = wintypes.DWORD(ctypes.sizeof(code))
                status_code = 0
                if _winhttp.WinHttpQueryHeaders(
                        req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        None, ctypes.byref(code), ctypes.byref(size), None):
                    status_code = int(code.value)

                chunks, total, truncated = [], 0, False
                buf = ctypes.create_string_buffer(8192)
                read = wintypes.DWORD(0)
                while True:
                    if not _winhttp.WinHttpReadData(req, buf, len(buf), ctypes.byref(read)):
                        raise OSError("WinHttpReadData 失败：%d" % ctypes.get_last_error())
                    if read.value == 0:
                        break
                    room = max_bytes - total
                    if read.value >= room:
                        chunks.append(buf.raw[:room])
                        total += room
                        truncated = True
                        break
                    chunks.append(buf.raw[: read.value])
                    total += read.value
                raw = b"".join(chunks)
                return raw.decode("utf-8", "replace"), status_code, len(raw), truncated
            finally:
                _winhttp.WinHttpCloseHandle(req)
        finally:
            _winhttp.WinHttpCloseHandle(conn)
    finally:
        _winhttp.WinHttpCloseHandle(session)


def _clamp(value, low, high, default):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return default
    return max(low, min(high, number))


def _handler(url, timeout=5, max_bytes=1 << 20):
    """服务处理函数：永远返回 dict（错误也走 status=error，不把异常抛给消费者）。"""
    result = {"status": "error", "body": "", "error": "", "code": 0,
              "url": str(url), "bytes": 0, "truncated": False}
    if not isinstance(url, str):
        result["error"] = "TypeError: url 必须是字符串"
        return result
    scheme = urlsplit(url).scheme.lower()
    if scheme not in ("http", "https"):
        result["error"] = "ValueError: 只允许 http/https（收到 %r）" % scheme
        return result
    timeout_ms = _clamp(timeout, 1, 120, 5) * 1000
    budget = _clamp(max_bytes, 1024, 16 << 20, 1 << 20)
    try:
        text, code, size, truncated = _http_get(url, timeout_ms, budget)
    except Exception as exc:  # 零静默：错误类型与文本原样带出去
        result["error"] = "%s: %s" % (type(exc).__name__, exc)
        return result
    result.update({"status": "ok", "body": text, "code": code,
                   "bytes": size, "truncated": truncated})
    return result


def register(api):
    api.services.register("net.http_get/1", _handler)
