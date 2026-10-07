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

#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/widget_facade.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

namespace xrkit::shell {
namespace {

namespace plug = xrkit::plugin;

const stk_ui* table() { return qt_plugin_ui(); }

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
};

plug::UiFacade& mutable_facade() {
    // 门面表本身是常量，只有 `log_append` 需要由外壳在装配时补上（它依赖 Hub）
    static plug::UiFacade instance = kQtFacade;
    return instance;
}

}  // namespace

// 外壳装配时调一次：装门面，并把"写宿主日志栏"这条接上（`sink` 可空 ⇒ 该条不可用、调用侧记账）
void install_qt_ui_facade(std::function<void(const std::string&)> log_sink) {
    mutable_facade().log_append = std::move(log_sink);
    plug::set_ui_facade(&mutable_facade());
}

}  // namespace xrkit::shell
