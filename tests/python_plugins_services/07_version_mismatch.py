# NON-PRODUCT-FIXTURE: 版本不匹配必须报"不兼容"，不是"没有提供者"（判据夹具）
def register(api):
    api.services.call("net.echo/2", "x")
