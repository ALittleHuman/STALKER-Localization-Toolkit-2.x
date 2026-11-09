# NON-PRODUCT-FIXTURE: 消费者示例（不随包）
"""消费者示例：不自己写 HTTP，只调 ``net.http_get/1``。

`depends` 演示**模组前置**：没有 `xrkit.net_http`（或版本不够）时，本插件在启动期会被
自动禁用（见 plugins_net/net_http.py）；放好联网插件后才会加载。
"""
PLUGIN_INFO = {
    "id": "xrkit.consumer_example",
    "name": "消费者示例",
    "version": "1.0.0",
    "depends": {"xrkit.net_http": ">=1.0"},
}


def register(api):
    # 注册期不联网（避免启动时阻塞）；真正取数据放到 headless/按钮回调里。
    return None


def headless(api, args):
    url = args.get("url", "https://example.com/")
    return api.services.call("net.http_get/1", url, args.get("timeout", 5))
