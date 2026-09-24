#pragma once
// xrkit::shell —— 可分栏容器（M2-3）。口径出处：对照表 ②（2.1~2.7）+ ④（4.2/4.3 的收起语义）。
//
// 用户原话（2026-09-21，勿改写）："那就还是不实时移动吧。松手一次重排。但是拖拽的时候预览框
// 不能只是一条线，而是要把那部分相应的背景也进行填充，不然很难看。" ⇒
//   2.1 拖动期**一步几何都不改**，只画"填色带预览"（被拉长那一格的底色 + 1 px 亮线），**松手一次重排**；
//   2.3 `SASH_HIT = 6`（96 DPI 设计值），实例化 `max(6, px(5))`；
//   2.4 pane 的 minsize 自实现：总空间不足时**让后面的 pane 先挨饿**；
//   2.5 重排去抖 `CLAMP_DEBOUNCE_MS = 120`；
//   4.2 **可完全收起**且改窗口尺寸**不弹回**（把该 pane 的最小值从 `_mins` 撤下存进 `_saved_mins`）；
//   4.3 收起顺序：**先记用户意图（collapsed 标记 + 撤下 minsize）再 clamp**（反了会把收起的栏立刻推回）。

#include <QVector>
#include <QWidget>

class QPaintEvent;
class QResizeEvent;
class QTimerEvent;

namespace xrkit::shell {

class SplitPane : public QWidget {
    Q_OBJECT

public:
    enum class Orient { Horizontal, Vertical };

    explicit SplitPane(Orient orient, QWidget* parent = nullptr);

    int add_pane(QWidget* pane, int minsize);
    int pane_count() const { return static_cast<int>(sizes_.size()); }
    QWidget* pane(int index) const;
    int pane_size(int index) const;
    int minsize(int index) const;

    // 2.3：命中区（96 DPI 设计值 6，实例化取 max(6, px(5))）
    int sash_hit() const;
    int sash_pos(int index) const;   // 第 index 条 sash 的当前位置（-1 = 没有）
    int sash_at(int pos) const;      // 命中测试：返回 sash 索引，没有则 -1

    // 拖动（2.1/2.2）：几何在拖动期**不变**，只更新预览
    bool begin_drag(int pos);
    void update_drag(int pos);
    void end_drag(bool commit);      // commit=true ⇒ **松手一次重排**
    bool dragging() const { return dragging_; }
    int preview_pos() const { return preview_; }

    // 收起 / 展开（4.2/4.3）
    void set_collapsed(int index, bool collapsed);
    bool is_collapsed(int index) const;

    // 2.5 去抖
    void set_clamp_debounce_ms(int ms);
    int clamp_debounce_ms() const { return debounce_ms_; }
    int clamp_count() const { return clamp_count_; }   // 自检用：合并后的 clamp 次数
    void request_clamp();                              // 触发一次（带去抖）的重排请求

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void timerEvent(QTimerEvent* event) override;

private:
    int axis_total() const;                 // 可分配空间（扣掉 sash 占位）
    void relayout();                        // 立即重排（几何落地）
    void clamp_now();                       // 让后面的 pane 先挨饿
    int sash_thickness() const;

    Orient orient_;
    QVector<QWidget*> panes_;
    QVector<int> mins_;        // 当前生效的最小值（收起时撤下）
    QVector<int> saved_mins_;  // 收起前的原最小值（4.2）
    QVector<bool> collapsed_;
    QVector<int> sizes_;       // 沿轴方向的尺寸
    bool dragging_ = false;
    int drag_sash_ = -1;
    int preview_ = -1;
    int debounce_ms_ = 120;
    int timer_id_ = 0;
    int clamp_count_ = 0;
};

}  // namespace xrkit::shell
