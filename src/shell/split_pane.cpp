#include "split_pane.h"

#include "theme.h"

#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimerEvent>
#include <algorithm>
#include <climits>

namespace xrkit::shell {
namespace {
constexpr int kSashThickness = 4;   // sash 本体厚度（px() 缩放后）
constexpr int kSashHitDesign = 6;   // 2.3：96 DPI 设计值

QColor role_color(const char* name, const char* fallback) {
    const char* hex = xrkit::core::theme_color_current(name);
    const QColor color(QString::fromUtf8(hex != nullptr ? hex : fallback));
    return color.isValid() ? color : QColor(QString::fromUtf8(fallback));
}

}  // namespace

SplitPane::SplitPane(Orient orient, QWidget* parent) : QWidget(parent), orient_(orient) {}

int SplitPane::sash_thickness() const { return px(kSashThickness); }

int SplitPane::sash_hit() const { return std::max(kSashHitDesign, px(5)); }  // 2.3

int SplitPane::add_pane(QWidget* pane, int minsize) {
    if (pane == nullptr) return -1;
    pane->setParent(this);
    // ★ 新格子加入时，先把**已有格子收敛到各自的最小值**：剩下的空间由新格子吃
    //   （它是弹性项）。否则先加的那格会独吞整幅空间，后加的只剩 min ⇒ 边界拖不动
    //   （实测：2 格分栏里 sash 拖不动，只有"后面的先挨饿"在起作用）。
    for (int i = 0; i < static_cast<int>(sizes_.size()); ++i) {
        if (!collapsed_[i]) sizes_[i] = mins_[i];
    }
    panes_.push_back(pane);
    mins_.push_back(std::max(0, minsize));
    maxs_.push_back(0);  // 默认不限
    saved_mins_.push_back(std::max(0, minsize));
    collapsed_.push_back(false);
    sizes_.push_back(0);
    pane->show();
    relayout();
    return static_cast<int>(panes_.size()) - 1;
}

QWidget* SplitPane::pane(int index) const {
    if (index < 0 || index >= panes_.size()) return nullptr;
    return panes_[index];
}

int SplitPane::pane_size(int index) const {
    if (index < 0 || index >= sizes_.size()) return 0;
    return sizes_[index];
}

int SplitPane::minsize(int index) const {
    if (index < 0 || index >= mins_.size()) return 0;
    return mins_[index];
}

void SplitPane::set_max_size(int index, int size) {
    if (index < 0 || index >= static_cast<int>(maxs_.size())) return;
    maxs_[index] = std::max(0, size);
    relayout();
}

int SplitPane::maxsize(int index) const {
    if (index < 0 || index >= static_cast<int>(maxs_.size())) return 0;
    return maxs_[index];
}

int SplitPane::axis_total() const {
    const int total = (orient_ == Orient::Horizontal) ? width() : height();
    const int n = static_cast<int>(panes_.size());
    const int sashes = std::max(0, n - 1) * sash_thickness();
    return std::max(0, total - sashes);
}

int SplitPane::sash_pos(int index) const {
    if (index < 0 || index >= static_cast<int>(panes_.size()) - 1) return -1;
    int pos = 0;
    for (int i = 0; i <= index; ++i) pos += sizes_[i] + sash_thickness();
    return pos - sash_thickness();  // sash 本体起点
}

int SplitPane::sash_at(int pos) const {
    const int hit = sash_hit();
    for (int i = 0; i < static_cast<int>(panes_.size()) - 1; ++i) {
        const int start = sash_pos(i);
        if (pos >= start - hit && pos <= start + sash_thickness() + hit) return i;
    }
    return -1;
}

// 2.4：总空间不足时**让后面的 pane 先挨饿** —— 先给每个 pane 保底（floor），
//      再从后往前从"有余量的 pane"身上收回差额（收回顺序也是后面的先挨饿）；
//      空间有余则给**最大的那个 pane**（天然的弹性项）。
// ★ 弹性项不能简单取"最后一个没收起的"：展开一个刚收起的 pane 时，最后那个正是它自己，
//   会把差额从它身上扣回去（实测：展开后又变 0）。收回/发放必须看"余量"。
// 4.3：收起的 pane 不参与（floor=0、size=0），所以本函数必须在**记完用户意图之后**调用。
void SplitPane::clamp_now() {
    const int total = axis_total();
    const int n = static_cast<int>(sizes_.size());
    QVector<int> floor_i(n, 0);
    for (int i = 0; i < n; ++i) {
        floor_i[i] = collapsed_[i] ? 0 : mins_[i];
        if (collapsed_[i]) sizes_[i] = 0;
        if (sizes_[i] < floor_i[i]) sizes_[i] = floor_i[i];
        // ★ 2026-09-27（用户实测"排版有很大问题"）：**每格上限要在每次 clamp 时先压一遍**。
        //   `sizes_` 是跨次保留的：只在"富余分配"里按上限发不够 —— 上一次给多了之后，
        //   下一次会走**收窄分支**，而收窄分支不会把超限的格子压回去（实测日志格 636 一直不收敛）。
        if (i < static_cast<int>(maxs_.size()) && maxs_[i] > 0 && sizes_[i] > maxs_[i]) {
            sizes_[i] = maxs_[i];
        }
    }
    int sum = 0;
    for (int v : sizes_) sum += v;

    if (sum > total) {
        int excess = sum - total;
        // 从后往前，从有余量的 pane 身上收（后面的先挨饿）
        for (int i = n - 1; i >= 0 && excess > 0; --i) {
            const int surplus = sizes_[i] - floor_i[i];
            if (surplus <= 0) continue;
            const int take = std::min(surplus, excess);
            sizes_[i] -= take;
            excess -= take;
        }
        // 还不够（连 floor 都超）：从后往前压到 0 —— 后面的先挨饿
        for (int i = n - 1; i >= 0 && excess > 0; --i) {
            const int take = std::min(sizes_[i], excess);
            sizes_[i] -= take;
            excess -= take;
        }
    } else if (sum < total) {
        // 空间有余 ⇒ 给**最后一个没收起的 pane**（1.x/Tk 里那个 `expand=True` 的弹性项）。
        // ★ 不能取"最大的那个"：add_pane 是逐个来的，第一格会先独吞整幅高度，
        //   之后再来一格就变成"第一格有余量、第二格只有 min" ⇒ 拖不动边界（实测踩到）。
        int give = total - sum;
        int elastic = -1;
        for (int i = n - 1; i >= 0; --i) {
            if (!collapsed_[i]) {
                elastic = i;
                break;
            }
        }
        if (elastic < 0) {
            for (int i = n - 1; i >= 0; --i) {
                if (sizes_[i] > 0) {
                    elastic = i;
                    break;
                }
            }
        }
        // ★ 2026-09-27：富余分配**按每格上限发放**（0 = 不限），而不是"一股脑给最后一格"。
        //   顺序仍是"后面的先吃饱"（与原语义一致），只是吃到上限就换下一格（继续往前）。
        for (int i = n - 1; i >= 0 && give > 0; --i) {
            if (collapsed_[i]) continue;
            const int cap = (i < static_cast<int>(maxs_.size()) && maxs_[i] > 0) ? maxs_[i]
                                                                               : INT_MAX;
            const int room = cap - sizes_[i];
            if (room <= 0) continue;
            const int take = std::min(room, give);
            sizes_[i] += take;
            give -= take;
        }
        // 所有格子都到顶了还有剩 ⇒ 还是给最后一格，保证高度用满（保持旧行为兜底）
        if (give > 0 && elastic >= 0) sizes_[elastic] += give;
    }
    ++clamp_count_;
}

void SplitPane::relayout() {
    clamp_now();
    const int th = sash_thickness();
    int pos = 0;
    for (int i = 0; i < static_cast<int>(panes_.size()); ++i) {
        if (orient_ == Orient::Horizontal) {
            panes_[i]->setGeometry(pos, 0, sizes_[i], height());
        } else {
            panes_[i]->setGeometry(0, pos, width(), sizes_[i]);
        }
        pos += sizes_[i];
        if (i < static_cast<int>(panes_.size()) - 1) pos += th;
    }
}

void SplitPane::request_clamp() {
    // 2.5：去抖 120 ms —— 连续的 Configure 只重排一次
    if (timer_id_ != 0) killTimer(timer_id_);
    timer_id_ = startTimer(debounce_ms_);
}

void SplitPane::set_clamp_debounce_ms(int ms) {
    debounce_ms_ = std::max(0, ms);
    if (timer_id_ != 0) {
        killTimer(timer_id_);
        timer_id_ = 0;
    }
}

void SplitPane::timerEvent(QTimerEvent* event) {
    if (event->timerId() == timer_id_) {
        killTimer(timer_id_);
        timer_id_ = 0;
        relayout();  // 去抖窗口到点：只重排这一次
    }
    QWidget::timerEvent(event);
}

void SplitPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    request_clamp();  // 2.5：窗口变化去抖
    update();
}

bool SplitPane::begin_drag(int pos) {
    const int index = sash_at(pos);
    if (index < 0) return false;
    dragging_ = true;
    drag_sash_ = index;
    preview_ = pos;
    update();  // 只重画预览，**不改几何**（2.1）
    return true;
}

void SplitPane::update_drag(int pos) {
    if (!dragging_) return;
    preview_ = pos;
    update();  // 拖动期只有重绘
}

void SplitPane::end_drag(bool commit) {
    if (!dragging_) return;
    const int index = drag_sash_;
    const int target = preview_;
    dragging_ = false;
    drag_sash_ = -1;
    preview_ = -1;
    if (commit && index >= 0 && index + 1 < static_cast<int>(sizes_.size())) {
        // **松手一次重排**（2.1）：sash 只分隔**相邻两个 pane** ⇒ 差额就在这两格之间搬，
        // 总和不变（否则 clamp 会按"后面的先挨饿"把差额从别处再收一遍，位置就回不到目标）。
        const int start = sash_pos(index);
        const int delta = target - start;
        if (delta != 0) {
            const int floor_a = collapsed_[index] ? 0 : mins_[index];
            const int floor_b = collapsed_[index + 1] ? 0 : mins_[index + 1];
            const int pair = sizes_[index] + sizes_[index + 1];
            int a = sizes_[index] + delta;
            a = std::max(a, floor_a);
            a = std::min(a, std::max(floor_a, pair - floor_b));
            sizes_[index] = std::max(0, a);
            sizes_[index + 1] = std::max(0, pair - sizes_[index]);
        }
        relayout();
    }
    update();
}

void SplitPane::set_collapsed(int index, bool collapsed) {
    if (index < 0 || index >= panes_.size()) return;
    if (collapsed_[index] == collapsed) return;
    // 4.3：**先记用户意图**（标记 + 撤下 minsize 存进 saved），再 clamp —— 反了会被立刻推回
    collapsed_[index] = collapsed;
    if (collapsed) {
        saved_mins_[index] = mins_[index];
        mins_[index] = 0;      // 撤下最小值（4.2：改窗口尺寸也不许弹回）
        sizes_[index] = 0;
    } else {
        // 长回：≥ 原最小值才装回去（4.2）
        const int want = saved_mins_[index];
        mins_[index] = want;
        sizes_[index] = std::max(sizes_[index], want);
    }
    relayout();
}

bool SplitPane::is_collapsed(int index) const {
    if (index < 0 || index >= collapsed_.size()) return false;
    return collapsed_[index];
}

void SplitPane::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    const QColor sash = role_color("sash", "#2f333d");
    const QColor light = role_color("sash_light", "#4c8dff");
    const int th = sash_thickness();

    // sash 本体
    for (int i = 0; i < static_cast<int>(panes_.size()) - 1; ++i) {
        const int start = sash_pos(i);
        if (orient_ == Orient::Horizontal) {
            painter.fillRect(start, 0, th, height(), sash);
        } else {
            painter.fillRect(0, start, width(), th, sash);
        }
    }

    // 2.1 拖动预览：**填色带**（被拉长那一格的底色）+ 1 px 亮线（不是只有一条线）
    if (dragging_ && preview_ >= 0 && drag_sash_ >= 0) {
        const int start = sash_pos(drag_sash_);
        const int from = std::min(start, preview_);
        const int to = std::max(start, preview_);
        // 被拉长的那一格：preview 往后 ⇒ 后面那个 pane 变长；往前 ⇒ 前面那个变长
        const int grower = (preview_ >= start) ? drag_sash_ + 1 : drag_sash_;
        const QColor band = role_color(
            grower == drag_sash_ ? "surface2" : "surface", "#2b2f38");
        if (orient_ == Orient::Horizontal) {
            painter.fillRect(from, 0, std::max(1, to - from), height(), band);
            painter.fillRect(preview_, 0, 1, height(), light);
        } else {
            painter.fillRect(0, from, width(), std::max(1, to - from), band);
            painter.fillRect(0, preview_, width(), 1, light);
        }
    }
}

}  // namespace xrkit::shell
