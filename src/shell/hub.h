#pragma once
// xrkit::shell —— Hub 主窗口（M2-1）。
//
// 六个内置页的顺序与名字**照抄 1.x `stalker_toolkit.BUILTIN_TOOLS`**（那份清单是模块级常量，
// 就是为了让探针核对**真实的**清单而不是手抄的一份 —— 手抄的探针少一页也照样通过）。
//
// 本类**不用 Q_OBJECT / 不用 signals**（回调一律 lambda），所以不需要 moc。

#include <QMainWindow>
#include <QString>

#include "xrkit/engine/plugin/registry.h"

#include <vector>

class QTabBar;
class QPushButton;
class QPlainTextEdit;
class QStackedWidget;
class QLabel;

namespace xrkit::shell {

class LogBox;
class PageBase;
class SplitPane;

// 内置页：id 是稳定标识（插件槽位/日志都按它走），title 是界面上的名字。
struct PageInfo {
    const char* id;
    const char* title;
    const char* subtitle = "";  // 一句话说明本页能干什么（界面重构口径）
};

int builtin_page_count();
const PageInfo& builtin_page(int index);

class Hub : public QMainWindow {
public:
    // `plugin_dir` 为空 ⇒ 用 exe 旁边的 `plugins/`；自检可以指定临时目录
    explicit Hub(QWidget* parent = nullptr, const QString& plugin_dir = QString());

    // 探针/自检用：核对**真实**创建的页面数、标题与顺序
    int page_count() const;
    QString page_title(int index) const;
    QString page_id(int index) const;

    // 插件：注册表 + 启动时的错误清单（⑫12.4：非法寻址要**在 Hub 启动时可见**）
    xrkit::plugin::Registry& registry() { return registry_; }
    const std::vector<xrkit::plugin::RegistrationError>& plugin_errors() const {
        return plugin_errors_;
    }
    // 启动时的插件提示（读几行、非空说明有插件问题）——Hub 会把它打日志 + 写状态栏
    QString plugin_notice() const { return plugin_notice_; }
    // 探针：加载插件时是否把外壳的 `api.ui`（Qt 16 方法）装上了
    bool plugin_ui_installed() const { return plugin_ui_installed_; }
    // ⑫12.4/12.1：把插件槽位**渲染出来**（工具栏菜单项先做）——
    // 注册成功但没人渲染 = FAIL，所以这一步也要有计数判据
    void render_plugin_slots();
    int plugin_slots_rendered() const { return plugin_slots_rendered_; }
    int plugin_pages_added() const { return plugin_pages_added_; }

    QTabBar* page_tabs() const { return page_tabs_; }
    QStackedWidget* stack() const { return stack_; }
    // 探针用：拿到**真实的**页对象（页框架/视口/minheight 都从它上面量，不自报）
    PageBase* page_at(int index) const;
    QPlainTextEdit* log_view() const { return log_view_; }
    QPushButton* log_clear_button() const { return log_clear_; }
    LogBox* log_box() const { return log_box_; }
    // ★ 方案一（Hana 重排）：导航改顶部标签条，日志改底部可折叠抽屉
    QPushButton* log_toggle() const { return log_toggle_; }
    QWidget* log_panel() const { return log_panel_; }
    QLabel* status_label() const { return status_label_; }

    // 记一行日志（1.x 的日志栏：主窗口那根滚动条**不**管它；攒批见 LogBox）
    void log_line(const QString& text);

    // ⑫12.4：加载插件并把注册期问题显示出来（构造末尾调一次；空目录＝没有插件）
    void load_plugins(const QString& plugin_dir);

private:
    QTabBar* page_tabs_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    LogBox* log_box_ = nullptr;
    QPushButton* log_clear_ = nullptr;
    QPushButton* log_toggle_ = nullptr;
    QWidget* log_panel_ = nullptr;
    QLabel* status_label_ = nullptr;
    bool log_expanded_ = false;
    xrkit::plugin::Registry registry_;
    std::vector<xrkit::plugin::RegistrationError> plugin_errors_;
    QString plugin_notice_;
    bool plugin_ui_installed_ = false;
    int plugin_slots_rendered_ = 0;
    int plugin_pages_added_ = 0;
    QList<QWidget*> plugin_pages_;  // 渲染插件页用（重渲染先清掉，保证幂等）
};

}  // namespace xrkit::shell
