# NON-PRODUCT-FIXTURE: 正常消费者（depends 满足）
PLUGIN_INFO = {"id": "demo.addon", "version": "1.0.0",
               "repo": "https://github.com/demo/addon",
               "depends": {"demo.base": ">=1.0,<2"}}

def register(api):
    return None