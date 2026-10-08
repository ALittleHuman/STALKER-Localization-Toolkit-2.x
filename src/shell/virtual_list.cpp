#include "virtual_list.h"

#include "theme.h"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <algorithm>

namespace xrkit::shell {

VirtualList::VirtualList(QWidget* parent) : QWidget(parent) {
    // ★ P13-2b：要收键盘（回车激活）⇒ 必须能拿焦点
    setFocusPolicy(Qt::StrongFocus);
    setObjectName(QStringLiteral("virtual_list"));
}

void VirtualList::set_row_height(int height) {
    row_height_ = std::max(1, height);
    publish_content_height();
    refresh();
}

void VirtualList::set_row_count(int total) {
    total_ = std::max(0, total);
    publish_content_height();
    refresh();
}

// ★★ 2026-10-05（#6）：把"我多高"**说出来**（Qt 动态属性），外层 `PageBase::sync_inner_scroll()`
//   读它当 `min_body_height`。为什么不直接让外层 `qobject_cast<VirtualList*>`：
//   那样基类就得认识这个具体类（基类只该认识"内容件"这个角色）；也不用 signals/slots ——
//   本页在改行数的地方**显式**调一次同步（改的行数点很少，显式更清楚）。
void VirtualList::publish_content_height() {
    setProperty("xrkit_content_height", content_height());
}

void VirtualList::set_row_text_provider(std::function<QString(int)> provider) {
    provider_ = std::move(provider);
    refresh();
}

QPair<int, int> VirtualList::visible_range(int top, int view_h, int row_h, int total) {
    if (row_h <= 0 || total <= 0) return {0, 0};
    const int first = top / row_h;                          // 10.2
    const int last = std::min(total, (top + view_h) / row_h + 2);
    return {std::max(0, first), std::max(0, last)};
}

QPair<int, int> VirtualList::visible_range() const { return range_; }

void VirtualList::set_scroll_top(int top) {
    top_ = std::max(0, top);
    refresh();
}

QString VirtualList::row_text(int row) const {
    if (row < 0 || row >= total_) return QString();
    return provider_ ? provider_(row) : QString::number(row);
}

void VirtualList::set_selected_row(int row) {
    selected_ = row;
    refresh();
}


// ── ★★ P13-2b：激活（双击 / 回车）──────────────────────────────────────────────
void VirtualList::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event == nullptr) return;
    if (row_height_ > 0 && total_ > 0 && on_activate_ != nullptr) {
        // ★ 行号 = **滚动偏移 + 视口内偏移 / 行高**（带偏移；滚动之后才对得上）
        // ★ `top_` 是**像素**偏移（见 `visible_range(top, view_h, row_h, total)`
        //   里的 `first = top // h`）⇒ 先加再除，别把像素当行号。
        const int row = (top_ + static_cast<int>(event->position().y())) / row_height_;
        const int clamped = row < 0 ? 0 : (row >= total_ ? total_ - 1 : row);
        ++activations_;
        on_activate_(clamped);
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VirtualList::keyPressEvent(QKeyEvent* event) {
    if (event == nullptr) return;
    const int key = event->key();
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && on_activate_ != nullptr &&
        selected_ >= 0 && selected_ < total_) {
        ++activations_;
        on_activate_(selected_);
        return;   // 吃掉（别让它再被当成普通按键）
    }
    QWidget::keyPressEvent(event);
}

void VirtualList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh();
}

void VirtualList::refresh() {
    range_ = visible_range(top_, height(), row_height_, total_);
    const int need = std::max(0, range_.second - range_.first);
    // 行控件池：只增不减到"当前需要"为止（复用优先 —— total 变化**不**影响池子大小）
    while (widgets_.size() < need) {
        auto* label = new QLabel(this);
        label->setObjectName(QStringLiteral("virtual_row"));
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        label->show();
        widgets_.push_back(label);
    }
    provider_calls_ = 0;
    for (int i = 0; i < widgets_.size(); ++i) {
        QLabel* label = widgets_[i];
        if (i >= need) {
            label->hide();          // 池子里富余的行控件：藏起来，不销毁（复用）
            continue;
        }
        const int row = range_.first + i;
        label->setText(provider_ ? provider_(row) : QString::number(row));
        if (provider_) ++provider_calls_;   // 只为视口内的行取文本（10.3 同族）
        label->setGeometry(0, row * row_height_ - top_, width(), row_height_);
        const char* role = (row == selected_) ? "selected" : "surface";
        const char* hex = xrkit::core::theme_color_current(role);
        label->setStyleSheet(
            QStringLiteral("background:%1; padding-left:%2px;")
                .arg(QString::fromUtf8(hex != nullptr ? hex : "#22252c"))
                .arg(px(6)));
        label->show();
        if (i < need) ++recycle_count_;     // 复用一次（重建会体现在池子增长上）
    }
    update();
}

}  // namespace xrkit::shell
