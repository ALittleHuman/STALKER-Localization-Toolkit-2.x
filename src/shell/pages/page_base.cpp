#include "page_base.h"

#include "xrkit/core/theme.h"

#include "../scroll_viewport.h"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QAction>
#include <QComboBox>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>

namespace xrkit::shell {

// 5.2：常数 430 是**按 96 DPI 定的设计稿值**，必须过 `px()`。做成函数是因为
// `px()` 依赖当前缩放（`core::set_ui_scale()` / 启动时的 DPI 探测）——
// 常量会让 150% 屏上仍然返回 430。
int page_min_h() { return xrkit::core::px(430); }

PageBase::PageBase(const QString& page_id, const QString& title, QWidget* parent)
    : QWidget(parent), page_id_(page_id), title_(title) {
    setObjectName(QStringLiteral("page_") + page_id);
    // ★ 用户 2026-09-27 真机反馈：「能输入的框点空白并不能取消，还是一闪一闪」
    //   真因：QLineEdit 拿到焦点后，点卡片/正文那些**不接焦点**的空白区，Qt 不会动焦点
    //   （默认 mousePressEvent 只 ignore，页面本身又是 NoFocus）⇒ 光标一直闪。
    //   修法两层：① 页面本体接**点击焦点**（setFocus 得有地方落）；
    //            ② 在空白容器上装事件过滤器，按下就把焦点**收回页面**（见 eventFilter）。
    //   为什么不只靠 ①：点击落在 card/body 这类 NoFocus 子控件上时，Qt 不保证把焦点
    //   交给父级；显式收焦点才是"点哪儿都会取消输入框焦点"的确定性做法。
    setFocusPolicy(Qt::ClickFocus);

    outer_ = new QVBoxLayout(this);
    // 边距/间距一律过 px()（14.3：按 96 DPI 定的像素都得过；Qt 侧只过 core::px，
    // 不自己乘 devicePixelRatio —— 逻辑像素已经设备无关了，再乘一次就是双重缩放）
    // ★ 用户真机反馈「又挤又空」：页边距/行距整体上调一档（原来 8/6 + spacing 6 太紧）
    outer_->setContentsMargins(xrkit::core::px(12), xrkit::core::px(10), xrkit::core::px(12),
                               xrkit::core::px(10));
    outer_->setSpacing(xrkit::core::px(10));

    header_ = new QLabel(title_, this);
    header_->setObjectName(QStringLiteral("page_header"));
    QFont header_font = header_->font();
    header_font.setPointSizeF(header_font.pointSizeF() + 3.0);
    header_font.setBold(true);
    header_->setFont(header_font);

    // 副标题：次要色、比正文略小（QSS 里 #page_subtitle 定色）
    subtitle_ = new QLabel(this);
    subtitle_->setObjectName(QStringLiteral("page_subtitle"));
    subtitle_->setWordWrap(true);
    subtitle_->setVisible(false);  // 没设文案就不占位

    // ── 5.1：每页**自己**套一层视口。fit=content（静态内容，条子由本视口造），
    //    minsize_y = page_min_h()（5.2/5.3 的具名 setter）。
    viewport_ = new ScrollViewport(this);
    viewport_->setObjectName(QStringLiteral("page_viewport"));
    viewport_->set_fit(ScrollViewport::Fit::Content);
    viewport_->set_min_body_height(page_min_h());

    body_ = new QWidget;
    body_->setObjectName(QStringLiteral("page_body"));
    // ★ 内容区包进**卡片**：页面照旧往 body_layout() 里加东西 ⇒ 自动落在卡片内（页面代码不用改）
    card_ = new QFrame(body_);
    card_->setObjectName(QStringLiteral("card"));
    body_layout_ = new QVBoxLayout(card_);
    body_layout_->setContentsMargins(xrkit::core::px(12), xrkit::core::px(12),
                                     xrkit::core::px(12), xrkit::core::px(12));
    body_layout_->setSpacing(xrkit::core::px(6));
    auto* body_outer = new QVBoxLayout(body_);
    body_outer->setContentsMargins(0, 0, 0, 0);
    body_outer->addWidget(card_);
    viewport_->set_body(body_);

    // ── 规则 2：操作栏**先放且贴底**。它是页的直接子控件、**不在视口里**，
    //    所以滚内容时它不动，窗口压矮时它也不被挤掉。
    action_bar_ = new QWidget(this);
    action_bar_->setObjectName(QStringLiteral("page_actions"));
    action_layout_ = new QHBoxLayout(action_bar_);
    action_layout_->setContentsMargins(0, 0, 0, 0);
    action_layout_->setSpacing(xrkit::core::px(6));
    action_layout_->addStretch(1);  // 按钮从右侧长出来（1.x 操作栏也是贴右）

    // ★ 用户 2026-09-27 真机反馈「太空又太挤」：把**页标题与说明合并成一行**（原来是两条薄行
    //   叠在一起，加上顶部标签条共四条，视觉上挤成一片）。标题左、说明右、同基线；
    //   顶部堆叠行数从 4 降到 2（标签条 + 本行）。
    header_row_ = new QWidget(this);
    header_row_->setObjectName(QStringLiteral("page_header_row"));
    auto* header_row_layout = new QHBoxLayout(header_row_);
    header_row_layout->setContentsMargins(0, 0, 0, 0);
    header_row_layout->setSpacing(xrkit::core::px(8));
    header_row_layout->addWidget(header_, 0);
    header_row_layout->addStretch(1);
    header_row_layout->addWidget(subtitle_, 0, Qt::AlignVCenter | Qt::AlignRight);
    outer_->addWidget(header_row_, 0);
    outer_->addWidget(viewport_, 1);  // 吃剩余空间
    outer_->addWidget(action_bar_, 0);

    // ② 空白容器：按在它们身上 = "点了空白" ⇒ 把输入框的焦点收回页面
    //   （只装在这几个**容器**上：按到 QLineEdit/QComboBox 等**接焦点**的控件时，
    //    事件先到那个控件，这里的过滤器不会被触发 ⇒ 不会误抢）
    for (QWidget* blank : {static_cast<QWidget*>(header_row_), card_, body_, action_bar_,
                           static_cast<QWidget*>(viewport_)}) {
        if (blank != nullptr) blank->installEventFilter(this);
    }
}

bool PageBase::eventFilter(QObject* watched, QEvent* event) {
    // 点空白 ⇒ 焦点收回页面（不吞事件：该冒泡的照旧冒泡）
    if (event != nullptr && event->type() == QEvent::MouseButtonPress) {
        setFocus(Qt::MouseFocusReason);
    }
    return QWidget::eventFilter(watched, event);
}

void PageBase::set_subtitle(const QString& text) {
    if (subtitle_ == nullptr) return;
    subtitle_->setText(text);
    subtitle_->setVisible(!text.isEmpty());
}

// ── ★★ 插件改装 P1（Hub 机制层）：宿主交付机制的唯一入口（见 page_base.h 的说明）──
// 默认只做两件事：**注册表**（插件槽位在页面侧的消费端要用）与**日志口**（页里的动作要
// 在日志栏看得见）。其余机制（状态栏 / 切页 / 任务壳）由覆写者按需取 —— 这里不主动替页做事，
// 免得多出一个"页没要却被改"的隐式行为。
void PageBase::attach_services(const PageServices& services) {
    services_ = services;
    if (services.registry != nullptr) plugin_registry_ = services.registry;
    if (services.log) log_sink_ = services.log;
}

// ── ★★ 2026-10-05（用户报告 #6）：页内自足内容区的**第二层视口**（见 page_base.h 的说明）──
//
// 为什么第二层视口也由基类造：门禁那条静态锁（"页内视口只能由 PageBase 造"）拦的就是
// "各页各写一份 fit/minsize" —— 1.x 的 `min_y=` 事故是这么来的。所以这里只提供**一个**入口，
// 页面负责把内容件交进来、并在内容变化时调 `sync_inner_scroll()`。
//
// fit＝**Content**（静态内容，条子由本视口造）：装得下时 `min_body_height` 就是内容高
// （小于视口高 ⇒ 画布与视口同高 ⇒ 条子不占位）；装不下时画布撑到内容高 ⇒ 条子出现、能滚。
ScrollViewport* PageBase::make_inner_scroll(QWidget* content, const QString& object_name) {
    if (content == nullptr) return nullptr;
    if (inner_viewport_ != nullptr) return inner_viewport_;   // 一页只造一层（幂等）
    inner_viewport_ = new ScrollViewport(this);
    inner_viewport_->setObjectName(object_name);
    inner_viewport_->set_fit(ScrollViewport::Fit::Content);
    // 内容件挂进来时它会接管 body 的尺寸（见 set_body）；先把内容件挂到视口下再 set_body，
    // 否则内容件没有父对象、Qt 会把它当顶层窗口。
    content->setParent(inner_viewport_);
    inner_viewport_->set_body(content);
    inner_content_ = content;
    // 与页视口同一套"点空白收焦点"口径（否则点结果区空白会留焦点在输入框里闪）
    inner_viewport_->installEventFilter(this);
    sync_inner_scroll();
    return inner_viewport_;
}

void PageBase::sync_inner_scroll() {
    if (inner_viewport_ == nullptr || inner_content_ == nullptr) return;
    // ★ 内容高从**内容件自己**问（VirtualList::content_height）—— 基类不认识 VirtualList，
    //   它只认"这个 QWidget 愿意说自己多高"。这样一来也不会有第二处"行数 × 行高"的算法。
    //   兜底：内容件没有那个属性 ⇒ 用 Qt 的 `sizeHint().height()`（原生控件也能用）。
    int content_h = inner_content_->property("xrkit_content_height").toInt();
    if (content_h <= 0) content_h = inner_content_->sizeHint().height();
    if (content_h > 0) inner_viewport_->set_min_body_height(content_h);
    inner_viewport_->sync_layout();
}

QPushButton* PageBase::add_action(const QString& text) {    auto* button = new QPushButton(text, action_bar_);
    button->setObjectName(QStringLiteral("action_") + text);
    // ★ 2026-09-30 修（真缺陷）：那个 stretch 是在构造函数里**第一个**加进来的（在 index 0），
    //   所以"插在末尾那个 stretch 之前"其实插到了 stretch **前面** ⇒ 按钮全被顶到**左边**，
    //   与「1.x 操作栏贴右 / 按钮从右侧长出来」的口径正好相反（`--shot` 截图里一眼可见：
    //   两个按钮孤儿一样挂在左下角，与卡片脱节）。要右对齐就得**追加到 stretch 之后**。
    action_layout_->addWidget(button);
    return button;
}

bool PageBase::minsize_ok() const {
    return viewport_ != nullptr && viewport_->min_body_height() == page_min_h() &&
           viewport_->min_body_height() > 0;
}

// ── ★★ 2026-10-06（①）：插件槽位在页面侧的消费端（context / option_entry）──────────
// 为什么放在基类：Hub 的 `render_plugin_slots` 只认识 `PageBase`（六页 + 插件页共用一个循环），
// 而"右键菜单里得有插件的命令""页面得读得到插件下拉的值"必须是**每页都能做**的事 ——
// 放基类就是一份实现，放各页就是六份（那正是 1.x 最贵的一类返工）。

int PageBase::append_plugin_context_items(QMenu* menu, const plugin::Context& context) {
    if (menu == nullptr || plugin_registry_ == nullptr) return 0;
    // 12.4：精确匹配 (host, location="context")，`when` 用**当次**的 context 判定
    const std::vector<const plugin::CommandReg*> items =
        plugin_registry_->menu_items_for(page_id_.toStdString(), "context", context);
    if (items.empty()) return 0;   // 1.x「无注册即无痕迹」：连分隔符都不加
    if (!menu->actions().isEmpty()) menu->addSeparator();
    int added = 0;
    for (const plugin::CommandReg* command : items) {
        if (command == nullptr) continue;
        const std::string plugin = command->plugin;
        const std::string label = command->label;
        const plugin::CommandHandler handler = command->handler;
        QAction* action = menu->addAction(QString::fromStdString(label));
        action->setObjectName(QStringLiteral("plugin_ctx_") +
                              QString::fromStdString(command->id));
        // 与 toolbar 槽同一条通道：⑫12.9 的 `Registry::invoke`（插件回调抛异常记进
        // registration_errors，绝不冒到 Qt 事件循环里）。
        QObject::connect(action, &QAction::triggered, this, [this, plugin, label, handler] {
            if (plugin_registry_ != nullptr) plugin_registry_->invoke(plugin, label, handler);
        });
        ++added;
    }
    return added;
}

void PageBase::set_plugin_option_combo(const QString& area, QComboBox* combo) {
    plugin_option_combos_.insert(area, combo);
}

QComboBox* PageBase::plugin_option_combo(const QString& area) const {
    return plugin_option_combos_.value(area, nullptr);
}

QString PageBase::plugin_option_value(const QString& area) const {
    QComboBox* combo = plugin_option_combos_.value(area, nullptr);
    return combo != nullptr ? combo->currentText() : QString();
}

}  // namespace xrkit::shell
