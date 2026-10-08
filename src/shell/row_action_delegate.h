#pragma once
// xrkit::shell —— **行尾动作委托**（插件改装 P13）：行尾那个小按钮（fs 页的 ✕）的**唯一实现**。
//
// 来历（用户原话，2026-10-01）：「移除这个按钮也可以优化掉，在包的条目后面增加一个叉叉按钮。」
// 实现要点（**逐条**从 `fs_page.cpp` 的原实现搬过来，一条没改口径）：
//   · 用**委托**画 + 自己处理点击，**不要 `setItemWidget`**（那会把整行内容换成自造控件，
//     勾选框/列宽/选择行为全被搞乱）；
//   · 点击走 `editorEvent`（判 `MouseButtonRelease` + 落点在方块内）⇒ **吃掉**这次点击，
//     别让它变成「点了一下整行」；
//   · 画与命中用**同一个** `action_rect` ⇒ 看到的和点得到的永远一致；
//   · 2026-10-02：「✕ **贴行尾**（右边缘留 `px(8)` 边距），不再居中」—— 末列是吃剩余宽度的
//     那一列，居中会飘在那一大块中间，用户要的是「包的条目后面」；
//   · 可见性**参数化**：fs 页的口径是「第 0 列 `UserRole` 存着源下标 ≥0 才画」（分组节点没有 ✕），
//     门面树的口径是「节点声明了 `has_action` 才画」。
//
// ★ 本文件是「同类东西只维护一套」的落实：fs 页与门面树**共用**它。

#include <QStyledItemDelegate>

#include <functional>
#include <string>

namespace xrkit::shell {

class RowActionDelegate : public QStyledItemDelegate {
public:
    using VisibleFn = std::function<bool(const QModelIndex&)>;
    using ClickFn = std::function<void(const QModelIndex&)>;

    RowActionDelegate(int column, std::string mark, VisibleFn visible, ClickFn on_click);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    // 判据用：方块位置（画与命中同源）、列号、某行是否可见
    QRect action_rect(const QStyleOptionViewItem& option) const;
    int column() const { return column_; }
    bool visible_at(const QModelIndex& index) const;

private:
    int column_ = 0;
    std::string mark_;
    VisibleFn visible_;
    ClickFn on_click_;
};

}  // namespace xrkit::shell
