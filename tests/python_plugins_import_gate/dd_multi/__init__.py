PLUGIN_INFO = {
    "id": "gate.multi",
    "name": "Gate Multi",
    "version": "1.0",
}

from . import helper


def register(api):
    api.log("dd_multi helper=" + helper.VALUE)
