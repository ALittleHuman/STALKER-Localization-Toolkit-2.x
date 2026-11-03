# plugins_net（示例，不随包）

`net_http.py` 是"联网插件"的**示例实现**：用 `plugins/_sdk/http_winhttp.py`
（ctypes + 系统 winhttp，零依赖）注册 `net.http_get/1` 服务。

用法：把 `net_http.py` 与本文件放进插件目录（`plugins/`），其他插件即可：

```python
result = api.services.call("net.http_get/1", url, timeout=5)
# {"status": "ok"|"error", "body": str, "error": str}
```

边界：
- 产品包**不随包**这个插件（base 包保持"无联网面"）；
- 宿主自己的"检查更新"走系统 WinHTTP，**不经过** `api.services`（否则插件服务反过来成了宿主依赖）；
- `plugins/_sdk/` 与子目录不会被加载器当插件（只收顶层 `*.py`）。
