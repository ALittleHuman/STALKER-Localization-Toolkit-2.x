#pragma once
// xrkit::shell —— 插件 ABI 里 `api.ui` 的 **Qt 实现**（M2-7f）。
//
// 12.2 说过：整套插件契约里只有**三处**与工具包强耦合（`api.widgets`、`register_panel`
// 的 builder、`register_tool` 的 builder）—— 这里就是那三处背后的实现，也是外壳唯一
// 需要认识 Qt 的插件相关代码。插件拿到的是不透明 `stk_widget*`，永远不 deref。
//
// 16 个方法的**名字与参数名**必须与 1.x `toolkit_plugin_ui.py` 一致（第三方契约），
// 那份清单由 `tools/gen_plugin_contract.py` 从源码 AST 读出、门禁 [13] 锁住。
//
// ★ 三个标准对话框（`ask_yes_no` / `ask_open_file` / `ask_save_file`）会**阻塞**：
//   自检不跑这条路径（只核对槽位非空）；真实路径由用户点击触发。

#include "xrkit/plugin/abi.h"

namespace xrkit::shell {

const stk_ui* qt_plugin_ui();

}  // namespace xrkit::shell
