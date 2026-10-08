#pragma once
// xrkit::plugin —— **宿主控件门面**（插件改装的 L1/L2，规格 §三）。
//
// ★ 文件名**故意不叫** `ui_*.h`（本文件原叫 `ui_facade.h`）：Qt 的 **AUTOUIC** 会把
//   `#include "ui_*.h"` 当成 `.ui` 生成的界面头 ⇒ 外壳直接 AutoUic 失败
//   （实测：`AutoUic error … includes the uic file "xrkit/engine/plugin/ui_facade.h"`）。
//   类名/函数名仍是 `UiFacade` / `set_ui_facade`，只有**文件名**要躲开那个前缀。
//
// 为什么要有它：规格 §二 的硬边界是「**插件永不接触 Qt**」，而 §七·补 的下限是
//   「**对于控件的操控，必须至少支持现有的所有功能和观感**」。两者夹出来的唯一出路就是
//   **宿主门面**：插件只描述"要什么控件、什么内容、什么回调"，渲染/排版/委托/滚动/像素
//   全在宿主 C++ 里；插件手里只有**不透明句柄**。
//
// 三条口径：
//   ① **门面由外壳注入**（`set_ui_facade`）：引擎层（Qt-free）只持有指针并转发 ——
//      与 `set_active_registry` 同族。没有门面（CLI/无 GUI 进程）时每个方法返回 null/0，
//      并由调用方**逐条记账**（零静默）；**注册面照常可用**（无 Qt 进程也能驱动整套注册）。
//   ② ★**表外无门**：Python 侧能看到的只有本文件里这些方法（判据＝把 `dir(xrkit.Ui)` /
//      `dir(xrkit.Api)` 与预期表对账，**多一个就红** —— 见 `tests/check_ui_capability_table.py`）。
//   ③ **parent==nullptr ⇒ 落到"当前面板根"**：宿主在调插件 builder 之前设好（`UiPanelScope`），
//      于是插件页/面板的写法可以是"往宿主给的根里塞控件"，不必自己造根控件。
//
// 档位（规格 §三）：
//   · L1 配置/装配 —— `label/button/row/checkbox/entry/text/combo/drop_box/pack/text_view`；
//   · L2 操作 —— `set_text/get_text/set_enabled/alive` + 宿主服务 `log_append/ask_*`；
//   · ★重型 L2（`TreeView/VirtualList/SplitPane/TaskPanel/DropZone` 的表内方法）是**下一片**，
//     它们不是"再造一遍 Qt 控件"，而是把六页已经在用的那几套能力逐个透出（补门面、不开口子）。
//   · L3/L4（绘制回调 / 裸 Qt 对象）在本文件里**没有任何入口**。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::plugin {

class Registry;   // ★ P13-2a-2：面板上下文（菜单追加注册表项）

// 不透明句柄：对插件而言就是"一个控件"。它只能把它传回门面，不能解引用。
using UiHandle = void*;
inline constexpr UiHandle kNoWidget = nullptr;

struct UiFacade {
    // ── L1：造控件（宿主负责排版规范 / 像素 / 主题）────────────────────────────
    std::function<UiHandle(UiHandle parent, const std::string& text, const std::string& style)> label;
    std::function<UiHandle(UiHandle parent, const std::string& text, std::function<void()> on_click,
                           int width, const std::string& style, bool enabled)>
        button;
    std::function<UiHandle(UiHandle parent, const std::string& side, const std::string& fill,
                           bool expand, int padx, int pady)>
        row;
    std::function<UiHandle(UiHandle parent, const std::string& text,
                           std::function<void(bool)> on_toggle)>
        checkbox;
    std::function<UiHandle(UiHandle parent, const std::string& initial, int width, bool password)>
        entry;
    // ★★ P15-0：**输入变化回调**（fs 搜索框「边打边筛」＝`QLineEdit::textChanged`）。
    //   回调给**当前文本**（与 `get_text` 同一份真值）。
    std::function<void(UiHandle widget, std::function<void(const std::string&)> on_change)>
        entry_on_change;
    std::function<UiHandle(UiHandle parent, int height, int width, const std::string& wrap)> text;
    std::function<UiHandle(UiHandle parent, const std::vector<std::string>& values,
                           std::function<void(const std::string&)> on_change)>
        combo;
    std::function<UiHandle(UiHandle parent, std::function<void(const std::string&)> on_file,
                           const std::string& placeholder)>
        drop_box;
    std::function<void(UiHandle parent, UiHandle child, const std::string& side,
                       const std::string& fill, bool expand, int padx, int pady)>
        pack;
    std::function<UiHandle(UiHandle parent, int height, bool mono)> text_view;
    // ★★ P12c：**进度条**（video 页转码进度；0–100，设值按 Qt 口径夹紧）
    std::function<UiHandle(UiHandle parent)> progress;
    std::function<void(UiHandle widget, int percent)> set_progress;
    std::function<int(UiHandle widget)> progress_value;
    // ── L2（轻）：改控件 / 取输入 / 宿主服务 ────────────────────────────────
    std::function<void(UiHandle widget, const std::string& text)> set_text;
    std::function<std::string(UiHandle widget)> get_text;
    std::function<void(UiHandle widget, bool enabled)> set_enabled;
    // ★★ P7c（样板页反向定义）：**读**当前值 —— 下拉选中的文本、勾选框的勾选态。
    //   C++ 页直接读控件（`currentData()` / `isChecked()`）；插件没有裸控件 ⇒ 补这两个。
    std::function<std::string(UiHandle widget)> combo_value;
    std::function<bool(UiHandle widget)> checkbox_state;
    // ★★ P9b：**写**勾选态（convert 页：输出目录非空 ⇒ 取消勾选并禁用「备份」）
    std::function<void(UiHandle widget, bool checked)> set_checked;
    // ★★ P11b-2：**重建下拉项**（xml 页切模式要换筛选下拉 —— 对应 `rebuild_filter_combo`）。
    //   第 0 项成为当前项（与“选第一项”同义）；用户回调**不**因此触发。
    std::function<void(UiHandle widget, const std::vector<std::string>& values)>
        combo_set_items;
    std::function<bool(UiHandle widget)> alive;
    std::function<void(const std::string& line)> log_append;   // 宿主日志栏（LogView）
    std::function<bool(const std::string& title, const std::string& message)> ask_yes_no;
    std::function<std::string(const std::string& title)> ask_open_file;
    std::function<std::string(const std::string& title)> ask_save_file;
    // ★★ P7c：**目录选择**（样板页要「浏览 XML 目录 / 输出目录」；C ABI 那张表没有这一项，
    //   属门面的新能力，实现在外壳）。
    std::function<std::string(const std::string& title)> ask_directory;
    // ★★ P8a：**字库选择器**（宿主唯一一份：`src/shell/font_picker.cpp`）——
    //   系统字体列表 + 四态标记 + 预览 + 缩放 + 「浏览其他目录…」，与内置页**同一个**界面。
    //   非模态 ⇒ 用户确认时回调（空字符串＝取消）。
    std::function<void(const std::string& current,
                       std::function<void(const std::string&)> on_picked)> pick_font;
    // ★★ P9b：在文件管理器里打开一个路径（convert 页的「打开输出」；宿主实现，返回是否成功）
    std::function<bool(const std::string& path)> open_path;
    // ★★ P13-2a：**右键菜单**（fs 页两个列表各一个；由宿主渲染，插件只给标签 + 收回调）。
    //   回调给**选中项的标签原文**；空串 = 用户没选就关掉了。
    std::function<void(UiHandle widget, const std::vector<std::string>& labels,
                       std::function<void(const std::string&)> on_choose)>
        context_menu;
    // ★★ P11b-2：写系统剪贴板（xml 页的「复制 ID」）
    std::function<void(const std::string& text)> copy_text;

    // ── ★★ L2（重型，第一批）：宿主组件的**表内方法** ──────────────────────────
    // 口径（规格 §三）：渲染/委托/滚动/像素全在宿主；插件只给"数据 + 回调"。
    //
    // VirtualList：**虚拟化**行列表（只为视口内的行取文本 —— 10.3 同族）。
    //   本批是**单列文本**形态（多列/三态勾选/行尾✕ 随 TreeView 批）。
    std::function<UiHandle(UiHandle parent, int row_height)> list;
    std::function<void(UiHandle list, int rows)> list_set_row_count;
    // ★L2.5：宿主在**可见行**上问插件要文本（`row` 是全局行号）
    std::function<void(UiHandle list, std::function<std::string(int)> provider)> list_set_row_text;
    std::function<std::string(UiHandle list, int row)> list_row_text;
    std::function<void(UiHandle list, int row)> list_select_row;
    std::function<int(UiHandle list)> list_selected_row;
    // ★★ P13-2b：**列表激活**（双击 / 回车）—— fs 页「双击包列表＝加载」。
    //   回调给**行号**（全体行里的下标，与 `list_selected_row` 同一坐标系）。
    std::function<void(UiHandle list, std::function<void(int)> on_activate)>
        list_on_activate;
    std::function<int(UiHandle list)> list_content_height;

    // LogView：行式日志视图（**插件自己的**日志区；宿主日志栏是 `log_append`）
    std::function<UiHandle(UiHandle parent, int height)> log_view;
    std::function<void(UiHandle view, const std::string& line)> log_view_append;
    std::function<void(UiHandle view)> log_view_clear;
    std::function<std::string(UiHandle view)> log_view_text;

    // DropZone：拖入目标（宿主负责"本地文件才接、其余给禁止光标"）
    std::function<UiHandle(UiHandle parent, const std::string& hint)> drop_zone;
    std::function<void(UiHandle zone, std::function<void(const std::vector<std::string>&)> on_drop)>
        drop_zone_on_drop;

    // TaskPanel：宿主任务壳（进度/消息/失败/取消/状态栏）—— 插件**不再自己起线程**。
    //   `task_run(panel, title, worker)`：worker 在**工作线程**里跑，收到两个 sink
    //   （`progress(0~100)` / `message(text)`）；**工作线程里只许调这两个 sink**，
    //   其余门面方法一律回主线程再调（`on_ui_thread` 用来判）。
    using TaskProgress = std::function<void(int)>;
    using TaskMessage = std::function<void(const std::string&)>;
    std::function<UiHandle(UiHandle parent, bool with_cancel)> task_panel;
    std::function<bool(UiHandle panel, const std::string& title,
                       std::function<void(TaskProgress, TaskMessage)> worker)>
        task_run;
    std::function<std::string(UiHandle panel)> task_status;
    std::function<void(UiHandle panel)> task_cancel;
    std::function<bool(UiHandle panel)> task_cancel_requested;
    // ★★ P12c-2：**任务壳的回主线程通道** —— 外壳自己就有（`TaskShell::set_message_handler`：
    //   worker 里 `message(...)` 之后排队回主线程再调它，**回调里可以直接碰控件**）。
    //   有了它，插件页才有一条**合法**的"干完活刷新界面"的路子
    //   （否则只能像 xml 页初版那样在 worker 里刷控件 —— 那是违反契约的写法）。
    std::function<void(UiHandle panel, std::function<void(const std::string&)> handler)>
        task_on_message;

    // 当前调用是否在**宿主 UI 线程**上：造控件/改控件只许在主线程；
    // 不在 ⇒ 一律返回空/0 并由调用方**记账**（零静默，防"工作线程里碰 Qt"那类崩）。
    std::function<bool()> on_ui_thread;

    // ── ★★ L2（重型，第二批）：滚动视口 / 可分栏 ──────────────────────────────
    // ScrollViewport：口径 1.x 1.6「**不许有裸滚动条：条子只能由本组件造**」——
    //   所以插件拿到的句柄是**画布**（body），往它里面 `pack` 控件即可；
    //   条子、滚轮路由、"装得下就不占位"全在宿主。
    //   `min_body_height`：内容高**不许**用"内容请求高度"当内容高（3.3），由插件/宿主给。
    std::function<UiHandle(UiHandle parent, int min_body_height)> viewport;
    std::function<void(UiHandle body, bool content_fit)> viewport_fit_content;
    std::function<void(UiHandle body, int min_body_height)> viewport_set_min_body_height;
    std::function<void(UiHandle body)> viewport_sync;
    std::function<bool(UiHandle body)> viewport_can_scroll;
    std::function<int(UiHandle body)> viewport_scroll_top;

    // SplitPane：可分栏容器（**拖动期不改几何、松手一次重排**；每格上限；可完全收起）。
    //   `split_add_pane` 返回**新的一格容器**（句柄），插件往它里面 `pack`。
    std::function<UiHandle(UiHandle parent, bool horizontal)> split;
    std::function<UiHandle(UiHandle split, int minsize)> split_add_pane;
    std::function<void(UiHandle split, int index, int max_size)> split_set_max_size;
    std::function<void(UiHandle split, int index, bool collapsed)> split_set_collapsed;
    std::function<bool(UiHandle split, int index)> split_is_collapsed;
    std::function<int(UiHandle split)> split_pane_count;
    std::function<int(UiHandle split, int index)> split_sash_pos;

    // ── ★★ P15-1a：**标签页容器**（fs 页「上面解包 / 下面封包 分两个标签」——
    //   用户 2026-09-29 拍板：「其实之前的设计可以，但是**上面解包和下面封包分两个标签**
    //   就没问题了。」）。`split` 是分栏、不是标签 ⇒ 不补就只能改交互口径。
    std::function<UiHandle(UiHandle parent)> tabs;
    // 加一页 ⇒ 返回**该页的容器**（插件往它里面 `pack`）；标题是标签上的字
    std::function<UiHandle(UiHandle tabs, const std::string& title)> tabs_add_page;
    std::function<int(UiHandle tabs)> tabs_page_count;
    std::function<void(UiHandle tabs, int index)> tabs_set_current;   // 越界不动
    std::function<int(UiHandle tabs)> tabs_current_index;             // -1 = 没有

    // ── ★★ L2（重型，第三批）：树（多列 / 懒建 / 展开态 / 过滤计数 / 三态勾选）────────
    // 口径（fs 页那棵树的用户可见语义，逐条搬过来）：
    //   · **懒建**：`has_children=true` 的行只画"有子"的箭头；用户真展开时才回调
    //     `on_expand(key)`，插件再 `tree_set_children(key, …)`（六页里的大树都是这么长的）；
    //   · **三态勾选**：勾/取消**向下传播**到子孙，再**向上重算**祖先（全勾=勾、部分=半），
    //     用户改勾选时回调 `on_check(key, checked)`；插件也能 `tree_set_checked` 改状态；
    //   · **过滤**：宿主按"任一列包含子串"匹配，命中行连同它的祖先一起显示，别的隐藏；
    //     `tree_filter_hits()` 与"命中加粗/计数"同源（六页的搜索框要的就是这个数）。
    // 本批**不含**：行尾 ✕（那个委托要从 fs 页抽出来共用，随 fs 页迁移一起做）、右键菜单。
    struct TreeColumn {
        std::string title;
        int width = 0;   // 0 = 宿主按规范给（最后一列吃剩余宽度）
    };
    struct TreeNodeSpec {
        std::string key;                  // 插件自己的标识（回调用它；同一棵树里唯一）
        std::vector<std::string> cells;   // 每列文本（少给 ⇒ 后面的列留空）
        bool has_children = false;        // 有子 ⇒ 画箭头，展开时才回调
        bool checkable = false;           // 这一行有没有勾选框
        bool checked = false;             // 声明时的初始勾选态（宿主会据此重算祖先）
        // ★★ P13：这一行**有没有行尾动作**（fs 页的 ✕ 那种）。
        //   fs 的口径是「源节点才有」（分组节点没有）；门面把它**参数化**成显式声明。
        bool has_action = false;
    };
    std::function<UiHandle(UiHandle parent, const std::vector<TreeColumn>& columns)> tree;
    // ★★ P11b-2：**换列声明**（xml 页切模式要换表头/列宽 —— 对应页面的
    //   `apply_result_columns()`；只在造树时给列是不够用的）。
    std::function<void(UiHandle tree, const std::vector<TreeColumn>& columns)>
        tree_set_columns;
    std::function<void(UiHandle tree, const std::vector<TreeNodeSpec>& nodes)> tree_set_root;
    std::function<void(UiHandle tree, const std::string& parent_key,
                       const std::vector<TreeNodeSpec>& nodes)>
        tree_set_children;
    // ★★ P13：**行尾动作被点**（fs 页那条「包的条目后面一个叉叉」）。
    //   回调给的是**行 key**（不是行号 —— 树化之后行号与数据下标不是一回事）；
    //   `mark` 是画在行尾的字（默认 ✕）；只有 `has_action` 的行会画、能点。
    std::function<void(UiHandle tree, const std::string& mark,
                       std::function<void(const std::string&)> on_action)>
        tree_on_action;
    std::function<void(UiHandle tree)> tree_clear;
    std::function<int(UiHandle tree)> tree_row_count;                  // 当前**可见**行数
    std::function<std::vector<std::string>(UiHandle tree)> tree_keys;  // 当前可见行的 key（树序）
    std::function<void(UiHandle tree, const std::string& key, bool expanded)> tree_set_expanded;
    std::function<std::vector<std::string>(UiHandle tree)> tree_expanded_keys;
    std::function<void(UiHandle tree, const std::string& key, bool checked)> tree_set_checked;
    std::function<bool(UiHandle tree, const std::string& key)> tree_checked;
    std::function<std::vector<std::string>(UiHandle tree)> tree_checked_keys;
    std::function<std::vector<std::string>(UiHandle tree)> tree_partial_keys;
    std::function<void(UiHandle tree, const std::string& text)> tree_set_filter;
    std::function<int(UiHandle tree)> tree_filter_hits;
    std::function<void(UiHandle tree, const std::string& key)> tree_select;
    std::function<std::string(UiHandle tree)> tree_selected_key;
    // ★★ P15-0：**多选读取**（按树序；fs 包列表「加载选中/取消加载」要一组）。
    std::function<std::vector<std::string>(UiHandle tree)> tree_selected_keys;
    // ★L2.5 回调（都在宿主 UI 线程上被调）
    std::function<void(UiHandle tree, std::function<void(const std::string&)> on_expand)>
        tree_on_expand;
    std::function<void(UiHandle tree, std::function<void(const std::string&, bool)> on_check)>
        tree_on_check;
    std::function<void(UiHandle tree, std::function<void(const std::string&)> on_activate)>
        tree_on_activate;
    // ★★ P15-0：**点行身体**（fs 页那条 `itemClicked`：单击＝展开/折叠或切勾选）。
    //   宿主只报行 key；展开还是切勾选由**插件**按自己的节点语义决定。
    std::function<void(UiHandle tree, std::function<void(const std::string&)> on_click)>
        tree_on_click;
};

// 外壳注入实现（`src/shell/ui_facade_qt.cpp`）。传 nullptr = 撤销（无 GUI 依赖的进程/用例）。
void set_ui_facade(const UiFacade* facade);
const UiFacade* ui_facade();

// 门面可用性（探针/判据用）。不可用时 `reason` 里是**为什么**（零静默）。
//   · 无门面（CLI/离屏自检没装）⇒ "宿主没注入控件门面（CLI/无 GUI 进程）"；
//   · 装了但缺方法 ⇒ 指出缺哪个（门面允许**部分实现**：没实现的方法一律空，调用侧记账）。
bool ui_facade_available(std::string* reason = nullptr);
// 某个方法是否可用（`name` = 上面那些字段名；不认识的名字返回 false）
bool ui_facade_has(const std::string& name);
// 门面里**已实现**的方法名（顺序稳定）——判据拿它和预期表对账。
std::vector<std::string> ui_facade_method_names();

// ── 当前面板根：宿主在调插件 builder 之前设好（见文件头 ③）──────────────────
void set_current_panel_root(UiHandle root);
// ★★ P13-2a-2：**当前面板的页面身份** —— 门面菜单要按它去注册表里取
//   「其它插件注册的右键命令」（宿主建面板时设、退出时恢复）。
void set_current_page_context(const std::string& page_id, Registry* registry);
std::string current_page_id();
Registry* current_page_registry();
UiHandle current_panel_root();

// RAII：作用域内"当前面板根"= root，析构时还原成上一个（可嵌套；宿主调 builder 时用）。
struct UiPanelScope {
    explicit UiPanelScope(UiHandle root);
    // ★ P13-2a-2：连页面身份一起设（宿主建插件页/面板时用）
    UiPanelScope(UiHandle root, const std::string& page_id, Registry* registry);
    ~UiPanelScope();
    UiPanelScope(const UiPanelScope&) = delete;
    UiPanelScope& operator=(const UiPanelScope&) = delete;

private:
    UiHandle previous_ = nullptr;
    // ★ P13-2a-2：只有「连页面身份」那个构造会置位 ⇒ 析构时才知道要不要恢复
    bool restores_page_context_ = false;
    std::string previous_page_id_;
    Registry* previous_registry_ = nullptr;
};

}  // namespace xrkit::plugin
