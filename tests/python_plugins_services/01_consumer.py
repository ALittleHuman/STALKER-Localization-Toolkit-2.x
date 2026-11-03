# NON-PRODUCT-FIXTURE: 服务消费者（判据夹具）
def register(api):
    api.log("SVC-CALL %s" % api.services.call("net.echo/1", "hi"))
