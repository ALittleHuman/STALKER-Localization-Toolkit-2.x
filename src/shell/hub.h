#pragma once
// xrkit::shell —— Hub 主窗口（M2-1）。
//
// 六个内置页的顺序与名字**照抄 1.x `stalker_toolkit.BUILTIN_TOOLS`**（那份清单是模块级常量，
// 就是为了让探针核对**真实的**清单而不是手抄的一份 —— 手抄的探针少一页也照样通过）。
//
// 本类**不用 Q_OBJECT / 不用 signals**（回调一律 lambda），所以不需要 moc。

#include <QMainWindow>
#include <QString>

class QListWidget;
class QPlainTextEdit;
class QStackedWidget;
class QLabel;

namespace xrkit::shell {

class LogBox;
class SplitPane;

// 内置页：id 是稳定标识（插件槽位/日志都按它走），title 是界面上的名字。
struct PageInfo {
    const char* id;
    const char* title;
};

int builtin_page_count();
const PageInfo& builtin_page(int index);

class Hub : public QMainWindow {
public:
    explicit Hub(QWidget* parent = nullptr);

    // 探针/自检用：核对**真实**创建的页面数、标题与顺序
    int page_count() const;
    QString page_title(int index) const;
    QString page_id(int index) const;

    QListWidget* page_list() const { return page_list_; }
    QStackedWidget* stack() const { return stack_; }
    QPlainTextEdit* log_view() const { return log_view_; }
    LogBox* log_box() const { return log_box_; }
    SplitPane* split() const { return split_; }
    QLabel* status_label() const { return status_label_; }

    // 4.5：两栏 minsize 用同一组值（px(260) / px(110)）
    int top_min() const { return top_min_; }
    int log_min() const { return log_min_; }

    // 记一行日志（1.x 的日志栏：主窗口那根滚动条**不**管它；攒批见 LogBox）
    void log_line(const QString& text);

private:
    QListWidget* page_list_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    LogBox* log_box_ = nullptr;
    SplitPane* split_ = nullptr;
    QLabel* status_label_ = nullptr;
    int top_min_ = 0;
    int log_min_ = 0;
};

}  // namespace xrkit::shell
