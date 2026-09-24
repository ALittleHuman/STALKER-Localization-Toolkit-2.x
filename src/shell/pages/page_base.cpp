#include "page_base.h"

#include "xrkit/core/theme.h"

#include "../scroll_viewport.h"

#include <QFont>
#include <QHBoxLayout>
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

    outer_ = new QVBoxLayout(this);
    // 边距/间距一律过 px()（14.3：按 96 DPI 定的像素都得过；Qt 侧只过 core::px，
    // 不自己乘 devicePixelRatio —— 逻辑像素已经设备无关了，再乘一次就是双重缩放）
    outer_->setContentsMargins(xrkit::core::px(8), xrkit::core::px(6), xrkit::core::px(8),
                               xrkit::core::px(6));
    outer_->setSpacing(xrkit::core::px(6));

    header_ = new QLabel(title_, this);
    header_->setObjectName(QStringLiteral("page_header"));
    QFont header_font = header_->font();
    header_font.setPointSizeF(header_font.pointSizeF() + 3.0);
    header_font.setBold(true);
    header_->setFont(header_font);

    // ── 5.1：每页**自己**套一层视口。fit=content（静态内容，条子由本视口造），
    //    minsize_y = page_min_h()（5.2/5.3 的具名 setter）。
    viewport_ = new ScrollViewport(this);
    viewport_->setObjectName(QStringLiteral("page_viewport"));
    viewport_->set_fit(ScrollViewport::Fit::Content);
    viewport_->set_min_body_height(page_min_h());

    body_ = new QWidget;
    body_->setObjectName(QStringLiteral("page_body"));
    body_layout_ = new QVBoxLayout(body_);
    body_layout_->setContentsMargins(0, 0, 0, 0);
    body_layout_->setSpacing(xrkit::core::px(6));
    viewport_->set_body(body_);

    // ── 规则 2：操作栏**先放且贴底**。它是页的直接子控件、**不在视口里**，
    //    所以滚内容时它不动，窗口压矮时它也不被挤掉。
    action_bar_ = new QWidget(this);
    action_bar_->setObjectName(QStringLiteral("page_actions"));
    action_layout_ = new QHBoxLayout(action_bar_);
    action_layout_->setContentsMargins(0, 0, 0, 0);
    action_layout_->setSpacing(xrkit::core::px(6));
    action_layout_->addStretch(1);  // 按钮从右侧长出来（1.x 操作栏也是贴右）

    outer_->addWidget(header_, 0);
    outer_->addWidget(viewport_, 1);  // 吃剩余空间
    outer_->addWidget(action_bar_, 0);
}

QPushButton* PageBase::add_action(const QString& text) {
    auto* button = new QPushButton(text, action_bar_);
    button->setObjectName(QStringLiteral("action_") + text);
    // 插在末尾那个 stretch 之前 ⇒ 按钮保持右对齐且**加的顺序 = 显示的顺序**
    action_layout_->insertWidget(action_layout_->count() - 1, button);
    return button;
}

bool PageBase::minsize_ok() const {
    return viewport_ != nullptr && viewport_->min_body_height() == page_min_h() &&
           viewport_->min_body_height() > 0;
}

}  // namespace xrkit::shell
