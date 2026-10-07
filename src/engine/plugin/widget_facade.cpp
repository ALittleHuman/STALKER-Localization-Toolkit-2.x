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

// 门面里"哪些字段是实现的"：名字与 `UiFacade` 的字段一一对应（判据按它对账）。
const char* const kMethodNames[] = {
    "label",      "button",     "row",       "checkbox",    "entry",     "text",
    "combo",      "drop_box",   "pack",      "text_view",   "set_text",  "get_text",
    "set_enabled", "alive",     "log_append", "ask_yes_no", "ask_open_file", "ask_save_file",
};

bool has_impl(const UiFacade& facade, const std::string& name) {
    if (name == "label") return static_cast<bool>(facade.label);
    if (name == "button") return static_cast<bool>(facade.button);
    if (name == "row") return static_cast<bool>(facade.row);
    if (name == "checkbox") return static_cast<bool>(facade.checkbox);
    if (name == "entry") return static_cast<bool>(facade.entry);
    if (name == "text") return static_cast<bool>(facade.text);
    if (name == "combo") return static_cast<bool>(facade.combo);
    if (name == "drop_box") return static_cast<bool>(facade.drop_box);
    if (name == "pack") return static_cast<bool>(facade.pack);
    if (name == "text_view") return static_cast<bool>(facade.text_view);
    if (name == "set_text") return static_cast<bool>(facade.set_text);
    if (name == "get_text") return static_cast<bool>(facade.get_text);
    if (name == "set_enabled") return static_cast<bool>(facade.set_enabled);
    if (name == "alive") return static_cast<bool>(facade.alive);
    if (name == "log_append") return static_cast<bool>(facade.log_append);
    if (name == "ask_yes_no") return static_cast<bool>(facade.ask_yes_no);
    if (name == "ask_open_file") return static_cast<bool>(facade.ask_open_file);
    if (name == "ask_save_file") return static_cast<bool>(facade.ask_save_file);
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

UiPanelScope::UiPanelScope(UiHandle root) {
    previous_ = current_panel_root();
    set_current_panel_root(root);
}

UiPanelScope::~UiPanelScope() { set_current_panel_root(previous_); }

}  // namespace xrkit::plugin
