#include "pair_tree.h"

#include "theme.h"

#include <QCursor>
#include <QHeaderView>
#include <QStyle>
#include <QTreeWidget>

namespace xrkit::shell {

void apply_pair_tree_spec(QTreeWidget* view, const PairTreeSpec& spec) {
    if (view == nullptr) return;
    view->setColumnCount(spec.headers.size());
    view->setHeaderLabels(spec.headers);
    view->setUniformRowHeights(true);        // 行高一致 ⇒ 几万行也不慢
    view->setAlternatingRowColors(false);
    // ★ **是树就得有根装饰**：包列表在"平表时代"留了一句 `setRootIsDecorated(false)`，
    //   树化之后它会让**顶层节点没有展开箭头**（默认收起时那些孩子就再也点不出来了）。
    view->setRootIsDecorated(true);
    view->setExpandsOnDoubleClick(false);    // 展开只认"单击条目身体"这一条路（见 click_on_indicator）
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int col = 0; col < spec.headers.size(); ++col) {
        view->header()->setSectionResizeMode(col, QHeaderView::Interactive);
        if (col < spec.widths.size()) {
            view->header()->resizeSection(col, xrkit::core::px(spec.widths[col]));
        }
    }
    view->header()->setMinimumSectionSize(xrkit::core::px(48));
    // ★★ "表头/行铺满面板"与"列可拖"要**同时**成立：靠**末列吃剩余宽度**（`stretchLastSection`），
    //   而不是把某一列设成 `Stretch` —— 后者会**失去拖拽**，正是用户 2026-09-29
    //   「某些字段还是不能全部显示……最好也是弄一个水平方向的可拖拽分隔条」要的东西
    //   （我第一版用首列 `Stretch` 实现"名称列吃满"，被自检当场拦下：树①/树② 两条红）。
    view->header()->setStretchLastSection(true);
    if (spec.min_height > 0) view->setMinimumHeight(xrkit::core::px(spec.min_height));
    view->setAcceptDrops(spec.accept_drops);
}

bool click_on_indicator(QTreeWidget* view, QTreeWidgetItem* item) {
    if (view == nullptr || view->viewport() == nullptr || item == nullptr) return false;
    const QPoint vp = view->viewport()->mapFromGlobal(QCursor::pos());
    const QRect row = view->visualItemRect(item);
    int depth = 0;
    for (QTreeWidgetItem* p = item->parent(); p != nullptr; p = p->parent()) ++depth;
    const int indent = view->indentation() * depth;
    const int indicator = view->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 8;
    return row.contains(vp) && vp.x() < row.x() + indent + indicator;
}

}  // namespace xrkit::shell
