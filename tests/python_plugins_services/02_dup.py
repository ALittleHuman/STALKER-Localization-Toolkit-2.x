# NON-PRODUCT-FIXTURE: 重名注册必须报错（判据夹具）
def register(api):
    api.services.register("net.echo/1", lambda text: "other")
