#include "facade_tree.h"

#include "row_action_delegate.h"

#include "pair_tree.h"
#include "theme.h"

#include <QHeaderView>
#include <QTreeWidgetItemIterator>

#include <algorithm>

namespace xrkit::shell {

FacadeTree::FacadeTree(QWidget* parent) : QTreeWidget(parent) {
    // 宿主规范（不是"随 Qt 默认"）：等行高、根装饰、多选、双击展开、不许交替行色
    setUniformRowHeights(true);
    setRootIsDecorated(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setExpandsOnDoubleClick(true);
    setAlternatingRowColors(false);
    setMinimumHeight(core::px(120));

    // 展开 ⇒ **懒建**：宿主只回调，插件自己决定要不要填子
    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (item == nullptr || on_expand_ == nullptr) return;
        const std::string key = key_of(item);
        if (!key.empty()) on_expand_(key);
    });
    // 用户改勾选 ⇒ **向下传播**（全勾/全不勾）+ **向上重算祖先**（三态）+ 回调
    connect(this, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (updating_ || item == nullptr || column != 0) return;
        if ((item->flags() & Qt::ItemIsUserCheckable) == 0) return;
        updating_ = true;
        const Qt::CheckState state = item->checkState(0);
        if (state == Qt::Checked || state == Qt::Unchecked) {
            // ★ 这一条是"三态"的另一半：勾**父**要带着子孙一起走（自检第一版漏了它，
            //   只做了向上重算 ⇒ 用户勾父时子仍不动，判据当场红）。
            std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* node) {
                for (int i = 0; i < node->childCount(); ++i) {
                    QTreeWidgetItem* child = node->child(i);
                    if ((child->flags() & Qt::ItemIsUserCheckable) != 0) {
                        child->setCheckState(0, state);
                    }
                    walk(child);
                }
            };
            walk(item);
        }
        refresh_ancestors(item->parent());
        updating_ = false;
        if (on_check_ != nullptr) {
            const std::string key = key_of(item);
            if (!key.empty()) {
                on_check_(key, item->checkState(0) == Qt::Checked);
            }
        }
    });
    // ★★ P15-0：**点行身体** ⇒ 回调（fs 页那条 `itemClicked`）。
    //   宿主**不**替他决定展开/折叠/切勾选 —— 只报 key，语义在插件侧。
    connect(this, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || on_click_ == nullptr) return;
        const std::string key = key_of(item);
        if (!key.empty()) on_click_(key);
    });
    // 双击**叶子** ⇒ 激活（有子的行双击是"展开"，那是 Qt 的默认，不抢）
    connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item == nullptr || on_activate_ == nullptr) return;
        if (item->childCount() > 0) return;
        const std::string key = key_of(item);
        if (!key.empty()) on_activate_(key);
    });
}

void FacadeTree::set_columns(const std::vector<plug::UiFacade::TreeColumn>& columns) {
    // ★★ 列策略**不在这里写**：门禁 D2④ 锁着"列策略只许在 `pair_tree.cpp` 里"——那一份是
    //   fs 页两棵树与 XML 页结果表共用的、用户 2026-09-29 亲自要过的三条（每列可拖、末列
    //   `setStretchLastSection` 吃剩余宽度、不许把某列设成 Stretch）。
    //   本函数只把门面的列声明翻成 `PairTreeSpec`，装配交给那份唯一实现。
    PairTreeSpec spec;
    spec.min_height = core::px(120);
    for (const plug::UiFacade::TreeColumn& column : columns) {
        spec.headers << QString::fromStdString(column.title);
        spec.widths << (column.width > 0 ? column.width : 0);
    }
    apply_pair_tree_spec(this, spec);
}

QTreeWidgetItem* FacadeTree::add_node(QTreeWidgetItem* parent,
                                      const plug::UiFacade::TreeNodeSpec& node) {
    auto* item = parent == nullptr ? new QTreeWidgetItem(this) : new QTreeWidgetItem(parent);
    for (size_t i = 0; i < node.cells.size(); ++i) {
        item->setText(static_cast<int>(i), QString::fromStdString(node.cells[i]));
    }
    item->setData(0, Qt::UserRole, QString::fromStdString(node.key));
    // ★ P13：这一行有没有行尾动作（fs 的「源节点才有」在门面里就是显式声明）
    item->setData(0, Qt::UserRole + 1, node.has_action ? 1 : 0);
    // ★★ 2026-10-10（P17-57，接口契约批 `TreeNodeSpec{+tooltip,+matched}`）：
    //   · `tooltip` ⇒ 逐列设悬停提示（空串不设，保持现状）；
    //   · `matched` ⇒ **记进 item**（UserRole+2），让"怎么画加粗"由**呈现层**决定 ——
    //     接口先把事实运到，UI 批再决定视觉（用户 2026-10-09 的"接口归地基、呈现归表层"）。
    if (!node.tooltip.empty()) {
        const QString tip = QString::fromStdString(node.tooltip);
        const int cols = std::max<int>(1, static_cast<int>(node.cells.size()));
        for (int c = 0; c < cols; ++c) item->setToolTip(c, tip);
    }
    item->setData(0, Qt::UserRole + 2, node.matched ? 1 : 0);
    if (node.has_children) {
        // ★ 只给"有箭头"的提示，**不塞占位子项**（塞了就会被算进行数/可见行，判据会跟着错）
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    if (node.checkable) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, node.checked ? Qt::Checked : Qt::Unchecked);
    } else {
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }
    if (!node.key.empty()) items_[node.key] = item;
    return item;
}

void FacadeTree::set_root(const std::vector<plug::UiFacade::TreeNodeSpec>& nodes) {
    clear_all();
    for (const plug::UiFacade::TreeNodeSpec& node : nodes) add_node(nullptr, node);
    // 声明里带勾选的 ⇒ 先把祖先态算平（不然"父是勾的、子没勾"这种声明会自相矛盾）
    updating_ = true;
    for (QTreeWidgetItemIterator it(this); *it != nullptr; ++it) {
        if ((*it)->checkState(0) == Qt::Checked) refresh_ancestors((*it)->parent());
    }
    updating_ = false;
}

void FacadeTree::set_children(const std::string& parent_key,
                              const std::vector<plug::UiFacade::TreeNodeSpec>& nodes) {
    QTreeWidgetItem* parent = item_of(parent_key);
    if (parent == nullptr) return;
    updating_ = true;
    // 重复填充同一行 ⇒ 先清掉旧的（插件在展开回调里可能被多次触发）
    const auto children = parent->takeChildren();
    for (QTreeWidgetItem* child : children) delete child;
    for (const plug::UiFacade::TreeNodeSpec& node : nodes) add_node(parent, node);
    // 声明里带勾选的 ⇒ 让父辈态跟着变
    for (const plug::UiFacade::TreeNodeSpec& node : nodes) {
        QTreeWidgetItem* item = item_of(node.key);
        if (item != nullptr && item->checkState(0) == Qt::Checked) refresh_ancestors(parent);
    }
    updating_ = false;
}

void FacadeTree::clear_all() {
    updating_ = true;
    clear();
    items_.clear();
    hits_ = 0;
    filter_.clear();
    updating_ = false;
}

QTreeWidgetItem* FacadeTree::item_of(const std::string& key) const {
    const auto it = items_.find(key);
    return it == items_.end() ? nullptr : it->second;
}

std::string FacadeTree::key_of(const QTreeWidgetItem* item) {
    if (item == nullptr) return std::string();
    return item->data(0, Qt::UserRole).toString().toStdString();
}

bool FacadeTree::item_visible(const QTreeWidgetItem* item) const {
    if (item == nullptr || item->isHidden()) return false;
    for (const QTreeWidgetItem* parent = item->parent(); parent != nullptr;
         parent = parent->parent()) {
        if (parent->isHidden() || !parent->isExpanded()) return false;
    }
    return true;
}

void FacadeTree::collect_visible(QTreeWidgetItem* item, std::vector<std::string>& out) const {
    if (item == nullptr) return;
    if (item->isHidden()) return;   // 被过滤掉的分支整支跳过
    out.push_back(key_of(item));
    if (!item->isExpanded()) return;
    for (int i = 0; i < item->childCount(); ++i) collect_visible(item->child(i), out);
}

std::vector<std::string> FacadeTree::visible_keys() const {
    std::vector<std::string> out;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        collect_visible(topLevelItem(i), out);
    }
    return out;
}

int FacadeTree::visible_row_count() const {
    return static_cast<int>(visible_keys().size());
}

void FacadeTree::collect_by_state(QTreeWidgetItem* item, Qt::CheckState state,
                                  std::vector<std::string>& out) const {
    if (item == nullptr) return;
    if (item->checkState(0) == state) out.push_back(key_of(item));
    for (int i = 0; i < item->childCount(); ++i) collect_by_state(item->child(i), state, out);
}

std::vector<std::string> FacadeTree::selected_keys() const {
    // ★ 按**树序**收集（`selectedItems()` 的顺序不作保证）；只取可见行之外的也照收
    std::vector<std::string> out;
    for (QTreeWidgetItemIterator it(const_cast<FacadeTree*>(this)); *it != nullptr; ++it) {
        if ((*it)->isSelected()) {
            const std::string key = key_of(*it);
            if (!key.empty()) out.push_back(key);
        }
    }
    return out;
}

std::vector<std::string> FacadeTree::checked_keys() const {
    std::vector<std::string> out;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        collect_by_state(topLevelItem(i), Qt::Checked, out);
    }
    return out;
}

std::vector<std::string> FacadeTree::partial_keys() const {
    std::vector<std::string> out;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        collect_by_state(topLevelItem(i), Qt::PartiallyChecked, out);
    }
    return out;
}

std::vector<std::string> FacadeTree::expanded_keys() const {
    std::vector<std::string> out;
    for (QTreeWidgetItemIterator it(const_cast<FacadeTree*>(this)); *it != nullptr; ++it) {
        if ((*it)->isExpanded()) out.push_back(key_of(*it));
    }
    return out;
}

bool FacadeTree::checked_of(const std::string& key) const {
    QTreeWidgetItem* item = item_of(key);
    return item != nullptr && item->checkState(0) == Qt::Checked;
}

// 勾选**向下**传播 + 祖先**向上**重算（三态）
void FacadeTree::refresh_ancestors(QTreeWidgetItem* item) {
    while (item != nullptr) {
        int checked = 0;
        int partial = 0;
        const int total = item->childCount();
        for (int i = 0; i < total; ++i) {
            const Qt::CheckState state = item->child(i)->checkState(0);
            if (state == Qt::Checked) ++checked;
            else if (state == Qt::PartiallyChecked) ++partial;
        }
        if (total == 0) {
            // 没子：不动它（叶子自己就是真相）
        } else if (checked == total) {
            item->setCheckState(0, Qt::Checked);
        } else if (checked == 0 && partial == 0) {
            item->setCheckState(0, Qt::Unchecked);
        } else {
            item->setCheckState(0, Qt::PartiallyChecked);
        }
        item = item->parent();
    }
}

void FacadeTree::set_checked(const std::string& key, bool checked) {
    QTreeWidgetItem* item = item_of(key);
    if (item == nullptr) return;
    updating_ = true;
    const Qt::CheckState state = checked ? Qt::Checked : Qt::Unchecked;
    if ((item->flags() & Qt::ItemIsUserCheckable) != 0) item->setCheckState(0, state);
    // 向下：子孙跟着走（全勾/全不勾，不留半态）
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* node) {
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if ((child->flags() & Qt::ItemIsUserCheckable) != 0) child->setCheckState(0, state);
            walk(child);
        }
    };
    walk(item);
    refresh_ancestors(item->parent());
    updating_ = false;
}

void FacadeTree::set_expanded(const std::string& key, bool expanded) {
    if (QTreeWidgetItem* item = item_of(key)) item->setExpanded(expanded);
}

void FacadeTree::select_key(const std::string& key) {
    QTreeWidgetItem* item = item_of(key);
    if (item == nullptr) return;
    scrollToItem(item);
    item->setSelected(true);
    setCurrentItem(item);
}

std::string FacadeTree::selected_key() const {
    const QList<QTreeWidgetItem*> selected = selectedItems();
    return selected.isEmpty() ? std::string() : key_of(selected.first());
}

void FacadeTree::apply_filter_to(QTreeWidgetItem* item, const QString& needle,
                                 bool parent_visible) {
    if (item == nullptr) return;
    bool self_match = needle.isEmpty();
    if (!self_match) {
        for (int column = 0; column < columnCount(); ++column) {
            if (item->text(column).contains(needle, Qt::CaseInsensitive)) {
                self_match = true;
                break;
            }
        }
    }
    if (self_match) ++hits_;
    // 命中 ⇒ 显示；未命中但有命中的子孙 ⇒ 也得显示（不然找不到）
    bool child_match = false;
    for (int i = 0; i < item->childCount(); ++i) {
        apply_filter_to(item->child(i), needle, parent_visible || self_match);
        if (!item->child(i)->isHidden()) child_match = true;
    }
    const bool visible = self_match || child_match || needle.isEmpty();
    item->setHidden(!visible);
}

void FacadeTree::set_filter(const std::string& text) {
    filter_ = QString::fromStdString(text);
    hits_ = 0;
    const bool empty = filter_.isEmpty();
    for (int i = 0; i < topLevelItemCount(); ++i) {
        apply_filter_to(topLevelItem(i), filter_, false);
    }
    if (empty) hits_ = 0;   // 空串 = "不过滤"，命中数记 0（与界面上的"无搜索"一致）
}


// ── ★★ P13：行尾动作（与 fs 页**共用**一份委托；口径见 `row_action_delegate.h`）────────
void FacadeTree::set_on_action(std::function<void(const std::string&)> fn) {
    on_action_ = std::move(fn);
    install_action_delegate();
}

void FacadeTree::set_action_mark(const std::string& mark) {
    action_mark_ = mark.empty() ? std::string("\u2715") : mark;
    install_action_delegate();
}

void FacadeTree::install_action_delegate() {
    const int columns = columnCount();
    if (columns <= 0) return;
    if (on_action_ == nullptr) return;   // 没登记回调 ⇒ 不画（免得出现「看得见点不着」的 ✕）
    const int column = columns - 1;      // 末列（吃剩余宽度的那一列 ⇒ ✕ 贴行尾）
    setItemDelegateForColumn(
        column,
        new RowActionDelegate(
            column, action_mark_,
            [](const QModelIndex& index) {
                if (!index.isValid()) return false;
                const QVariant flag = index.sibling(index.row(), 0).data(Qt::UserRole + 1);
                return flag.isValid() && flag.toInt() != 0;
            },
            [this](const QModelIndex& index) {
                const QTreeWidgetItem* item = itemFromIndex(index);
                if (item == nullptr || on_action_ == nullptr) return;
                on_action_(key_of(item));
            }));
}

}  // namespace xrkit::shell
