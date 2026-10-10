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
#include <memory>
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
    // ★★ 2026-10-06（② 用户拍板）**谁能有「取消」**：
    //   用户原话：「**封包和字库的取消，在这里本质是完全没有的，故直接删除即可。**」
    //   ⇒ 文件系统页（封包/解包）与汉化包生成页**不再创建这颗按钮**（`CancelButton::No`）；
    //     其余三页（编码转换 / 文本提取 / XML 校对）照旧共用这一颗（默认 `Yes`）。
    //   为什么是"造不造按钮"而不是"藏起来/置灰"：藏起来或置灰的取消**仍是假的取消**
    //   （用户要的就是"整块删除"）；`cancel_ == nullptr` 之后，本类里所有
    //   `if (cancel_ != nullptr)` 分支自然什么都不做，那两页也**没有**取消位可点。
    enum class CancelButton { Yes, No };

    explicit TaskShell(QWidget* parent = nullptr, CancelButton cancel = CancelButton::Yes);
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

    // ── ★★ 2026-10-10（P17-1「线程与取消契约」，审计 03 F-01）────────────────────
    // **收尾回调（主线程）**：worker 里算出来的结果要画到控件上时走这条。
    // 壳保证两件事：① 在**主线程**上跑（⇒ 回调里可以碰控件）；② 与 worker 里的
    // `message`/`progress` **同一个排队队列、同序**，且在 worker 返回之后才入队
    // （⇒ 回调里读 worker 写过的值一定看得见；worker 自己绝不许碰控件）。
    //
    // 为什么需要它：修前汉化包页的 worker 直接调 `build()`（里面读下拉框、写 QLabel）
    // ⇒ 跨线程碰 QWidget；页析构时 `result_label_` 先于 `task_` 被删 ⇒ 访问违例。
    // 有了这条回调，"重活算完 ⇒ 回主线程画"才是结构约束，而不是口口相传的纪律。
    void set_finish_handler(std::function<void()> handler);

    // 13.1：重入闸门在**主线程同步置位**，然后才启动工作线程。重入返回 false（拒绝）。
    bool run(const QString& title, const WorkerFn& worker);
    bool running() const { return running_; }
    int rejected_count() const { return rejected_; }
    // ★★ P17-4（审计 03 F-08 Low）：**线程槽位**（自检用）。修前 `threads_` 只增不减 ——
    //   每次 run 都 push_back 一个线程对象、已结束的线程保持 joinable 直到析构 ⇒
    //   长会话下无界增长、已结束线程的栈/句柄长期占着。现在每次 run 之前**回收**已完成的。
    int worker_slots() const { return static_cast<int>(workers_.size()); }

    // ── ★★ 2026-10-10（P17-1「线程与取消契约」）──────────────────────────────────
    // **确定性收工**（主线程）：无条件置取消位 ⇒ join 全部 worker ⇒ 清空线程表。
    // 为什么需要它：析构本来也做这件事，但析构发生在**页面子控件被删之后** —— 页面想在
    // "自己的析构函数体里、控件还在"时就把 worker 收干净，得有一个能主动调的入口
    // （审计 03 F-01 的时序面：`result_label_` 先于 `task_` 被删）。
    // 幂等：析构里会再调一次（线程表已空 ⇒ 什么都不做）。
    void stop_and_wait();

    // 13.7：**只发一个 0~100 值**，壳里不做任何归一化
    // ── ★★ 2026-10-10（P17-5，审计 03 F-07 Medium）：**这四个是"从任何线程调都安全"的入口** ──
    //   修前它们直接改控件（`bar_->setValue` / `status_->setText`）⇒ 从 worker 调就是跨线程
    //   碰控件（偶发崩溃），而同一族的 `run()` sink 却是排队回主线程的安全路径 ⇒
    //   调用者极易误用（分册原话：「公开 API 的地雷」）。
    //   现在：主线程调用直落、其它线程**一律排队**（`QueuedConnection`）⇒ 语义只剩
    //   "报告进度/消息/错误/收工"，不再隐含"必须在主线程"这个暗坑。
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
    // ★★ P17-4（审计 03 F-08）：回收**已完成**的 worker（join + 摘除）。只在主线程调
    //   （`run()` 开头与析构/`stop_and_wait` 前）。判据：smoke「顺序跑多次 ⇒ 槽位不增」。
    void reap_finished_workers();

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
    // ★★ P17-4（审计 03 F-08）：一个槽位＝一个 worker 线程 + 它的"跑完了"标志。
    //   修前是裸 `std::vector<std::thread>`：只能追加、无法知道谁跑完了 ⇒ 只能等析构。
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers_;
    std::function<void(const QString&)> message_handler_;
    std::function<void()> finish_handler_;   // ★ P17-1：主线程收尾（见 set_finish_handler）

    // ★ 取消位：**工作线程要读** ⇒ 必须是原子的；理由只在主线程读写。
    std::atomic<bool> cancel_requested_{false};
    std::atomic<int> cancel_requests_{0};
    QString cancel_reason_;
    std::function<bool()> cancel_confirm_;   // 可空：取消前确认（见 set_cancel_confirm）
};

}  // namespace xrkit::shell
