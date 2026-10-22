#include "plugin_ui.h"

#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>
#include <QCoreApplication>
#include <cstdlib>

#include "xrkit/core/theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace xrkit::shell {
namespace {

// `stk_widget*` 就是 `QWidget*` —— 插件永远不 deref，只把它传回宿主
stk_widget* wrap(QWidget* widget) { return reinterpret_cast<stk_widget*>(widget); }
QWidget* unwrap(stk_widget* widget) { return reinterpret_cast<QWidget*>(widget); }

QString from_utf8(const char* text) {
    return text == nullptr ? QString() : QString::fromUtf8(text);
}

// 12.2：九种"造控件"的 Qt 实现。参数名与 1.x `TkUiFactory` 一致（就是第三方契约）。
stk_widget* ui_label(stk_host*, stk_widget* parent, const char* text, const char* style) {
    auto* widget = new QLabel(from_utf8(text), unwrap(parent));
    // ★★ 2026-10-10（P17-51，接口契约批「标签换行」）：**插件标签必须能换行**。
    //   修前没有 `setWordWrap` ⇒ 长文案被**截断成一行**（1.x 的 `TkUiFactory.label` 是按宽度折行的），
    //   插件页里稍长的一句说明就看不全。`hub.cpp:58` 的提示标签早就这么做了，这里补齐同款。
    widget->setWordWrap(true);
    if (style != nullptr && *style != '\0') widget->setProperty("xrkit_style", from_utf8(style));
    return wrap(widget);
}

stk_widget* ui_button(stk_host*, stk_widget* parent, const char* text, stk_click_fn on_click,
                      void* user, const char* style, int state) {
    auto* widget = new QPushButton(from_utf8(text), unwrap(parent));
    if (style != nullptr && *style != '\0') widget->setProperty("xrkit_style", from_utf8(style));
    widget->setEnabled(state != 0);
    if (on_click != nullptr) {
        QObject::connect(widget, &QPushButton::clicked, widget,
                         [on_click, user] { on_click(user); });
    }
    return wrap(widget);
}

stk_widget* ui_row(stk_host*, stk_widget* parent, const char* side, const char*, int, int, int) {
    // 行容器：1.x 的 row() 是个 frame；Qt 侧给一个带水平布局的裸 QWidget
    auto* widget = new QWidget(unwrap(parent));
    new QHBoxLayout(widget);
    widget->setProperty("xrkit_side", from_utf8(side));
    return wrap(widget);
}

stk_widget* ui_checkbox(stk_host*, stk_widget* parent, const char* text, stk_change_fn on_toggle,
                        void* user) {
    auto* widget = new QCheckBox(from_utf8(text), unwrap(parent));
    if (on_toggle != nullptr) {
        QObject::connect(widget, &QCheckBox::toggled, widget, [on_toggle, user](bool on) {
            on_toggle(user, on ? "1" : "0");
        });
    }
    return wrap(widget);
}

stk_widget* ui_entry(stk_host*, stk_widget* parent, const char* initial, int width,
                     const char* show) {
    auto* widget = new QLineEdit(from_utf8(initial), unwrap(parent));
    if (width > 0) widget->setMaximumWidth(core::px(width));
    if (show != nullptr && *show != '\0') widget->setEchoMode(QLineEdit::Password);
    return wrap(widget);
}

stk_widget* ui_text(stk_host*, stk_widget* parent, int height, int width, const char* wrap_mode) {
    auto* widget = new QPlainTextEdit(unwrap(parent));
    if (height > 0) widget->setMinimumHeight(core::px(height) * 16);
    if (width > 0) widget->setMaximumWidth(core::px(width));
    widget->setLineWrapMode(from_utf8(wrap_mode) == QStringLiteral("none")
                                ? QPlainTextEdit::NoWrap
                                : QPlainTextEdit::WidgetWidth);
    return wrap(widget);
}

stk_widget* ui_combo(stk_host*, stk_widget* parent, const char* const* values, int count,
                     stk_change_fn on_change, void* user) {
    auto* widget = new QComboBox(unwrap(parent));
    for (int i = 0; i < count; ++i) widget->addItem(from_utf8(values[i]));
    if (on_change != nullptr) {
        QObject::connect(widget, &QComboBox::currentTextChanged, widget,
                         [on_change, user](const QString& text) {
                             on_change(user, text.toUtf8().constData());
                         });
    }
    return wrap(widget);
}

stk_widget* ui_drop_box(stk_host*, stk_widget* parent, stk_file_fn on_file, void* user,
                        const char* placeholder, int width) {
    // 只读的拖入目标框（用户口径：点"识别"才弹选文件窗口，框只显示与接收拖入）
    auto* widget = new QLineEdit(unwrap(parent));
    widget->setReadOnly(true);
    widget->setAcceptDrops(true);
    widget->setPlaceholderText(from_utf8(placeholder));
    if (width > 0) widget->setMaximumWidth(core::px(width));
    widget->setProperty("xrkit_drop_target", true);
    if (on_file != nullptr) {
        // 拖入的落地由外壳的 dragEnter/dropEvent 过滤器统一处理（见 drop_filter.h）
        widget->setProperty("xrkit_on_file", QVariant::fromValue(reinterpret_cast<quintptr>(
                                                 reinterpret_cast<void*>(on_file))));
        widget->setProperty("xrkit_on_file_user",
                            QVariant::fromValue(reinterpret_cast<quintptr>(user)));
    }
    return wrap(widget);
}

void ui_pack(stk_host*, stk_widget* parent, stk_widget* child, const char* side, const char* fill,
             int expand, int padx, int pady) {
    QWidget* host = unwrap(parent);
    QWidget* widget = unwrap(child);
    if (host == nullptr || widget == nullptr) return;
    auto* layout = qobject_cast<QVBoxLayout*>(host->layout());
    if (layout == nullptr) {
        layout = new QVBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0);
    }
    const int spacing = core::px(padx > pady ? padx : pady);
    if (spacing > 0) layout->setSpacing(spacing);
    const bool stretch = expand != 0;
    if (from_utf8(side) == QStringLiteral("top")) {
        layout->insertWidget(0, widget, stretch ? 1 : 0);
    } else {
        layout->addWidget(widget, stretch ? 1 : 0);
    }
    if (fill != nullptr && *fill != '\0') widget->setProperty("xrkit_fill", from_utf8(fill));
}

void ui_set_text(stk_host*, stk_widget* widget, const char* text) {
    QWidget* target = unwrap(widget);
    if (target == nullptr) return;
    if (auto* label = qobject_cast<QLabel*>(target)) {
        label->setText(from_utf8(text));
    } else if (auto* button = qobject_cast<QPushButton*>(target)) {
        button->setText(from_utf8(text));
    } else if (auto* edit = qobject_cast<QLineEdit*>(target)) {
        edit->setText(from_utf8(text));
    } else if (auto* view = qobject_cast<QPlainTextEdit*>(target)) {
        view->setPlainText(from_utf8(text));
    }
}

void ui_set_state(stk_host*, stk_widget* widget, int state) {
    QWidget* target = unwrap(widget);
    if (target != nullptr) target->setEnabled(state != 0);
}

stk_widget* ui_text_view(stk_host*, stk_widget* parent, int height, int mono) {
    auto* view = new QPlainTextEdit(unwrap(parent));
    view->setReadOnly(true);  // 只读多行文本视图（报告/明细都往这里写）
    if (height > 0) view->setMinimumHeight(core::px(height) * 16);
    if (mono != 0) view->setProperty("xrkit_mono", true);
    return wrap(view);
}

int ui_alive(stk_host*, stk_widget* widget) {
    // 探针口径：句柄还指向一个活着的对象（QPointer 语义的轻量版）——宿主统一记账
    return widget != nullptr && unwrap(widget) != nullptr ? 1 : 0;
}

// 三个标准对话框：**只在用户点了才会走到**（自检不跑这条路径，会阻塞）
int ui_ask_yes_no(stk_host*, const char* title, const char* message) {
    return QMessageBox::question(nullptr, from_utf8(title), from_utf8(message)) ==
                   QMessageBox::Yes
               ? 1
               : 0;
}

const char* ui_ask_open_file(stk_host*, const char* title);

const char* ui_ask_save_file(stk_host*, const char* title);

// 对话框返回值要活到宿主拷走：用一个静态缓冲（单线程 UI，够用；文档里写明）
std::string& dialog_buffer() {
    static std::string buffer;
    return buffer;
}

const char* ui_ask_open_file(stk_host*, const char* title) {
    dialog_buffer() = QFileDialog::getOpenFileName(nullptr, from_utf8(title)).toUtf8().toStdString();
    return dialog_buffer().empty() ? nullptr : dialog_buffer().c_str();
}

const char* ui_ask_save_file(stk_host*, const char* title) {
    dialog_buffer() = QFileDialog::getSaveFileName(nullptr, from_utf8(title)).toUtf8().toStdString();
    return dialog_buffer().empty() ? nullptr : dialog_buffer().c_str();
}

const stk_ui kQtUi = {
    &ui_label,        &ui_button,       &ui_row,       &ui_checkbox,
    &ui_entry,        &ui_text,         &ui_combo,     &ui_drop_box,
    &ui_pack,         &ui_set_text,     &ui_set_state, &ui_text_view,
    &ui_alive,        &ui_ask_yes_no,   &ui_ask_open_file,
    &ui_ask_save_file};

}  // namespace

const stk_ui* qt_plugin_ui() { return &kQtUi; }

// ★★ 插件改装 P3：控件文本的**唯一实现**（C ABI 侧没有取文本这一路，Python 门面要它）。
std::string qt_plugin_widget_text(stk_widget* widget) {
    QWidget* target = unwrap(widget);
    if (target == nullptr) return std::string();
    if (auto* label = qobject_cast<QLabel*>(target)) return label->text().toStdString();
    if (auto* button = qobject_cast<QPushButton*>(target)) return button->text().toStdString();
    if (auto* edit = qobject_cast<QLineEdit*>(target)) return edit->text().toStdString();
    if (auto* view = qobject_cast<QPlainTextEdit*>(target)) return view->toPlainText().toStdString();
    if (auto* combo = qobject_cast<QComboBox*>(target)) return combo->currentText().toStdString();
    return std::string();
}

}  // namespace xrkit::shell


// ══ ★ 2026-10-22（用户口径）：page_trace 的**外壳侧 ring**（文件作用域）════════════════
//   要点：① 只做"过滤 + 追加"，**零文件 I/O**；② `flush_shell_page_trace()` 一次性落盘，
//   并且**由 xrkit_app 自己在退出前调用**（Python 判据是父进程，拿不到子进程内存）；
//   ③ 每轮第一次 append 时**截断**文件，避免跨轮混在一起。
namespace {
struct ShellPageTraceRing {
    QMutex mutex;
    QStringList lines;
    int dropped = 0;
    bool truncated = false;
};
ShellPageTraceRing& shell_trace_ring() {
    static ShellPageTraceRing ring;
    return ring;
}
bool shell_trace_enabled() {
    static const bool enabled = qEnvironmentVariableIsSet("XRKIT_PAGE_TRACE") &&
                                qEnvironmentVariable("XRKIT_PAGE_TRACE") == QStringLiteral("1");
    return enabled;
}
QString shell_trace_filter() {
    static const QString filter = qEnvironmentVariable("XRKIT_PAGE_TRACE_FILTER");
    return filter;
}
}  // namespace

void append_shell_page_trace(const std::string& page, const std::string& kind,
                             const std::string& detail) {
    if (!shell_trace_enabled()) return;
    const QString filter = shell_trace_filter();
    if (!filter.isEmpty() &&
        !QString::fromStdString(page).contains(filter, Qt::CaseInsensitive)) {
        return;
    }
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    const QString line = QStringLiteral("%1|%2|%3|%4|%5|%6\n")
                             .arg(QString::fromStdString(page))
                             .arg(QString::fromStdString(kind))
                             .arg(QString::fromStdString(detail))
                             .arg(QString::number(now, 'f', 6))
                             .arg(reinterpret_cast<quintptr>(QThread::currentThread()))
                             .arg(QCoreApplication::applicationPid());
    QMutexLocker lock(&shell_trace_ring().mutex);
    QStringList& ring = shell_trace_ring().lines;
    ring << line;
    if (ring.size() > 4096) {
        ring.removeFirst();
        ++shell_trace_ring().dropped;
    }
}

void flush_shell_page_trace() {
    if (!shell_trace_enabled()) return;
    QStringList copy;
    int dropped = 0;
    bool first = false;
    {
        QMutexLocker lock(&shell_trace_ring().mutex);
        copy = shell_trace_ring().lines;
        shell_trace_ring().lines.clear();
        dropped = shell_trace_ring().dropped;
        shell_trace_ring().dropped = 0;
        first = !shell_trace_ring().truncated;
        shell_trace_ring().truncated = true;
    }
    if (copy.isEmpty() && dropped == 0) return;
    QFile out(QDir::temp().filePath(QStringLiteral("page_trace.txt")));
    const auto mode = first ? (QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)
                            : (QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (!out.open(mode)) return;
    if (dropped > 0) {
        out.write(QStringLiteral("shell|trace_dropped|count=%1\n").arg(dropped).toUtf8());
    }
    for (const QString& line : copy) out.write(line.toUtf8());
}
