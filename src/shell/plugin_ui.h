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

#include "xrkit/engine/plugin/facade_api.h"

#include <functional>
#include <string>

// ★ 2026-10-23：`make_host_widget()` 的返回类型 —— 本头文件故意**不引 Qt**（12.2 的边界），
//   所以只前置声明（`QWidget` 在全局命名空间）。
class QWidget;

namespace xrkit::shell {

const stk_ui* qt_plugin_ui();

// ★★ 插件改装 P3（控件门面 L1/L2）：装配控件门面 —— **外壳装配时调一次**。
// `log_sink` 非空时把"写宿主日志栏"这条接上（门面的 `log_append`）；空 ⇒ 该条不可用
// （`ui_facade_available()` 会如实报"缺方法"，调用侧记账）。
// 门面实现见 `ui_facade_qt.cpp`：它把 `ui_facade.h` 的 18 个方法**转发**给本文件这张
// C ABI 表，因此"C ABI 插件"与"Python 插件"用的**是同一套控件语义**（只许一份实现）。
// ★★ 2026-10-21（用户指出的结构性问题）：`log_append` 是**进程级 sink**，而每个 Hub 装配时
//   都用 `[this]` 捕获自己的日志栏 ⇒ 后装配的 Hub 覆盖前一个；先装配的那个 Hub 一析构，
//   全局 sink 里还留着悬垂的 `this`（插件再调 `api.ui.log_append` 就是 UAF）。
//   修法：① 只有**主线程**的装配才设置 sink；② 装配时登记 owner，`~Hub` 调
//   `clear_qt_ui_facade_log_sink(this)` —— 只有"当前 sink 确实是自己"时才清空。
void install_qt_ui_facade(std::function<void(const std::string&)> log_sink,
                          const void* owner = nullptr);
void clear_qt_ui_facade_log_sink(const void* owner);

// ★ 启动时打诊断用：现在"是不是 UI 线程"的**真实判定**（以 Qt 为准，见 .cpp 注释）。
bool qt_ui_facade_on_ui_thread();

// ★★ 2026-10-21（用户口径）：**page_trace 的统一入口**。写 `%TEMP%/page_trace.txt`，
//   一行 = `page_id|kind|detail|monotonic|thread|pid`；`kind` 例：
//   `build_start` / `built` / `error:<what>` / `settle:…` / `scope_ctor` / `scope_dtor` /
//   `resolve` / `place` / `container_delete_later`。
//   只在诊断里用；**不许改产品行为**（写失败就放弃）。
void trace_page_event(const std::string& kind, const std::string& detail);

// ★ 2026-10-22（用户口径）：外壳侧 page_trace 的**轻量追加**与**一次性落盘**。
//   `append_shell_page_trace` 零文件 I/O；`flush_shell_page_trace` 由 xrkit_app 退出前调用。
void append_shell_page_trace(const std::string& page, const std::string& kind,
                             const std::string& detail);
void flush_shell_page_trace();

// ★ 2026-10-22：引擎侧 ring 的显式落盘转发（smoke 不必 include 引擎头）。
void flush_engine_page_trace();

// ★★ 2026-10-22（用户口径）：**菜单就绪钩子**（只发通知，不做等待）。
//   两处菜单共用同一个回调：`button_on_menu`（按钮左键动作菜单）与 `context_menu`（右键）。
//   时机＝**actions 已装完、popup/exec 之前**；带 `id` 与 `actions` 数。
// ★★ 2026-10-23（用户口径）：**page 由调用点传**（菜单构建时捕获），
//   不在 ready 时读 `current_page_id()` —— 那时页上下文可能已还原（为空）。
void set_menu_ready_hook(
    std::function<void(const char* menu_id, int actions, const char* page)> hook);
void notify_menu_ready(const char* menu_id, int actions, const char* page);

// ★★ 插件改装 P3（控件门面 L1/L2）：**取控件当前文本**。
// 为什么放在这里而不是在门面实现里另写一份：`plugin_ui.cpp` 是"控件语义"的**唯一实现**
// （C ABI 的 `api.ui` 与 Python 侧的 `api.ui` 都走它）—— 门面只是把它再包一层。
// 认得的控件：QLabel/QPushButton/QLineEdit/QPlainTextEdit；不认得返回空串。
std::string qt_plugin_widget_text(stk_widget* widget);

// ★★ 2026-10-23（P5 实证）：**把宿主新造的页容器认成"在册且活着"的句柄**。
//
//   事实（`build/logs/p5_probe/summary.json` 的实测）：插件页存在时，页容器的地址会
//   **复用**某个已销毁门面控件的地址；而 `unwrap()` 对"已登记但已死"的地址返回 nullptr
//   ⇒ `resolve_parent()` 拿到 null ⇒ 该页**所有**控件都无父（`place_null`）
//   ⇒ 页看起来"根本没建出来"。10 次里 6 次出现 `root_stale=1`，1 次直接红 8 条
//   （P17-82 ×4 / P16-3b-2 / P16-3c / P4 ×2）。
//
//   修法就是这一句：**新容器登记进去（覆盖任何陈旧条目）** ⇒ 它不会再被死项遮住；
//   容器销毁后自然变成死项，语义与其它句柄一致（插件对它 `alive()` 变 false 是对的）。
void adopt_host_widget(void* widget);

// ★★ 2026-10-23（用户口径「下次换个页又会踩地址复用」）：**宿主自己造的、会当 parent/句柄用的控件**
//   一律走这个工厂 —— 它 = `new QWidget` + `adopt_host_widget`，把"忘了登记"从"可能忘记"变成"不可能"。
//   判据：`tests/check_host_widget_adopt.py`（hub.cpp 里不许再出现 `container = new QWidget`）。
QWidget* make_host_widget();

}  // namespace xrkit::shell
