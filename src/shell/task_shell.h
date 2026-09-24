#pragma once
// xrkit::shell —— 任务壳（M2-4）。口径出处：对照表 ⑬（13.1~13.7）。
//
// 1.x 的 `TaskRunner` 是可量口径集中在四件事上（另有两条**缺陷**要在 2.0 关掉）：
//   · **13.1 `running` 是真重入闸门**：主线程**同步置位、早于线程启动**（不是"线程起来再看"）；
//   · **13.2 工作线程里不许碰任何 UI**：数据在主线程读好传进去，工作线程只 `emit` 进度/消息；
//   · **13.3 失败路径必须回主线程**，且 `_handle_error` 先入队、`_finish` 后入队（同队列保序）；
//   · **13.4 `finish()` 恢复前置状态**：没给最终文案、无错误、且状态栏还是 `run()` 那句 ⇒ 恢复 `_prev_status`；
//   · **13.5 `idle_status` 兜底必须**（否则加载完状态栏变空串）；
//   · **13.7（2.0 关掉）**：进度**只发一个 0~100 值** —— 1.x 是 `TaskRunner` 先 `*100/total`、
//     页面又除一次 `_progress_total`，`total<100` 时很早就顶到 100%。

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

namespace xrkit::shell {

using ProgressSink = std::function<void(int)>;                  // 0~100，**原样**转发
using MessageSink = std::function<void(const std::string&)>;
using WorkerFn = std::function<void(const ProgressSink&, const MessageSink&)>;

class TaskShell : public QObject {
    Q_OBJECT

public:
    explicit TaskShell(QWidget* parent = nullptr);
    ~TaskShell() override;

    QWidget* panel() const { return panel_; }
    QProgressBar* progress_bar() const { return bar_; }
    QLabel* status_label() const { return status_; }
    QPushButton* cancel_button() const { return cancel_; }

    void set_idle_status(const QString& text) { idle_status_ = text; }
    QString idle_status() const { return idle_status_; }
    QString status() const { return status_text_; }

    // 13.1：重入闸门在**主线程同步置位**，然后才启动工作线程。重入返回 false（拒绝）。
    bool run(const QString& title, const WorkerFn& worker);
    bool running() const { return running_; }
    int rejected_count() const { return rejected_; }

    // 13.7：**只发一个 0~100 值**，壳里不做任何归一化
    void report_progress(int percent_0_100);
    void report_message(const std::string& text);
    void report_error(const std::string& text);
    void finish(const QString& final_status = QString());

    int last_progress() const { return last_progress_; }
    // 事件序列（自检用）：p=进度 / m=消息 / e=错误 / f=结束；并记下**到达时的线程**
    struct Event {
        char kind;
        int value;
        bool on_main_thread;
    };
    QVector<Event> take_events();

private:
    void apply_progress(int percent);
    void apply_message(QString text);
    void apply_error(QString text);
    void apply_finish(QString final_status);

    QWidget* panel_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* cancel_ = nullptr;

    bool running_ = false;
    int rejected_ = 0;
    int last_progress_ = 0;
    QString status_text_;
    QString prev_status_;
    QString idle_status_ = QStringLiteral("就绪");
    QString run_line_;
    QString error_text_;

    std::mutex mutex_;
    QVector<Event> events_;
    std::vector<std::thread> threads_;
};

}  // namespace xrkit::shell
