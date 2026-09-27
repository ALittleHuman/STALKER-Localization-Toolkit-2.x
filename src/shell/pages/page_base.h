#pragma once
// xrkit::shell —— 页基类（M2-6a）：对照表 ⑤（页内滚动 + 标签条）的落地形态。
//
// ⑤ 的三条口径逐条照抄（出处见 `docs/2.0.0-行为口径对照表.md`）：
//   5.1 **Notebook 绝不能被包进滚动视口**（否则一滚标签条被滚走、主导航消失）；
//       每页**自己**套一层 `ScrollViewport(fit="content", minsize_y=page_min_h())`。
//       Hub 的页列表/页栈在页**外面**，谁都不许把它塞进视口 —— 自检会沿父链查。
//   5.2 `page_min_h() = px(430)` 必须是**函数**（`px()` 依赖缩放就绪）。
//       写成常量在 150% 屏上照样返回 430，等于把"按 96 DPI 定的值"钉死在别的缩放上。
//   5.3 参数名是 **`minsize_y`**（1.x 里写成 `min_y=` 直接 `TclError`、程序都起不来）。
//       C++ 没有关键字参数，所以把这条契约做成**具名 setter** `set_min_body_height()`，
//       并且让"忘了设"变成可判定的红：`ScrollViewport` 的默认值是 0，不等于 `page_min_h()`。
//
// 另外照抄对照表 §2.0 规则 2：**操作栏先放且贴底**。两条实质约束：
//   · 操作栏**不许进视口**（进了就会被滚走，用户滚到中间就找不到"开始"）；
//   · 窗口压到 `page_min_h()` 时操作栏**必须还在**（不能被内容挤没）。
// 两条都在自检里量，不靠"看着对"。

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace xrkit::shell {

class ScrollViewport;

// 5.2：页最小高 = `px(430)`。**函数**，不是常量（缩放变了值就得变）。
int page_min_h();

class PageBase : public QWidget {
public:
    PageBase(const QString& page_id, const QString& title, QWidget* parent = nullptr);

    QString page_id() const { return page_id_; }
    QString page_title() const { return title_; }

    QVBoxLayout* outer_layout() const { return outer_; }
    QLabel* header() const { return header_; }
    // ★ 2026-09-27 界面重构：页标题下面加一句**副标题**（说明本页能干什么），
    //   内容区包进**卡片**（QFrame#card：边框+圆角+内边距）——原来六页都是
    //   "标题/说明/操作栏/内容区同一平面 + 裸列表铺满"。自检用它做几何断言。
    QLabel* subtitle() const { return subtitle_; }
    // ★ 排版口径（真机反馈'太挤'）：标题与说明**合并成一行**的容器
    QWidget* header_row() const { return header_row_; }
    QWidget* card() const { return card_; }
    void set_subtitle(const QString& text);
    ScrollViewport* viewport() const { return viewport_; }
    QWidget* body() const { return body_; }
    QVBoxLayout* body_layout() const { return body_layout_; }
    QWidget* action_bar() const { return action_bar_; }
    QHBoxLayout* action_layout() const { return action_layout_; }

    // 往操作栏里加一个按钮（右对齐；操作栏贴底且不进视口）
    QPushButton* add_action(const QString& text);

    // 5.3 的判据：页自己的视口是否**真的**按 `page_min_h()` 设了 minsize
    //（自检用它做"忘了设就红"的断言，而不是自己抄一份期望值）
    bool minsize_ok() const;

    // ② 点页面空白 ⇒ 把输入框焦点收回页面（用户真机："点空白并不能取消，还是一闪一闪"）
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString page_id_;
    QString title_;
    QVBoxLayout* outer_ = nullptr;
    QLabel* header_ = nullptr;
    QWidget* header_row_ = nullptr;  // 标题与说明同一行（真机反馈：别叠薄行）
    QLabel* subtitle_ = nullptr;
    QWidget* card_ = nullptr;
    ScrollViewport* viewport_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
    QWidget* action_bar_ = nullptr;
    QHBoxLayout* action_layout_ = nullptr;
};

}  // namespace xrkit::shell
