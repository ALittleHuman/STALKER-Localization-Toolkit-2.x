// xrkit::shell —— 行尾动作委托（唯一实现；口径见头文件）。
//
// ★ 判据现场的两条硬要求（都来自 fs 页的真事故，写在这里免得以后被「优化」掉）：
//   ① **画与命中同源**：`action_rect` 只有一份，`paint`/`editorEvent`/`sizeHint` 都用它
//      ⇒ 不会出现「看得见点不着」；
//   ② 命中后**返回 true 吃掉事件** ⇒ 不会退化成「点了一下整行」（那是选择语义，不是删除语义）。

#include "row_action_delegate.h"

#include "xrkit/core/theme.h"

#include <QEvent>
#include <QFont>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionViewItem>

namespace xrkit::shell {

RowActionDelegate::RowActionDelegate(int column, std::string mark, VisibleFn visible,
                                     ClickFn on_click)
    : column_(column), mark_(std::move(mark)), visible_(std::move(visible)),
      on_click_(std::move(on_click)) {}

bool RowActionDelegate::visible_at(const QModelIndex& index) const {
    if (!index.isValid() || index.column() != column_) return false;
    if (visible_ == nullptr) return false;
    return visible_(index);
}

QRect RowActionDelegate::action_rect(const QStyleOptionViewItem& option) const {
    const int box = core::px(20);
    const QRect rect = option.rect;
    // ★ 贴行尾（右边缘留 px(8)），不再居中
    return QRect(rect.right() - core::px(8) - box, rect.center().y() - box / 2, box, box);
}

void RowActionDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    if (!visible_at(index)) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    painter->save();
    painter->setPen(option.palette.color(QPalette::Text));
    QFont font = option.font;
    font.setPointSizeF(font.pointSizeF() + 1.0);   // 字号 +1（fs 页原样）
    painter->setFont(font);
    painter->drawText(action_rect(option), Qt::AlignCenter, QString::fromStdString(mark_));
    painter->restore();
}

bool RowActionDelegate::editorEvent(QEvent* event, QAbstractItemModel* model,
                                    const QStyleOptionViewItem& option,
                                    const QModelIndex& index) {
    (void)model;
    if (!visible_at(index) || event == nullptr) return false;
    if (event->type() != QEvent::MouseButtonRelease) return false;
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() != Qt::LeftButton) return false;
    if (!action_rect(option).contains(mouse->position().toPoint())) return false;
    if (on_click_ != nullptr) on_click_(index);
    return true;   // 吃掉：别变成「点了一下整行」
}

QSize RowActionDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (visible_at(index)) size.setWidth(core::px(30));   // fs 页原样
    return size;
}

}  // namespace xrkit::shell
