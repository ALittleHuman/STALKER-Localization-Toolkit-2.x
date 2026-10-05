#include "hub.h"

#include "log_box.h"
#include "plugin_ui.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "pages/fs_page.h"
#include "pages/xml_page.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"
#include "xrkit/core/log.h"
#include "xrkit/engine/plugin/loader.h"
#include "xrkit/engine/xml/compare.h"
#include "theme.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QTabBar>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QStringConverter>
#include <QTextStream>
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
// ★ 副标题（`subtitle`）：用户 2026-09-27「先把我截图的文字全删了」⇒ 他截到的两句
//   （文件系统 / 汉化包生成）**留空**（`set_subtitle("")` 会让它不占位）。
// ★★ 2026-10-04：用户**又截了四张图**（编码转换 / 文本提取 / XML校对 / 视频转换）说
//   「**我截图的文字全部删除。**」⇒ 剩下这四句也一并删掉 —— **六页副标题至此全部为空**。
//   六条都进 `smoke.cpp` 的禁用字串锁（被点名删掉的文字不许回来，逐字串判红）。
//   卡片里没有别的说明段（反向后仍成立：那组判据只看 QLabel / 占位符 / 下拉项）。
constexpr PageInfo kBuiltinPages[] = {
    {"fs", "文件系统", ""},
    {"convert", "编码转换", ""},
    {"text", "文本提取", ""},
    {"xml", "XML校对", ""},
    {"video", "视频转换", ""},
    {"fontpack", "汉化包生成", ""},
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

// ── ★ 2026-10-06（③）落盘日志的目录（进程级；见 hub.h 的说明）────────────
namespace {
QString g_log_dir;  // 空 = 用默认（exe 目录 / logs）
}  // namespace

QString hub_log_dir() {
    if (!g_log_dir.isEmpty()) return g_log_dir;
    // 1.x `os.path.join(app_dir(), "logs")`：**exe 所在目录**（发行版就在程序旁边）
    return QDir(QString::fromStdString(xrkit::core::app_dir())).filePath(QStringLiteral("logs"));
}

void set_hub_log_dir(const QString& dir) { g_log_dir = dir; }

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
            // ★★ 2026-10-06（①）：**每一页**都要拿到注册表 —— `context`（右键菜单）与
            //   `option_entry`（下拉取值）都是"页面侧的消费端"，只有 fs/xml 拿表的话，
            //   其余四页的右键槽/下拉条目照样是"注册了却没人读"。
            as_page->set_plugin_registry(&registry_);
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
    // ★★ 2026-10-06（③）「导出」（1.x `stalker_toolkit.py:355-373, 390`）——
    //   2.x 原来只有「清空」，`导出` 全仓零命中 ⇒ 用户拿不到可发出来的日志文件。
    log_export_ = new QPushButton(QStringLiteral("导出日志"));
    log_export_->setObjectName(QStringLiteral("log_export"));
    QObject::connect(log_export_, &QPushButton::clicked, this, [this] {
        QString path;
        QString why;
        if (export_log(&path, &why)) {
            log_line(QStringLiteral("日志已导出：%1").arg(QDir::toNativeSeparators(path)));
        } else {
            // 零静默：**写不出去必须明确报**（日志栏 + 状态栏两处可见）
            const QString text = QStringLiteral("导出日志失败：%1").arg(why);
            log_line(text);
            if (status_label_ != nullptr) status_label_->setText(text);
        }
    });

    log_panel_ = new QWidget;
    log_panel_->setObjectName(QStringLiteral("log_panel"));
    // ★ 用户真机反馈（截图里折叠后的「日志 / 就绪」只剩一条窄缝）：给抽屉**头部一行**
    //   一个最小高度，让它始终是「看得见、点得着」的一条把手（口径 ≈px(28)）。
    log_panel_->setMinimumHeight(xrkit::core::px(28));
    auto* log_panel_layout = new QHBoxLayout(log_panel_);
    log_panel_layout->setContentsMargins(px(8), px(4), px(8), px(4));
    log_panel_layout->setSpacing(px(6));
    log_panel_layout->addWidget(log_box_, 1);
    log_panel_layout->addWidget(log_export_, 0);
    log_panel_layout->addWidget(log_clear_, 0);
    log_panel_->setVisible(false);  // 默认折叠

    // ★★ 2026-10-06（③）：**把本次运行的详细日志落盘**（改动前 `set_log_file` 零命中 ⇒
    //   GUI 日志一条都不落盘）。1.x 口径：`logs/log_YYYYmmdd_HHMMSS.txt`；**挂不上要说**。
    {
        const QString log_dir = hub_log_dir();
        QDir().mkpath(log_dir);
        log_file_path_ = QDir(log_dir).filePath(
            QStringLiteral("log_%1.txt")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
        xrkit::core::set_log_file(log_file_path_.toStdString());
        // 1.x A-2：「日志**写不出去**必须让用户看见」——`set_log_file` 自己会静默降级，
        //   所以这里读它的状态位把话说明白（进日志栏，用户至少知道"这次没有文件可查"）。
        if (!log_file_active()) {
            log_line(QStringLiteral("日志无法落盘：%1（本次运行的日志没有保存到文件）")
                         .arg(QDir::toNativeSeparators(log_file_path_)));
        }
    }

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

    // ★ 用户 2026-09-30：「上面除了主题之外，再增加一个调整 UI 控件大小（当然，包括里面的字体）
    //   的下拉菜单。」⇒ 视图菜单里加「界面大小」子菜单（立即生效；字体与控件一起缩放）。
    QMenu* size_menu = view_menu->addMenu(QStringLiteral("界面大小"));
    {
        for (const xrkit::shell::UiSizeChoice& item : xrkit::shell::ui_size_choices()) {
            const QString label = QString::fromUtf8(item.label);
            QAction* act = size_menu->addAction(label);
            const double scale = item.scale;
            connect(act, &QAction::triggered, this, [this, scale, label] {
                // ★ 2026-10-06：改动**立即写盘**（用户：'字体大小没有保留继承的功能'）。
                //   写失败**零静默**：`apply_ui_size` 返回后 `problem` 非空 ⇒ 状态栏 + 日志都要说出来。
                QString problem;
                apply_ui_size(scale, &problem);
                log_line(problem.isEmpty()
                             ? QStringLiteral("界面大小已切到「%1」（缩放 %2×，已记住）")
                                   .arg(label)
                                   .arg(scale)
                             : QStringLiteral("界面大小已切到「%1」（缩放 %2×），但%3")
                                   .arg(label)
                                   .arg(scale)
                                   .arg(problem));
                if (!problem.isEmpty() && status_label_ != nullptr) {
                    status_label_->setText(problem);
                }
            });
        }
    }
    QObject::connect(dark, &QAction::triggered, this, [this] { apply_theme_mode(xrkit::core::ThemeMode::Dark); });
    QObject::connect(light, &QAction::triggered, this, [this] { apply_theme_mode(xrkit::core::ThemeMode::Light); });

    log_line(QStringLiteral("Hub 就绪：%1 个内置页").arg(kBuiltinPageCount));
    load_plugins(plugin_dir);
}

int Hub::page_count() const { return stack_ == nullptr ? 0 : stack_->count(); }

void Hub::apply_theme_mode(xrkit::core::ThemeMode mode, QString* problem) {
    if (problem != nullptr) problem->clear();
    // ① 先落地在界面上（与改动前逐字一致：属性 + 主题 + 标题栏）
    qApp->setProperty("xrkit_theme",
                      QString::fromUtf8(xrkit::core::theme_mode_name(mode)));
    apply_theme(*qApp, mode);
    apply_dark_titlebar(this, mode);
    // ② 再记住（写 user.ltx 的 [theme] mode；**只动这一个键**）
    if (!xrkit::core::save_theme_mode(mode)) {
        if (problem != nullptr) {
            *problem = QStringLiteral("写不进 user.ltx 的 [theme] mode —— 下次启动会回到上次记住的主题");
        }
        log_line(QStringLiteral("警告: 无法写入 user.ltx，下次启动需重新选择主题"));
        return;
    }
    log_line(mode == xrkit::core::ThemeMode::Light ? QStringLiteral("已切到亮色主题（已记住）")
                                                   : QStringLiteral("已切到暗色主题（已记住）"));
}

void Hub::apply_ui_size(double scale, QString* problem) {
    if (problem != nullptr) problem->clear();
    // ① 立即生效（控件尺寸 + 字体 + 重套样式表；模式从属性里读）
    xrkit::shell::apply_ui_size(*qApp, scale);
    // ② 再记住（写 user.ltx 的 [theme] scale；**只动这一个键**）
    if (!xrkit::core::save_ui_scale(scale)) {
        if (problem != nullptr) {
            *problem = QStringLiteral("写不进 user.ltx 的 [theme] scale —— 下次启动会回到屏幕自适应的档位");
        }
        log_line(QStringLiteral("警告: 无法写入 user.ltx，下次启动需重新调整界面大小"));
        return;
    }
}

Hub::~Hub() {
    // 引擎读入口的活动注册表钩子只有**指针**：本对象一死，`registry_` 也跟着死。
    // 只在钩子确实指着**我自己这一份**时收回（两个 Hub 同时活着时不许互相踩）。
    if (plugin::active_registry() == &registry_) plugin::set_active_registry(nullptr);
}

void Hub::set_current_page(int index) {
    if (page_tabs_ == nullptr || index < 0 || index >= page_tabs_->count()) return;
    page_tabs_->setCurrentIndex(index);   // 与页栈的连接已存在 ⇒ 两处同步
    if (stack_ != nullptr) stack_->setCurrentIndex(index);
}

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

bool Hub::log_file_active() const { return xrkit::core::log_file_active(); }

// ★★ 2026-10-06（③）：1.x `stalker_toolkit.py:355-373` 的 `export_log()` 等价物 ——
//   **GUI 简略日志 + 落盘详细日志合并**写一份新文件（文件名 `log_YYYYmmdd_HHMMSS.txt`）。
// 零静默：① 目录建不出来 / ② 文件打不开 / ③ 详细日志读不回来 —— 全部要**说出来**
//   （③ 按 1.x 的做法写进导出文件里，① ② 直接返回非空理由让调用方报给用户）。
bool Hub::export_log(QString* out_path, QString* error) {
    const auto bail = [this, error](const QString& why) {
        last_export_error_ = why;
        if (error != nullptr) *error = why;
        return false;
    };
    const QString dir = hub_log_dir();
    if (!QDir().mkpath(dir)) {
        return bail(QStringLiteral("日志目录建不出来：%1").arg(QDir::toNativeSeparators(dir)));
    }
    const QString path = QDir(dir).filePath(
        QStringLiteral("log_%1.txt")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return bail(QStringLiteral("日志文件打不开：%1（%2）")
                        .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << QStringLiteral("═══ 简略日志 (GUI) ═══\n");
    if (log_box_ != nullptr) {
        // 先落 pending（80 ms 攒批）—— 否则"刚刚写的那条标记"可能还在缓冲里，导出看不到它。
        log_box_->flush();
        out << log_box_->toPlainText();
    }
    out << QStringLiteral("\n═══ 详细日志 (%1) ═══\n")
               .arg(QFileInfo(log_file_path_).fileName());
    QFile detail(log_file_path_);
    if (detail.open(QIODevice::ReadOnly)) {
        out << QString::fromUtf8(detail.readAll());
    } else {
        out << QStringLiteral("(详细日志读取失败：%1)\n")
                   .arg(QDir::toNativeSeparators(log_file_path_));
    }
    out.flush();
    file.close();
    if (file.error() != QFileDevice::NoError) {
        return bail(QStringLiteral("日志写盘出错：%1（%2）")
                        .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    last_export_error_.clear();
    if (out_path != nullptr) *out_path = path;
    return true;
}

void Hub::render_plugin_slots() {
    // ⑫12.1：注册表里的每一类槽位都要**真的出现在界面上**（用户 2026-09-26 决策"补六类"）。
    // 本函数覆盖六类：工具栏菜单项 / 新建下拉 / 下拉条目 / 面板 / Hub 插件页 / **右键菜单
    //（context —— 2026-10-06 ① 补齐；它的渲染端在**页面**里：`PageBase::append_plugin_context_items`
    //   由各页在弹右键菜单时调用，Hub 只负责把注册表注入下去，见构造函数）**。
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
        // ★★ 2026-10-06（①）：area → 下拉 的登记**从局部 QHash 挪到页面自己身上**
        //    （`PageBase::set_plugin_option_combo`）。修前那张 QHash 是**局部变量**，
        //    函数一返回指针就没人拿着 ⇒ 插件加的下拉条目"只显示、不生效"（页面读不到它的值）；
        //    挪到页面之后，页面侧的取值入口就是 `plugin_option_value(area)`。
        page->clear_plugin_option_combos();   // 重渲染：先摘掉上一轮的登记（幂等）
        for (const plugin::OptionReg* option : registry_.options_for(host)) {
            const QString area = QString::fromStdString(option->area);
            QComboBox* combo = page->plugin_option_combo(area);
            if (combo == nullptr) {
                combo = new QComboBox(page);
                combo->setObjectName(QStringLiteral("plugin_option_") +
                                     QString::fromStdString(option->label));
                if (page->action_bar()->layout() != nullptr) {
                    page->action_bar()->layout()->addWidget(combo);
                }
                page->set_plugin_option_combo(area, combo);
            }
            for (const std::string& choice : option->choices) {
                combo->addItem(QString::fromStdString(choice));
            }
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
            QComboBox* combo = page->plugin_option_combo(area);
            if (combo == nullptr) {
                combo = new QComboBox(page);  // 该 area 没有下拉 ⇒ 现建一个（与 1.x 一致）
                combo->setObjectName(QStringLiteral("plugin_area_") + area);
                if (page->action_bar()->layout() != nullptr) {
                    page->action_bar()->layout()->addWidget(combo);
                }
                page->set_plugin_option_combo(area, combo);
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
