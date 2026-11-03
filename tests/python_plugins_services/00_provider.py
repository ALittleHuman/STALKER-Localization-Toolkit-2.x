# NON-PRODUCT-FIXTURE: 服务提供者（判据夹具，不是产品插件）
def register(api):
    api.services.register("net.echo/1", lambda text: "echo:" + text)
    api.log("SVC-PROVIDED %s" % ",".join(api.services.available()))
