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

TaskShell::TaskShell(QWidget* parent, CancelButton cancel) : QObject(parent) {
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
    row->addWidget(bar_, 1);
    // ★★ 2026-10-06（② 用户拍板）：`CancelButton::No`（文件系统页 / 汉化包生成页）
    //   ⇒ **连控件都不造** —— 不是隐藏、不是置灰（那两种都还是"假的取消"）。
    //   事实（改动前）：这两页共用任务壳那颗「取消」，而封包是一次原子写、字库打包+写盘
    //   也不可中途停 ⇒ 用户口径「**本质是完全没有的，故直接删除即可**」。
    if (cancel == CancelButton::Yes) {
        cancel_ = new QPushButton(QStringLiteral("取消"), panel_);
        cancel_->setEnabled(false);
        cancel_->setObjectName(QStringLiteral("task_cancel"));
        // ★★ 2026-10-06（①）：**这颗按钮以前是死的** —— 全文件只有三处 `setEnabled`，
        //   没有一处 `connect` ⇒ 六页的「取消」都是摆设。这里就是那条缺失的接线。
        //   （静默锁：`tests/run_ci.py` 的 shell-smoke 静态锁要求本文件必须出现
        //    `connect(cancel_` —— 拆掉它立刻就红。）
        QObject::connect(cancel_, &QPushButton::clicked, this, [this] {
            // ★ D2 批：页面级确认（只有编码转换页装它）。返回 false ⇒ **什么都不发生**：
            //   不置位、不留痕、按钮保持可点 —— 1.x 的 `askyesno` 就是这个语义。
            if (cancel_confirm_ && !cancel_confirm_()) return;
            request_cancel(QStringLiteral("已取消：用户点了「取消」"));
        });
        row->addWidget(cancel_);
    }
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
    // ★ 10-06：取消位**每次 run 都归零**（上一轮的取消不许把这一轮一开始就掐掉），
    //   理由也清掉；按钮恢复成可点、文案回到「取消」。
    cancel_requested_.store(false);
    cancel_reason_.clear();
    if (cancel_ != nullptr) {
        cancel_->setText(QStringLiteral("取消"));
        cancel_->setEnabled(true);
    }
    bar_->setValue(0);
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

QString TaskShell::cancel_reason() const { return cancel_reason_; }

void TaskShell::request_cancel(const QString& reason) {
    // 没在跑 ⇒ 不记（否则"空闲时点一下"会污染下一轮的结论）。
    if (!running_) return;
    cancel_requested_.store(true);
    const int n = ++cancel_requests_;
    cancel_reason_ = reason.isEmpty() ? QStringLiteral("已取消：用户请求停止") : reason;
    if (cancel_ != nullptr) {
        // 按钮收起来（已请求 ⇒ 再点没有意义），并把**正在取消**这件事摆在界面上。
        cancel_->setEnabled(false);
        cancel_->setText(QStringLiteral("正在取消…"));
    }
    // 理由**立刻**可见（不等 worker 回来）：状态栏就是用户看的那个地方。
    status_text_ = cancel_reason_;
    status_->setText(cancel_reason_);
    const bool on_main = QThread::currentThread() == QCoreApplication::instance()->thread();
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back({'c', n, on_main});
}

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
    if (cancel_ != nullptr) {
        cancel_->setEnabled(false);
        cancel_->setText(QStringLiteral("取消"));   // 文案复位（下一轮 run 还会再置一次）
    }
    // 13.4：没给最终文案、无错误、且状态栏还是 run() 那句 ⇒ **恢复前置状态**
    QString next = final_status;
    if (next.isEmpty() && error_text_.isEmpty() && status_text_ == run_line_) {
        next = prev_status_;
    }
    if (next.isEmpty() && error_text_.isEmpty()) next = idle_status_;  // 13.5 兜底
    if (!error_text_.isEmpty()) next = QStringLiteral("失败：") + error_text_;
    // ★ 2026-10-06（用户红线：取消要**理由可见**）：取消理由**优先收尾** ——
    //   否则会被 worker 的最后一句（"完成"/idle 兜底）盖掉，用户就看不到"为什么停了"。
    //   若同时还有错误，两个都报（不许把错误吞掉）。
    if (cancel_requested_.load() && !cancel_reason_.isEmpty()) {
        next = cancel_reason_;
        if (!error_text_.isEmpty()) {
            next += QStringLiteral("（另有错误：") + error_text_ + QStringLiteral("）");
        }
    }
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
