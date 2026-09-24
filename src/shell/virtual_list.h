#pragma once
// xrkit::shell —— 虚拟化列表（M2-5）。口径出处：对照表 ⑩（10.2 + 10.3 的同族规则）。
//
// 1.x `xml_compare_app` 的明细 id 列表：**只为视口内的行建控件**，范围规则是
//     `first = top // h`
//     `last  = min(total, (top + view_h) // h + 2)`   ← **视口 + 2 行 overscan**，无硬编码上限
// 判据：**换选一行与差异条数无关**（实测 7~10 ms）。
//
// 两条设计约束：
//   · 本组件**不自带滚动条**（对照表 1.6：条子只能由 `ScrollViewport` 造）—— 它只接受
//     一个滚动偏移，由外层视口驱动；
//   · 行文本只对**视口内的行**取（10.3 是同一条原则：`detail` 列不许对全量调 builder），
//     所以 `provider_calls()` 应当 ≈ 视口行数，而**与 total 无关**。

#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>
#include <functional>

class QLabel;

namespace xrkit::shell {

class VirtualList : public QWidget {
    Q_OBJECT

public:
    explicit VirtualList(QWidget* parent = nullptr);

    void set_row_height(int height);
    int row_height() const { return row_height_; }

    void set_row_count(int total);
    int row_count() const { return total_; }

    // 10.3 同族：行文本只为**视口内**的行取
    void set_row_text_provider(std::function<QString(int)> provider);
    int provider_calls() const { return provider_calls_; }

    // 取某一行"会显示成什么"（自检核对行文本用；不改变池子与选中的状态）
    QString row_text(int row) const;

    // 10.2 的范围规则（静态版便于逐组对拍）
    static QPair<int, int> visible_range(int top, int view_h, int row_h, int total);
    QPair<int, int> visible_range() const;

    void set_scroll_top(int top);
    int scroll_top() const { return top_; }

    // 真正建了控件的行数（应当 = 视口行数 + 2，而**不是** total）
    int built_count() const { return widgets_.size(); }
    int recycle_count() const { return recycle_count_; }

    void set_selected_row(int row);
    int selected_row() const { return selected_; }

    void refresh();  // 按当前偏移/尺寸重算范围并复用行控件

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    int row_height_ = 20;
    int total_ = 0;
    int top_ = 0;
    int selected_ = -1;
    int provider_calls_ = 0;
    int recycle_count_ = 0;
    QPair<int, int> range_ = {0, 0};
    std::function<QString(int)> provider_;
    QVector<QLabel*> widgets_;
};

}  // namespace xrkit::shell
