# NON-PRODUCT-FIXTURE: 字面量 PLUGIN_INFO（判据夹具）
PLUGIN_INFO = {"name": "net_http", "provides": ["net.http_get/1"]}


def register(api):
    api.services.register("net.http_get/1", lambda url, timeout=5: {"status": "ok"})
