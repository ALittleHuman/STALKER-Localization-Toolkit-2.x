PLUGIN_INFO = {
    "id": "gate.consumer",
    "name": "Gate Consumer",
    "version": "1.0.0",
    "depends": {"gate.base": ">=1.0"},
}

import aa_base

VALUE = aa_base.VALUE


def register(api):
    api.log("bb_consumer imported=" + VALUE)
