# NON-PRODUCT-FIXTURE: 循环依赖 a（必须都被禁用）
PLUGIN_INFO = {"id": "demo.cycle_a", "version": "1.0", "depends": {"demo.cycle_b": "*"}}

def register(api):
    raise RuntimeError("循环依赖的插件不许 register")