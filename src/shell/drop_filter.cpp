#include "drop_filter.h"

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QStyle>
#include <QUrl>
#include <QWidget>

namespace xrkit::shell {
namespace {

// 「这一块**接住了**」的视觉反馈：给区域控件置一个属性 + 让 QSS 重算。
// 为什么用属性而不是自己在区域上盖一个色块：反馈必须**不改变布局**（`--shot` 的几何判据
// 与 5.1/5.2 的页高判据都靠几何），属性只驱动 QSS，一个像素都不动。
constexpr const char* kDropActiveProperty = "drop_hover";

void set_drop_hover(QWidget* target, bool on) {
    if (target == nullptr || target->property(kDropActiveProperty).toBool() == on) return;
    target->setProperty(kDropActiveProperty, on);
    // 属性选择器（QSS `[drop_hover="true"]`）要重新计算样式才会生效
    target->style()->unpolish(target);
    target->style()->polish(target);
    target->update();
}

}  // namespace

FileDropFilter::FileDropFilter(Handler handler, QObject* parent)
    : QObject(parent), handler_(std::move(handler)) {}

void FileDropFilter::attach(QWidget* widget) {
    if (widget == nullptr) return;
    widget->setAcceptDrops(true);  // 没有它 Qt 不会派发 DragEnter/Drop
    // 属性**先建出来**（值 false）：QSS 的属性选择器 `[drop_hover="true"]` 只会命中
    // "有这个属性且为 true"的控件 —— 先建出来，既保证拖进时能立刻高亮，也避免
    // "属性从未存在"这种半吊子状态（自检也据此断言"区域自己认得自己接住了"）。
    if (!widget->property(kDropActiveProperty).isValid()) {
        widget->setProperty(kDropActiveProperty, false);
    }
    widget->installEventFilter(this);
    if (!watched_.contains(widget)) watched_.push_back(widget);
}

bool FileDropFilter::is_attached_to(const QWidget* widget) const {
    return widget != nullptr && watched_.contains(const_cast<QWidget*>(widget));
}

QStringList FileDropFilter::local_paths(const QMimeData* data) {
    QStringList out;
    if (data == nullptr || !data->hasUrls()) return out;
    for (const QUrl& url : data->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (!path.isEmpty()) out << path;
    }
    return out;
}

bool FileDropFilter::dispatch(QEvent* event) { return dispatch(event, nullptr); }

bool FileDropFilter::dispatch(QEvent* event, QWidget* target) {
    if (event == nullptr) return false;
    switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            // QDragMoveEvent 继承 QDropEvent，取 mimeData 用同一套
            auto* move = static_cast<QDropEvent*>(event);
            if (local_paths(move->mimeData()).isEmpty()) return false;
            move->acceptProposedAction();
            set_drop_hover(target, true);   // 用户要的"看得出来它接住了"
            return true;
        }
        case QEvent::Drop: {
            auto* drop = static_cast<QDropEvent*>(event);
            const QStringList paths = local_paths(drop->mimeData());
            if (paths.isEmpty()) return false;
            drop->acceptProposedAction();
            set_drop_hover(target, false);
            if (handler_) handler_(paths);
            return true;
        }
        case QEvent::DragLeave:
        case QEvent::Leave:
            // 光标走了 ⇒ 撤掉高亮。**不吃掉事件**（leave 是别人也在用的常规事件）
            set_drop_hover(target, false);
            return false;
        default:
            break;
    }
    return false;
}

bool FileDropFilter::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (dispatch(event, widget)) return true;
    return QObject::eventFilter(watched, event);
}

}  // namespace xrkit::shell
