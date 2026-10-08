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

    // ★★ 2026-10-05（用户报告 #6「编码转换的中间那块区域并没有设计滚动条。」）：
    //   **内容总高**（＝ `总行数 × 行高`）。本来这个类够不到"自己要多大"——它只是个
    //   `QWidget`，高度由所在布局给（于是"装不下"的那部分行**永远没有机会显示**：
    //   `top_` 恒为 0，`visible_range()` 只覆盖视口那几行 ⇒ 后面的行用户在界面上够不着）。
    //   现在把内容高**说出来**，由外层 `ScrollViewport` 的 `min_body_height` 接住：
    //   装得下 ⇒ 与视口同高（条子不占位）；装不下 ⇒ 撑到内容高（条子出现，能滚）。
    //   ★ 判据见 smoke：`set_row_count(200)` ⇒ 条子存在且 `maximum()>0`；
    //     `set_row_count(3)` ⇒ 装得下 ⇒ **不占位**。
    int content_height() const { return total_ * row_height_; }
    void refresh();  // 按当前偏移/尺寸重算范围并复用行控件

    // ★★ P13-2b：**激活**（双击 / 回车）—— fs 页「双击包列表＝加载」要的就是它。
    //   回调给**行号**（与 `selected_row()` 同一坐标系：全体行里的下标，不是视口下标）。
    void set_on_activate(std::function<void(int)> fn) { on_activate_ = std::move(fn); }
    int activations() const { return activations_; }   // 判据用：真的回调过几次

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;   // ★ P13-2b
    void keyPressEvent(QKeyEvent* event) override;             // ★ P13-2b（回车）

private:
    // 把"内容总高"发布成 Qt 动态属性（外层页的 `sync_inner_scroll()` 读它当 min_body_height）
    void publish_content_height();

    int row_height_ = 20;
    int total_ = 0;
    int top_ = 0;
    int selected_ = -1;
    int provider_calls_ = 0;
    int recycle_count_ = 0;
    QPair<int, int> range_ = {0, 0};
    std::function<QString(int)> provider_;
    std::function<void(int)> on_activate_;
    int activations_ = 0;
    QVector<QLabel*> widgets_;
};

}  // namespace xrkit::shell
