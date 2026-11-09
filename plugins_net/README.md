# plugins_net（联网插件；**独立分发，不随包、不内置**）

`net_http.py` 是**单文件自包含**的联网插件：内嵌 ctypes + 系统 `winhttp.dll` 实现，
**不依赖** `plugins/_sdk/` 的相对路径。把这一个文件放进插件目录即可用：

```text
<你的插件目录>\
    net_http.py        ← 就这一个文件
```

（`plugins/_sdk/http_winhttp.py` 仍是**参考实现**，给别的插件抄；它不是加载目标。）

## 服务契约：`net.http_get/1`

```python
result = api.services.call("net.http_get/1", url, timeout=5, max_bytes=1048576)
# {
#   "status": "ok" | "error",
#   "body": str,      # UTF-8 文本（错误时为空）
#   "error": str,     # 错误时 "类型: 说明"
#   "code": int,      # HTTP 状态码（拿不到为 0）
#   "url": str,
#   "bytes": int,     # 实际读到的字节数
#   "truncated": bool # 是否被 max_bytes 截断
# }
```

- 只允许 `http`/`https`；其他 scheme 返回 `status=error`（不把异常抛给消费者）；
- `timeout` 1..120 秒，`max_bytes` 1 KiB..16 MiB（默认 1 MiB）；
- 同步阻塞、在调用者线程执行；同进程、无隔离、无超时保证；
- 走系统代理与系统证书校验，**不关校验**；禁止 https→http 降级。

## 清单

```python
PLUGIN_INFO = {
    "id": "xrkit.net_http",          # 稳定唯一键（依赖判定只认 id）
    "name": "联网（WinHTTP）",
    "version": "1.0.0",
    "repo": "https://github.com/ALittleHuman/STALKER-Localization-Toolkit-2.x",  # 仅元数据
    "provides": ["xrkit.net"],
}
```

## 边界

- 产品包**不随包**本插件（base 包保持"无联网面"；`tests/check_host_offline.py` 断言包内无 `plugins_net`）；
- 宿主自己的"查看最新版本"不经过本服务（插件服务不得反过来成为宿主依赖）；
- 插件目录只扫**顶层** `*.py`，子目录不会被当插件。

## 判据

```text
py tests/check_net_plugin.py
```

离线跑：起本地 `127.0.0.1` HTTP server → 加载本插件 → 服务调用 → 正文回读；另测 `ftp://`
必须 `status=error`。门禁步：`net-plugin`。
