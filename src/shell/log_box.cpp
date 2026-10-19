#include "log_box.h"

#include <utility>

#include <QScrollBar>
#include <QStringList>
#include <QTextCursor>
#include <QTimerEvent>

namespace xrkit::shell {

LogBox::LogBox(QWidget* parent) : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("log_box"));
    setReadOnly(true);              // 4.4：只读（程序可写，用户不可改）
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMaximumBlockCount(0);        // 不用 Qt 的逐行上限，按 1.x 的**整块截断**自己管
    setMinimumWidth(1);             // 4.4：`width=1` —— 宽度不参与布局撑开
    setMaximumWidth(QWIDGETSIZE_MAX);
}

void LogBox::append_batched(const QString& line) {
    pending_.append(line);
    if (timer_id_ == 0) timer_id_ = startTimer(batch_ms_);  // 攒批：80 ms 一次
}

void LogBox::flush() {
    if (timer_id_ != 0) {
        killTimer(timer_id_);
        timer_id_ = 0;
    }
    if (pending_.isEmpty()) return;
    const QString text = pending_.join(QLatin1Char('\n'));
    pending_.clear();
    // 一次性插入（不是逐行 appendPlainText —— 那会把"批量"这个口径丢掉）
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (!document()->isEmpty()) cursor.insertText(QStringLiteral("\n"));
    cursor.insertText(text);
    ++flush_count_;
    // ★★ C-13：**落盘镜像**必须在插入文档之后 —— 界面上看到的每一条都要在文件里。
    //   （以前这里没有回调，所以 `logs/*.txt` 只有引擎行，UI 行一条都没有 ⇒ 5 份 0 字节）
    if (persist_) persist_(text);
    enforce_limit();
}

void LogBox::set_persist(std::function<void(const QString&)> fn) { persist_ = std::move(fn); }

void LogBox::enforce_limit() {
    const int blocks = document()->blockCount();
    if (blocks <= line_limit_) return;
    // 4.4：超 5000 行**删到 3000 行**（整块删掉最前面的 2000 行）
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::Start);
    for (int i = 0; i < blocks - keep_lines_; ++i) cursor.movePosition(QTextCursor::Down,
                                                                      QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    ++trim_count_;
}

void LogBox::clear_log() {
    pending_.clear();
    if (timer_id_ != 0) {
        killTimer(timer_id_);
        timer_id_ = 0;
    }
    clear();
}

void LogBox::timerEvent(QTimerEvent* event) {
    if (event->timerId() == timer_id_) {
        timer_id_ = 0;
        flush();
    }
    QPlainTextEdit::timerEvent(event);
}

}  // namespace xrkit::shell
