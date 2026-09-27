#include "drop_filter.h"

#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QMimeData>
#include <QUrl>
#include <QWidget>

namespace xrkit::shell {

FileDropFilter::FileDropFilter(Handler handler, QObject* parent)
    : QObject(parent), handler_(std::move(handler)) {}

void FileDropFilter::attach(QWidget* widget) {
    if (widget == nullptr) return;
    widget->setAcceptDrops(true);  // 没有它 Qt 不会派发 DragEnter/Drop
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

bool FileDropFilter::dispatch(QEvent* event) {
    if (event == nullptr) return false;
    switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            // QDragMoveEvent 继承 QDropEvent，取 mimeData 用同一套
            auto* move = static_cast<QDropEvent*>(event);
            if (local_paths(move->mimeData()).isEmpty()) return false;
            move->acceptProposedAction();
            return true;
        }
        case QEvent::Drop: {
            auto* drop = static_cast<QDropEvent*>(event);
            const QStringList paths = local_paths(drop->mimeData());
            if (paths.isEmpty()) return false;
            drop->acceptProposedAction();
            if (handler_) handler_(paths);
            return true;
        }
        default:
            break;
    }
    return false;
}

bool FileDropFilter::eventFilter(QObject* watched, QEvent* event) {
    if (dispatch(event)) return true;
    return QObject::eventFilter(watched, event);
}

}  // namespace xrkit::shell
