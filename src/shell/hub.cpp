#include "hub.h"

#include "log_box.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "split_pane.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/loader.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace xrkit::shell {
namespace {

// 六个内置页（**顺序与名字照抄 1.x `BUILTIN_TOOLS`**；id 是我们自己的稳定标识）
constexpr PageInfo kBuiltinPages[] = {
    {"fs", "文件系统"},
    {"convert", "编码转换"},
    {"text", "文本提取"},
    {"xml", "XML校对"},
    {"video", "视频转换"},
    {"fontpack", "汉化包生成"},
};

constexpr int kBuiltinPageCount =
    static_cast<int>(sizeof(kBuiltinPages) / sizeof(kBuiltinPages[0]));

// 页面占位：**只在页工厂返回空时兜底**（正常路径走 `create_page`，六页各有框架）。
// 留着它是因为"页建不出来"必须能被看见，而不是变成空白页。
QWidget* make_placeholder(const PageInfo& page) {
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    auto* title = new QLabel(QString::fromUtf8(page.title));
    QFont font = title->font();
    font.setPointSizeF(font.pointSizeF() + 3.0);
    font.setBold(true);
    title->setFont(font);
    auto* note = new QLabel(QStringLiteral("页 id: %1 —— 业务内容按 M2-6 逐片接入"
                                           "（外壳已把布局/主题/日志壳就位）")
                                .arg(QString::fromUtf8(page.id)));
    note->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(note);
    layout->addStretch(1);
    return widget;
}

}  // namespace

int builtin_page_count() { return kBuiltinPageCount; }

const PageInfo& builtin_page(int index) {
    static const PageInfo kFallback{"", ""};
    if (index < 0 || index >= kBuiltinPageCount) return kFallback;
    return kBuiltinPages[index];
}

Hub::Hub(QWidget* parent, const QString& plugin_dir) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("STALKER 汉化工具集 2.0"));
    resize(1180, 760);

    // ── 左侧：页列表；右侧：页栈（1.x 的 Hub 形态：一列 + 一栈）
    page_list_ = new QListWidget;
    page_list_->setObjectName(QStringLiteral("page_list"));
    stack_ = new QStackedWidget;
    stack_->setObjectName(QStringLiteral("page_stack"));
    for (int i = 0; i < kBuiltinPageCount; ++i) {
        page_list_->addItem(QString::fromUtf8(kBuiltinPages[i].title));
        QWidget* page = create_page(i);
        stack_->addWidget(page != nullptr ? page : make_placeholder(kBuiltinPages[i]));
    }
    page_list_->setCurrentRow(0);
    QObject::connect(page_list_, &QListWidget::currentRowChanged, stack_,
                     &QStackedWidget::setCurrentIndex);

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(page_list_);
    splitter->addWidget(stack_);
    splitter->setStretchFactor(1, 1);

    // ── 4.1/4.5：栏目区 + 日志栏 = **竖分栏**（没有窗口级视口 ⇒ 日志不归主窗口那根条管），
    //    两栏 minsize 用同一组值：px(260) / px(110)。
    top_min_ = px(260);
    log_min_ = px(110);
    split_ = new SplitPane(SplitPane::Orient::Vertical, this);
    split_->setObjectName(QStringLiteral("hub_split"));
    split_->add_pane(splitter, top_min_);

    auto* log_panel = new QWidget;
    auto* log_layout = new QVBoxLayout(log_panel);
    log_layout->setContentsMargins(px(6), px(4), px(6), px(4));
    log_box_ = new LogBox(log_panel);
    log_view_ = log_box_;  // 兼容 M2-1 的访问器（LogBox 是 QPlainTextEdit 子类）
    status_label_ = new QLabel(QStringLiteral("就绪"));
    status_label_->setObjectName(QStringLiteral("status_label"));
    log_layout->addWidget(log_box_);
    log_layout->addWidget(status_label_);
    split_->add_pane(log_panel, log_min_);
    setCentralWidget(split_);

    // ── 菜单：主题（暗/亮，原地更新）
    QMenu* view_menu = menuBar()->addMenu(QStringLiteral("视图"));
    QAction* dark = view_menu->addAction(QStringLiteral("暗色主题"));
    QAction* light = view_menu->addAction(QStringLiteral("亮色主题"));
    QObject::connect(dark, &QAction::triggered, this, [this] {
        qApp->setProperty("xrkit_theme", "dark");
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        apply_dark_titlebar(this, xrkit::core::ThemeMode::Dark);
        log_line(QStringLiteral("已切到暗色主题"));
    });
    QObject::connect(light, &QAction::triggered, this, [this] {
        qApp->setProperty("xrkit_theme", "light");
        apply_theme(*qApp, xrkit::core::ThemeMode::Light);
        apply_dark_titlebar(this, xrkit::core::ThemeMode::Light);
        log_line(QStringLiteral("已切到亮色主题"));
    });

    log_line(QStringLiteral("Hub 就绪：%1 个内置页").arg(kBuiltinPageCount));
    load_plugins(plugin_dir);
}

int Hub::page_count() const { return stack_ == nullptr ? 0 : stack_->count(); }

QString Hub::page_title(int index) const {
    if (page_list_ == nullptr || index < 0 || index >= page_list_->count()) return QString();
    return page_list_->item(index)->text();
}

QString Hub::page_id(int index) const {
    if (index < 0 || index >= kBuiltinPageCount) return QString();
    return QString::fromUtf8(kBuiltinPages[index].id);
}

PageBase* Hub::page_at(int index) const {
    if (stack_ == nullptr || index < 0 || index >= stack_->count()) return nullptr;
    // dynamic_cast 而不是 qobject_cast：`PageBase` 刻意不带 Q_OBJECT（本模块不用 moc），
    // 而 qobject_cast 要求 Q_OBJECT。RTTI 在 MSVC 下默认开，够用。
    return dynamic_cast<PageBase*>(stack_->widget(index));
}

void Hub::log_line(const QString& text) {
    // 攒批写入（4.4）；用户在日志栏里看到的是它**自己的**条子（4.1）
    if (log_box_ != nullptr) log_box_->append_batched(text);
}

void Hub::load_plugins(const QString& plugin_dir) {
    // ⑫12.4：插件注册期的非法寻址要在 **Hub 启动时可见** —— 这里做三件事：
    // ① 真去加载（失败不阻塞启动）；② 把错误逐条打进日志栏；③ 状态栏给一句汇总。
    // 无 Qt 的 UI 工厂这一版先传 nullptr（16 方法的 Qt 实现是下一步）：
    // 插件照样能注册命令/格式/下拉条目/比较模式，只是暂时造不出控件。
    const QString dir = plugin_dir.isEmpty()
                            ? QDir(xrkit::core::app_dir().c_str()).filePath(QStringLiteral("plugins"))
                            : plugin_dir;
    xrkit::plugin::load_plugins_from_dir(dir.toUtf8().toStdString(), registry_, nullptr);
    plugin_errors_ = registry_.registration_errors();

    int loaded = 0;
    for (const xrkit::plugin::RegistrationError& error : plugin_errors_) {
        log_line(QStringLiteral("插件问题：%1 —— %2")
                     .arg(QString::fromStdString(error.plugin))
                     .arg(QString::fromStdString(error.message)));
    }
    if (plugin_errors_.empty()) {
        plugin_notice_ = QStringLiteral("插件：无（目录 %1）").arg(QDir::toNativeSeparators(dir));
    } else {
        ++loaded;
        plugin_notice_ = QStringLiteral("插件：%1 个问题（详见日志）").arg(plugin_errors_.size());
    }
    log_line(plugin_notice_);
    if (status_label_ != nullptr && !plugin_errors_.empty()) status_label_->setText(plugin_notice_);
}

}  // namespace xrkit::shell
