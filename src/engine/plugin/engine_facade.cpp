#include "xrkit/engine/plugin/engine_facade.h"

#include <mutex>

namespace xrkit::plugin {

// 引擎自带的默认表（实现在 `engine_facade_impl.cpp`；只依赖引擎、不依赖外壳）
const EngineFacade* builtin_engine_facade();

namespace {

std::mutex& facade_mutex() {
    static std::mutex instance;
    return instance;
}

const EngineFacade*& facade_storage() {
    static const EngineFacade* instance = nullptr;
    return instance;
}

// 名字与 `EngineFacade` 的字段一一对应（判据按它对账）
const char* const kMethodNames[] = {
    "font_check",
    "font_pack",
    "font_state_marks",
    "font_size_slots",
};

bool has_impl(const EngineFacade& facade, const std::string& name) {
    if (name == "font_check") return static_cast<bool>(facade.font_check);
    if (name == "font_pack") return static_cast<bool>(facade.font_pack);
    if (name == "font_state_marks") return static_cast<bool>(facade.font_state_marks);
    if (name == "font_size_slots") return static_cast<bool>(facade.font_size_slots);
    return false;
}

// ★★ P7a：**没注入就用引擎自带的默认表** —— 字体那几件只依赖引擎、不依赖外壳，而 CLI
//   （`--py-plugins`）也是插件宿主 ⇒ 默认表让两个宿主都能用（实测：实现在外壳里时，CLI 里的
//   插件调 `font_size_slots()` 拿到的是 None）。外壳仍可 `set_engine_facade()` 覆盖/扩展
//   —— 表是宿主的东西，只是**有默认值**。
const EngineFacade* facade_or_default() {
    return facade_storage() != nullptr ? facade_storage() : builtin_engine_facade();
}

}  // namespace

void set_engine_facade(const EngineFacade* facade) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    facade_storage() = facade;
}

const EngineFacade* engine_facade() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    return facade_or_default();
}

bool engine_facade_available(std::string* reason) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    if (facade == nullptr) {
        if (reason != nullptr) {
            *reason = "宿主没注入引擎调用门面，且引擎没带默认表 —— 插件的注册面照常可用，"
                      "引擎调用一律返回空并在日志里记账";
        }
        return false;
    }
    for (const char* name : kMethodNames) {
        if (!has_impl(*facade, name)) {
            if (reason != nullptr) {
                *reason = std::string("引擎调用门面缺方法「") + name + "」（部分实现 ⇒ 该方法不可用）";
            }
            return false;
        }
    }
    if (reason != nullptr) reason->clear();
    return true;
}

bool engine_facade_has(const std::string& name) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    return facade != nullptr && has_impl(*facade, name);
}

std::vector<std::string> engine_facade_method_names() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    std::vector<std::string> out;
    if (facade == nullptr) return out;
    for (const char* name : kMethodNames) {
        if (has_impl(*facade, name)) out.emplace_back(name);
    }
    return out;
}

}  // namespace xrkit::plugin
