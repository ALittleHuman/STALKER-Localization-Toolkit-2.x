#pragma once
// xrkit::shell —— 日志栏（M2-4）。口径出处：对照表 ④（4.1/4.4）。
//
// 1.x `LogBox` 的可量口径：
//   · **4.1 日志栏不归主窗口滚动条管**（由它自己的条子管）⇒ 它是个自带滚动区的控件，
//     不是被外层视口滚的画布内容；
//   · **4.4** `width=1`（宽度不参与布局撑开）、只读、**批量插入 `after(80)` 攒批**、
//     超 **5000 行**删到 **3000 行**、清空必须走 `LogBox.clear()`（不许各页自己 delete）。

#include <QPlainTextEdit>
#include <QString>

#include <functional>

class QTimerEvent;

namespace xrkit::shell {

class LogBox : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit LogBox(QWidget* parent = nullptr);

    // 攒批追加（累计到 80 ms 才动文档一次）
    void append_batched(const QString& line);
    // 立刻落盘（收尾/异常路径用）
    void flush();

    // ★★ C-13（用户 2026-10-19 决策）：把这个控件里的日志**镜像**到落盘文件。
    //   选 `flush()` 而不是 `append_batched()` 挂回调的理由：LogBox 已经是 80 ms 批量，
    //   写文件跟着批量走 ⇒ 解包 3.9 万条覆盖时不会变成 3.9 万次 flush。
    //   回调收到的是**这一批**的整块文本（多行）。
    void set_persist(std::function<void(const QString&)> fn);

    // 超上限截断：> 5000 行 ⇒ 删到 3000 行（**整块删**，不是逐行丢）
    void enforce_limit();

    // 4.4：清空必须走这里
    void clear_log();

    int flush_count() const { return flush_count_; }
    int trim_count() const { return trim_count_; }
    int pending() const { return pending_.size(); }
    int line_limit() const { return line_limit_; }
    int keep_lines() const { return keep_lines_; }
    int batch_ms() const { return batch_ms_; }

protected:
    void timerEvent(QTimerEvent* event) override;

private:
    QStringList pending_;
    int timer_id_ = 0;
    int flush_count_ = 0;
    std::function<void(const QString&)> persist_;   // C-13：落盘镜像回调（可空）
    int trim_count_ = 0;
    int batch_ms_ = 80;
    int line_limit_ = 5000;
    int keep_lines_ = 3000;
};

}  // namespace xrkit::shell
