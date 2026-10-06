#pragma once
// xrkit::shell —— Hub 主窗口（M2-1）。
//
// 六个内置页的顺序与名字**照抄 1.x `stalker_toolkit.BUILTIN_TOOLS`**（那份清单是模块级常量，
// 就是为了让探针核对**真实的**清单而不是手抄的一份 —— 手抄的探针少一页也照样通过）。
//
// 本类**不用 Q_OBJECT / 不用 signals**（回调一律 lambda），所以不需要 moc。

#include <QMainWindow>
#include <QString>

#include "xrkit/core/theme.h"
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
// ★ 2026-10-06（审计 3-8）：这里原来还有一个 `class SplitPane;` 的前向声明，**全文件没人用**
//   —— 而它正是"`SplitPane` 是死代码"那条静态锁（`tests/run_ci.py` 的 `shell-smoke` 步）
//   会命中的那处引用。删掉它之后，`src/shell` 里除 `split_pane.{h,cpp}` 与自检夹具
//   `smoke.cpp` 之外**一个引用都没有**（判据不挂死代码这条因此可被机械验证）。

// 内置页：id 是稳定标识（插件槽位/日志都按它走），title 是界面上的名字。
struct PageInfo {
    const char* id;
    const char* title;
    const char* subtitle = "";  // 一句话说明本页能干什么（界面重构口径）
};

int builtin_page_count();
const PageInfo& builtin_page(int index);

// ── ★ 2026-10-06（③）日志**落盘**目录（1.x `stalker_toolkit.py:355-391`）──────────
// 事实（改动前）：`hub.cpp` 里 `导出` / `set_log_file` **零命中** ⇒ GUI 的日志
//   **完全不落盘**（连文件都没挂），用户出问题后没有任何东西可查。
// 口径：默认落 `exe 目录/logs/`（1.x `os.path.join(app_dir(), "logs")`）。
//   这两个是**进程级**设置：自检先 `set_hub_log_dir(临时目录)` 再建 Hub，就不会往
//   构建目录里写东西；只影响"往哪写"，不改任何既有行为。
QString hub_log_dir();
void set_hub_log_dir(const QString& dir);

class Hub : public QMainWindow {
public:
    // `plugin_dir` 为空 ⇒ 用 exe 旁边的 `plugins/`；自检可以指定临时目录
    explicit Hub(QWidget* parent = nullptr, const QString& plugin_dir = QString());
    // ★ 2026-10-04：引擎读入口现在借的是**指针**（`plugin::set_active_registry`，由 fs 页
    //   的 `set_registry` 注入）。Hub 死掉时那份注册表也跟着死 ⇒ 必须在析构里把钩子收回来，
    //   否则钩子指向已死对象（自检里 `Hub` 是局部对象，下一个用例就会踩到野指针）。
    ~Hub() override;

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
    // 切页（`--shot` 抓图与自检用；标签条与页栈由既有连接同步）
    void set_current_page(int index);
    // 探针用：拿到**真实的**页对象（页框架/视口/minheight 都从它上面量，不自报）
    PageBase* page_at(int index) const;
    QPlainTextEdit* log_view() const { return log_view_; }
    QPushButton* log_clear_button() const { return log_clear_; }
    // ★ 2026-10-06（③）：「导出」按钮（1.x `:390`）—— 把内存日志 + 落盘详细日志**合并**
    //   写一份 `<log_dir>/log_YYYYmmdd_HHMMSS.txt`。
    QPushButton* log_export_button() const { return log_export_; }
    // 本次运行 `core::set_log_file` 挂上的那个文件（空 = 没挂上 ⇒ 日志没有文件可查）
    QString log_file_path() const { return log_file_path_; }
    bool log_file_active() const;
    // 合并导出。失败 ⇒ **返回 false 且 `error` 非空**（零静默：写不出去必须说得出来）。
    bool export_log(QString* out_path, QString* error);
    // 最近一次导出失败的理由（空 = 没失败过）——供自检直接读
    QString last_export_error() const { return last_export_error_; }
    LogBox* log_box() const { return log_box_; }
    // ★ 方案一（Hana 重排）：导航改顶部标签条，日志改底部可折叠抽屉
    QPushButton* log_toggle() const { return log_toggle_; }
    QWidget* log_panel() const { return log_panel_; }
    QLabel* status_label() const { return status_label_; }

    // 记一行日志（1.x 的日志栏：主窗口那根滚动条**不**管它；攒批见 LogBox）
    // ★ Q_INVOKABLE 必须有：各页用 `QMetaObject::invokeMethod(hub, "log_line", ...)` 转发日志
    //   （页面不 include hub.h，靠名字解耦）；**按名字调用要求它是可调用的**，否则 invokeMethod
    //   静默返回 false ⇒ 页面日志一条都进不了日志栏。用户 2026-09-30 报的
    //   「日志记录的是什么东西，为什么没有在app当中的操作（例如，加载包）？」就是这个根因。
    Q_INVOKABLE void log_line(const QString& text);

    // ⑫12.4：加载插件并把注册期问题显示出来（构造末尾调一次；空目录＝没有插件）
    void load_plugins(const QString& plugin_dir);

    // ★ 2026-10-06：视图菜单的「暗色/亮色主题」与「界面大小」——**改动立即写盘**
    //   （用户原话：「主题和字体大小目前并没有保留继承的功能」⇒ 下次启动要回默认）。
    //   口径：`apply` + `core::save_*` 同一个入口，**保证"看到的"与"记下的"不可能不同**；
    //   写失败**零静默**（`problem` 非空 + 落一行日志），界面照常切换（不回滚）。
    void apply_theme_mode(xrkit::core::ThemeMode mode, QString* problem = nullptr);
    void apply_ui_size(double scale, QString* problem = nullptr);

private:
    QTabBar* page_tabs_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    LogBox* log_box_ = nullptr;
    QPushButton* log_clear_ = nullptr;
    QPushButton* log_export_ = nullptr;
    QPushButton* log_toggle_ = nullptr;
    QWidget* log_panel_ = nullptr;
    QLabel* status_label_ = nullptr;
    bool log_expanded_ = false;
    QString log_file_path_;      // 本次运行落盘的那个文件（见 log_file_path()）
    QString last_export_error_;  // 导出失败理由（零静默：失败要留得住）
    xrkit::plugin::Registry registry_;
    std::vector<xrkit::plugin::RegistrationError> plugin_errors_;
    QString plugin_notice_;
    bool plugin_ui_installed_ = false;
    int plugin_slots_rendered_ = 0;
    int plugin_pages_added_ = 0;
    QList<QWidget*> plugin_pages_;  // 渲染插件页用（重渲染先清掉，保证幂等）
};

}  // namespace xrkit::shell
