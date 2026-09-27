#include "hub.h"

#include "log_box.h"
#include "plugin_ui.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "pages/fs_page.h"
#include "pages/xml_page.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/loader.h"
#include "xrkit/engine/xml/compare.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QTabBar>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QHash>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace plugin = xrkit::plugin;
namespace xml = xrkit::engine::xml;

namespace xrkit::shell {
namespace {

// 六个内置页（**顺序与名字照抄 1.x `BUILTIN_TOOLS`**；id 是我们自己的稳定标识）
constexpr PageInfo kBuiltinPages[] = {
    {"fs", "文件系统",
     "把包（.db / .sqfs）拖进来，看包内文件树、勾选后解包或重打包"},
    {"convert", "编码转换",
     "批量转换文本编码（cp1251 / cp866 / utf-8 …），逐条对比转换前后"},
    {"text", "文本提取",
     "从语料里提取可翻译文本，导出/导入翻译表"},
    {"xml", "XML校对",
     "比较两版 XML 的结构与条目差异，定位漏翻/多翻"},
    {"video", "视频转换",
     "调用 ffmpeg 转换过场视频（需自备 ffmpeg，缺失时可见跳过）"},
    {"fontpack", "汉化包生成",
     "把字体/贴图打成一个汉化包，产出可直接分发的压缩包"},
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
    // 默认窗口尺寸过 px()（自适应入口在 main.cpp 用 adaptive_window_size 覆盖；这里只给一个
    //   不写死 96 DPI 的初始值，销掉 px-only 那条 debt）
    resize(px(1180), px(760));

    // ⑫12.1：内置两套比较模式也**注册进表** —— 与插件模式同一条通道
    for (plugin::CompareModeReg& mode : plugin::builtin_compare_modes()) {
        registry_.register_compare_mode(xml::kBuiltinPlugin, std::move(mode));
    }

    // ── 方案一（Hana 重排）：顶部标签条 + 内容区。六页照旧，只是导航从左侧列表
    //   改成顶部标签（横向铺开、当前页高亮），把左右宽度都还给内容区。
    stack_ = new QStackedWidget;
    stack_->setObjectName(QStringLiteral("page_stack"));
    page_tabs_ = new QTabBar;
    page_tabs_->setObjectName(QStringLiteral("page_tabs"));
    page_tabs_->setDrawBase(false);   // 不画标签条底基线，下划线高亮交给 QSS
    page_tabs_->setExpanding(false);  // 标签不拉伸、靠左排列
    for (int i = 0; i < kBuiltinPageCount; ++i) {
        page_tabs_->addTab(QString::fromUtf8(kBuiltinPages[i].title));
        QWidget* page = create_page(i);
        if (auto* as_page = dynamic_cast<PageBase*>(page)) {
            as_page->set_subtitle(QString::fromUtf8(kBuiltinPages[i].subtitle));
        }
        stack_->addWidget(page != nullptr ? page : make_placeholder(kBuiltinPages[i]));
        if (auto* fs_page = dynamic_cast<FsPage*>(page)) {
            // ⑫12.1：fs 页加载包之前要问插件解密器（用户 2026-09-26 决策的"必须"项）
            fs_page->set_registry(&registry_);
        }
        if (auto* xml_page = dynamic_cast<XmlPage*>(page)) {
            // ⑫12.1：插件注册的比较模式要出现在 XML 页的**同一个**下拉里
            xml_page->set_registry(&registry_);
        }
    }
    page_tabs_->setCurrentIndex(0);
    QObject::connect(page_tabs_, &QTabBar::currentChanged, stack_,
                     &QStackedWidget::setCurrentIndex);

    // ── 底部：折叠日志抽屉 + 状态栏。日志默认收起、点「▸ 日志」才展开，
    //   不再像旧版那样常驻一块 200px 横条去抢内容区。
    log_box_ = new LogBox;
    log_view_ = log_box_;  // 兼容访问器（LogBox 是 QPlainTextEdit 子类）
    log_clear_ = new QPushButton(QStringLiteral("清空"));
    log_clear_->setObjectName(QStringLiteral("log_clear"));
    QObject::connect(log_clear_, &QPushButton::clicked, this, [this] {
        if (log_box_ != nullptr) log_box_->clear_log();
    });

    log_panel_ = new QWidget;
    log_panel_->setObjectName(QStringLiteral("log_panel"));
    auto* log_panel_layout = new QHBoxLayout(log_panel_);
    log_panel_layout->setContentsMargins(px(8), px(4), px(8), px(4));
    log_panel_layout->setSpacing(px(6));
    log_panel_layout->addWidget(log_box_, 1);
    log_panel_layout->addWidget(log_clear_, 0);
    log_panel_->setVisible(false);  // 默认折叠

    log_toggle_ = new QPushButton(QStringLiteral("▸ 日志"));
    log_toggle_->setObjectName(QStringLiteral("log_toggle"));
    QObject::connect(log_toggle_, &QPushButton::clicked, this, [this] {
        log_expanded_ = !log_expanded_;
        if (log_panel_ != nullptr) log_panel_->setVisible(log_expanded_);
        if (log_toggle_ != nullptr) {
            log_toggle_->setText(log_expanded_ ? QStringLiteral("▾ 日志")
                                               : QStringLiteral("▸ 日志"));
        }
    });

    status_label_ = new QLabel(QStringLiteral("就绪"));
    status_label_->setObjectName(QStringLiteral("status_label"));

    // 整体：顶部标签条 → 内容区（吃剩余）→ 折叠日志按钮 → 日志面板 → 状态栏
    auto* central = new QWidget;
    central->setObjectName(QStringLiteral("hub_central"));
    auto* central_layout = new QVBoxLayout(central);
    central_layout->setContentsMargins(0, 0, 0, 0);
    central_layout->setSpacing(0);
    central_layout->addWidget(page_tabs_, 0);
    central_layout->addWidget(stack_, 1);
    central_layout->addWidget(log_toggle_, 0);
    central_layout->addWidget(log_panel_, 0);
    central_layout->addWidget(status_label_, 0);
    setCentralWidget(central);

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
    if (page_tabs_ == nullptr || index < 0 || index >= page_tabs_->count()) return QString();
    return page_tabs_->tabText(index);
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

void Hub::render_plugin_slots() {
    // ⑫12.1：注册表里的每一类槽位都要**真的出现在界面上**（用户 2026-09-26 决策"补六类"）。
    // 本函数覆盖五类：工具栏菜单项 / 新建下拉 / 下拉条目 / 面板 / Hub 插件页
    //（decryptor 走 fs 页闸门，format 六格式仍内置、可选）。
    // **幂等**：重渲染先把上一轮的插件页摘掉，避免"点两次按钮装两遍"。
    for (QWidget* widget : plugin_pages_) {
        stack_->removeWidget(widget);
        for (int i = 0; i < page_tabs_->count(); ++i) {
            if (page_tabs_->tabText(i) == widget->windowTitle()) {
                page_tabs_->removeTab(i);
                break;
            }
        }
        widget->deleteLater();
    }
    plugin_pages_.clear();
    plugin_pages_added_ = 0;
    plugin_slots_rendered_ = 0;
    for (int i = 0; i < page_count(); ++i) {
        PageBase* page = page_at(i);
        if (page == nullptr) continue;
        const std::string host = page->page_id().toStdString();
        for (const plugin::CommandReg* command :
             registry_.menu_items_for(host, "toolbar")) {
            auto* button = page->add_action(QString::fromStdString(command->label));
            button->setObjectName(QStringLiteral("plugin_") + QString::fromStdString(command->id));
            const std::string plugin = command->plugin;
            const std::string label = command->label;
            const plugin::CommandHandler handler = command->handler;
            QObject::connect(button, &QPushButton::clicked, this,
                             [this, plugin, label, handler] {
                                 // ⑫12.9：插件回调统一走 invoke（异常记账，不冒到 UI）
                                 registry_.invoke(plugin, label, handler);
                             });
            ++plugin_slots_rendered_;
            log_line(QStringLiteral("插件槽位：%1 → %2（%3）")
                         .arg(QString::fromStdString(host))
                         .arg(QString::fromStdString(command->label))
                         .arg(QString::fromStdString(plugin)));
        }

        // ② 新建下拉（option）+ ③ 下拉条目（option_entry）：都落在页面的操作栏里。
        //    条目按 area 塞进"该 area 的下拉"；没有就为该 area 现建一个（与 1.x 一致）。
        QHash<QString, QComboBox*> combos;
        for (const plugin::OptionReg* option : registry_.options_for(host)) {
            auto* combo = new QComboBox(page);
            combo->setObjectName(QStringLiteral("plugin_option_") +
                                 QString::fromStdString(option->label));
            for (const std::string& choice : option->choices) {
                combo->addItem(QString::fromStdString(choice));
            }
            if (page->action_bar()->layout() != nullptr) {
                page->action_bar()->layout()->addWidget(combo);
            }
            combos.insert(QString::fromStdString(option->area), combo);
            ++plugin_slots_rendered_;
            log_line(QStringLiteral("插件槽位：%1 → 下拉 %2（%3）")
                         .arg(QString::fromStdString(host))
                         .arg(QString::fromStdString(option->label))
                         .arg(QString::fromStdString(option->plugin)));
        }
        for (const plugin::OptionEntryReg& entry : registry_.option_entry_regs()) {
            if (entry.host != host) continue;
            if (!plugin::when_ok(entry.when, plugin::Context{})) continue;
            const QString area = QString::fromStdString(entry.area);
            QComboBox* combo = combos.value(area, nullptr);
            if (combo == nullptr) {
                combo = new QComboBox(page);  // 该 area 没有下拉 ⇒ 现建一个（与 1.x 一致）
                combo->setObjectName(QStringLiteral("plugin_area_") + area);
                if (page->action_bar()->layout() != nullptr) {
                    page->action_bar()->layout()->addWidget(combo);
                }
                combos.insert(area, combo);
            }
            combo->addItem(QString::fromStdString(entry.value));
            ++plugin_slots_rendered_;
            log_line(QStringLiteral("插件槽位：%1 → 下拉条目 %2（area=%3，%4）")
                         .arg(QString::fromStdString(host))
                         .arg(QString::fromStdString(entry.value))
                         .arg(area)
                         .arg(QString::fromStdString(entry.plugin)));
        }

        // ④ 面板（panel）：builder 造出来的控件塞进**页面正文**（body）。
        for (const plugin::PanelReg* panel : registry_.panels_for(host)) {
            auto* widget = reinterpret_cast<QWidget*>(panel->builder(nullptr));
            if (widget == nullptr) continue;
            widget->setObjectName(QStringLiteral("plugin_panel_") +
                                  QString::fromStdString(panel->title));
            if (page->body()->layout() == nullptr) new QVBoxLayout(page->body());
            page->body()->layout()->addWidget(widget);
            ++plugin_slots_rendered_;
            log_line(QStringLiteral("插件槽位：%1 → 面板 %2（%3）")
                         .arg(QString::fromStdString(host))
                         .arg(QString::fromStdString(panel->title))
                         .arg(QString::fromStdString(panel->plugin)));
        }
    }

    // ⑤ Hub 插件页（tool）：按 order 升序 + 注册顺序，追加到页列表末尾（内置六页不动）
    for (const plugin::ToolReg* tool : registry_.tools_sorted()) {
        auto* widget = reinterpret_cast<QWidget*>(tool->builder(nullptr));
        if (widget == nullptr) {
            widget = new QWidget(stack_);
            auto* fallback = new QLabel(QStringLiteral("插件页未造出控件"), widget);
            Q_UNUSED(fallback);
        }
        widget->setWindowTitle(QString::fromStdString(tool->name));
        widget->setObjectName(QStringLiteral("plugin_page_") + QString::fromStdString(tool->name));
        stack_->addWidget(widget);
        page_tabs_->addTab(QString::fromStdString(tool->name));
        plugin_pages_.push_back(widget);
        ++plugin_pages_added_;
        ++plugin_slots_rendered_;
        log_line(QStringLiteral("插件页：%1（%2）")
                     .arg(QString::fromStdString(tool->name))
                     .arg(QString::fromStdString(tool->plugin)));
    }
}

void Hub::load_plugins(const QString& plugin_dir) {
    // ⑫12.4：插件注册期的非法寻址要在 **Hub 启动时可见** —— 这里做三件事：
    // ① 真去加载（失败不阻塞启动）；② 把错误逐条打进日志栏；③ 状态栏给一句汇总。
    // 无 Qt 的 UI 工厂这一版先传 nullptr（16 方法的 Qt 实现是下一步）：
    // 插件照样能注册命令/格式/下拉条目/比较模式，只是暂时造不出控件。
    const QString dir = plugin_dir.isEmpty()
                            ? QDir(xrkit::core::app_dir().c_str()).filePath(QStringLiteral("plugins"))
                            : plugin_dir;
    // ⑫12.2：把外壳实现的 `api.ui`（Qt 16 方法）交给插件 —— 插件的面板/按钮
    // 才真能造出控件；`stk_widget*` 仍是不透明句柄，插件不碰 Qt 类型。
    const stk_ui* ui = qt_plugin_ui();
    plugin_ui_installed_ = ui != nullptr;
    xrkit::plugin::load_plugins_from_dir(dir.toUtf8().toStdString(), registry_, ui);
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
    render_plugin_slots();
}

}  // namespace xrkit::shell
