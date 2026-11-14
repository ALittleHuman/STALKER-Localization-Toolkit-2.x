PLUGIN_INFO = {
    "id": "gate.missing",
    "name": "Gate Missing",
    "version": "1.0",
    "depends": {"gate.absent": "*"},
}


def register(api):
    api.log("should not load")
