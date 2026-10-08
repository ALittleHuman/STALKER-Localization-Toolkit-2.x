#include "xrkit/engine/plugin/host_facade.h"

#include <mutex>

namespace xrkit::plugin {

namespace {

std::mutex& facade_mutex() {
    static std::mutex instance;
    return instance;
}

const HostFacade*& facade_storage() {
    static const HostFacade* instance = nullptr;
    return instance;
}

const char* const kMethodNames[] = {
    "pref_get",
    "pref_set",
    "app_dir",
    "system_font_dirs",
    "run_process",
};

bool has_impl(const HostFacade& facade, const std::string& name) {
    if (name == "pref_get") return static_cast<bool>(facade.pref_get);
    if (name == "pref_set") return static_cast<bool>(facade.pref_set);
    if (name == "app_dir") return static_cast<bool>(facade.app_dir);
    if (name == "system_font_dirs") return static_cast<bool>(facade.system_font_dirs);
    if (name == "run_process") return static_cast<bool>(facade.host_run_process);
    return false;
}

}  // namespace

void set_host_facade(const HostFacade* facade) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    facade_storage() = facade;
}

// 引擎自带的默认表（实现在 `host_facade_impl.cpp`；只依赖引擎 ⇒ CLI 那个宿主也能用）
const HostFacade* builtin_host_facade();

const HostFacade* host_facade() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    return facade_storage() != nullptr ? facade_storage() : builtin_host_facade();
}

bool host_facade_available(std::string* reason) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const HostFacade* facade = facade_storage() != nullptr ? facade_storage() : builtin_host_facade();
    if (facade == nullptr) {
        if (reason != nullptr) *reason = "宿主服务门面不可用（既没注入、也没有默认表）";
        return false;
    }
    for (const char* name : kMethodNames) {
        if (!has_impl(*facade, name)) {
            if (reason != nullptr) {
                *reason = std::string("宿主服务门面缺方法「") + name + "」（部分实现 ⇒ 该方法不可用）";
            }
            return false;
        }
    }
    if (reason != nullptr) reason->clear();
    return true;
}

bool host_facade_has(const std::string& name) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const HostFacade* facade = facade_storage() != nullptr ? facade_storage() : builtin_host_facade();
    return facade != nullptr && has_impl(*facade, name);
}

std::vector<std::string> host_facade_method_names() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const HostFacade* facade = facade_storage() != nullptr ? facade_storage() : builtin_host_facade();
    std::vector<std::string> out;
    if (facade == nullptr) return out;
    for (const char* name : kMethodNames) {
        if (has_impl(*facade, name)) out.emplace_back(name);
    }
    return out;
}

}  // namespace xrkit::plugin
