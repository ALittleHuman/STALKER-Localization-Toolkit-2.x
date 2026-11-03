# NON-PRODUCT-FIXTURE: 未注册服务必须抛明确异常（判据夹具）
def register(api):
    api.services.call("net.nope", "x")
