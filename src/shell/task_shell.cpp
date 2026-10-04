#include "task_shell.h"

#include "theme.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

namespace xrkit::shell {

TaskShell::TaskShell(QWidget* parent) : QObject(parent) {
    panel_ = new QWidget(parent);
    // ★ 2026-10-05：给它一个 objectName，好让 QSS **只把这一处**置成透明 —— 它是"字段行"
    //   之外的另一个裸容器现场（用户截图图二"缺了一块"），自己不涂底、底交给所在卡片。
    panel_->setObjectName(QStringLiteral("task_panel"));
    auto* layout = new QVBoxLayout(panel_);
    layout->setContentsMargins(px(6), px(4), px(6), px(4));
    auto* row = new QHBoxLayout;
    bar_ = new QProgressBar(panel_);
    bar_->setRange(0, 100);          // 13.7：只认 0~100
    bar_->setValue(0);
    bar_->setTextVisible(true);
    status_ = new QLabel(panel_);
    status_->setObjectName(QStringLiteral("task_status"));
    cancel_ = new QPushButton(QStringLiteral("取消"), panel_);
    cancel_->setEnabled(false);
    row->addWidget(bar_, 1);
    row->addWidget(cancel_);
    layout->addLayout(row);
    layout->addWidget(status_);
    status_text_ = idle_status_;
    status_->setText(status_text_);
}

TaskShell::~TaskShell() {
    for (std::thread& t : threads_) {
        if (t.joinable()) t.join();
    }
}

bool TaskShell::run(const QString& title, const WorkerFn& worker) {
    if (worker == nullptr) return false;
    // 13.1：**主线程同步置位**，早于线程启动 —— 重入在这里就被挡住（不是线程起来才发现）
    if (running_) {
        ++rejected_;
        return false;
    }
    running_ = true;
    prev_status_ = status_text_;
    run_line_ = title;
    error_text_.clear();
    last_progress_ = 0;
    bar_->setValue(0);
    cancel_->setEnabled(true);
    status_text_ = title;
    status_->setText(status_text_);

    // 13.2/13.3：工作线程只拿两个 sink；一切 UI 更新都**排队回主线程**
    QObject* context = this;
    const ProgressSink progress = [context](int percent) {
        QMetaObject::invokeMethod(
            context, [context, percent] { static_cast<TaskShell*>(context)->apply_progress(percent); },
            Qt::QueuedConnection);
    };
    const MessageSink message = [context](const std::string& text) {
        const QString qtext = QString::fromStdString(text);
        QMetaObject::invokeMethod(
            context, [context, qtext] { static_cast<TaskShell*>(context)->apply_message(qtext); },
            Qt::QueuedConnection);
    };

    threads_.emplace_back([this, worker, progress, message] {
        try {
            worker(progress, message);
        } catch (const std::exception& exc) {
            // 13.3：失败**回主线程**，且先于 finish 入队（同一队列保序）
            QString text = QString::fromUtf8(exc.what());
            QMetaObject::invokeMethod(
                this, [this, text] { apply_error(text); }, Qt::QueuedConnection);
        } catch (...) {
            QMetaObject::invokeMethod(
                this, [this] { apply_error(QStringLiteral("未知错误")); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { apply_finish(QString()); }, Qt::QueuedConnection);
    });
    return true;
}

void TaskShell::report_progress(int percent_0_100) { apply_progress(percent_0_100); }
void TaskShell::report_message(const std::string& text) {
    apply_message(QString::fromStdString(text));
}
void TaskShell::report_error(const std::string& text) { apply_error(QString::fromStdString(text)); }
void TaskShell::finish(const QString& final_status) { apply_finish(final_status); }

void TaskShell::apply_progress(int percent) {
    // 13.7：**一个 0~100 值，原样用**（不乘不除；越界才夹）
    const int value = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    last_progress_ = value;
    bar_->setValue(value);
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({'p', value, on_main});
}

void TaskShell::set_message_handler(std::function<void(const QString&)> handler) {
    message_handler_ = std::move(handler);
}

void TaskShell::apply_message(QString text) {
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({'m', 0, on_main});
    }
    if (!text.isEmpty()) status_->setText(text);
    // 回调只在**主线程**跑（工作线程的报告已经在上面排队回来了）——
    // 所以页面可以在回调里直接往表里加行，而工作线程那边永远碰不到控件（13.2）。
    if (on_main && message_handler_) message_handler_(text);
}

void TaskShell::apply_error(QString text) {
    error_text_ = text;
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({'e', 0, on_main});
    }
}

void TaskShell::apply_finish(QString final_status) {
    running_ = false;
    cancel_->setEnabled(false);
    // 13.4：没给最终文案、无错误、且状态栏还是 run() 那句 ⇒ **恢复前置状态**
    QString next = final_status;
    if (next.isEmpty() && error_text_.isEmpty() && status_text_ == run_line_) {
        next = prev_status_;
    }
    if (next.isEmpty() && error_text_.isEmpty()) next = idle_status_;  // 13.5 兜底
    if (!error_text_.isEmpty()) next = QStringLiteral("失败：") + error_text_;
    // 13.5：兜底**必须**非空（空串会让状态栏变成空白）
    if (next.isEmpty()) next = idle_status_.isEmpty() ? QStringLiteral("就绪") : idle_status_;
    status_text_ = next;
    status_->setText(next);
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({'f', 0, on_main});
}

QVector<TaskShell::Event> TaskShell::take_events() {
    std::lock_guard<std::mutex> lock(mutex_);
    QVector<Event> copy = events_;
    events_.clear();
    return copy;
}

}  // namespace xrkit::shell
