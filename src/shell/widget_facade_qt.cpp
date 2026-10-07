// xrkit::shell —— **控件门面的 Qt 实现**（插件改装 P3：L1/L2 的宿主侧）。
//
// 口径（规格 §三「控件的操控必须至少支持现有的所有功能和观感」＋"补门面、不开口子"）：
//   · 本文件**不另写一套控件语义**：`plugin_ui.cpp` 是控件语义的唯一实现（C ABI 的 `api.ui`
//     与 Python 的 `api.ui` 走同一批函数），这里只做**三层转换**：
//       ① 句柄：`void*` ↔ `stk_widget*`（都是 `QWidget*`，只是两边的类型别名）；
//       ② 回调：`std::function` ↔ C ABI 的"裸函数指针 + user 指针"（用**进程级回调盒**保活）；
//       ③ `std::string` ↔ `const char*`。
//   · ★**L1 的装配承诺**：宿主负责排版 —— 造出来的控件自动落进父容器的纵向布局（`place()`），
//     插件只描述"要什么控件"。显式 `pack()` 仍可用（它决定顺序/拉伸）。
//   · 回调盒**刻意与进程同寿**（`new` 出去不删）：控件活多久它就得活多久，而"控件先死"这件事
//     只能靠 Qt 的对象树；提前删盒＝回调里读悬垂。与 C ABI 侧的 `new PluginApiAdapter`
//     同一条纪律（那份也是"活到进程结束"）。
//   · 弹出式对话框（ask_*）会**阻塞**：只有用户点了才会走到（自检不走这条路径）。

#include "plugin_ui.h"

#include "drop_filter.h"
#include "task_shell.h"
#include "virtual_list.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/widget_facade.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace xrkit::shell {
namespace {

namespace plug = xrkit::plugin;

const stk_ui* table() { return qt_plugin_ui(); }

// ── P4 的几件宿主侧状态 ───────────────────────────────────────────────────────
// 拖放目标的回调盒：`FileDropFilter` 的 handler 是**构造时**给的（没有 setter），所以先装一个
// "转发到盒子里"的 handler，`drop_zone_on_drop` 再往盒子里填真正的回调。
// ★ 盒与过滤器都**进程级保活**（与控件同寿；同 L1 回调盒的纪律）。
struct DropBox {
    std::function<void(const std::vector<std::string>&)> handler;
    FileDropFilter* filter = nullptr;
};

std::map<void*, DropBox*>& drop_boxes() {
    static std::map<void*, DropBox*>* instance = new std::map<void*, DropBox*>();
    return *instance;
}

std::map<void*, TaskShell*>& task_shells() {
    static std::map<void*, TaskShell*>* instance = new std::map<void*, TaskShell*>();
    return *instance;
}

TaskShell* task_of(plug::UiHandle panel) {
    const auto it = task_shells().find(panel);
    return it == task_shells().end() ? nullptr : it->second;
}

// 门面是在哪个线程装的，就以那个线程为"UI 线程"（`on_ui_thread` 的判据）
std::thread::id& ui_thread_id() {
    static std::thread::id instance;
    return instance;
}

stk_widget* wrap(QWidget* widget) { return reinterpret_cast<stk_widget*>(widget); }
QWidget* unwrap(plug::UiHandle handle) { return reinterpret_cast<QWidget*>(handle); }
plug::UiHandle handle_of(stk_widget* widget) { return reinterpret_cast<plug::UiHandle>(widget); }

// 当前面板根（引擎层记的）：`parent == nullptr` 时落在这里（见 ui_facade.h 口径 ③）
QWidget* resolve_parent(plug::UiHandle parent) {
    QWidget* widget = unwrap(parent);
    if (widget != nullptr) return widget;
    return unwrap(plug::current_panel_root());
}

// ★ L1 的"装配"承诺：**宿主负责排版**。插件只描述"要一个 label"，不该被要求自己 pack ——
//   所以造出来的控件会自动按宿主规范落进父容器的**纵向布局**（没有就建一个，边距/间距由宿主定）。
//   显式 `pack()` 仍然可用（那时控件只是被移到调用者指定的位置/顺序）。
void place(QWidget* parent, QWidget* child) {
    if (parent == nullptr || child == nullptr) return;
    auto* layout = qobject_cast<QVBoxLayout*>(parent->layout());
    if (layout == nullptr) {
        layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(core::px(6));
    }
    layout->addWidget(child);
}

// ── 回调盒（保活 + 把 std::function 变成 C ABI 要的"裸函数指针 + user"）────────
struct ClickBox {
    std::function<void()> fn;
};
struct ToggleBox {
    std::function<void(bool)> fn;
};
struct ChangeBox {
    std::function<void(const std::string&)> fn;
};

template <typename T>
T* keep(std::unique_ptr<T> box) {
    // ★ 进程级保活（故意不释放，见文件头）；容器本身是静态的，避免静态析构顺序问题。
    static std::vector<std::unique_ptr<T>>* kept = new std::vector<std::unique_ptr<T>>();
    kept->push_back(std::move(box));
    return kept->back().get();
}

void on_click_trampoline(void* user) {
    auto* box = static_cast<ClickBox*>(user);
    if (box != nullptr && box->fn) box->fn();
}

void on_toggle_trampoline(void* user, const char* value) {
    auto* box = static_cast<ToggleBox*>(user);
    if (box != nullptr && box->fn) box->fn(value != nullptr && std::string(value) == "1");
}

void on_change_trampoline(void* user, const char* value) {
    auto* box = static_cast<ChangeBox*>(user);
    if (box != nullptr && box->fn) box->fn(value == nullptr ? std::string() : std::string(value));
}

void on_file_trampoline(void* user, const char* path) {
    auto* box = static_cast<ChangeBox*>(user);
    if (box != nullptr && box->fn) box->fn(path == nullptr ? std::string() : std::string(path));
}

// ── 18 个方法（名字与 ui_facade.h 的字段一一对应；顺序也必须一致）──────────────
const plug::UiFacade kQtFacade = {
    /* label */
    [](plug::UiHandle parent, const std::string& text, const std::string& style) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created = table()->label(nullptr, wrap(host), text.c_str(), style.c_str());
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* button */
    [](plug::UiHandle parent, const std::string& text, std::function<void()> on_click, int width,
       const std::string& style, bool enabled) {
        QWidget* host = resolve_parent(parent);
        auto* box = keep(std::make_unique<ClickBox>(ClickBox{std::move(on_click)}));
        stk_widget* created = table()->button(nullptr, wrap(host), text.c_str(),
                                              on_click_trampoline, box, style.c_str(),
                                              enabled ? 1 : 0);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* row */
    [](plug::UiHandle parent, const std::string& side, const std::string& fill, bool expand,
       int padx, int pady) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created = table()->row(nullptr, wrap(host), side.c_str(), fill.c_str(),
                                           expand ? 1 : 0, padx, pady);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* checkbox */
    [](plug::UiHandle parent, const std::string& text, std::function<void(bool)> on_toggle) {
        QWidget* host = resolve_parent(parent);
        auto* box = keep(std::make_unique<ToggleBox>(ToggleBox{std::move(on_toggle)}));
        stk_widget* created = table()->checkbox(nullptr, wrap(host), text.c_str(),
                                                on_toggle_trampoline, box);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* entry */
    [](plug::UiHandle parent, const std::string& initial, int width, bool password) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created = table()->entry(nullptr, wrap(host), initial.c_str(), width,
                                             password ? "1" : nullptr);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* text */
    [](plug::UiHandle parent, int height, int width, const std::string& wrap_mode) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created =
            table()->text(nullptr, wrap(host), height, width, wrap_mode.c_str());
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* combo */
    [](plug::UiHandle parent, const std::vector<std::string>& values,
       std::function<void(const std::string&)> on_change) {
        QWidget* host = resolve_parent(parent);
        std::vector<const char*> raw;
        raw.reserve(values.size());
        for (const std::string& value : values) raw.push_back(value.c_str());
        auto* box = keep(std::make_unique<ChangeBox>(ChangeBox{std::move(on_change)}));
        stk_widget* created =
            table()->combo(nullptr, wrap(host), raw.empty() ? nullptr : raw.data(),
                           static_cast<int>(raw.size()), on_change_trampoline, box);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* drop_box */
    [](plug::UiHandle parent, std::function<void(const std::string&)> on_file,
       const std::string& placeholder) {
        QWidget* host = resolve_parent(parent);
        auto* box = keep(std::make_unique<ChangeBox>(ChangeBox{std::move(on_file)}));
        stk_widget* created = table()->drop_box(nullptr, wrap(host), on_file_trampoline, box,
                                                placeholder.c_str(), 0);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* pack */
    [](plug::UiHandle parent, plug::UiHandle child, const std::string& side,
       const std::string& fill, bool expand, int padx, int pady) {
        table()->pack(nullptr, wrap(resolve_parent(parent)), wrap(unwrap(child)), side.c_str(),
                      fill.c_str(), expand ? 1 : 0, padx, pady);
    },
    /* text_view */
    [](plug::UiHandle parent, int height, bool mono) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created = table()->text_view(nullptr, wrap(host), height, mono ? 1 : 0);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* set_text */
    [](plug::UiHandle widget, const std::string& text) {
        table()->set_text(nullptr, wrap(unwrap(widget)), text.c_str());
    },
    /* get_text */
    [](plug::UiHandle widget) { return qt_plugin_widget_text(wrap(unwrap(widget))); },
    /* set_enabled */
    [](plug::UiHandle widget, bool enabled) {
        table()->set_state(nullptr, wrap(unwrap(widget)), enabled ? 1 : 0);
    },
    /* alive */
    [](plug::UiHandle widget) { return unwrap(widget) != nullptr; },
    /* log_append：宿主日志栏。装配时由外壳接上（`install_qt_ui_facade` 的 log_sink），
       这样引擎层不必认识 Hub。 */
    nullptr,
    /* ask_yes_no */
    [](const std::string& title, const std::string& message) {
        return table()->ask_yes_no(nullptr, title.c_str(), message.c_str()) != 0;
    },
    /* ask_open_file */
    [](const std::string& title) {
        const char* path = table()->ask_open_file(nullptr, title.c_str());
        return path == nullptr ? std::string() : std::string(path);
    },
    /* ask_save_file */
    [](const std::string& title) {
        const char* path = table()->ask_save_file(nullptr, title.c_str());
        return path == nullptr ? std::string() : std::string(path);
    },
    // ── ★★ P4：重型 L2 第一批（虚拟列表 / 日志视图 / 拖放目标 / 任务壳 / 线程判定）──
    // 为什么这几件**可以**在门面实现里直接碰 Qt（而 L1 那 18 个不行）：L1 是对既有控件语义的
    // 转发（唯一实现必须是 `plugin_ui.cpp`）；这里几件是**新能力**（行列表 / 日志视图 / 拖放区 /
    // 任务壳的表内方法），宿主此前没有对外入口 ⇒ 实现在外壳这一层，且只碰宿主自己的组件。
    /* list：容器 + `VirtualList` 子件（**单列文本**；多列/三态勾选/行尾✕ 随 TreeView 批）。
       句柄＝容器本身（`pack` 用得上），列表用 objectName 找回来。 */
    [](plug::UiHandle parent, int row_height) {
        QWidget* host = resolve_parent(parent);
        auto* container = new QWidget;
        auto* view = new VirtualList(container);
        if (row_height > 0) view->set_row_height(row_height);
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(view);
        place(host, container);
        return handle_of(wrap(container));
    },
    /* list_set_row_count */
    [](plug::UiHandle list, int rows) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return;
        if (auto* view = container->findChild<VirtualList*>()) {
            view->set_row_count(rows);
            view->refresh();
        }
    },
    /* list_set_row_text（★L2.5：宿主只在**可见行**上问插件要文本 —— 10.3 同族） */
    [](plug::UiHandle list, std::function<std::string(int)> provider) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return;
        auto* view = container->findChild<VirtualList*>();
        if (view == nullptr) return;
        if (!provider) {
            view->set_row_text_provider(nullptr);
            return;
        }
        view->set_row_text_provider(
            [provider](int row) { return QString::fromStdString(provider(row)); });
    },
    /* list_row_text（读回：判据用；不改变池子与选中状态） */
    [](plug::UiHandle list, int row) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return std::string();
        auto* view = container->findChild<VirtualList*>();
        return view == nullptr ? std::string() : view->row_text(row).toStdString();
    },
    /* list_select_row */
    [](plug::UiHandle list, int row) {
        QWidget* container = unwrap(list);
        if (container != nullptr) {
            if (auto* view = container->findChild<VirtualList*>()) view->set_selected_row(row);
        }
    },
    /* list_selected_row */
    [](plug::UiHandle list) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return -1;
        auto* view = container->findChild<VirtualList*>();
        return view == nullptr ? -1 : view->selected_row();
    },
    /* list_content_height：内容总高（＝行数×行高）—— 宿主页/视口据此撑开滚动范围 */
    [](plug::UiHandle list) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return 0;
        auto* view = container->findChild<VirtualList*>();
        return view == nullptr ? 0 : view->content_height();
    },
    /* log_view：只读多行（复用 `plugin_ui.cpp` 的 text_view），行式追加在下面 */
    [](plug::UiHandle parent, int height) {
        QWidget* host = resolve_parent(parent);
        stk_widget* created = table()->text_view(nullptr, wrap(host), height, 1);
        if (auto* view = qobject_cast<QPlainTextEdit*>(unwrap(created))) view->setReadOnly(true);
        place(host, unwrap(created));
        return handle_of(created);
    },
    /* log_view_append：**一次一行**追加（不重写全文 ⇒ 不 O(n²)、不破坏选中/滚动位置） */
    [](plug::UiHandle view, const std::string& line) {
        if (auto* edit = qobject_cast<QPlainTextEdit*>(unwrap(view))) {
            edit->appendPlainText(QString::fromStdString(line));
        }
    },
    /* log_view_clear */
    [](plug::UiHandle view) {
        if (auto* edit = qobject_cast<QPlainTextEdit*>(unwrap(view))) edit->clear();
    },
    /* log_view_text */
    [](plug::UiHandle view) {
        auto* edit = qobject_cast<QPlainTextEdit*>(unwrap(view));
        return edit == nullptr ? std::string() : edit->toPlainText().toStdString();
    },
    /* drop_zone：容器 + 提示行 + 宿主拖放过滤器（**本地文件才接**，其余给禁止光标） */
    [](plug::UiHandle parent, const std::string& hint) {
        QWidget* host = resolve_parent(parent);
        auto* container = new QWidget;
        container->setObjectName(QStringLiteral("xrkit_facade_drop_zone"));
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* label = new QLabel(QString::fromStdString(hint), container);
        label->setWordWrap(true);
        layout->addWidget(label);
        auto* box = new DropBox;   // 进程级保活（与控件同寿，见文件头那条纪律）
        // ★ 过滤器**挂在容器上**（父子关系＝生命周期）：容器没了过滤器就没了，
        //   自检也能顺着控件树找到它去派发真事件（`findChild<FileDropFilter*>`）。
        box->filter = new FileDropFilter(
            [box](const QStringList& paths) {
                if (box->handler == nullptr) return;
                std::vector<std::string> out;
                out.reserve(static_cast<size_t>(paths.size()));
                for (const QString& path : paths) out.push_back(path.toStdString());
                box->handler(out);
            },
            container);
        box->filter->attach(container);
        drop_boxes()[reinterpret_cast<void*>(container)] = box;
        place(host, container);
        return handle_of(wrap(container));
    },
    /* drop_zone_on_drop */
    [](plug::UiHandle zone, std::function<void(const std::vector<std::string>&)> on_drop) {
        auto it = drop_boxes().find(zone);
        if (it == drop_boxes().end() || it->second == nullptr) return;
        it->second->handler = std::move(on_drop);
    },
    /* task_panel：宿主任务壳（进度 / 消息 / 失败 / 取消 / 状态栏） */
    [](plug::UiHandle parent, bool with_cancel) {
        QWidget* host = resolve_parent(parent);
        auto* shell = new TaskShell(
            host, with_cancel ? TaskShell::CancelButton::Yes : TaskShell::CancelButton::No);
        place(host, shell->panel());
        task_shells()[reinterpret_cast<void*>(shell->panel())] = shell;
        return handle_of(wrap(shell->panel()));
    },
    /* task_run：worker 在**工作线程**里跑，收到两个 sink（progress 0~100 / message） */
    [](plug::UiHandle panel, const std::string& title,
       std::function<void(plug::UiFacade::TaskProgress, plug::UiFacade::TaskMessage)> worker) {
        TaskShell* shell = task_of(panel);
        if (shell == nullptr || !worker) return false;
        return shell->run(QString::fromStdString(title),
                          [worker](const ProgressSink& progress, const MessageSink& message) {
                              worker([progress](int percent) { progress(percent); },
                                     [message](const std::string& text) { message(text); });
                          });
    },
    /* task_status */
    [](plug::UiHandle panel) {
        TaskShell* shell = task_of(panel);
        return shell == nullptr ? std::string() : shell->status().toStdString();
    },
    /* task_cancel */
    [](plug::UiHandle panel) {
        if (TaskShell* shell = task_of(panel)) {
            shell->request_cancel(QStringLiteral("插件请求取消"));
        }
    },
    /* task_cancel_requested */
    [](plug::UiHandle panel) {
        TaskShell* shell = task_of(panel);
        return shell != nullptr && shell->cancel_requested();
    },
    /* on_ui_thread：门面是在哪个线程装的，就以那个线程为"UI 线程" */
    []() { return std::this_thread::get_id() == ui_thread_id(); },
};

plug::UiFacade& mutable_facade() {
    // 门面表本身是常量，只有 `log_append` 需要由外壳在装配时补上（它依赖 Hub）
    static plug::UiFacade instance = kQtFacade;
    return instance;
}

}  // namespace

// 外壳装配时调一次：装门面，并把"写宿主日志栏"这条接上（`sink` 可空 ⇒ 该条不可用、调用侧记账）
void install_qt_ui_facade(std::function<void(const std::string&)> log_sink) {
    // ★ "UI 线程"＝装门面的那个线程（外壳在 GUI 主线程装配；工作线程里的 `api.ui.*` 会被挡住）
    ui_thread_id() = std::this_thread::get_id();
    mutable_facade().log_append = std::move(log_sink);
    plug::set_ui_facade(&mutable_facade());
}

}  // namespace xrkit::shell
