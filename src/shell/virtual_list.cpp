#include "virtual_list.h"

#include "theme.h"

#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QResizeEvent>
#include <algorithm>

namespace xrkit::shell {

VirtualList::VirtualList(QWidget* parent) : QWidget(parent) {
    // ★ P13-2b：要收键盘（回车激活）⇒ 必须能拿焦点
    setFocusPolicy(Qt::StrongFocus);
    setObjectName(QStringLiteral("virtual_list"));
}

void VirtualList::set_row_height(int height) {
    row_height_ = std::max(1, height);
    publish_content_height();
    refresh();
}

void VirtualList::set_row_count(int total) {
    total_ = std::max(0, total);
    publish_content_height();
    refresh();
}

// ★★ 2026-10-05（#6）：把"我多高"**说出来**（Qt 动态属性），外层 `PageBase::sync_inner_scroll()`
//   读它当 `min_body_height`。为什么不直接让外层 `qobject_cast<VirtualList*>`：
//   那样基类就得认识这个具体类（基类只该认识"内容件"这个角色）；也不用 signals/slots ——
//   本页在改行数的地方**显式**调一次同步（改的行数点很少，显式更清楚）。
void VirtualList::publish_content_height() {
    setProperty("xrkit_content_height", content_height());
}

void VirtualList::set_row_text_provider(std::function<QString(int)> provider) {
    provider_ = std::move(provider);
    refresh();
}

QPair<int, int> VirtualList::visible_range(int top, int view_h, int row_h, int total) {
    if (row_h <= 0 || total <= 0) return {0, 0};
    const int first = top / row_h;                          // 10.2
    const int last = std::min(total, (top + view_h) / row_h + 2);
    return {std::max(0, first), std::max(0, last)};
}

QPair<int, int> VirtualList::visible_range() const { return range_; }

void VirtualList::set_scroll_top(int top) {
    top_ = std::max(0, top);
    refresh();
}

QString VirtualList::row_text(int row) const {
    if (row < 0 || row >= total_) return QString();
    return provider_ ? provider_(row) : QString::number(row);
}

namespace {
// ★ P16-3a：选中集合**按升序就地插入**（集合很小——用户点几下；也免得为一个 std::sort
//   再拉 <algorithm> 进来）。重复的不再插第二个。
void add_sorted(QVector<int>& rows, int row) {
    int pos = 0;
    while (pos < rows.size() && rows.at(pos) < row) ++pos;
    if (pos < rows.size() && rows.at(pos) == row) return;
    rows.insert(pos, row);
}
}  // namespace

void VirtualList::set_selected_row(int row) {
    selected_ = row;
    // 单击＝单选一行（多选模式下也一样：QListWidget 的语义是"无修饰键＝重选一行"）
    selection_.clear();
    if (row >= 0 && row < total_) selection_.append(row);
    anchor_ = selected_;
    refresh();
}

void VirtualList::set_multi_selection(bool on) {
    multi_ = on;
    if (!on) {
        // ★ 退回**纯单选**：只留锚点/当前行（旧语义），别把"选中一堆"偷偷留着
        selection_.clear();
        if (selected_ >= 0 && selected_ < total_) selection_.append(selected_);
    }
    refresh();
}

QVector<int> VirtualList::selected_rows() const {
    QVector<int> out;
    for (int row : selection_) {
        if (row >= 0 && row < total_) out.append(row);   // 行数变小后越界的自动不算
    }
    return out;
}

void VirtualList::set_selected_rows(const QVector<int>& rows) {
    selection_.clear();
    for (int row : rows) {
        if (row >= 0 && row < total_) add_sorted(selection_, row);   // 越界的不收
    }
    selected_ = selection_.isEmpty() ? -1 : selection_.first();
    anchor_ = selected_;
    refresh();
}

void VirtualList::toggle_row(int row) {
    if (row < 0 || row >= total_) return;
    if (selection_.contains(row)) {
        selection_.removeAll(row);
    } else {
        add_sorted(selection_, row);
    }
    selected_ = row;      // 当前行＝刚点的那一行（旧名在这一支上的语义）
    anchor_ = row;
    refresh();
}

void VirtualList::extend_to_row(int row) {
    if (row < 0 || row >= total_) return;
    const int from = anchor_ >= 0 ? anchor_ : selected_;
    if (from < 0) {
        set_selected_row(row);
        return;
    }
    const int lo = from < row ? from : row;
    const int hi = from < row ? row : from;
    selection_.clear();
    for (int r = lo; r <= hi; ++r) selection_.append(r);
    selected_ = row;      // 锚点**不动**（QListWidget 的语义：锚点留在按下修饰键前的起点）
    refresh();
}

// ★★ P16-3a：鼠标左键（多选语义）—— 行号＝**滚动偏移 + 视口内偏移 / 行高**（与双击那条同一算法）
void VirtualList::mousePressEvent(QMouseEvent* event) {
    if (event == nullptr || row_height_ <= 0 || total_ <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const int row = (top_ + static_cast<int>(event->position().y())) / row_height_;
        const int clamped = row < 0 ? 0 : (row >= total_ ? total_ - 1 : row);
        const Qt::KeyboardModifiers mods = event->modifiers();
        if (multi_ && (mods & Qt::ControlModifier)) {
            toggle_row(clamped);
        } else if (multi_ && (mods & Qt::ShiftModifier)) {
            extend_to_row(clamped);
        } else {
            set_selected_row(clamped);
        }
    }
    QWidget::mousePressEvent(event);
}


// ── ★★ P13-2b：激活（双击 / 回车）──────────────────────────────────────────────
void VirtualList::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event == nullptr) return;
    if (row_height_ > 0 && total_ > 0 && on_activate_ != nullptr) {
        // ★ 行号 = **滚动偏移 + 视口内偏移 / 行高**（带偏移；滚动之后才对得上）
        // ★ `top_` 是**像素**偏移（见 `visible_range(top, view_h, row_h, total)`
        //   里的 `first = top // h`）⇒ 先加再除，别把像素当行号。
        const int row = (top_ + static_cast<int>(event->position().y())) / row_height_;
        const int clamped = row < 0 ? 0 : (row >= total_ ? total_ - 1 : row);
        ++activations_;
        on_activate_(clamped);
    }
    QWidget::mouseDoubleClickEvent(event);
}

void VirtualList::keyPressEvent(QKeyEvent* event) {
    if (event == nullptr) return;
    const int key = event->key();
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && on_activate_ != nullptr &&
        selected_ >= 0 && selected_ < total_) {
        ++activations_;
        on_activate_(selected_);
        return;   // 吃掉（别让它再被当成普通按键）
    }
    QWidget::keyPressEvent(event);
}

void VirtualList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refresh();
}

void VirtualList::refresh() {
    range_ = visible_range(top_, height(), row_height_, total_);
    const int need = std::max(0, range_.second - range_.first);
    // 行控件池：只增不减到"当前需要"为止（复用优先 —— total 变化**不**影响池子大小）
    while (widgets_.size() < need) {
        auto* label = new QLabel(this);
        label->setObjectName(QStringLiteral("virtual_row"));
        label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        label->show();
        widgets_.push_back(label);
    }
    provider_calls_ = 0;
    for (int i = 0; i < widgets_.size(); ++i) {
        QLabel* label = widgets_[i];
        if (i >= need) {
            label->hide();          // 池子里富余的行控件：藏起来，不销毁（复用）
            continue;
        }
        const int row = range_.first + i;
        label->setText(provider_ ? provider_(row) : QString::number(row));
        if (provider_) ++provider_calls_;   // 只为视口内的行取文本（10.3 同族）
        label->setGeometry(0, row * row_height_ - top_, width(), row_height_);
        // ★ P16-3a：**选中集合**里就算选中（多选时可见范围内所有选中行都上"选中"样式）
        const char* role = selection_.contains(row) ? "selected" : "surface";
        const char* hex = xrkit::core::theme_color_current(role);
        label->setStyleSheet(
            QStringLiteral("background:%1; padding-left:%2px;")
                .arg(QString::fromUtf8(hex != nullptr ? hex : "#22252c"))
                .arg(px(6)));
        label->show();
        if (i < need) ++recycle_count_;     // 复用一次（重建会体现在池子增长上）
    }
    update();
}

}  // namespace xrkit::shell
