# NON-PRODUCT-FIXTURE: 坏插件注册的服务必须已经不可用（判据夹具）
def register(api):
    api.log("SVC-PARTIAL %s" % api.services.call("net.partial"))
