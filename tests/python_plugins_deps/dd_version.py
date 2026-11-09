# NON-PRODUCT-FIXTURE: 版本不满足 ⇒ 必须被自动禁用
PLUGIN_INFO = {"id": "demo.old", "name": "Old", "version": "1.0.0",
               "depends": {"demo.base": ">=2.0"}}

def register(api):
    raise RuntimeError("版本不满足的插件不许 register")