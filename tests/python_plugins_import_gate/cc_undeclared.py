PLUGIN_INFO = {
    "id": "gate.undeclared",
    "name": "Gate Undeclared",
    "version": "1.0",
}

import aa_base  # noqa: F401  —— 没有在 depends 里声明，必须被守卫拒绝


def register(api):
    api.log("should not load")
