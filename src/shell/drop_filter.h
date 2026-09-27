#pragma once
// xrkit::shell —— 拖放过滤器（对照表 6.4 的落地件）。
//
// 口径（1.x 明文写着，2.0 沿用）：**没有 `drop_zone` 控件** —— 拖放直接注册在
// **既有控件**上。所以这里不造控件，只给已有控件装一个事件过滤器：
//   · `DragEnter` / `DragMove`：mimeData 里有**本地文件**就 accept（否则不接，
//     让 Qt 给"禁止"光标 —— 接了又什么都不做才是最坑的）；
//   · `Drop`：把本地路径交给回调（页面负责"这些路径怎么加载"）。
// `plugin_ui.cpp` 里那句"…由外壳的 dragEnter/dropEvent 过滤器统一处理（见 drop_filter.h）"
// 指的就是本文件 —— 之前它并不存在（那句注释一直悬空）。
//
// 为什么不是"页面里各写一份 dropEvent"：六页都要拖放，而判据只有一条
// （"本地文件路径 → 交给页面"），写六份就一定会有一份先腐坏。

#include <QList>
#include <QObject>
#include <QStringList>

#include <functional>

class QEvent;
class QMimeData;
class QWidget;

namespace xrkit::shell {

class FileDropFilter : public QObject {
public:
    // 回调收到的是**本机路径**（Qt 的 `QUrl::toLocalFile` 结果，UTF-8 的 QString）
    using Handler = std::function<void(const QStringList& local_paths)>;

    explicit FileDropFilter(Handler handler, QObject* parent = nullptr);

    // 给控件装过滤器（顺带 `setAcceptDrops(true)` —— 缺了它 Qt 根本不会派发拖放事件）
    void attach(QWidget* widget);
    // 自检用：这一条到底装上了没有（**结构判据**；行为判据见 dispatch）
    bool is_attached_to(const QWidget* widget) const;

    // 只认本地文件（HTTP/自定义 mime 一律过滤掉）
    static QStringList local_paths(const QMimeData* data);

    // 事件分发。真机上是 Qt 通过 `eventFilter` 调进来的；自检**在离屏环境里没有平台
    // 拖放**（实测：`QApplication::sendEvent(widget, &QDropEvent)` 会被 Qt 拦掉 ——
    // `sendEvent` 返回 false、事件根本没到控件，真实的 drop 走的是 QWidgetWindow 那条
    // 平台通路），所以自检直接调这里：**过滤逻辑与接线一起被验到**，只有"Qt 会不会调
    // 到过滤器"这一环依赖 Qt 自己的契约（`setAcceptDrops(true)` + `installEventFilter`
    // 都是同一个函数里做的，漏一个自检就会红）。
    bool dispatch(QEvent* event);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Handler handler_;
    QList<QWidget*> watched_;
};

}  // namespace xrkit::shell
