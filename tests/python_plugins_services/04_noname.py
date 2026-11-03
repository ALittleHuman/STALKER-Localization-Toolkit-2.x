# NON-PRODUCT-FIXTURE: 不带命名空间的服务名必须被拒（判据夹具）
def register(api):
    api.services.register("plain", lambda: None)
