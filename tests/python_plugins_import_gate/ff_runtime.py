PLUGIN_INFO = {
    "id": "gate.runtime",
    "name": "Gate Runtime",
    "version": "1.0",
}


def register(api):
    def handler():
        try:
            import aa_base  # noqa: F401  —— 运行时没有"当前插件"上下文，必须被拒
        except ModuleNotFoundError as exc:
            api.log("runtime-blocked:" + str(exc))

    command_id = api.register_command("gate_runtime_probe", "Gate Runtime Probe", handler)
    api.register_menu_item("fs", "toolbar", command_id, 0)
