# NON-PRODUCT-FIXTURE: 注册后抛错 ⇒ 该 owner 的服务必须被回滚（判据夹具）
def register(api):
    api.services.register("net.partial", lambda: "should-not-survive")
    raise RuntimeError("fixture boom")
