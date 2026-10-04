#pragma once
// xrkit::shell —— "成对的树"统一装配件（2026-10-06 从 fs_page.cpp 抽出来共享）。
//
// 出处（用户原话，2026-10-02）：「为什么包树和文件树的形式不同。文件树的形式已经相当
// 成熟，包树直接引用树的统一函数就行啊。」
// ⇒ 装配（`apply_pair_tree_spec`）与单击判据（`click_on_indicator`）各抽成**一份**，
//   凡是"多列 + 可拖列 + 单击展开"的表都用它。
//
// ★ 2026-10-06（D2 批）为什么从 `fs_page.cpp` 的匿名命名空间搬到这里：XML 校对页的
//   结果表要从"单列 VirtualList、用 `\t` 拼文本"改成**按模式声明的多列**（1.x 是
//   `ttk.Treeview` 多列 + tag 配色）。用户的口径是「同类东西只维护一套组件」——
//   在 xml_page.cpp 里再写一份 `setSectionResizeMode` + `setStretchLastSection` 就是
//   第二份（而且最容易漏的就是"末列吃剩余宽度"这条，漏了右半边就是死区）。
//
// 列策略（用户 2026-09-29 明确要过，**不许**改成别的）：
//   · 每一列一律 `Interactive`（列之间能拖）；
//   · **末列**靠 `setStretchLastSection(true)` 吃剩余宽度；
//   · 谁都**不许**把某一列设成 `Stretch` —— 那会丢掉"列可拖"。
//   自检（smoke 第6条 + D2 批的 XML 页判据）正是按这三条判的。

#include <QList>
#include <QStringList>

class QTreeWidget;
class QTreeWidgetItem;

namespace xrkit::shell {

struct PairTreeSpec {
    QStringList headers;
    QList<int> widths;        // 每列**初始宽**（设计稿像素，内部过 px()）
    int min_height = 0;       // 0 ⇒ 不设
    bool accept_drops = false;
};

// ★ 装配**唯一一份**：列/行高/多选/拖放/最小高/根装饰/单击才展开。
void apply_pair_tree_spec(QTreeWidget* view, const PairTreeSpec& spec);

// 光标是不是落在"行首那一小块"（勾选框 / 展开箭头）上：
// 只有**光标确实在这一行里、且落在那小块上**才算；离屏/自检里光标不在行上 ⇒ 视为点
// **条目身体**（否则自检没法验"单击展开"这条）。
bool click_on_indicator(QTreeWidget* view, QTreeWidgetItem* item);

}  // namespace xrkit::shell
