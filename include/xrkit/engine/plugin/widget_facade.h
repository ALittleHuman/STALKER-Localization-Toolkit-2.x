#pragma once
// xrkit::plugin —— **宿主控件门面**（插件改装的 L1/L2，规格 §三）。
//
// ★ 文件名**故意不叫** `ui_*.h`（本文件原叫 `ui_facade.h`）：Qt 的 **AUTOUIC** 会把
//   `#include "ui_*.h"` 当成 `.ui` 生成的界面头 ⇒ 外壳直接 AutoUic 失败
//   （实测：`AutoUic error … includes the uic file "xrkit/engine/plugin/ui_facade.h"`）。
//   类名/函数名仍是 `UiFacade` / `set_ui_facade`，只有**文件名**要躲开那个前缀。
//
// 为什么要有它：规格 §二 的硬边界是「**插件永不接触 Qt**」，而 §七·补 的下限是
//   「**对于控件的操控，必须至少支持现有的所有功能和观感**」。两者夹出来的唯一出路就是
//   **宿主门面**：插件只描述"要什么控件、什么内容、什么回调"，渲染/排版/委托/滚动/像素
//   全在宿主 C++ 里；插件手里只有**不透明句柄**。
//
// 三条口径：
//   ① **门面由外壳注入**（`set_ui_facade`）：引擎层（Qt-free）只持有指针并转发 ——
//      与 `set_active_registry` 同族。没有门面（CLI/无 GUI 进程）时每个方法返回 null/0，
//      并由调用方**逐条记账**（零静默）；**注册面照常可用**（无 Qt 进程也能驱动整套注册）。
//   ② ★**表外无门**：Python 侧能看到的只有本文件里这些方法（判据＝把 `dir(xrkit.Ui)` /
//      `dir(xrkit.Api)` 与预期表对账，**多一个就红** —— 见 `tests/check_ui_capability_table.py`）。
//   ③ **parent==nullptr ⇒ 落到"当前面板根"**：宿主在调插件 builder 之前设好（`UiPanelScope`），
//      于是插件页/面板的写法可以是"往宿主给的根里塞控件"，不必自己造根控件。
//
// 档位（规格 §三）：
//   · L1 配置/装配 —— `label/button/row/checkbox/entry/text/combo/drop_box/pack/text_view`；
//   · L2 操作 —— `set_text/get_text/set_enabled/alive` + 宿主服务 `log_append/ask_*`；
//   · ★重型 L2（`TreeView/VirtualList/SplitPane/TaskPanel/DropZone` 的表内方法）是**下一片**，
//     它们不是"再造一遍 Qt 控件"，而是把六页已经在用的那几套能力逐个透出（补门面、不开口子）。
//   · L3/L4（绘制回调 / 裸 Qt 对象）在本文件里**没有任何入口**。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::plugin {

// 不透明句柄：对插件而言就是"一个控件"。它只能把它传回门面，不能解引用。
using UiHandle = void*;
inline constexpr UiHandle kNoWidget = nullptr;

struct UiFacade {
    // ── L1：造控件（宿主负责排版规范 / 像素 / 主题）────────────────────────────
    std::function<UiHandle(UiHandle parent, const std::string& text, const std::string& style)> label;
    std::function<UiHandle(UiHandle parent, const std::string& text, std::function<void()> on_click,
                           int width, const std::string& style, bool enabled)>
        button;
    std::function<UiHandle(UiHandle parent, const std::string& side, const std::string& fill,
                           bool expand, int padx, int pady)>
        row;
    std::function<UiHandle(UiHandle parent, const std::string& text,
                           std::function<void(bool)> on_toggle)>
        checkbox;
    std::function<UiHandle(UiHandle parent, const std::string& initial, int width, bool password)>
        entry;
    std::function<UiHandle(UiHandle parent, int height, int width, const std::string& wrap)> text;
    std::function<UiHandle(UiHandle parent, const std::vector<std::string>& values,
                           std::function<void(const std::string&)> on_change)>
        combo;
    std::function<UiHandle(UiHandle parent, std::function<void(const std::string&)> on_file,
                           const std::string& placeholder)>
        drop_box;
    std::function<void(UiHandle parent, UiHandle child, const std::string& side,
                       const std::string& fill, bool expand, int padx, int pady)>
        pack;
    std::function<UiHandle(UiHandle parent, int height, bool mono)> text_view;
    // ── L2（轻）：改控件 / 取输入 / 宿主服务 ────────────────────────────────
    std::function<void(UiHandle widget, const std::string& text)> set_text;
    std::function<std::string(UiHandle widget)> get_text;
    std::function<void(UiHandle widget, bool enabled)> set_enabled;
    std::function<bool(UiHandle widget)> alive;
    std::function<void(const std::string& line)> log_append;   // 宿主日志栏（LogView）
    std::function<bool(const std::string& title, const std::string& message)> ask_yes_no;
    std::function<std::string(const std::string& title)> ask_open_file;
    std::function<std::string(const std::string& title)> ask_save_file;

    // ── ★★ L2（重型，第一批）：宿主组件的**表内方法** ──────────────────────────
    // 口径（规格 §三）：渲染/委托/滚动/像素全在宿主；插件只给"数据 + 回调"。
    //
    // VirtualList：**虚拟化**行列表（只为视口内的行取文本 —— 10.3 同族）。
    //   本批是**单列文本**形态（多列/三态勾选/行尾✕ 随 TreeView 批）。
    std::function<UiHandle(UiHandle parent, int row_height)> list;
    std::function<void(UiHandle list, int rows)> list_set_row_count;
    // ★L2.5：宿主在**可见行**上问插件要文本（`row` 是全局行号）
    std::function<void(UiHandle list, std::function<std::string(int)> provider)> list_set_row_text;
    std::function<std::string(UiHandle list, int row)> list_row_text;
    std::function<void(UiHandle list, int row)> list_select_row;
    std::function<int(UiHandle list)> list_selected_row;
    std::function<int(UiHandle list)> list_content_height;

    // LogView：行式日志视图（**插件自己的**日志区；宿主日志栏是 `log_append`）
    std::function<UiHandle(UiHandle parent, int height)> log_view;
    std::function<void(UiHandle view, const std::string& line)> log_view_append;
    std::function<void(UiHandle view)> log_view_clear;
    std::function<std::string(UiHandle view)> log_view_text;

    // DropZone：拖入目标（宿主负责"本地文件才接、其余给禁止光标"）
    std::function<UiHandle(UiHandle parent, const std::string& hint)> drop_zone;
    std::function<void(UiHandle zone, std::function<void(const std::vector<std::string>&)> on_drop)>
        drop_zone_on_drop;

    // TaskPanel：宿主任务壳（进度/消息/失败/取消/状态栏）—— 插件**不再自己起线程**。
    //   `task_run(panel, title, worker)`：worker 在**工作线程**里跑，收到两个 sink
    //   （`progress(0~100)` / `message(text)`）；**工作线程里只许调这两个 sink**，
    //   其余门面方法一律回主线程再调（`on_ui_thread` 用来判）。
    using TaskProgress = std::function<void(int)>;
    using TaskMessage = std::function<void(const std::string&)>;
    std::function<UiHandle(UiHandle parent, bool with_cancel)> task_panel;
    std::function<bool(UiHandle panel, const std::string& title,
                       std::function<void(TaskProgress, TaskMessage)> worker)>
        task_run;
    std::function<std::string(UiHandle panel)> task_status;
    std::function<void(UiHandle panel)> task_cancel;
    std::function<bool(UiHandle panel)> task_cancel_requested;

    // 当前调用是否在**宿主 UI 线程**上：造控件/改控件只许在主线程；
    // 不在 ⇒ 一律返回空/0 并由调用方**记账**（零静默，防"工作线程里碰 Qt"那类崩）。
    std::function<bool()> on_ui_thread;

    // ── ★★ L2（重型，第二批）：滚动视口 / 可分栏 ──────────────────────────────
    // ScrollViewport：口径 1.x 1.6「**不许有裸滚动条：条子只能由本组件造**」——
    //   所以插件拿到的句柄是**画布**（body），往它里面 `pack` 控件即可；
    //   条子、滚轮路由、"装得下就不占位"全在宿主。
    //   `min_body_height`：内容高**不许**用"内容请求高度"当内容高（3.3），由插件/宿主给。
    std::function<UiHandle(UiHandle parent, int min_body_height)> viewport;
    std::function<void(UiHandle body, bool content_fit)> viewport_fit_content;
    std::function<void(UiHandle body, int min_body_height)> viewport_set_min_body_height;
    std::function<void(UiHandle body)> viewport_sync;
    std::function<bool(UiHandle body)> viewport_can_scroll;
    std::function<int(UiHandle body)> viewport_scroll_top;

    // SplitPane：可分栏容器（**拖动期不改几何、松手一次重排**；每格上限；可完全收起）。
    //   `split_add_pane` 返回**新的一格容器**（句柄），插件往它里面 `pack`。
    std::function<UiHandle(UiHandle parent, bool horizontal)> split;
    std::function<UiHandle(UiHandle split, int minsize)> split_add_pane;
    std::function<void(UiHandle split, int index, int max_size)> split_set_max_size;
    std::function<void(UiHandle split, int index, bool collapsed)> split_set_collapsed;
    std::function<bool(UiHandle split, int index)> split_is_collapsed;
    std::function<int(UiHandle split)> split_pane_count;
    std::function<int(UiHandle split, int index)> split_sash_pos;
};

// 外壳注入实现（`src/shell/ui_facade_qt.cpp`）。传 nullptr = 撤销（无 GUI 依赖的进程/用例）。
void set_ui_facade(const UiFacade* facade);
const UiFacade* ui_facade();

// 门面可用性（探针/判据用）。不可用时 `reason` 里是**为什么**（零静默）。
//   · 无门面（CLI/离屏自检没装）⇒ "宿主没注入控件门面（CLI/无 GUI 进程）"；
//   · 装了但缺方法 ⇒ 指出缺哪个（门面允许**部分实现**：没实现的方法一律空，调用侧记账）。
bool ui_facade_available(std::string* reason = nullptr);
// 某个方法是否可用（`name` = 上面那些字段名；不认识的名字返回 false）
bool ui_facade_has(const std::string& name);
// 门面里**已实现**的方法名（顺序稳定）——判据拿它和预期表对账。
std::vector<std::string> ui_facade_method_names();

// ── 当前面板根：宿主在调插件 builder 之前设好（见文件头 ③）──────────────────
void set_current_panel_root(UiHandle root);
UiHandle current_panel_root();

// RAII：作用域内"当前面板根"= root，析构时还原成上一个（可嵌套；宿主调 builder 时用）。
struct UiPanelScope {
    explicit UiPanelScope(UiHandle root);
    ~UiPanelScope();
    UiPanelScope(const UiPanelScope&) = delete;
    UiPanelScope& operator=(const UiPanelScope&) = delete;

private:
    UiHandle previous_ = nullptr;
};

}  // namespace xrkit::plugin
