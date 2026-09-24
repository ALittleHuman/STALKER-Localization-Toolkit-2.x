#include "scroll_viewport.h"

#include "theme.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QChildEvent>
#include <QEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QDateTime>
#include <QWheelEvent>
#include <algorithm>
#include <QWidget>

namespace xrkit::shell {
namespace {

// 1.5：沿父链上溯的上限（1.x 是 ≤60 层）
constexpr int kMaxParentHops = 60;
// 1.3：没有事件身份（serial）时退回的时间窗口
constexpr qint64 kClaimWindowMs = 60;

// 同一滚轮事件只允许一个条子动：认领表（serial → 是否已被认领）
quint64 g_last_serial = 0;
qint64 g_last_claim_ms = 0;

// 1.3：按事件身份取 serial；Qt 里最稳的身份就是事件对象本身的地址
//（一个 QWheelEvent 实例只会被派发一次；沿父链冒泡时地址不变）。
quint64 serial_of(const QWheelEvent* event) {
    return static_cast<quint64>(reinterpret_cast<quintptr>(event));
}

}  // namespace

ScrollViewport::ScrollViewport(QWidget* parent) : QWidget(parent) {
    // 1.6：条子只能由本组件造（门禁静态扫描 `new QScrollBar(`）
    vbar_ = new QScrollBar(Qt::Vertical, this);
    hbar_ = new QScrollBar(Qt::Horizontal, this);
    vbar_->setObjectName(QStringLiteral("viewport_vbar"));
    hbar_->setObjectName(QStringLiteral("viewport_hbar"));
    vbar_->hide();
    hbar_->hide();
}

void ScrollViewport::install_wheel_filter(QWidget* widget) {
    if (widget == nullptr) return;
    widget->installEventFilter(this);
    for (QObject* child : widget->children()) {
        if (auto* child_widget = qobject_cast<QWidget*>(child)) install_wheel_filter(child_widget);
    }
}

void ScrollViewport::set_body(QWidget* body) {
    if (body_ == body) return;
    body_ = body;
    if (body_ != nullptr) {
        body_->setParent(this);
        // 控件级拦：给 body 及其所有子控件装过滤器（1.1）
        install_wheel_filter(body_);
    }
    sync_layout();
}

void ScrollViewport::set_fit(Fit fit) {
    fit_ = fit;
    sync_layout();
}

void ScrollViewport::set_min_body_height(int height) {
    min_body_height_ = height > 0 ? height : 0;
    sync_layout();
}

bool ScrollViewport::bar_shown(const QScrollBar* bar) {
    if (bar == nullptr) return false;
    // ① 不是显式隐藏（`isHidden()` = 自己或祖先被 hide）
    if (bar->isHidden()) return false;
    // ② 在自己的父链里可见（= 真的被加进了布局/层级，而不是"代码里不算滚动条"）
    if (!bar->isVisibleTo(bar->parentWidget())) return false;
    // ③ 已 mapped
    if (!bar->isVisible()) return false;
    // ④ 尺寸 > 1（被压成 0/1 px 的条子等于不存在）
    return bar->width() > 1 && bar->height() > 1;
}

bool ScrollViewport::can_scroll_vertical() const {
    return vbar_ != nullptr && vbar_->maximum() > vbar_->minimum();
}

bool ScrollViewport::can_scroll_horizontal() const {
    return hbar_ != nullptr && hbar_->maximum() > hbar_->minimum();
}

int ScrollViewport::offset_vertical() const { return vbar_ != nullptr ? vbar_->value() : 0; }
int ScrollViewport::offset_horizontal() const { return hbar_ != nullptr ? hbar_->value() : 0; }

void ScrollViewport::apply_fit() {
    if (body_ == nullptr) return;
    const int bar = px(12);
    const int bar_w = need_v_ ? bar : 0;
    const int bar_h = need_h_ ? bar : 0;

    // 内容"请求尺寸"：Qt 里 = `sizeHint()`（有布局的容器会反映子控件）；裸 QWidget 的
    // sizeHint 是无效值（-1）⇒ 退回 `minimumSizeHint()`（设过 minimumSize 才有效）。
    // 语义对齐 1.x 的 `winfo_reqheight()/reqwidth()`（几何管理器算出的请求尺寸）。
    const auto request_size = [&]() {
        QSize hint = body_->sizeHint();
        const QSize min_hint = body_->minimumSizeHint();
        if (hint.width() <= 0) hint.setWidth(min_hint.width());
        if (hint.height() <= 0) hint.setHeight(min_hint.height());
        return hint;
    };

    // 两遍：条子的显隐会改变可用视口 ⇒ 先按"没有条子"算，需要条子再算一次（1.8 的精神）
    const auto compute = [&](int avail_w, int avail_h) {
        const QSize req = request_size();
        if (fit_ == Fit::Content) {
            // 3.2 `fit="content"`：内容高 = max(内容请求高, minsize_y, 视口高)
            body_h_ = std::max({req.height(), min_body_height_, avail_h});
        } else {
            // 3.2 `fit="viewport"`：内容高 = max(minsize_y, 视口高) —— **不掺**内容请求高
            body_h_ = std::max(min_body_height_, avail_h);
        }
        body_w_ = std::max(req.width(), avail_w);
    };

    const auto avail_w = [&](bool need_v) { return std::max(0, width() - (need_v ? bar : 0)); };
    const auto avail_h = [&](bool need_h) { return std::max(0, height() - (need_h ? bar : 0)); };

    compute(std::max(0, width() - bar_w), std::max(0, height() - bar_h));
    bool need_v = body_h_ > avail_h(false);
    bool need_h = body_w_ > avail_w(false);
    if (need_v || need_h) {
        compute(avail_w(need_v), avail_h(need_h));
        need_v = body_h_ > avail_h(need_h);
        need_h = body_w_ > avail_w(need_v);
    }
    body_w_ = std::max(body_w_, avail_w(need_v));
    body_h_ = std::max(body_h_, avail_h(need_h));

    // 1.2：装得下就收起（滞回 >= 1.0）；否则必须看得见条
    need_v_ = need_v;
    need_h_ = need_h;
    vbar_->setRange(0, need_v ? body_h_ - avail_h(need_h) : 0);
    vbar_->setPageStep(avail_h(need_h));
    vbar_->setSingleStep(px(24));
    hbar_->setRange(0, need_h ? body_w_ - avail_w(need_v) : 0);
    hbar_->setPageStep(avail_w(need_v));
    hbar_->setSingleStep(px(24));
    if (!need_v) vbar_->setValue(0);
    if (!need_h) hbar_->setValue(0);
    vbar_->setVisible(need_v);
    hbar_->setVisible(need_h);
}

void ScrollViewport::sync_layout() {
    if (syncing_) return;  // 防重入（几何变化会再触发 resizeEvent）
    syncing_ = true;
    apply_fit();

    // 1.8：**显式整组重排**（竖条(右) → 横条(底) → 画布(expand 吃剩余)），不靠记忆
    // ★ 这里必须用 `need_v_/need_h_`（本轮算出来的），**不能**用 `isVisible()` ——
    //   父窗口还没 show 时 `isVisible()` 恒为假，会把条子几何算成 0 宽（实测：
    //   `bar_shown` 的第四看当场红：0x120）。这正是"条子被压成 0/1 px 等于不存在"那条。
    const int bar = px(12);
    const int vw = need_v_ ? bar : 0;
    const int hh = need_h_ ? bar : 0;
    const int view_w = std::max(0, width() - vw);
    const int view_h = std::max(0, height() - hh);
    if (body_ != nullptr) {
        // 画布位置 = 负偏移（等价 Tk 里 canvas 的滑动）
        body_->setGeometry(-hbar_->value(), -vbar_->value(), body_w_, body_h_);
    }
    vbar_->setGeometry(view_w, 0, vw, view_h);
    hbar_->setGeometry(0, view_h, view_w, hh);
    syncing_ = false;
}

void ScrollViewport::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    sync_layout();
}

bool ScrollViewport::claim_wheel(quint64 serial) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (serial != 0) {
        if (g_last_serial == serial) return false;  // 同一事件已被别的条子认领
        g_last_serial = serial;
        g_last_claim_ms = now;
        return true;
    }
    // 没有身份信息：退回时间窗口（1.3）
    if (now - g_last_claim_ms < kClaimWindowMs) return false;
    g_last_claim_ms = now;
    return true;
}

void ScrollViewport::reset_wheel_claims() {
    g_last_serial = 0;
    g_last_claim_ms = 0;
}

bool ScrollViewport::route_wheel(QWidget* target, QWheelEvent* event) {
    if (target == nullptr || event == nullptr) return false;
    const int dy = event->angleDelta().y();
    const int dx = event->angleDelta().x();
    if (dy == 0 && dx == 0) return false;

    // 1.5：从事件目标沿父链上溯，找**第一个能滚的**视口（≤60 层）
    QWidget* widget = target;
    for (int hop = 0; widget != nullptr && hop <= kMaxParentHops; ++hop) {
        if (auto* viewport = qobject_cast<ScrollViewport*>(widget)) {
            const bool can_v = (dy != 0) && viewport->can_scroll_vertical();
            const bool can_h = (dx != 0) && viewport->can_scroll_horizontal();
            if (!can_v && !can_h) {
                // 装得下 ⇒ 这个视口不许滚（1.1/1.7），继续往上找
                widget = widget->parentWidget();
                continue;
            }
            QScrollBar* bar = can_v ? viewport->vbar() : viewport->hbar();
            const int step = (can_v ? dy : dx) > 0 ? -1 : 1;
            const int before = bar->value();
            const int delta = px(24) * 3 * step;  // 一格 = 3 行，与 1.x 的滚轮步进同量级
            bar->setValue(before + delta);

            // 1.4：声明在**确认能滚之后**；被抢先就撤销自己刚才的滚动
            if (bar->value() != before && !claim_wheel(serial_of(event))) {
                bar->setValue(before);
                return true;  // 事件已被（别人）消费掉，别再往上滚
            }
            if (bar->value() != before) {
                // 滚动了 ⇒ 同步画布位置
                if (auto* body = viewport->body()) {
                    body->move(-viewport->offset_horizontal(), -viewport->offset_vertical());
                }
            }
            event->accept();
            return true;
        }
        widget = widget->parentWidget();
    }

    // ② 链上**没有能滚的视口**时，还有一种必须在这一层拦住的东西：
    //    "会自己滚的原生滚动区"（Qt 里的 `QAbstractScrollArea` 子类：QPlainTextEdit、QListView…）。
    //    1.1 的口径是**控件级拦** —— 它的条子不可见时，不许它自己滚（否则就是"没有滚动条也在滚"）。
    //    条子**看得见**时则"完全不干预"，让它按原生方式滚（1.x 同一句话）。
    for (QWidget* walk = target; walk != nullptr; walk = walk->parentWidget()) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(walk)) {
            QScrollBar* bar = (dy != 0) ? area->verticalScrollBar() : area->horizontalScrollBar();
            if (bar != nullptr && !bar_shown(bar)) {
                event->accept();
                return true;  // 吃事件：挡住类绑定的自滚
            }
            return false;  // 看得见 ⇒ 完全不干预
        }
        if (qobject_cast<ScrollViewport*>(walk) != nullptr) break;  // 到了视口边界就停
    }
    return false;  // 没有能滚的视口：不吃事件（交给默认处理）
}

bool ScrollViewport::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ChildAdded) {
        // 新加进来的子控件也要被"控件级拦"覆盖（否则运行期加进去的 Text 会自己滚）
        auto* child_event = static_cast<QChildEvent*>(event);
        if (auto* child_widget = qobject_cast<QWidget*>(child_event->child())) {
            if (child_widget != vbar_ && child_widget != hbar_) {
                install_wheel_filter(child_widget);
            }
        }
        return false;
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        QWidget* target = qobject_cast<QWidget*>(watched);
        if (target == nullptr) target = this;
        if (route_wheel(target, wheel)) return true;  // 消费掉：别让 Tk/Qt 的默认绑定再滚一次
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace xrkit::shell
