# NON-PRODUCT-FIXTURE: 注册不带版本必须被拒（判据夹具）
def register(api):
    api.services.register("net.nover", lambda: None)
