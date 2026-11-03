# NON-PRODUCT-FIXTURE: 消费者示例（不随包）
"""消费者示例：不自己写 HTTP，只调 net.http_get/1。"""


def register(api):
    result = api.services.call("net.http_get/1", "https://example.com/", 5)
    api.log("NET-EXAMPLE status=%s body_len=%d error=%s"
            % (result["status"], len(result["body"]), result["error"]))
