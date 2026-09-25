#include "xrkit/engine/plugin/registry.h"

#include "xrkit/core/log.h"
#include "xrkit/engine/plugin/contract.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace xrkit::plugin {
namespace {

bool valid_action_id(const std::string& action) {
    if (action.empty() || action.size() > 64) return false;
    for (char c : action) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' ||
                        c == '-';
        if (!ok) return false;
    }
    return true;
}

// 12.5：order 升序 + **注册顺序**稳定（`idx` 是注册下标，永远参与比较）
template <typename T>
std::vector<const T*> sorted_by_order(const std::vector<T>& items) {
    std::vector<std::pair<std::pair<int, size_t>, const T*>> staged;
    staged.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        staged.push_back({{items[i].order, i}, &items[i]});
    }
    std::stable_sort(staged.begin(), staged.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const T*> out;
    out.reserve(staged.size());
    for (const auto& item : staged) out.push_back(item.second);
    return out;
}

}  // namespace

bool when_ok(const WhenSpec& when, const Context& context) {
    // 12.6：None / **空 dict** 都恒真（`if not when: return True`）
    for (const auto& [key, want] : when.match) {
        const auto it = context.find(key);
        if (it == context.end()) {
            // Python 是 `ctx.get(k) == v`：**缺键**时 get 给 None，
            // 所以期望值也是 None 的话照样命中（这条容易漏）
            if (want.has_value()) return false;
            continue;
        }
        if (it->second != want) return false;
    }
    if (when.predicate) {
        try {
            return when.predicate(context);
        } catch (...) {
            return false;  // 12.6：可调用抛异常 ⇒ False
        }
    }
    return true;
}

void Registry::reject(const std::string& plugin, const std::string& message) {
    // 12.4/12.9：非法注册**绝不静默丢弃** —— 记进 registration_errors 并写日志
    errors_.push_back({plugin, message});
    core::log(core::Level::Error, plugin + ": " + message);
}

bool Registry::bad_slot(const std::string& plugin, const std::string& what,
                        const std::string& host_key, const std::string& location_key) {
    if (!known_host(host_key)) {
        std::string available;
        for (int i = 0; i < host_count(); ++i) {
            if (!available.empty()) available += ", ";
            available += plugin::host(i).key;  // 名字别与形参同名（否则调不到函数）
        }
        reject(plugin, what + " 的 host=" + host_key + " 不是已知组件（可用: " + available + "）");
        return true;
    }
    if (!location_key.empty() && !known_location(location_key)) {
        std::string available;
        for (int i = 0; i < location_count(); ++i) {
            if (!available.empty()) available += ", ";
            available += plugin::location(i).key;
        }
        reject(plugin,
               what + " 的 location=" + location_key + " 不是已知槽位（可用: " + available + "）");
        return true;
    }
    return false;
}

bool Registry::register_decryptor(const std::string& plugin, DecryptCheck check, DecryptFn decrypt) {
    if (!check || !decrypt) {
        reject(plugin, "register_decryptor 的 check/decrypt 不可为空");
        return false;
    }
    decryptors_.push_back({plugin, std::move(check), std::move(decrypt)});
    return true;
}

bool Registry::register_format(const std::string& plugin, const std::string& name,
                               FormatHandler handler) {
    if (name.empty() || !handler) {
        reject(plugin, "register_format 需要非空 name 与可调用的 handler");
        return false;
    }
    formats_.push_back({plugin, name, std::move(handler)});
    return true;
}

std::optional<std::string> Registry::register_command(const std::string& plugin,
                                                      const std::string& action_id,
                                                      const std::string& label,
                                                      CommandHandler handler, WhenSpec when) {
    if (!valid_action_id(action_id)) {
        reject(plugin, "register_command 的动作名 " + action_id +
                           " 非法（只允许字母/数字/_.-，1~64 字符）");
        return std::nullopt;
    }
    if (!handler) {
        reject(plugin, "register_command(" + action_id + ") 的 handler 不可调用");
        return std::nullopt;
    }
    const std::string id = plugin + ":" + action_id;
    if (commands_.count(id) != 0) {
        reject(plugin, "命令 " + id + " 重复注册");
        return std::nullopt;
    }
    CommandReg reg;
    reg.id = id;
    reg.action = action_id;
    reg.label = label;
    reg.handler = std::move(handler);
    reg.when = std::move(when);
    reg.plugin = plugin;
    commands_[id] = std::move(reg);
    return id;
}

bool Registry::register_menu_item(const std::string& plugin, const std::string& host,
                                  const std::string& location, const std::string& command_id,
                                  int order) {
    if (bad_slot(plugin, "register_menu_item", host, location)) return false;
    if (commands_.count(command_id) == 0) {
        reject(plugin, "挂载点引用了未注册的命令 " + command_id);
        return false;
    }
    menu_items_.push_back({plugin, command_id, host, location, order});
    return true;
}

bool Registry::register_option_entry(const std::string& plugin, const std::string& host,
                                     const std::string& area, const std::string& value,
                                     WhenSpec when) {
    // 1.x 只查 host（`area` 由组件自己解释；没有对应下拉就加不进去）
    if (bad_slot(plugin, "register_option_entry", host)) return false;
    option_entries_.push_back({plugin, host, area, value, std::move(when)});
    return true;
}

bool Registry::register_option(const std::string& plugin, const std::string& label,
                               std::vector<std::string> choices, CommandHandler callback,
                               const std::string& host, const std::string& area, WhenSpec when,
                               int order) {
    if (bad_slot(plugin, "register_option", host)) return false;
    OptionReg reg;
    reg.plugin = plugin;
    reg.label = label;
    reg.choices = std::move(choices);
    reg.callback = std::move(callback);
    reg.host = host;
    reg.area = area;
    reg.when = std::move(when);
    reg.order = order;
    options_.push_back(std::move(reg));
    return true;
}

bool Registry::register_panel(const std::string& plugin, const std::string& title,
                              PanelBuilder builder, const std::string& host,
                              const std::string& area, WhenSpec when, int order) {
    if (bad_slot(plugin, "register_panel", host)) return false;
    if (!builder) {
        reject(plugin, "register_panel(" + title + ") 的 builder 不可调用");
        return false;
    }
    PanelReg reg;
    reg.plugin = plugin;
    reg.title = title;
    reg.builder = std::move(builder);
    reg.host = host;
    reg.area = area;
    reg.when = std::move(when);
    reg.order = order;
    panels_.push_back(std::move(reg));
    return true;
}

bool Registry::register_tool(const std::string& plugin, const std::string& name,
                             PanelBuilder builder, int order) {
    if (name.empty() || !builder) {
        reject(plugin, "register_tool 需要非空 name 与可调用的 builder");
        return false;
    }
    tools_.push_back({plugin, name, std::move(builder), order});
    return true;
}

bool Registry::register_compare_mode(const std::string& plugin, CompareModeReg mode) {
    if (mode.spec.name.empty() || mode.spec.key.empty()) {
        reject(plugin, "register_compare_mode 需要非空 name 与内部键");
        return false;
    }
    if (mode.spec.columns.empty() || !mode.compare) {
        reject(plugin, "register_compare_mode(" + mode.spec.name + ") 需要 columns 与 compare");
        return false;
    }
    // 12.8：哨兵值由**注册表**统一（插件不许自带"我也是内置"的说法）
    mode.spec.plugin = plugin;
    compare_modes_.push_back(std::move(mode));
    return true;
}

std::vector<const CommandReg*> Registry::menu_items_for(const std::string& host,
                                                        const std::string& location,
                                                        const Context& context) const {
    std::vector<std::pair<std::pair<int, size_t>, const CommandReg*>> staged;
    for (size_t i = 0; i < menu_items_.size(); ++i) {
        const MenuItemReg& item = menu_items_[i];
        if (item.host != host || item.location != location) continue;  // 12.4：精确匹配
        const auto it = commands_.find(item.command_id);
        if (it == commands_.end()) continue;  // 命令不存在（不该发生：注册时已挡）
        if (!when_ok(it->second.when, context)) continue;
        staged.push_back({{item.order, i}, &it->second});
    }
    std::stable_sort(staged.begin(), staged.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const CommandReg*> out;
    for (const auto& item : staged) out.push_back(item.second);
    return out;
}

std::vector<const OptionReg*> Registry::options_for(const std::string& host,
                                                    const std::string& area,
                                                    const Context& context) const {
    std::vector<const OptionReg*> filtered;
    for (const OptionReg& option : options_) {
        if (option.host != host) continue;
        if (!area.empty() && option.area != area) continue;
        if (!when_ok(option.when, context)) continue;
        filtered.push_back(&option);
    }
    // 12.5：`options_for` 按 order 升序 + 注册顺序；这里的 `filtered` 已保注册顺序
    std::stable_sort(filtered.begin(), filtered.end(),
                     [](const OptionReg* a, const OptionReg* b) { return a->order < b->order; });
    return filtered;
}

std::vector<std::string> Registry::option_entries_for(const std::string& host,
                                                      const std::string& area,
                                                      const Context& context) const {
    // 12.5：**不排序**、按注册顺序保序去重（与 options_for 的语义刻意不同）
    std::vector<std::string> out;
    for (const OptionEntryReg& entry : option_entries_) {
        if (entry.host != host) continue;
        if (!area.empty() && entry.area != area) continue;
        if (!when_ok(entry.when, context)) continue;
        if (std::find(out.begin(), out.end(), entry.value) == out.end()) out.push_back(entry.value);
    }
    return out;
}

std::vector<const PanelReg*> Registry::panels_for(const std::string& host, const std::string& area,
                                                  const Context& context) const {
    std::vector<const PanelReg*> filtered;
    for (const PanelReg& panel : panels_) {
        if (panel.host != host) continue;
        if (!area.empty() && panel.area != area) continue;
        if (!when_ok(panel.when, context)) continue;
        filtered.push_back(&panel);
    }
    // `sorted(key=order)`（stable）⇒ 同 order 保注册顺序
    std::stable_sort(filtered.begin(), filtered.end(),
                     [](const PanelReg* a, const PanelReg* b) { return a->order < b->order; });
    return filtered;
}

std::vector<const ToolReg*> Registry::tools_sorted() const { return sorted_by_order(tools_); }

std::vector<const CompareModeReg*> Registry::compare_modes_for() const {
    // 12.5/12.8：同名**内置优先**（与注册顺序无关），其余按 order 升序 + 注册顺序
    std::map<std::string, std::pair<std::pair<int, size_t>, const CompareModeReg*>> seen;
    for (size_t i = 0; i < compare_modes_.size(); ++i) {
        const CompareModeReg& mode = compare_modes_[i];
        const bool builtin = mode.spec.plugin == engine::xml::kBuiltinPlugin;
        auto it = seen.find(mode.spec.name);
        if (it == seen.end()) {
            seen[mode.spec.name] = {{mode.spec.order, i}, &mode};
            continue;
        }
        const bool stored_builtin = it->second.second->spec.plugin == engine::xml::kBuiltinPlugin;
        if (stored_builtin) continue;         // 已存的是内置 ⇒ 保持
        if (builtin) it->second = {{mode.spec.order, i}, &mode};  // 内置后到也赢
    }
    std::vector<std::pair<std::pair<int, size_t>, const CompareModeReg*>> staged;
    for (const auto& [name, value] : seen) staged.push_back(value);
    std::stable_sort(staged.begin(), staged.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const CompareModeReg*> out;
    for (const auto& item : staged) out.push_back(item.second);
    return out;
}

const CompareModeReg* Registry::compare_mode(const std::string& name) const {
    // 1.x 是**线性取第一条**（不去重、不排序）—— 保持同样的语义
    for (const CompareModeReg& mode : compare_modes_) {
        if (mode.spec.name == name || mode.spec.key == name) return &mode;
    }
    return nullptr;
}

const DecryptorReg* Registry::find_decryptor(const std::string& path) const {
    for (const DecryptorReg& reg : decryptors_) {
        try {
            if (reg.check(path)) return &reg;
        } catch (...) {
            continue;  // 1.x：check 抛异常就跳过这一个
        }
    }
    return nullptr;
}

void Registry::invoke(const std::string& plugin, const std::string& what,
                      const std::function<void()>& fn) {
    if (!fn) return;
    try {
        fn();
    } catch (const std::exception& exc) {
        // 12.9：**绝不静默、也绝不冒到 UI 层** —— 记一条诊断（宿主启动时可见）
        errors_.push_back({plugin, what + " 回调抛异常：" + exc.what()});
        core::log(core::Level::Error, plugin + ": " + what + " 回调抛异常：" + exc.what());
    } catch (...) {
        errors_.push_back({plugin, what + " 回调抛异常（未知类型）"});
        core::log(core::Level::Error, plugin + ": " + what + " 回调抛异常（未知类型）");
    }
}

void Registry::scan_clear(const std::vector<RegistrationError>& carried_errors) {
    // 12.7：`scan()` **不清空 compare_modes**（内置与插件共用一张表）
    decryptors_.clear();
    formats_.clear();
    commands_.clear();
    menu_items_.clear();
    option_entries_.clear();
    options_.clear();
    panels_.clear();
    tools_.clear();
    errors_ = carried_errors;
}

size_t Registry::registration_count() const {
    return decryptors_.size() + formats_.size() + commands_.size() + menu_items_.size() +
           option_entries_.size() + options_.size() + panels_.size() + tools_.size() +
           compare_modes_.size();
}

}  // namespace xrkit::plugin
