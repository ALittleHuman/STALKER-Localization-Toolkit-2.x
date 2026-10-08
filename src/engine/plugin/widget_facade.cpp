#include "xrkit/engine/plugin/widget_facade.h"

#include <mutex>

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
    "pick_font", "open_path", "context_menu",
    // ★ P7c：读控件当前值
    "combo_value", "checkbox_state", "set_checked", "combo_set_items",
    // ★★ P4：重型 L2 第一批（虚拟列表 / 日志视图 / 拖放目标 / 任务壳 / 线程判定）
    "list",        "list_set_row_count", "list_set_row_text", "list_row_text",
    "list_select_row", "list_selected_row", "list_on_activate", "list_content_height",
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
    if (name == "combo_value") return static_cast<bool>(facade.combo_value);
    if (name == "checkbox_state") return static_cast<bool>(facade.checkbox_state);
    if (name == "set_checked") return static_cast<bool>(facade.set_checked);
    if (name == "combo_set_items") return static_cast<bool>(facade.combo_set_items);
    if (name == "list") return static_cast<bool>(facade.list);
    if (name == "list_set_row_count") return static_cast<bool>(facade.list_set_row_count);
    if (name == "list_set_row_text") return static_cast<bool>(facade.list_set_row_text);
    if (name == "list_row_text") return static_cast<bool>(facade.list_row_text);
    if (name == "list_select_row") return static_cast<bool>(facade.list_select_row);
    if (name == "list_selected_row") return static_cast<bool>(facade.list_selected_row);
    if (name == "list_on_activate") return static_cast<bool>(facade.list_on_activate);
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

void set_current_panel_root(UiHandle root) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    panel_root_storage() = root;
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

std::string current_page_id() {
    std::lock_guard<std::mutex> guard(facade_mutex());
    return page_id_storage();
}

xrkit::plugin::Registry* current_page_registry() {
    std::lock_guard<std::mutex> guard(facade_mutex());
    return page_registry_storage();
}

UiPanelScope::UiPanelScope(UiHandle root) {
    previous_ = current_panel_root();
    set_current_panel_root(root);
}

// ★ P13-2a-2：连页面身份一起设（宿主建插件页/面板时用）
UiPanelScope::UiPanelScope(UiHandle root, const std::string& page_id,
                           xrkit::plugin::Registry* registry)
    : restores_page_context_(true),
      previous_page_id_(current_page_id()),
      previous_registry_(current_page_registry()) {
    previous_ = current_panel_root();
    set_current_panel_root(root);
    set_current_page_context(page_id, registry);
}

UiPanelScope::~UiPanelScope() {
    set_current_panel_root(previous_);
    if (restores_page_context_) {
        set_current_page_context(previous_page_id_, previous_registry_);
    }
}

}  // namespace xrkit::plugin
