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
#include <atomic>
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

    // 主线程上的消息回调（界面上"逐文件一行"的表就是靠它填的）。
    // 在工作线程里 `report_message` 之后，壳会排队回主线程再调它 —— 所以回调里
    // **可以直接碰控件**，而工作线程那边永远碰不到。
    void set_message_handler(std::function<void(const QString&)> handler);

    // 13.1：重入闸门在**主线程同步置位**，然后才启动工作线程。重入返回 false（拒绝）。
    bool run(const QString& title, const WorkerFn& worker);
    bool running() const { return running_; }
    int rejected_count() const { return rejected_; }

    // 13.7：**只发一个 0~100 值**，壳里不做任何归一化
    void report_progress(int percent_0_100);
    void report_message(const std::string& text);
    void report_error(const std::string& text);
    void finish(const QString& final_status = QString());

    // ── ★ 2026-10-06 取消（①，用户红线：真停 + 留痕 + 理由可见）─────────────
    // 事实（改动前）：`cancel_->setEnabled(...)` 只有三处，**全文件没有 connect**
    //   ⇒ 六页那颗「取消」按钮是一个**死按钮**（点了什么都不发生，也不留任何痕迹）。
    // 口径：
    //   · 点「取消」＝ 置一个**取消请求位**（原子；工作线程只读它，不碰 UI）；
    //   · worker 在**自己的循环边界**轮询 `cancel_requested()`，自己决定停在哪儿，
    //     并把"停在哪、已处理多少、半成品怎么处理"**报出来**（零静默）；
    //   · 壳保证"请求"这件事**一定留痕**：事件流里记一条 `c`，状态栏立刻显示理由，
    //     收尾时**取消理由优先**（不会被 worker 的最后一句或 idle 文案盖掉）。
    void request_cancel(const QString& reason = QString());
    bool cancel_requested() const { return cancel_requested_.load(); }
    QString cancel_reason() const;
    int cancel_requests() const { return cancel_requests_.load(); }

    // ★ 2026-10-06（D2 批）：**取消确认**是页面级的事，但按钮属于**共用**的任务壳 ⇒
    //   壳出一个可选钩子：非空时点「取消」先问它，返回 false ⇒ **什么都不发生**
    //   （不置位、不留痕、按钮照样可点）。1.x `convert_app.cancel_convert` 就是
    //   "先 `askyesno` 再置 cancelled"；六页里只有它要确认，所以不能把这颗按钮
    //   拆成两套接线（那正是"同类东西维护两套"）。
    //   默认空 ⇒ 老行为（点了就请求），其余五页不受影响。
    void set_cancel_confirm(std::function<bool()> confirm) { cancel_confirm_ = std::move(confirm); }
    bool has_cancel_confirm() const { return cancel_confirm_ != nullptr; }

    int last_progress() const { return last_progress_; }
    // 事件序列（自检用）：p=进度 / m=消息 / e=错误 / c=取消请求 / f=结束；并记下**到达时的线程**
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
    std::function<void(const QString&)> message_handler_;

    // ★ 取消位：**工作线程要读** ⇒ 必须是原子的；理由只在主线程读写。
    std::atomic<bool> cancel_requested_{false};
    std::atomic<int> cancel_requests_{0};
    QString cancel_reason_;
    std::function<bool()> cancel_confirm_;   // 可空：取消前确认（见 set_cancel_confirm）
};

}  // namespace xrkit::shell
