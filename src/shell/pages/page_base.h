#pragma once

#include <functional>
#include "../drop_zone.h"
// xrkit::shell —— 页基类（M2-6a）：对照表 ⑤（页内滚动 + 标签条）的落地形态。
//
// ⑤ 的三条口径逐条照抄（出处见 `docs/2.0.0-行为口径对照表.md`）：
//   5.1 **Notebook 绝不能被包进滚动视口**（否则一滚标签条被滚走、主导航消失）；
//       每页**自己**套一层 `ScrollViewport(fit="content", minsize_y=page_min_h())`。
//       Hub 的页列表/页栈在页**外面**，谁都不许把它塞进视口 —— 自检会沿父链查。
//   5.2 `page_min_h() = px(430)` 必须是**函数**（`px()` 依赖缩放就绪）。
//       写成常量在 150% 屏上照样返回 430，等于把"按 96 DPI 定的值"钉死在别的缩放上。
//   5.3 参数名是 **`minsize_y`**（1.x 里写成 `min_y=` 直接 `TclError`、程序都起不来）。
//       C++ 没有关键字参数，所以把这条契约做成**具名 setter** `set_min_body_height()`，
//       并且让"忘了设"变成可判定的红：`ScrollViewport` 的默认值是 0，不等于 `page_min_h()`。
//
// 另外照抄对照表 §2.0 规则 2：**操作栏先放且贴底**。两条实质约束：
//   · 操作栏**不许进视口**（进了就会被滚走，用户滚到中间就找不到"开始"）；
//   · 窗口压到 `page_min_h()` 时操作栏**必须还在**（不能被内容挤没）。
// 两条都在自检里量，不靠"看着对"。

#include "xrkit/engine/plugin/registry.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QVBoxLayout;

namespace xrkit::shell {

class ScrollViewport;

// 5.2：页最小高 = `px(430)`。**函数**，不是常量（缩放变了值就得变）。
int page_min_h();

// ── ★★ 2026-10-10（P17-1「线程与取消契约」，审计 §一·1 / 03 F-01）────────────────
// **UI 线程守卫（13.2 的可判形态）**。契约原文（1.x `TaskRunner` 13.2）：
//   「工作线程里不许碰任何控件 —— 数据在主线程读好传进去，工作线程只 emit 进度/消息」。
//
// 为什么要有它（修前事实）：汉化包页点「生成汉化包」后，`task_->run(...)` 的 worker **直接**
// 调 `FontPackPage::build()`；而 `build()` 里 `options_for()` 读下拉框/勾选框、又对
// `result_label_`/`state_label_` 写文本 ⇒ 跨线程碰 QWidget（未定义行为）；页析构时
// `result_label_`（页的子控件，先于 `task_` 被删）与 worker 的写操作撞上就是访问违例
// （审计 03 F-01，Critical）。而且这条在自检里**不可见**：smoke 只在主线程直调 `build()`。
//
// 可判形态不是"文件里写没写遵守"，而是「**碰控件的那条路自己会拒绝**」：
//   · 在 UI 线程 ⇒ 返回 true（照旧干活）；
//   · 不在 UI 线程 ⇒ **记账 + 返回 false**（调用者必须当场放弃，不许静默改控件）。
// 判据：`tests/check_thread_contract.py`（静态：worker 体不许落到碰控件的成员函数上）+
//       smoke（运行期：工作线程里调 `FontPackPage::build/options_for` ⇒ 守卫计数 +1、
//       控件的文本**一个字都不许变**）。
bool ui_thread_guard(const char* what);
int ui_thread_violation_count();
QString ui_thread_last_violation();
void reset_ui_thread_violations();

// ── ★ 插件改装 P1（Hub 机制层）：宿主交给页的**机制**（页只借用、不持有所有权）──────
// 规格 §一：Hub 要把「页注册 / 日志栏 / 任务壳 / 状态栏 / 页面切换」这些机制**暴露**给插件页。
// 这就是那份"暴露"的 C++ 形态：**一个入口**，页要什么从这里取；而不是让页各自去认 Hub ——
// 修前 `page_factory.cpp` 正是 `dynamic_cast<Hub*>(page->window())` 那一路（页与 Hub 互相认识，
// 且 Hub 为了给 fs/xml 注入注册表还得 `dynamic_cast<FsPage*>`/`<XmlPage*>`：双重耦合）。
struct PageServices {
    plugin::Registry* registry = nullptr;             // 插件注册表（context / option_entry / 解密闸门）
    std::function<void(const QString&)> log;          // 日志栏（Hub::log_line）
    std::function<void(const QString&)> set_status;   // 状态栏文字
    std::function<bool(const QString&)> switch_page;  // 按页 id 切页（导航请求）
};

class PageBase : public QWidget {
public:
    PageBase(const QString& page_id, const QString& title, QWidget* parent = nullptr);

    QString page_id() const { return page_id_; }
    QString page_title() const { return title_; }

    QVBoxLayout* outer_layout() const { return outer_; }
    QLabel* header() const { return header_; }
    // ★ 2026-09-27 界面重构：页标题下面加一句**副标题**（说明本页能干什么），
    //   内容区包进**卡片**（QFrame#card：边框+圆角+内边距）——原来六页都是
    //   "标题/说明/操作栏/内容区同一平面 + 裸列表铺满"。自检用它做几何断言。
    QLabel* subtitle() const { return subtitle_; }
    // ★ 排版口径（真机反馈'太挤'）：标题与说明**合并成一行**的容器
    QWidget* header_row() const { return header_row_; }
    QWidget* card() const { return card_; }
    void set_subtitle(const QString& text);

    // ★ 用户 2026-09-30：「日志记录的是什么东西，为什么没有在app当中的操作（例如，加载包）？」
    //   根因：Hub **不用 Q_OBJECT**（回调一律 lambda），页面用
    //   `QMetaObject::invokeMethod(hub, "log_line")` 按名字调用 ⇒ 元对象系统不认识它 ⇒ **静默失败**。
    //   ⇒ 改用**回调**：Hub 造页时把它的 log_line 接进来，页面用 log_to_host() 转发。
    void set_log_sink(std::function<void(const QString&)> sink) { log_sink_ = std::move(sink); }
    void log_to_host(const QString& text) const { if (log_sink_) log_sink_(text); }

    // ★★ 插件改装 P1：宿主在**页造好之后**调这一次（`Hub::sync_pages()` 里唯一一处调用点）。
    //   默认实现只把"注册表 + 日志口"存下来；需要别的机制的页覆写它
    //   （例：fs / xml 要拿注册表去装解密闸门与比较模式表）。**虚函数 + 唯一入口**
    //   ⇒ Hub 不需要认识任何具体页类（静态锁见 `tests/check_page_host_lock.py`）。
    virtual void attach_services(const PageServices& services);
    const PageServices& services() const { return services_; }
    ScrollViewport* viewport() const { return viewport_; }
    QWidget* body() const { return body_; }
    QVBoxLayout* body_layout() const { return body_layout_; }
    QWidget* action_bar() const { return action_bar_; }
    QHBoxLayout* action_layout() const { return action_layout_; }

    // ── ★★ 2026-10-05（用户报告 #6）：**页内一块自足内容区的滚动条**（第二层视口）──────
    //
    // 现状（修前）：页里那块"结果区"（编码转换/文本提取的行列表 `VirtualList`）**根本没有
    // 自己的条子** —— 它只是个 QWidget，高度由卡片给（实测 1034×232），行数超了就**够不着**。
    // 用户原话：「编码转换的中间那块区域并没有设计滚动条。」
    //
    // 为什么入口在**基类**而不是各页自己 `new ScrollViewport`：
    //   门禁那条"页内视口（5.1）：视口只能由 PageBase 造"的静态锁**就是这么要求的** ——
    //   各页自己 new 一层视口＝各写各的 `fit`/`min_body_height`，而 1.x 的 `min_y=` 事故
    //   正是这么来的（一处写错、整页起不来）。所以第二层视口**也**由基类造、各页只登记内容件。
    //   判据（smoke）：`inner_scroll_active()` ⇒ 条子存在 + `maximum()>0`（能滚）；
    //   装得下 ⇒ `bar_shown()` 假（不占位）。**改回"没有条子"即红。**
    ScrollViewport* make_inner_scroll(QWidget* content, const QString& object_name);
    ScrollViewport* inner_scroll() const { return inner_viewport_; }
    QWidget* inner_scroll_content() const { return inner_content_; }
    // 内容件（VirtualList）行数变了 ⇒ 把它**自己说的内容高**同步给视口（能滚/装得下由它决定）。
    void sync_inner_scroll();

    // 往操作栏里加一个按钮（右对齐；操作栏贴底且不进视口）
    QPushButton* add_action(const QString& text);

    // 5.3 的判据：页自己的视口是否**真的**按 `page_min_h()` 设了 minsize
    //（自检用它做"忘了设就红"的断言，而不是自己抄一份期望值）
    bool minsize_ok() const;

    // ★ 2026-10-04 用户第 1 条：「所有目录都需要拖拽，而且是**拖拽到相应区域**。」
    //   每页一份的拖放区域登记簿 = **只有一个实例**（在基类里）—— 六页各写一份成员就是
    //   "同类东西维护六套"（用户红线）。页面想加区域就 `drop_zones().attach_row(整行, this)`。
    DropZones& drop_zones() { return drop_zones_; }
    const DropZones& drop_zones() const { return drop_zones_; }

    // ── ★★ 2026-10-06（① 用户拍板「插件的架构Bug，那必须修啊。」）插件槽位在页面侧的入口 ──
    // 两类能力的"消费端"都必须落在**页面**上，否则插件注册的东西只是画出来了、没人读：
    //   · `context`（右键菜单）——页面把插件命令追加进**自己那张表的右键菜单**；
    //   · `option_entry`（下拉条目）——页面要能**读到**插件下拉的当前值。
    // 注册表由 Hub 造页时注入（同一份 `Hub::registry_`，页面只借用、不持有所有权）。
    void set_plugin_registry(plugin::Registry* registry) { plugin_registry_ = registry; }
    plugin::Registry* plugin_registry() const { return plugin_registry_; }

    // ⑫12.1 `context` 槽：把插件注册在这页（`page_id`）的右键命令追加到 menu 末尾，
    // 顺序照 1.x `attach_plugin_context_menu` / `PLUGINS.md` §3.3：**「原有项 → 分隔符 → 插件项」**。
    //   · 返回追加了几条；**一条都没有时连分隔符都不加**（1.x「无注册即无痕迹」，
    //     绝不吞掉控件原有的右键行为）；
    //   · `context` 由**页面**给（例：fs 页的 `{"loaded": "1"|"0"}`）⇒ 菜单每次弹出都现算，
    //     所以 `when` 不是"构造时冻结的死条件"（1.x `fs_app.py:314-349` 同口径）；
    //   · 菜单项点击⇒ `Registry::invoke(plugin, label, handler)`：**命令回传走既有
    //     `CommandReg`/契约**，ABI 一个字都没加。
    int append_plugin_context_items(QMenu* menu, const plugin::Context& context);

    // ⑫12.1 `option_entry` 值回流：Hub 为某页渲染出来的插件下拉（`register_option` 新建的、
    // 或某个 area 现建的）都**登记到页面**上（area → 控件）；页面侧的取值入口就是下面三个。
    // 修前的事实：`hub.cpp` 建完 QComboBox 就把指针放在**局部 QHash** 里，函数一返回就没了
    // ⇒ 插件加的下拉条目"只显示不生效"（页面拿不到它的值）。
    void set_plugin_option_combo(const QString& area, QComboBox* combo);
    QComboBox* plugin_option_combo(const QString& area) const;
    // 该 area 插件下拉的**当前值**（空串 = 这页没有该 area 的插件下拉）
    QString plugin_option_value(const QString& area) const;
    // ★★ 2026-10-10（P17-41，审计 03 F-05）：Hub 重渲染前**真销毁**上一轮的插件下拉。
    //   修前这里只是 `plugin_option_combos_.clear();` —— **只摘登记、不删控件**；
    //   而 Hub 的重渲染在「该 area 没有下拉」时会新建一个并 `addWidget` 到同一布局
    //   ⇒ 每重渲染一次就多留一套控件（线性累积、布局越排越长）。
    //   现在：摘布局 → `deleteLater()` → 清表（实现见 .cpp）。
    void clear_plugin_option_combos();

    // ② 点页面空白 ⇒ 把输入框焦点收回页面（用户真机："点空白并不能取消，还是一闪一闪"）
    bool eventFilter(QObject* watched, QEvent* event) override;

protected:
    // ★ 2026-10-04 用户第 1 条：拖放区域登记簿**唯一实例**在基类（页面直接 `drop_zones_` 用）
    DropZones drop_zones_;

private:
    std::function<void(const QString&)> log_sink_;   // ★ Hub 接进来的日志口（见 set_log_sink）
    PageServices services_;                          // ★ P1：宿主交付的机制（见 attach_services）
    // ★★ 2026-10-06（①）：插件槽位在页面侧的两样东西（见上面的说明）
    plugin::Registry* plugin_registry_ = nullptr;          // Hub 注入（借用，不持有）
    QHash<QString, QComboBox*> plugin_option_combos_;      // area → 插件下拉（取值入口）
    QString page_id_;
    QString title_;
    QVBoxLayout* outer_ = nullptr;
    QLabel* header_ = nullptr;
    QWidget* header_row_ = nullptr;  // 标题与说明同一行（真机反馈：别叠薄行）
    QLabel* subtitle_ = nullptr;
    QWidget* card_ = nullptr;
    ScrollViewport* viewport_ = nullptr;
    // ★ 2026-10-05（#6）：页内的第二层视口（可选；由 make_inner_scroll() 造）
    ScrollViewport* inner_viewport_ = nullptr;
    QWidget* inner_content_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;
    QWidget* action_bar_ = nullptr;
    QHBoxLayout* action_layout_ = nullptr;
};

}  // namespace xrkit::shell
