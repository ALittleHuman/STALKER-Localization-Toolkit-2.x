#include "xrkit/engine/plugin/widget_facade.h"

#include <mutex>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <iomanip>
#include <fstream>
#include <filesystem>

namespace xrkit::plugin {
namespace {

// 门面指针与"当前面板根"都是**进程级**的：外壳启动时注入、退出时撤销；
// 面板根在宿主调 builder 的那个作用域里有效（`UiPanelScope`）。
// ★ 用 `std::mutex` 保护不是为了并发（UI 全在主线程），而是为了"读的人不会读到写一半"——
//   探针/自检可能从别的线程问可用性。
std::mutex& facade_mutex() {
    static std::mutex instance;
    return instance;
}

const UiFacade*& facade_storage() {
    static const UiFacade* instance = nullptr;
    return instance;
}

UiHandle& panel_root_storage() {
    static UiHandle instance = nullptr;
    return instance;
}

// ★★ P13-2a-2：当前面板的**页面身份**（页面 id + 注册表）—— 门面菜单用它去取
//   "其它插件注册的右键命令"。写法与面板根同一套（进程级 + 互斥保护）。
std::string& page_id_storage() {
    static std::string instance;
    return instance;
}

xrkit::plugin::Registry*& page_registry_storage() {
    static xrkit::plugin::Registry* instance = nullptr;
    return instance;
}

// 门面里"哪些字段是实现的"：名字与 `UiFacade` 的字段一一对应（判据按它对账）。
const char* const kMethodNames[] = {
    "label",      "button",     "row",       "checkbox",    "entry",     "text",
    "entry_on_change",
    "combo",      "drop_box",   "pack",      "text_view",   "progress",  "set_progress",
    "progress_value", "set_text",  "get_text",
    "set_enabled", "alive",     "log_append", "ask_yes_no", "ask_open_file", "ask_save_file",
    // ★ P7c：目录选择（门面新能力；C ABI 那张表没有它）
    "ask_directory",
    "pick_font", "open_path", "context_menu", "copy_text",
    // ★ P7c：读控件当前值
    "combo_value", "checkbox_state", "set_checked", "combo_set_items",
    // ★★ P4：重型 L2 第一批（虚拟列表 / 日志视图 / 拖放目标 / 任务壳 / 线程判定）
    "list",        "list_self_scrolling", "button_on_menu", "list_set_row_count",
    "list_set_row_text",
    "list_row_text",
    "list_select_row", "list_selected_row", "list_set_multi", "list_selected_rows",
    "list_set_selected_rows", "list_on_activate", "list_content_height",
    // ★★ P17-82：拖放到插件造的 tree/list（**本体拖入**；用户 2026-10-09 已拍板要补）
    "list_on_drop", "tree_on_drop",
    "log_view",    "log_view_append",    "log_view_clear",    "log_view_text",
    "drop_zone",   "drop_zone_on_drop",
    "task_panel",  "task_run",           "task_status",       "task_cancel",
    "task_cancel_requested", "task_on_message",
    "on_ui_thread",
    // ★★ P5：重型 L2 第二批（滚动视口 / 可分栏）
    "viewport",    "viewport_fit_content", "viewport_set_min_body_height", "viewport_sync",
    "viewport_can_scroll", "viewport_scroll_top",
    "split",       "split_add_pane",       "split_set_max_size",          "split_set_collapsed",
    "split_is_collapsed", "split_pane_count", "split_sash_pos",
    "tabs", "tabs_add_page", "tabs_page_count", "tabs_set_current", "tabs_current_index",
    // ★★ P6：重型 L2 第三批（树）
    "tree",           "tree_set_columns",   "tree_set_root",       "tree_set_children",
    "tree_on_action", "tree_clear",
    "tree_row_count", "tree_keys",          "tree_set_expanded",   "tree_expanded_keys",
    "tree_set_checked", "tree_checked",     "tree_checked_keys",   "tree_partial_keys",
    "tree_set_filter", "tree_filter_hits",  "tree_select",         "tree_selected_key",
    "tree_selected_keys",
    "tree_on_expand",  "tree_on_check",     "tree_on_activate",
    "tree_on_click",
};

bool has_impl(const UiFacade& facade, const std::string& name) {
    if (name == "label") return static_cast<bool>(facade.label);
    if (name == "button") return static_cast<bool>(facade.button);
    if (name == "row") return static_cast<bool>(facade.row);
    if (name == "checkbox") return static_cast<bool>(facade.checkbox);
    if (name == "entry") return static_cast<bool>(facade.entry);
    if (name == "entry_on_change") return static_cast<bool>(facade.entry_on_change);
    if (name == "text") return static_cast<bool>(facade.text);
    if (name == "combo") return static_cast<bool>(facade.combo);
    if (name == "drop_box") return static_cast<bool>(facade.drop_box);
    if (name == "pack") return static_cast<bool>(facade.pack);
    if (name == "text_view") return static_cast<bool>(facade.text_view);
    if (name == "progress") return static_cast<bool>(facade.progress);
    if (name == "set_progress") return static_cast<bool>(facade.set_progress);
    if (name == "progress_value") return static_cast<bool>(facade.progress_value);
    if (name == "set_text") return static_cast<bool>(facade.set_text);
    if (name == "get_text") return static_cast<bool>(facade.get_text);
    if (name == "set_enabled") return static_cast<bool>(facade.set_enabled);
    if (name == "alive") return static_cast<bool>(facade.alive);
    if (name == "log_append") return static_cast<bool>(facade.log_append);
    if (name == "ask_yes_no") return static_cast<bool>(facade.ask_yes_no);
    if (name == "ask_open_file") return static_cast<bool>(facade.ask_open_file);
    if (name == "ask_save_file") return static_cast<bool>(facade.ask_save_file);
    if (name == "ask_directory") return static_cast<bool>(facade.ask_directory);
    if (name == "pick_font") return static_cast<bool>(facade.pick_font);
    if (name == "open_path") return static_cast<bool>(facade.open_path);
    if (name == "context_menu") return static_cast<bool>(facade.context_menu);
    // ★★ P16-0：补登记（修前只有字段与 Qt 实现，`kMethodNames`/`has_impl` 都没有 ⇒
    //   `ui_facade_has("copy_text")` 恒 false、`api.ui.copy_text()` 恒返回 False，
    //   而 xml 插件不看返回值照样显示"已复制 ID"）。见 tests/check_facade_registry.py。
    if (name == "copy_text") return static_cast<bool>(facade.copy_text);
    if (name == "combo_value") return static_cast<bool>(facade.combo_value);
    if (name == "checkbox_state") return static_cast<bool>(facade.checkbox_state);
    if (name == "set_checked") return static_cast<bool>(facade.set_checked);
    if (name == "combo_set_items") return static_cast<bool>(facade.combo_set_items);
    if (name == "list") return static_cast<bool>(facade.list);
    if (name == "list_self_scrolling") return static_cast<bool>(facade.list_self_scrolling);
    if (name == "button_on_menu") return static_cast<bool>(facade.button_on_menu);
    if (name == "list_set_row_count") return static_cast<bool>(facade.list_set_row_count);
    if (name == "list_set_row_text") return static_cast<bool>(facade.list_set_row_text);
    if (name == "list_row_text") return static_cast<bool>(facade.list_row_text);
    if (name == "list_select_row") return static_cast<bool>(facade.list_select_row);
    if (name == "list_selected_row") return static_cast<bool>(facade.list_selected_row);
    if (name == "list_set_multi") return static_cast<bool>(facade.list_set_multi);
    if (name == "list_selected_rows") return static_cast<bool>(facade.list_selected_rows);
    if (name == "list_set_selected_rows") return static_cast<bool>(facade.list_set_selected_rows);
    if (name == "list_on_activate") return static_cast<bool>(facade.list_on_activate);
    // ★★ P17-82：按结构体字段顺序排（`check_facade_order` 会逐项对位：
    //   结构体里 `list_on_drop` 紧跟 `list_on_activate`、`tree_on_drop` 在 `tree_on_click` 之后）
    if (name == "list_on_drop") return static_cast<bool>(facade.list_on_drop);
    if (name == "list_content_height") return static_cast<bool>(facade.list_content_height);
    if (name == "log_view") return static_cast<bool>(facade.log_view);
    if (name == "log_view_append") return static_cast<bool>(facade.log_view_append);
    if (name == "log_view_clear") return static_cast<bool>(facade.log_view_clear);
    if (name == "log_view_text") return static_cast<bool>(facade.log_view_text);
    if (name == "drop_zone") return static_cast<bool>(facade.drop_zone);
    if (name == "drop_zone_on_drop") return static_cast<bool>(facade.drop_zone_on_drop);
    if (name == "task_panel") return static_cast<bool>(facade.task_panel);
    if (name == "task_run") return static_cast<bool>(facade.task_run);
    if (name == "task_status") return static_cast<bool>(facade.task_status);
    if (name == "task_cancel") return static_cast<bool>(facade.task_cancel);
    if (name == "task_cancel_requested") return static_cast<bool>(facade.task_cancel_requested);
    if (name == "task_on_message") return static_cast<bool>(facade.task_on_message);
    if (name == "on_ui_thread") return static_cast<bool>(facade.on_ui_thread);
    if (name == "viewport") return static_cast<bool>(facade.viewport);
    if (name == "viewport_fit_content") return static_cast<bool>(facade.viewport_fit_content);
    if (name == "viewport_set_min_body_height") {
        return static_cast<bool>(facade.viewport_set_min_body_height);
    }
    if (name == "viewport_sync") return static_cast<bool>(facade.viewport_sync);
    if (name == "viewport_can_scroll") return static_cast<bool>(facade.viewport_can_scroll);
    if (name == "viewport_scroll_top") return static_cast<bool>(facade.viewport_scroll_top);
    if (name == "split") return static_cast<bool>(facade.split);
    if (name == "split_add_pane") return static_cast<bool>(facade.split_add_pane);
    if (name == "split_set_max_size") return static_cast<bool>(facade.split_set_max_size);
    if (name == "split_set_collapsed") return static_cast<bool>(facade.split_set_collapsed);
    if (name == "split_is_collapsed") return static_cast<bool>(facade.split_is_collapsed);
    if (name == "split_pane_count") return static_cast<bool>(facade.split_pane_count);
    if (name == "split_sash_pos") return static_cast<bool>(facade.split_sash_pos);
    if (name == "tabs") return static_cast<bool>(facade.tabs);
    if (name == "tabs_add_page") return static_cast<bool>(facade.tabs_add_page);
    if (name == "tabs_page_count") return static_cast<bool>(facade.tabs_page_count);
    if (name == "tabs_set_current") return static_cast<bool>(facade.tabs_set_current);
    if (name == "tabs_current_index") return static_cast<bool>(facade.tabs_current_index);
    if (name == "tree") return static_cast<bool>(facade.tree);
    if (name == "tree_set_columns") return static_cast<bool>(facade.tree_set_columns);
    if (name == "tree_set_root") return static_cast<bool>(facade.tree_set_root);
    if (name == "tree_set_children") return static_cast<bool>(facade.tree_set_children);
    if (name == "tree_on_action") return static_cast<bool>(facade.tree_on_action);
    if (name == "tree_clear") return static_cast<bool>(facade.tree_clear);
    if (name == "tree_row_count") return static_cast<bool>(facade.tree_row_count);
    if (name == "tree_keys") return static_cast<bool>(facade.tree_keys);
    if (name == "tree_set_expanded") return static_cast<bool>(facade.tree_set_expanded);
    if (name == "tree_expanded_keys") return static_cast<bool>(facade.tree_expanded_keys);
    if (name == "tree_set_checked") return static_cast<bool>(facade.tree_set_checked);
    if (name == "tree_checked") return static_cast<bool>(facade.tree_checked);
    if (name == "tree_checked_keys") return static_cast<bool>(facade.tree_checked_keys);
    if (name == "tree_partial_keys") return static_cast<bool>(facade.tree_partial_keys);
    if (name == "tree_set_filter") return static_cast<bool>(facade.tree_set_filter);
    if (name == "tree_filter_hits") return static_cast<bool>(facade.tree_filter_hits);
    if (name == "tree_select") return static_cast<bool>(facade.tree_select);
    if (name == "tree_selected_key") return static_cast<bool>(facade.tree_selected_key);
    if (name == "tree_selected_keys") return static_cast<bool>(facade.tree_selected_keys);
    if (name == "tree_on_expand") return static_cast<bool>(facade.tree_on_expand);
    if (name == "tree_on_check") return static_cast<bool>(facade.tree_on_check);
    if (name == "tree_on_activate") return static_cast<bool>(facade.tree_on_activate);
    if (name == "tree_on_click") return static_cast<bool>(facade.tree_on_click);
    if (name == "tree_on_drop") return static_cast<bool>(facade.tree_on_drop);
    return false;
}

}  // namespace

void set_ui_facade(const UiFacade* facade) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    facade_storage() = facade;
}

const UiFacade* ui_facade() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    return facade_storage();
}

bool ui_facade_available(std::string* reason) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const UiFacade* facade = facade_storage();
    if (facade == nullptr) {
        if (reason != nullptr) {
            *reason = "宿主没注入控件门面（CLI/无 GUI 进程，或外壳还没装）—— "
                      "插件的注册面照常可用，控件调用一律返回空并在日志里记账";
        }
        return false;
    }
    for (const char* name : kMethodNames) {
        if (!has_impl(*facade, name)) {
            if (reason != nullptr) {
                *reason = std::string("控件门面缺方法「") + name + "」（部分实现 ⇒ 该方法不可用）";
            }
            return false;
        }
    }
    if (reason != nullptr) reason->clear();
    return true;
}

bool ui_facade_has(const std::string& name) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const UiFacade* facade = facade_storage();
    return facade != nullptr && has_impl(*facade, name);
}

std::vector<std::string> ui_facade_method_names() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const UiFacade* facade = facade_storage();
    std::vector<std::string> out;
    if (facade == nullptr) return out;
    for (const char* name : kMethodNames) {
        if (has_impl(*facade, name)) out.emplace_back(name);
    }
    return out;
}

// ── ★★ P17-1：跨线程只读白名单（声明与理由见 widget_facade.h）────────────────────
// 与 `kMethodNames[]` 放在同一处（**唯一一份口径**）：评审时两张名单一眼能对着看。
// 进表的两个条件缺一不可：① 只读（不碰控件）；② 实现侧线程安全。
namespace {
const char* const kWorkerSafeMethods[] = {
    // 只读 `TaskShell` 的**原子取消位**（`task_shell.h` 的 `cancel_requested_`）——
    // 这是插件 worker 轮询取消的唯一入口（引擎的 should_stop/cancel hook 都在调用线程）。
    "task_cancel_requested",
    // 只比线程 id（`widget_facade_qt.cpp` 的 `ui_thread_id()`）—— 自检/插件都可能问。
    "on_ui_thread",
};
}  // namespace

bool ui_method_worker_safe(const std::string& method) {
    for (const char* name : kWorkerSafeMethods) {
        if (method == name) return true;
    }
    return false;
}

bool ui_method_worker_safe(const char* method) {
    return method != nullptr && ui_method_worker_safe(std::string(method));
}

std::vector<std::string> ui_worker_safe_method_names() {
    std::vector<std::string> out;
    for (const char* name : kWorkerSafeMethods) out.emplace_back(name);
    return out;
}

// 前置声明：`set_current_panel_root` 里要用它（定义在下面，且必须是**无锁**版本）
void trace_event_raw(const std::string& page, const std::string& kind,
                     const std::string& detail);

void set_current_panel_root(UiHandle root, const char* reason) {
    UiHandle old_root = nullptr;
    std::string page;
    {
        std::lock_guard<std::mutex> lock(facade_mutex());
        old_root = panel_root_storage();
        page = page_id_storage();
        panel_root_storage() = root;
    }
    // ★★ 2026-10-22（用户口径）：`current_root=0` 要当成**事件**看 —— 谁写的、哪个线程、当时哪页。
    std::ostringstream detail;
    detail << "old=" << old_root << " new=" << root
           << " page=" << page
           << " reason=" << (reason != nullptr ? reason : "other")
           << " thread_std=" << std::hash<std::thread::id>{}(std::this_thread::get_id());
    trace_event_raw(page, "root_set", detail.str());
}

UiHandle current_panel_root() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    return panel_root_storage();
}

void set_current_page_context(const std::string& page_id, xrkit::plugin::Registry* registry) {
    std::lock_guard<std::mutex> guard(facade_mutex());
    page_id_storage() = page_id;
    page_registry_storage() = registry;
}

// ★ 2026-10-22（用户口径）：**带 page 的无锁版本** —— `set_current_panel_root` 持有
//   `facade_mutex` 时不能再调 `current_page_id()`（同一把锁 ⇒ 自死锁）。
// ★★ 2026-10-22：**诊断开关**。这套 trace 每行一次 open/write/close，
//   一次 `--smoke` 会写几百 KB；开着它本身就会拖慢器件、改变时序
//   ⇒ 我把它从"总是开"改成"默认关、`XRKIT_PAGE_TRACE=1` 才开"。
//   （教训与用户 2026-10-21 那条一致：**加诊断不许改变被测对象**。）
bool page_trace_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("XRKIT_PAGE_TRACE");
        return v != nullptr && v[0] == '1';
    }();
    return enabled;
}

// ★ 2026-10-22（用户口径）：**只筛一页**（`XRKIT_PAGE_TRACE_FILTER=门面L2夹具页`），
//   避免全量 trace 把文件写大、也避免 I/O 改变时序。
const std::string& page_trace_filter() {
    static const std::string filter = [] {
        const char* v = std::getenv("XRKIT_PAGE_TRACE_FILTER");
        return std::string(v != nullptr ? v : "");
    }();
    return filter;
}

bool page_trace_accepts(const std::string& page) {
    const std::string& filter = page_trace_filter();
    if (filter.empty()) return true;
    // ★ 2026-10-22（用户口径）：**ASCII 子串**（过滤值只许 ASCII，避开 PowerShell 的编码坑）。
    return page.find(filter) != std::string::npos;
}

// ★ 2026-10-22（用户口径）：**进程内 ring buffer**（固定容量、丢最旧），
//   失败时一次性 dump —— 不再每条 open/write/close（前一版正是它把 P5 竞态拖过去的）。
std::mutex& trace_ring_mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::string>& trace_ring() {
    static std::vector<std::string> ring;
    return ring;
}
constexpr size_t kTraceRingCap = 4096;

void flush_page_trace();   // 前置声明（下面的自动 flush 结构要用）

namespace {
// ★ 2026-10-22（用户口径）：**由引擎自己**在进程退出前 flush —— 不靠外部调用者，
//   也不从 Python 父进程调（父进程拿不到子进程的 ring）。
struct PageTraceAutoFlush {
    PageTraceAutoFlush() { std::atexit([] { flush_page_trace(); }); }
};
PageTraceAutoFlush g_page_trace_auto_flush;
}  // namespace

void flush_page_trace() {
    if (!page_trace_enabled()) return;
    std::vector<std::string> copy;
    {
        std::lock_guard<std::mutex> lock(trace_ring_mutex());
        copy.swap(trace_ring());
    }
    if (copy.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = fs::temp_directory_path(ec) / "page_trace_engine.txt";
    if (ec) return;
    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out.is_open()) return;
    for (const std::string& line : copy) out << line;
}

void trace_event_raw(const std::string& page, const std::string& kind,
                     const std::string& detail) {
    // ★ 2026-10-22（用户口径）：page 为空时**不许盲用当前页**（scope_dtor 之后可能已还原），
    //   用 thread-local 的"最后一个非空页"兜底。
    static thread_local std::string engine_last_page;
    const std::string effective_page = !page.empty() ? page : engine_last_page;
    if (!page.empty()) engine_last_page = page;
    if (!page_trace_enabled() || !page_trace_accepts(effective_page)) return;
    // ★ 只记这几类（用户口径）：place 太密，默认不记。
    static const bool trace_place = [] {
        const char* v = std::getenv("XRKIT_PAGE_TRACE_PLACE");
        return v != nullptr && v[0] == '1';
    }();
    if (!trace_place && kind == "place") return;
    const auto now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    std::ostringstream line;
    line << effective_page << '|' << kind << '|' << detail << '|' << std::fixed
         << std::setprecision(6) << now << '|'
         << std::hash<std::thread::id>{}(std::this_thread::get_id()) << '|'
         << std::this_thread::get_id() << '\n';
    std::lock_guard<std::mutex> lock(trace_ring_mutex());
    auto& ring = trace_ring();
    ring.push_back(line.str());
    if (ring.size() > kTraceRingCap) ring.erase(ring.begin());
}

void trace_event(const std::string& kind, const std::string& detail) {
    trace_event_raw(current_page_id(), kind, detail);
}

std::string current_page_id() {
    std::lock_guard<std::mutex> guard(facade_mutex());
    return page_id_storage();
}

xrkit::plugin::Registry* current_page_registry() {
    std::lock_guard<std::mutex> guard(facade_mutex());
    return page_registry_storage();
}

UiPanelScope::UiPanelScope(UiHandle root) : scope_root_(root) {
    previous_ = current_panel_root();
    set_current_panel_root(root, "py_scope_ctor");
    // ★ 2026-10-22（用户口径）：带 **scope 地址**（this）与 previous/root ——
    //   只看 ctor/dtor 数量不够，要看 dtor 的 `current_before` 是不是它自己的 root。
    std::ostringstream detail;
    detail << "scope=" << static_cast<const void*>(this) << " root=" << root
           << " previous=" << previous_
           << " thread_std=" << std::hash<std::thread::id>{}(std::this_thread::get_id());
    trace_event("scope_ctor", detail.str());
}

// ★ P13-2a-2：连页面身份一起设（宿主建插件页/面板时用）
UiPanelScope::UiPanelScope(UiHandle root, const std::string& page_id,
                           xrkit::plugin::Registry* registry)
    : restores_page_context_(true),
      scope_root_(root),
      previous_page_id_(current_page_id()),
      previous_registry_(current_page_registry()) {
    previous_ = current_panel_root();
    set_current_panel_root(root, "scope_ctor");
    set_current_page_context(page_id, registry);
    std::ostringstream detail;
    detail << "scope=" << static_cast<const void*>(this) << " root=" << root
           << " previous=" << previous_ << " page=" << page_id
           << " thread_std=" << std::hash<std::thread::id>{}(std::this_thread::get_id());
    trace_event("scope_ctor", detail.str());
}

UiPanelScope::~UiPanelScope() {
    {
        const UiHandle before = current_panel_root();
        std::ostringstream detail;
        detail << "scope=" << static_cast<const void*>(this) << " current_before=" << before
               << " own_root=" << scope_root_ << " previous=" << previous_
               << " lifo=" << (before == scope_root_ ? 1 : 0)
               << " thread_std=" << std::hash<std::thread::id>{}(std::this_thread::get_id());
        trace_event("scope_dtor", detail.str());
    }
    set_current_panel_root(previous_, restores_page_context_ ? "scope_dtor" : "py_scope_dtor");
    if (restores_page_context_) {
        set_current_page_context(previous_page_id_, previous_registry_);
    }
}

}  // namespace xrkit::plugin
