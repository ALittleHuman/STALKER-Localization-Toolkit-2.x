#include "page_base.h"

#include "xrkit/core/theme.h"

#include "../scroll_viewport.h"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
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

QPushButton* PageBase::add_action(const QString& text) {
    auto* button = new QPushButton(text, action_bar_);
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

}  // namespace xrkit::shell
