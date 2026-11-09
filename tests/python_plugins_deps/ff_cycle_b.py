# NON-PRODUCT-FIXTURE: 循环依赖 b（必须都被禁用）
PLUGIN_INFO = {"id": "demo.cycle_b", "version": "1.0", "depends": {"demo.cycle_a": "*"}}

def register(api):
    raise RuntimeError("循环依赖的插件不许 register")