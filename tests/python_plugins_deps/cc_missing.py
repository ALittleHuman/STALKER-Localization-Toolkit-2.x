# NON-PRODUCT-FIXTURE: 缺前置 ⇒ 必须被自动禁用（register 不许被调）
PLUGIN_INFO = {"id": "demo.broken", "name": "Broken", "version": "1.0.0",
               "depends": {"demo.absent": ">=1"}}

def register(api):
    raise RuntimeError("缺前置的插件不许 register")