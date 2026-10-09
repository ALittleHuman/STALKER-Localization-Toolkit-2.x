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
#include "drop_zone.h"
#include "facade_tree.h"
#include "plugin_context_menu.h"
#include "scroll_viewport.h"
#include "task_shell.h"
#include "virtual_list.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/widget_facade.h"

#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QUrl>

#include "font_picker.h"
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
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
// 任务壳消息回调盒（P12c-2）：与控件同寿（同 L1 回调盒的纪律）
// 右键菜单回调盒（P13-2a）：与控件同寿（同 L1 回调盒的纪律）
struct MenuBox {
    std::function<void(const std::string&)> fn;
};

struct MessageBox {
    std::function<void(const std::string&)> fn;
};

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

// ★ P5：画布 → 视口（句柄是画布，视口是它的宿主；单线程访问）
std::map<void*, ScrollViewport*>& viewports() {
    static std::map<void*, ScrollViewport*>* instance = new std::map<void*, ScrollViewport*>();
    return *instance;
}

ScrollViewport* viewport_of(plug::UiHandle body) {
    const auto it = viewports().find(body);
    return it == viewports().end() ? nullptr : it->second;
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
//
// ★★ P16-2c：**"标签 + 输入框"要成组为"字段行"**（与内置六页**同一条**机制：
//   `DropZones::attach_row()` 打 `field_row` 名字 + 同一套主题规则 + 拖文件落进本行输入框）。
//   修前门面只是**逐个控件纵排** ⇒ 插件页根本没有"字段行"容器，三个后果：
//     ① 用户面：内置页可以把文件拖到"标签+输入框"那一行上，插件页不行（观感/能力不对等）；
//     ② 主题面：`QWidget#field_row { background: transparent; }` 那条规则对插件页永远不生效；
//     ③ 判据面：包内自检「⑭ 标签背后不许有深灰底板」扫 `field_row` ⇒ 候选集为 0 ⇒ 红
//        （它**自带"候选集空就红"的自证**，所以不许靠删自证变绿）。
//   ⇒ 正确的修法是**宿主把行容器真的给插件页**（这里），**不是**改判据。
bool is_field_input(QWidget* widget) {
    // 与 `DropZones::attach_row` 的处理器同一类：只认"能装一行文本路径"的输入框
    return qobject_cast<QLineEdit*>(widget) != nullptr;
}

void place(QWidget* parent, QWidget* child) {
    if (parent == nullptr || child == nullptr) return;
    auto* layout = qobject_cast<QVBoxLayout*>(parent->layout());
    if (layout == nullptr) {
        layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(core::px(6));
    }
    // 上一件是标签、这一件是输入框 ⇒ 两件成组为一行（顺序不变：标签在左、输入框吃剩余宽度）
    if (layout->count() > 0) {
        QLayoutItem* last = layout->itemAt(layout->count() - 1);
        QWidget* previous = last != nullptr ? last->widget() : nullptr;
        if (previous != nullptr && qobject_cast<QLabel*>(previous) != nullptr &&
            is_field_input(child) && previous->objectName() != QStringLiteral("field_row")) {
            auto* row = new QWidget;
            auto* row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            row_layout->setSpacing(core::px(6));
            layout->removeWidget(previous);
            row_layout->addWidget(previous);
            row_layout->addWidget(child, 1);
            DropZones zones;
            zones.attach_row(row, parent);
            layout->addWidget(row);
            return;
        }
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
    /* entry_on_change：输入变化 ⇒ 回调当前文本（换行不是问题，单行控件） */
    [](plug::UiHandle widget, std::function<void(const std::string&)> on_change) {
        auto* edit = qobject_cast<QLineEdit*>(unwrap(widget));
        if (edit == nullptr) return;
        // ★ 回调盒与控件同寿（同本文件口径）：`new` 出去不删，靠 Qt 对象树收尾
        auto* box = new std::function<void(const std::string&)>(std::move(on_change));
        QObject::connect(edit, &QLineEdit::textChanged, edit,
                         [box](const QString& text) {
                             if (box != nullptr && *box) (*box)(text.toStdString());
                         });
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
    /* progress：进度条（0–100） */
    [](plug::UiHandle parent) {
        QWidget* host = resolve_parent(parent);
        auto* bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(0);
        place(host, bar);
        return handle_of(wrap(bar));
    },
    /* set_progress：设值 —— ★**门面自己夹到 0–100**。
       （判据现场：`QProgressBar::setValue` 对越界值**不改也不夹**（留旧值），与
        `QAbstractSlider` 的行为不同 ⇒ 口径不能"托付给 Qt 版本"，必须我们自己定死。） */
    [](plug::UiHandle widget, int percent) {
        if (auto* bar = qobject_cast<QProgressBar*>(unwrap(widget))) {
            bar->setValue(std::clamp(percent, 0, 100));
        }
    },
    /* progress_value：读回（判据用） */
    [](plug::UiHandle widget) -> int {
        if (auto* bar = qobject_cast<QProgressBar*>(unwrap(widget))) return bar->value();
        return 0;
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
    /* combo_value：下拉当前文本（读接口，不走"改"那条路） */
    [](plug::UiHandle widget) {
        if (auto* combo = qobject_cast<QComboBox*>(unwrap(widget))) {
            return combo->currentText().toStdString();
        }
        return std::string();
    },
    /* checkbox_state：勾选框当前勾选态 */
    [](plug::UiHandle widget) {
        if (auto* check = qobject_cast<QCheckBox*>(unwrap(widget))) return check->isChecked();
        return false;
    },
    /* set_checked：写勾选态（P9b；convert 页要用"输出目录非空 ⇒ 取消勾选「备份」"） */
    [](plug::UiHandle widget, bool checked) {
        if (auto* check = qobject_cast<QCheckBox*>(unwrap(widget))) check->setChecked(checked);
    },
    /* combo_set_items：重建下拉项（第 0 项成为当前项；不触发用户回调） */
    [](plug::UiHandle widget, const std::vector<std::string>& values) {
        if (auto* combo = qobject_cast<QComboBox*>(unwrap(widget))) {
            const QSignalBlocker blocker(combo);   // 换项不是用户操作 ⇒ 不回调
            combo->clear();
            for (const std::string& value : values) {
                combo->addItem(QString::fromStdString(value));
            }
            if (combo->count() > 0) combo->setCurrentIndex(0);
        }
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
    /* ask_directory：门面**新能力**（C ABI 那张表没有目录选择）—— 宿主弹自己的目录对话框 */
    [](const std::string& title) {
        const QString picked =
            QFileDialog::getExistingDirectory(nullptr, QString::fromStdString(title));
        return picked.toStdString();
    },
    /* pick_font：**字库选择器**（宿主唯一一份 `font_picker.cpp`）—— 非模态 + 回调 */
    [](const std::string& current, std::function<void(const std::string&)> on_picked) {
        QDialog* dialog = xrkit::shell::make_font_picker(
            nullptr, QString::fromStdString(current), nullptr,
            [on_picked](const QString& path) {
                if (on_picked) on_picked(path.toStdString());
            });
        if (dialog != nullptr) dialog->show();
    },
    /* open_path：在文件管理器里打开一个路径（宿主实现；C++ 页走 QDesktopServices） */
    [](const std::string& path) {
        if (path.empty()) return false;
        return QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(path)));
    },
    /* context_menu：宿主渲染右键菜单（自定义菜单策略 + `exec`），选中回调给标签原文 */
    [](plug::UiHandle widget, const std::vector<std::string>& labels,
       std::function<void(const std::string&)> on_choose) {
        QWidget* target = unwrap(widget);
        if (target == nullptr) return;
        target->setContextMenuPolicy(Qt::CustomContextMenu);
        auto* box = keep(std::make_unique<MenuBox>(MenuBox{std::move(on_choose)}));
        QObject::connect(target, &QWidget::customContextMenuRequested, target,
                         [target, labels, box](const QPoint& pos) {
                             QMenu menu(target);
                             for (const std::string& label : labels) {
                                 QAction* action = menu.addAction(QString::fromStdString(label));
                                 // ★ 接 **action 自己的 `triggered`**（不是 `exec()` 的返回值）：
                                 //   `exec()` 只在"用户点菜单项"那条路径上返回被选项，程序里
                                 //   `trigger()` 或 `close()` 都会让它返回 nullptr ⇒ 用返回值接回调
                                 //   会**静默丢事件**（判据现场就是这样空的）。
                                 QObject::connect(action, &QAction::triggered, target,
                                                  [box, label] {
                                                      if (box->fn) box->fn(label);
                                                  });
                             }
                             // ★ P13-2a-2：把「其它插件注册的右键命令」追加进来
                             //   （与内置六页**共用** `append_registry_context_items`；
                             //    `when` 用**当次**的 context 判定 —— 插件页目前给空 context，
                             //    因为「页面实时 context」是内置六页才有的东西，如实登记）
                             append_registry_context_items(
                                 &menu, plug::current_page_registry(),
                                 plug::current_page_id(), xrkit::plugin::Context(), target);
                             menu.exec(target->mapToGlobal(pos));   // 点空白关掉 ⇒ 无回调
                         });
    },
    /* copy_text：写系统剪贴板（xml 页「复制 ID」；与 C++ 页同一份实现） */
    [](const std::string& text) {
        if (QClipboard* board = QApplication::clipboard()) {
            board->setText(QString::fromStdString(text));
        }
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
        auto* view = container->findChild<VirtualList*>();
        if (view == nullptr) return;
        view->set_row_count(rows);
        view->refresh();
        // ★★ P5：列表变长了 ⇒ 把"内容总高"交给**祖先里的宿主视口**（3.3：内容高不许用请求高度顶）。
        //   这一步让"插件页里放一个长列表"**自动滚得动**，插件一句都不用写。
        for (QWidget* ancestor = container->parentWidget(); ancestor != nullptr;
             ancestor = ancestor->parentWidget()) {
            if (auto* viewport = qobject_cast<ScrollViewport*>(ancestor)) {
                viewport->set_min_body_height(view->content_height());
                viewport->sync_layout();
                break;
            }
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
    /* list_on_activate：列表激活（双击 / 回车）—— fs 页「双击包列表＝加载」 */
    [](plug::UiHandle list, std::function<void(int)> on_activate) {
        QWidget* container = unwrap(list);
        if (container == nullptr) return;
        if (auto* view = container->findChild<VirtualList*>()) {
            view->set_on_activate(std::move(on_activate));
        }
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
    /* task_on_message：**回主线程的消息回调**透给插件。
       ★ 壳保证：worker 里 `message(...)` 之后**排队回主线程**再调它 ⇒ 回调里可以碰控件。
       这是插件页"干完活刷新界面"的合法通道（worker 里碰控件违反契约 —— xml 页初版就踩过）。 */
    [](plug::UiHandle panel, std::function<void(const std::string&)> handler) {
        TaskShell* shell = task_of(panel);
        if (shell == nullptr) return;
        auto* box = keep(std::make_unique<MessageBox>(MessageBox{std::move(handler)}));
        shell->set_message_handler([box](const QString& text) {
            if (box->fn) box->fn(text.toStdString());
        });
    },
    /* on_ui_thread：门面是在哪个线程装的，就以那个线程为"UI 线程" */
    []() { return std::this_thread::get_id() == ui_thread_id(); },
    // ── ★★ P5：重型 L2 第二批（滚动视口 / 可分栏）──────────────────────────────
    /* viewport：宿主造「容器 + ScrollViewport + 画布」，**句柄＝画布**（插件往它里面 pack）。
       条子只由宿主造（口径 1.6）；`min_body_height` 由调用方给（口径 3.3）。 */
    [](plug::UiHandle parent, int min_body_height) {
        QWidget* host = resolve_parent(parent);
        auto* container = new QWidget;
        auto* layout = new QVBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* viewport = new ScrollViewport(container);
        auto* body = new QWidget;
        viewport->set_body(body);
        viewport->set_fit(ScrollViewport::Fit::Content);
        if (min_body_height > 0) viewport->set_min_body_height(min_body_height);
        auto* body_layout = new QVBoxLayout(body);
        body_layout->setContentsMargins(0, 0, 0, 0);
        body_layout->setSpacing(core::px(6));
        layout->addWidget(viewport);
        viewports()[reinterpret_cast<void*>(body)] = viewport;
        place(host, container);
        return handle_of(wrap(body));
    },
    /* viewport_fit_content：true = Content（静态内容，1.x 3.2） */
    [](plug::UiHandle body, bool content_fit) {
        if (ScrollViewport* viewport = viewport_of(body)) {
            viewport->set_fit(content_fit ? ScrollViewport::Fit::Content
                                          : ScrollViewport::Fit::Viewport);
            viewport->sync_layout();
        }
    },
    /* viewport_set_min_body_height */
    [](plug::UiHandle body, int min_body_height) {
        if (ScrollViewport* viewport = viewport_of(body)) {
            viewport->set_min_body_height(min_body_height);
            viewport->sync_layout();
        }
    },
    /* viewport_sync：任何尺寸/内容变化后调一次（1.8 显式整组重排） */
    [](plug::UiHandle body) {
        if (ScrollViewport* viewport = viewport_of(body)) viewport->sync_layout();
    },
    /* viewport_can_scroll */
    [](plug::UiHandle body) {
        ScrollViewport* viewport = viewport_of(body);
        return viewport != nullptr && viewport->can_scroll_vertical();
    },
    /* viewport_scroll_top */
    [](plug::UiHandle body) {
        ScrollViewport* viewport = viewport_of(body);
        return viewport == nullptr ? 0 : viewport->offset_vertical();
    },
    // ── ★★ P5：可分栏：**`QSplitter`（产品真实用的那个）** ──────────────────────
    // ★ 这一处被门禁 3-8 当场纠正过：我第一版用了 `SplitPane`，而那个类**只作自检夹具**保留
    //   （门禁锁着：产品路径不许引用它），产品分栏用的是 `QSplitter`（fs 页 `fs_split_list_tree`）。
    //   口径就此对齐：**门面必须暴"页面在用的那个控件"的能力**，否则"对等性"是假的。
    //   · `setOpaqueResize(false)` ＝ 用户 2026-09-21 要的"拖动期不实时移动、松手一次重排"；
    //   · 每格上限/是否可收起/收起态都落在**格容器**上（QSplitter 按子控件的 min/max 分配）。
    /* split：造一个可分栏（面板）容器 */
    [](plug::UiHandle parent, bool horizontal) {
        QWidget* host = resolve_parent(parent);
        auto* split = new QSplitter(horizontal ? Qt::Horizontal : Qt::Vertical);
        split->setObjectName(QStringLiteral("xrkit_facade_split"));
        split->setChildrenCollapsible(false);   // 收起要么显式做，要么不做（不许拖成 0）
        split->setOpaqueResize(false);          // 松手一次重排（拖的时候不实时改几何）
        place(host, split);
        return handle_of(wrap(split));
    },
    /* split_add_pane：造**新的一格容器**返回给插件（插件往它里面 pack） */
    [](plug::UiHandle split, int minsize) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        if (splitter == nullptr) return plug::kNoWidget;
        auto* cell = new QWidget;
        auto* layout = new QVBoxLayout(cell);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(core::px(6));
        if (minsize > 0) {
            if (splitter->orientation() == Qt::Horizontal) {
                cell->setMinimumWidth(minsize);
            } else {
                cell->setMinimumHeight(minsize);
            }
        }
        splitter->addWidget(cell);
        return handle_of(wrap(cell));
    },
    /* split_set_max_size：每格上限（size<=0 = 不限）——落在格容器的 max 尺寸上 */
    [](plug::UiHandle split, int index, int max_size) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        if (splitter == nullptr || index < 0 || index >= splitter->count()) return;
        QWidget* cell = splitter->widget(index);
        if (cell == nullptr) return;
        if (splitter->orientation() == Qt::Horizontal) {
            cell->setMaximumWidth(max_size > 0 ? max_size : QWIDGETSIZE_MAX);
        } else {
            cell->setMaximumHeight(max_size > 0 ? max_size : QWIDGETSIZE_MAX);
        }
    },
    /* split_set_collapsed：完全收起 —— **hide 掉那一格**。这是 QSplitter 的正法：隐藏的子件
       被当作 0 尺寸、空间全给别的格，`show()` 时它自己还原原来的大小。
       ★ 别用"把 sizes 压到 0"：那会被 `minimumSizeHint` 顶回去（本批自检第一次就是这么红的：
       `setSizes` 之后第 3 格仍是 239 px）。 */
    [](plug::UiHandle split, int index, bool collapsed) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        if (splitter == nullptr || index < 0 || index >= splitter->count()) return;
        QWidget* cell = splitter->widget(index);
        if (cell == nullptr) return;
        cell->setVisible(!collapsed);
    },
    /* split_is_collapsed：显式隐藏（`isHidden()` 与"布局跑没跑"无关 —— 未 show 也能判） */
    [](plug::UiHandle split, int index) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        if (splitter == nullptr || index < 0 || index >= splitter->count()) return false;
        QWidget* cell = splitter->widget(index);
        return cell != nullptr && cell->isHidden();
    },
    /* split_pane_count */
    [](plug::UiHandle split) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        return splitter == nullptr ? 0 : splitter->count();
    },
    /* split_sash_pos：第 index 条分隔条的位置（-1 = 没有）。用**累计尺寸**算 —— 不依赖
       "分隔条自己有没有被布局出来"，离屏/未 show 时也能判。 */
    [](plug::UiHandle split, int index) {
        auto* splitter = qobject_cast<QSplitter*>(unwrap(split));
        if (splitter == nullptr || index < 0 || index + 1 >= splitter->count()) return -1;
        const QList<int> sizes = splitter->sizes();
        int pos = 0;
        for (int i = 0; i <= index && i < sizes.size(); ++i) pos += sizes.at(i);
        return pos;
    },
    // ── ★★ P6：树（多列 / 懒建 / 展开态 / 过滤计数 / 三态勾选）────────────────────
    // 语义全在 `facade_tree.{h,cpp}`（宿主唯一的实现），这里只做句柄与参数转换。
    /* tabs：标签页容器（fs 页「解包 / 封包」两标签） */
    [](plug::UiHandle parent) {
        QWidget* host = resolve_parent(parent);
        auto* tabs = new QTabWidget;
        tabs->setObjectName(QStringLiteral("xrkit_facade_tabs"));
        place(host, tabs);
        return handle_of(wrap(tabs));
    },
    /* tabs_add_page：加一页 ⇒ 返回**该页容器**（插件往它里面 pack） */
    [](plug::UiHandle tabs, const std::string& title) {
        auto* widget = qobject_cast<QTabWidget*>(unwrap(tabs));
        if (widget == nullptr) return plug::kNoWidget;
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(core::px(6));
        widget->addTab(page, QString::fromStdString(title));
        return handle_of(wrap(page));
    },
    /* tabs_page_count */
    [](plug::UiHandle tabs) {
        auto* widget = qobject_cast<QTabWidget*>(unwrap(tabs));
        return widget == nullptr ? 0 : widget->count();
    },
    /* tabs_set_current：切到第 index 页（越界不动） */
    [](plug::UiHandle tabs, int index) {
        auto* widget = qobject_cast<QTabWidget*>(unwrap(tabs));
        if (widget != nullptr && index >= 0 && index < widget->count()) {
            widget->setCurrentIndex(index);
        }
    },
    /* tabs_current_index：当前页下标（-1 = 没有） */
    [](plug::UiHandle tabs) {
        auto* widget = qobject_cast<QTabWidget*>(unwrap(tabs));
        return widget == nullptr ? -1 : widget->currentIndex();
    },
    /* tree */
    [](plug::UiHandle parent, const std::vector<plug::UiFacade::TreeColumn>& columns) {
        QWidget* host = resolve_parent(parent);
        auto* tree = new FacadeTree;
        tree->set_columns(columns);
        place(host, tree);
        return handle_of(wrap(tree));
    },
    /* tree_set_columns：换列声明（表头 + 列宽）—— 与 `FacadeTree` 同一份列策略 */
    [](plug::UiHandle tree, const std::vector<plug::UiFacade::TreeColumn>& columns) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_columns(columns);
        }
    },
    /* tree_set_root */
    [](plug::UiHandle tree, const std::vector<plug::UiFacade::TreeNodeSpec>& nodes) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) facade->set_root(nodes);
    },
    /* tree_set_children */
    [](plug::UiHandle tree, const std::string& parent_key,
       const std::vector<plug::UiFacade::TreeNodeSpec>& nodes) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_children(parent_key, nodes);
        }
    },
    /* tree_on_action：行尾动作（fs 页的 ✕）—— 与 fs 页**共用**一份委托 */
    [](plug::UiHandle tree, const std::string& mark,
       std::function<void(const std::string&)> on_action) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_action_mark(mark);
            facade->set_on_action(std::move(on_action));
        }
    },
    /* tree_clear */
    [](plug::UiHandle tree) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) facade->clear_all();
    },
    /* tree_row_count */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? 0 : facade->visible_row_count();
    },
    /* tree_keys */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::vector<std::string>() : facade->visible_keys();
    },
    /* tree_set_expanded */
    [](plug::UiHandle tree, const std::string& key, bool expanded) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_expanded(key, expanded);
        }
    },
    /* tree_expanded_keys */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::vector<std::string>() : facade->expanded_keys();
    },
    /* tree_set_checked */
    [](plug::UiHandle tree, const std::string& key, bool checked) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) facade->set_checked(key, checked);
    },
    /* tree_checked */
    [](plug::UiHandle tree, const std::string& key) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade != nullptr && facade->checked_of(key);
    },
    /* tree_checked_keys */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::vector<std::string>() : facade->checked_keys();
    },
    /* tree_partial_keys */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::vector<std::string>() : facade->partial_keys();
    },
    /* tree_set_filter */
    [](plug::UiHandle tree, const std::string& text) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) facade->set_filter(text);
    },
    /* tree_filter_hits */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? 0 : facade->filter_hits();
    },
    /* tree_select */
    [](plug::UiHandle tree, const std::string& key) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) facade->select_key(key);
    },
    /* tree_selected_key */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::string() : facade->selected_key();
    },
    /* tree_selected_keys：当前选中的**一组** key（按树序） */
    [](plug::UiHandle tree) {
        auto* facade = qobject_cast<FacadeTree*>(unwrap(tree));
        return facade == nullptr ? std::vector<std::string>() : facade->selected_keys();
    },
    /* tree_on_expand */
    [](plug::UiHandle tree, std::function<void(const std::string&)> on_expand) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_on_expand(std::move(on_expand));
        }
    },
    /* tree_on_check */
    [](plug::UiHandle tree, std::function<void(const std::string&, bool)> on_check) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_on_check(std::move(on_check));
        }
    },
    /* tree_on_activate */
    [](plug::UiHandle tree, std::function<void(const std::string&)> on_activate) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_on_activate(std::move(on_activate));
        }
    },
    /* tree_on_click：点行身体（宿主只报 key） */
    [](plug::UiHandle tree, std::function<void(const std::string&)> on_click) {
        if (auto* facade = qobject_cast<FacadeTree*>(unwrap(tree))) {
            facade->set_on_click(std::move(on_click));
        }
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
    // ★ "UI 线程"＝装门面的那个线程（外壳在 GUI 主线程装配；工作线程里的 `api.ui.*` 会被挡住）
    ui_thread_id() = std::this_thread::get_id();
    mutable_facade().log_append = std::move(log_sink);
    plug::set_ui_facade(&mutable_facade());
}

}  // namespace xrkit::shell
