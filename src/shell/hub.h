#pragma once
// xrkit::shell —— Hub 主窗口（M2-1）。
//
// 六个内置页的顺序与名字**照抄 1.x `stalker_toolkit.BUILTIN_TOOLS`**（那份清单是模块级常量，
// 就是为了让探针核对**真实的**清单而不是手抄的一份 —— 手抄的探针少一页也照样通过）。
//
// 本类**不用 Q_OBJECT / 不用 signals**（回调一律 lambda），所以不需要 moc。

#include <QMainWindow>
#include <QString>

#include "page_host.h"
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

// ── ★★ 插件改装 P1（Hub 机制层）：页的声明不再住在 Hub 里 ─────────────────────────
// 修前：`struct PageInfo` 与内置六页清单都在**本文件/本实现**，Hub 还直接
// `dynamic_cast<FsPage*>` / `<XmlPage*>` 去注入注册表 —— 与规格 §一「Hub ＝ 容器 + 注册表 +
// 机制提供者，**不含业务**」相反。现在：清单位于 `pages/page_factory.cpp`（页模块），
// Hub 只消费 `PageRegistry`；给页交机制的唯一入口是 `PageBase::attach_services()`
//（静态锁：`hub.cpp` 里不许出现任何具体页类名，见 `tests/check_page_host_lock.py`）。

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
    QString page_owner(int index) const;
    // ★★ P17-88（§四·8② 运行期那一半）：这一页的**身份** —— 走注册表里**显式宣告**的 `kind`，
    //   **不再拿 `<builtin>` 与 owner 比字符串**（那是"暗号"，冒充者只要写对字符串就过）。
    //   口径：`page_owner()` 仍是"谁注册的"（给人看的字），**判断"是不是内置页"一律用本函数**。
    PageKind page_kind(int index) const;

    // ── ★★ 插件改装 P1：Hub ＝ 容器 + 注册表 + 机制提供者（规格 §一）──────────────
    // 页注册表：**内置六页与插件页走同一条路**（修前是两套：`kBuiltinPages[]` 造内置 +
    // `plugin_pages_` 列表塞插件页 ⇒ 同一件事两份实现）。
    PageRegistry& pages() { return pages_; }
    // 把注册表里**可见的**页同步进标签条/页栈。**增量**：已建页**不重建**
    //（页内状态不丢：只有"缺的补、多余的摘、顺序不对的搬"）。返回本次**新建**的页数。
    int sync_pages();
    // 页 id → 当前下标（没有返回 -1）/ 按 id 切页（宿主给页与插件的"页面切换"机制）
    int page_index(const QString& id) const;
    bool switch_to_page(const QString& id);

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
    // 本次运行 `core::set_log_file` 挂上的那个文件（空 = 没挂上 ⇒ 日志没有文件可查）
    QString log_file_path() const { return log_file_path_; }
    bool log_file_active() const;
    // 合并导出。失败 ⇒ **返回 false 且 `error` 非空**（零静默：写不出去必须说得出来）。
    // 最近一次导出失败的理由（空 = 没失败过）——供自检直接读
    LogBox* log_box() const { return log_box_; }
    // ★ 方案一（Hana 重排）：导航改顶部标签条，日志改底部可折叠抽屉
    QPushButton* log_toggle() const { return log_toggle_; }
    QWidget* log_panel() const { return log_panel_; }
    QLabel* status_label() const { return status_label_; }

    // 记一行日志（1.x 的日志栏：主窗口那根滚动条**不**管它；攒批见 LogBox）
    // ★★ P17-94（审计 `03 F-11`）：这里原来是 `Q_INVOKABLE` —— **那是空标**：本类
    //   **不用 `Q_OBJECT`**（见本文件开头），所以元对象系统根本不认识它，`invokeMethod(hub, "log_line")`
    //   只会**静默返回 false**。功能坑当时已改用**回调**绕开（`hub.cpp` 里
    //   `services.log = [this](const QString& line) { log_line(line); };`；页面侧 `page_base.h` 的
    //   `log_to_host()`）。既然没人再按名字调它，就把那个**误导性的标注删掉**，并把口径留在这里。
    void log_line(const QString& text);

    // ⑫12.4：加载插件并把注册期问题显示出来（构造末尾调一次；空目录＝没有插件）
    void load_plugins(const QString& plugin_dir);

    // ★ 2026-10-06：视图菜单的「暗色/亮色主题」与「界面大小」——**改动立即写盘**
    //   （用户原话：「主题和字体大小目前并没有保留继承的功能」⇒ 下次启动要回默认）。
    //   口径：`apply` + `core::save_*` 同一个入口，**保证"看到的"与"记下的"不可能不同**；
    //   写失败**零静默**（`problem` 非空 + 落一行日志），界面照常切换（不回滚）。
    void apply_theme_mode(xrkit::core::ThemeMode mode, QString* problem = nullptr);
    void apply_ui_size(double scale, QString* problem = nullptr);

private:
    // 页注册诊断（被拒 / 替换声明）→ 日志栏 + 状态栏（零静默；取走即清空）
    void drain_page_diagnostics();
    // 给页交付宿主机制（**唯一**一处：`PageBase::attach_services`）
    void attach_page_services(PageBase* page);

    QTabBar* page_tabs_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QPlainTextEdit* log_view_ = nullptr;
    LogBox* log_box_ = nullptr;
    QPushButton* log_clear_ = nullptr;
    QPushButton* log_toggle_ = nullptr;
    QWidget* log_panel_ = nullptr;
    QLabel* status_label_ = nullptr;
    bool log_expanded_ = false;
    QString log_file_path_;      // 本次运行落盘的那个文件（见 log_file_path()）
    xrkit::plugin::Registry registry_;
    std::vector<xrkit::plugin::RegistrationError> plugin_errors_;
    QString plugin_notice_;
    bool plugin_ui_installed_ = false;
    int plugin_slots_rendered_ = 0;
    int plugin_pages_added_ = 0;   // 当前**可见**的插件页数（owner != kBuiltinPageOwner）
    // ★ P1：三份数组**同长同序**（标签条下标 = 页栈下标 = 这里的下标），自检会交叉核对。
    //   修前是"页栈 + 标签条"两处各自维护，插件页另有一个 `plugin_pages_` 列表 ⇒ 三份状态。
    PageRegistry pages_;
    std::vector<QString> page_ids_;
    std::vector<QString> page_owners_;
    std::vector<QWidget*> page_widgets_;
};

}  // namespace xrkit::shell
