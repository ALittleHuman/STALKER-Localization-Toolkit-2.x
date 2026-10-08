#pragma once
// xrkit::shell —— **门面树**（插件改装 P6）：宿主给插件的那棵树的**唯一实现**。
//
// 与 `VirtualList` 同族：渲染 / 展开 / **勾选传播** / 过滤 / 计数全在宿主，
// 插件只做两件事：**声明节点** + **收回调**。
//
// 口径（逐条对齐 fs 页那棵树的**用户可见语义**，但**不共用它的实现** —— fs 页现在把
// 勾选传播/委托/规格都写在自己页面里；等 fs 页迁到门面时，它的那份会并到这里来，
// 那时才是真正的"只许一份"。本批把这条边界写在注释里，免得以后误以为已经统一了）：
//   · **懒建**：`has_children` 的行只画箭头；用户真展开时才 `on_expand(key)`，
//     插件再 `set_children(key, …)`。
//   · **三态勾选**：勾/取消**向下**传播到子孙，再**向上**重算祖先（全勾=勾、部分=半态）；
//     用户改勾选 ⇒ `on_check(key, checked)`（key 是被点的那一行；要看全貌就查 `checked_keys()`）。
//   · **过滤**：任一列包含子串即命中；命中行连**祖先**一起显示，其余隐藏；
//     `filter_hits()` 与界面上的"命中数"同源。
//   · **不做**：行尾 ✕（那个委托要从 fs 页抽出来共用，随 fs 页迁移一起做）、右键菜单。

#include "xrkit/engine/plugin/widget_facade.h"

#include <QTreeWidget>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace xrkit::shell {

namespace plug = xrkit::plugin;

class FacadeTree : public QTreeWidget {
    Q_OBJECT

public:
    explicit FacadeTree(QWidget* parent = nullptr);

    // ── 声明 ────────────────────────────────────────────────────────────────
    void set_columns(const std::vector<plug::UiFacade::TreeColumn>& columns);
    void set_root(const std::vector<plug::UiFacade::TreeNodeSpec>& nodes);   // 清空重建
    void set_children(const std::string& parent_key,
                      const std::vector<plug::UiFacade::TreeNodeSpec>& nodes);
    void clear_all();

    // ── 状态（判据也靠它们）────────────────────────────────────────────────
    int visible_row_count() const;
    std::vector<std::string> visible_keys() const;      // 可见行（祖先都展开 + 未被过滤掉）
    std::vector<std::string> expanded_keys() const;
    std::vector<std::string> checked_keys() const;
    std::vector<std::string> partial_keys() const;
    bool checked_of(const std::string& key) const;
    void set_checked(const std::string& key, bool checked);
    void set_expanded(const std::string& key, bool expanded);
    void select_key(const std::string& key);
    std::string selected_key() const;

    // ── 过滤 ────────────────────────────────────────────────────────────────
    void set_filter(const std::string& text);
    int filter_hits() const { return hits_; }

    // ── 回调（L2.5；都在宿主 UI 线程上被调）────────────────────────────────
    void set_on_expand(std::function<void(const std::string&)> fn) { on_expand_ = std::move(fn); }
    void set_on_check(std::function<void(const std::string&, bool)> fn) {
        on_check_ = std::move(fn);
    }
    void set_on_activate(std::function<void(const std::string&)> fn) {
        on_activate_ = std::move(fn);
    }
    // ★★ P13：行尾动作（fs 页的 ✕）—— 实现见 `row_action_delegate.h`
    void set_on_action(std::function<void(const std::string&)> fn);
    void set_action_mark(const std::string& mark);

    // 自检/内部用：key → 项
    QTreeWidgetItem* item_of(const std::string& key) const;
    static std::string key_of(const QTreeWidgetItem* item);

private:
    QTreeWidgetItem* add_node(QTreeWidgetItem* parent,
                              const plug::UiFacade::TreeNodeSpec& node);
    void refresh_ancestors(QTreeWidgetItem* item);
    void apply_filter_to(QTreeWidgetItem* item, const QString& needle, bool parent_visible);
    bool item_visible(const QTreeWidgetItem* item) const;
    void collect_visible(QTreeWidgetItem* item, std::vector<std::string>& out) const;
    void collect_by_state(QTreeWidgetItem* item, Qt::CheckState state,
                          std::vector<std::string>& out) const;

    std::map<std::string, QTreeWidgetItem*> items_;
    std::function<void(const std::string&)> on_expand_;
    std::function<void(const std::string&)> on_action_;
    std::string action_mark_;
    void install_action_delegate();
    std::function<void(const std::string&, bool)> on_check_;
    std::function<void(const std::string&)> on_activate_;
    QString filter_;
    int hits_ = 0;
    bool updating_ = false;   // 程序化改勾选时挡住 `itemChanged` 的二次传播
};

}  // namespace xrkit::shell
