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

#include <functional>
#include <string>

namespace xrkit::shell {

const stk_ui* qt_plugin_ui();

// ★★ 插件改装 P3（控件门面 L1/L2）：装配控件门面 —— **外壳装配时调一次**。
// `log_sink` 非空时把"写宿主日志栏"这条接上（门面的 `log_append`）；空 ⇒ 该条不可用
// （`ui_facade_available()` 会如实报"缺方法"，调用侧记账）。
// 门面实现见 `ui_facade_qt.cpp`：它把 `ui_facade.h` 的 18 个方法**转发**给本文件这张
// C ABI 表，因此"C ABI 插件"与"Python 插件"用的**是同一套控件语义**（只许一份实现）。
void install_qt_ui_facade(std::function<void(const std::string&)> log_sink);

// ★★ 插件改装 P3（控件门面 L1/L2）：**取控件当前文本**。
// 为什么放在这里而不是在门面实现里另写一份：`plugin_ui.cpp` 是"控件语义"的**唯一实现**
// （C ABI 的 `api.ui` 与 Python 侧的 `api.ui` 都走它）—— 门面只是把它再包一层。
// 认得的控件：QLabel/QPushButton/QLineEdit/QPlainTextEdit；不认得返回空串。
std::string qt_plugin_widget_text(stk_widget* widget);

}  // namespace xrkit::shell
