#pragma once
// xrkit::shell —— 滚动视口（M2-2）：1.x 滚动一族三条闸门的 Qt 等价实现。
//
// 口径出处：`docs/2.0.0-行为口径对照表.md` ①（1.1~1.8）+ ③（fit 3.2/3.3/3.5）。**逐条照抄，
// 不许"Qt 里有现成的 QScrollArea 就用它"** —— 1.x 的钱就是花在这一族返工上的：
//
//   1.1 **没有滚动条就不许滚**，且在**控件级**拦（Tk 的 `Text`/`Listbox` 类绑定自带滚轮、
//       根本不看我们的条子；Qt 同理：`QPlainTextEdit`/`QAbstractScrollArea` 会自己吃掉并滚动）。
//   1.2 装得下就收起；滞回用 `>=1.0`（不是 0.999）。
//   1.3 **同一滚轮事件只能有一个滚动条动**：按**事件身份 serial** 认领（无 serial 退回时间窗口）。
//   1.4 声明必须在**确认能滚之后**；被抢先要**撤销自己刚才的滚动**。
//   1.5 全局路由：从事件目标沿父链上溯（≤60 层）找**第一个能滚的视口**；
//       **刻意没有**"指针在 Text/树上就跳过那一支"。
//   1.6 不许有裸滚动条：条子只能由本组件造（门禁静态扫描）。
//   1.7 两向不变量：能滚 ⇒ 条子必须看得见；装得下 ⇒ 不许占着条。
//   1.8 每次**显式整组重排**：横条(底) → 竖条(右) → 画布(expand 吃剩余)。

#include <QWidget>

class QResizeEvent;
class QScrollBar;
class QWheelEvent;

namespace xrkit::shell {

class ScrollViewport : public QWidget {
    // 1.5 的"沿父链找视口"要用 `qobject_cast`，而 Qt 的 qobject_cast 要求 Q_OBJECT
    //（AUTOMOC 已由 `qt_standard_project_setup()` 打开，加宏即可）。本类不发自定义信号。
    Q_OBJECT

public:
    // 1.x `fit`：自带条子的弹性内容 → Viewport；静态内容 → Content（3.2/3.3）
    enum class Fit { Content, Viewport };

    explicit ScrollViewport(QWidget* parent = nullptr);

    // 被滚控件：挂上来之后，本视口会给它（及其所有子控件）装滚轮过滤器
    void set_body(QWidget* body);
    QWidget* body() const { return body_; }

    void set_fit(Fit fit);
    Fit fit() const { return fit_; }

    // 1.x `minsize_y`：内容高**不许**用"内容请求高度"当内容高（3.3）
    void set_min_body_height(int height);
    int min_body_height() const { return min_body_height_; }

    QScrollBar* vbar() const { return vbar_; }
    QScrollBar* hbar() const { return hbar_; }

    // 1.x `bar_shown(sb)` 四看（Qt 等价）：
    //   ① 不是显式隐藏  ② 在自己的父链里可见（= 被加进了布局）
    //   ③ 已 mapped（真的上屏/上布局）  ④ 尺寸 > 1
    static bool bar_shown(const QScrollBar* bar);

    // 本视口现在**能不能滚**（= 1.2 的 `first<=0 且 last>=1` 的 Qt 等价：
    // 内容装得下 ⇒ 范围为空 ⇒ 不能滚）
    bool can_scroll_vertical() const;
    bool can_scroll_horizontal() const;

    int offset_vertical() const;
    int offset_horizontal() const;

    // 同步内容尺寸 + 重排（1.8）。任何尺寸/内容变化后调用。
    void sync_layout();

    // 滚轮处理（1.3/1.4/1.5 的合体）：从 `target` 起沿父链找第一个能滚的视口；
    // 命中本视口才消费，并做 serial 认领（被抢先则撤销本次滚动）。返回是否消费。
    static bool route_wheel(QWidget* target, QWheelEvent* event);

    // 1.x `wheel_claim(serial)`：同一事件只有一个认领者。0 表示"没有身份信息"。
    static bool claim_wheel(quint64 serial);
    static void reset_wheel_claims();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void install_wheel_filter(QWidget* widget);
    void apply_fit();

    QWidget* body_ = nullptr;
    QScrollBar* vbar_ = nullptr;
    QScrollBar* hbar_ = nullptr;
    Fit fit_ = Fit::Content;
    int min_body_height_ = 0;
    int body_w_ = 0;   // apply_fit 算出的"内容尺寸"（画布几何用它，别用视口尺寸覆盖）
    int body_h_ = 0;
    bool need_v_ = false;  // 本轮算出来的"要不要条"（几何用它，**别用 isVisible()**）
    bool need_h_ = false;
    bool syncing_ = false;
};

}  // namespace xrkit::shell
