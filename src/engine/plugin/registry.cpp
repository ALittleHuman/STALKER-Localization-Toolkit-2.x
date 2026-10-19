#include "xrkit/engine/plugin/registry.h"

// ★ S-05：随机 token / 哈希 / atexit / 进程探活
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "probe_decls.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/log.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/plugin/contract.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
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
    // ★★ 2026-10-06（审计处理批·追加②）：when 的参数非法 ⇒ **拒绝注册 + 留痕**（零静默）。
    //   修前 `when_from_abi` 完全信任插件给的 `count`（越界读风险），而且就算发现也只能
    //   "回 0 不写 reason" ⇒ 插件侧看到的是"注册成功但永不生效"。这条判据把两者都堵上。
    if (when.invalid()) {
        reject(plugin, "register_command(" + action_id + ") 的 when 参数非法：" + when.invalid_reason);
        return std::nullopt;
    }
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
    if (when.invalid()) {
        reject(plugin, "register_option_entry(" + host + "/" + area + ") 的 when 参数非法：" +
                           when.invalid_reason);
        return false;
    }
    option_entries_.push_back({plugin, host, area, value, std::move(when)});
    return true;
}

bool Registry::register_option(const std::string& plugin, const std::string& label,
                               std::vector<std::string> choices, CommandHandler callback,
                               const std::string& host, const std::string& area, WhenSpec when,
                               int order) {
    if (bad_slot(plugin, "register_option", host)) return false;
    if (when.invalid()) {
        reject(plugin, "register_option(" + label + ") 的 when 参数非法：" + when.invalid_reason);
        return false;
    }
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
    if (when.invalid()) {
        reject(plugin, "register_panel(" + title + ") 的 when 参数非法：" + when.invalid_reason);
        return false;
    }
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

// ★★ P7c：带 `replaces` / `subtitle` 的那一条（样板页迁移要用：插件页要**顶掉**内置页的槽位）。
bool Registry::register_tool(const std::string& plugin, const std::string& name,
                             PanelBuilder builder, int order, const std::string& replaces,
                             const std::string& subtitle, const std::string& title) {
    if (name.empty() || !builder) {
        reject(plugin, "register_tool 需要非空 name 与可调用的 builder");
        return false;
    }
    if (!replaces.empty() && replaces == name) {
        // 自替换＝什么都没换（而且会让"可见页 = 全部 − 被 replaces 命中的"把这一页也摘掉）
        reject(plugin, "register_tool(" + name + ") 的 replaces 指向了自己");
        return false;
    }
    if (!replaces.empty() && !tools_.empty()) {
        for (const ToolReg& other : tools_) {
            if (other.replaces == replaces) {
                reject(plugin, "页「" + replaces + "」已被 " + other.plugin + " 的页「" +
                                   other.name + "」覆盖");
                return false;
            }
        }
    }
    tools_.push_back({plugin, name, std::move(builder), order, replaces, subtitle, title});
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

std::string PluginResidue::describe() const {
    // 逐项 `k=v`：门禁/自检按 owner 断言时**一眼能看出**是哪个容器脏了（不是"总数不对"）
    std::string out = "plugin=" + plugin;
    const std::pair<const char*, size_t> items[] = {{"decryptors", decryptors},
                                                    {"formats", formats},
                                                    {"commands", commands},
                                                    {"menu_items", menu_items},
                                                    {"option_entries", option_entries},
                                                    {"options", options},
                                                    {"panels", panels},
                                                    {"tools", tools},
                                                    {"compare_modes", compare_modes},
                                                    {"errors", errors}};
    for (const auto& item : items) {
        out += " ";
        out += item.first;
        out += "=";
        out += std::to_string(item.second);
    }
    out += " registrations=" + std::to_string(registrations());
    return out;
}

PluginResidue Registry::residue_for(const std::string& plugin) const {
    PluginResidue r;
    r.plugin = plugin;
    const auto count_vec = [&plugin](const auto& items) {
        size_t n = 0;
        for (const auto& item : items) {
            if (item.plugin == plugin) ++n;
        }
        return n;
    };
    r.decryptors = count_vec(decryptors_);
    r.formats = count_vec(formats_);
    for (const auto& [id, reg] : commands_) {
        (void)id;
        if (reg.plugin == plugin) ++r.commands;
    }
    r.menu_items = count_vec(menu_items_);
    r.option_entries = count_vec(option_entries_);
    r.options = count_vec(options_);
    r.panels = count_vec(panels_);
    r.tools = count_vec(tools_);
    for (const CompareModeReg& mode : compare_modes_) {
        // 归属在 `spec.plugin`（内置是 kBuiltinPlugin ⇒ 不会误算到插件头上）
        if (mode.spec.plugin == plugin) ++r.compare_modes;
    }
    for (const RegistrationError& e : errors_) {
        if (e.plugin == plugin) ++r.errors;
    }
    return r;
}

size_t Registry::rollback_plugin(const std::string& plugin) {
    // 事务的 **abort** 面：把这次 init 期间属于 `plugin` 的注册**逐容器**撤掉。
    // ★ 刻意**不碰 `errors_`**（留痕/诊断：失败理由必须看得见，见头文件那段）。
    // ★ `compare_modes_` 按 `spec.plugin` 过滤 ⇒ 内置（`kBuiltinPlugin`）不可能被误撤。
    // ★ 不按"调用 start 时的总数"截断：`registration_count()` 是总量不是归属
    //   （期间别家也可能注册 / 总数相等不等于没撤错）。
    size_t rolled = 0;
    const auto erase_vec = [&plugin, &rolled](auto& items) {
        const size_t before = items.size();
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&plugin](const auto& item) { return item.plugin == plugin; }),
                    items.end());
        rolled += before - items.size();
    };
    erase_vec(decryptors_);
    erase_vec(formats_);
    // commands_ 是 map：键是命令 id，归属看 `CommandReg::plugin`
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (it->second.plugin == plugin) {
            it = commands_.erase(it);
            ++rolled;
        } else {
            ++it;
        }
    }
    erase_vec(menu_items_);
    erase_vec(option_entries_);
    erase_vec(options_);
    erase_vec(panels_);
    erase_vec(tools_);
    {
        const size_t before = compare_modes_.size();
        compare_modes_.erase(
            std::remove_if(compare_modes_.begin(), compare_modes_.end(),
                           [&plugin](const CompareModeReg& mode) {
                               return mode.spec.plugin == plugin;
                           }),
            compare_modes_.end());
        rolled += before - compare_modes_.size();
    }
    return rolled;
}

std::vector<CompareModeReg> builtin_compare_modes() {
    std::vector<CompareModeReg> out;
    for (const engine::xml::XmlModeSpec& spec : engine::xml::builtin_modes()) {
        CompareModeReg mode;
        mode.spec = spec;
        const bool text_mode = spec.key == "text";
        mode.compare = text_mode ? CompareFn(engine::xml::compare_text)
                                 : CompareFn(engine::xml::compare_stats);
        mode.row_builder = [](const engine::xml::XmlCompareResult& result) {
            if (result.only_one_side) return std::string("only_") + result.only_side;
            if (result.consistent) return std::string("same");
            return std::string("diff");
        };
        mode.detail_builder = [text_mode](const engine::xml::XmlCompareResult& result,
                                          const std::string& na, const std::string& nb) {
            return text_mode ? engine::xml::full_text_detail(result, na, nb)
                             : engine::xml::full_detail(result, na, nb);
        };
        out.push_back(std::move(mode));
    }
    return out;
}

DecryptGateResult gate_db_path(const Registry& registry, const std::string& path,
                               const std::string& out_dir) {
    DecryptGateResult result;
    result.path = path;
    const DecryptorReg* decryptor = registry.find_decryptor(path);
    if (decryptor == nullptr) return result;  // 没有插件解密器 ⇒ 一切照旧

    std::string plain;
    if (!decryptor->decrypt(path, plain)) {
        // 命中却解不开：**可见失败**，绝不回落成"当普通包读"（那会读出垃圾还判成功）
        result.ok = false;
        result.error = "这个包需要插件解密器，但 " + decryptor->plugin +
                       " 的解密失败（不回落成普通包读取）：" + path;
        return result;
    }

    // 取输入文件名（去目录）拼一个可读的临时文件名
    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // ★ S-05：文件名用**源路径哈希**（不带原文件名 —— 不泄露包内容清单）
    const std::string target =
        (out_dir.empty() ? std::string(".") : out_dir) + "/" + decrypt_artifact_name(path);

    const std::vector<uint8_t> bytes(plain.begin(), plain.end());
    std::string error;
    if (!core::write_file_atomic(target, bytes, /*atomic=*/true, error)) {
        result.ok = false;
        result.error = "解密结果落盘失败：" + error;
        return result;
    }
    register_decrypt_artifact(target);
    result.used_plugin = true;
    result.path = target;
    core::log(core::Level::Info, "已用插件解密器 " + decryptor->plugin + " 解开：" + path);
    return result;
}

// ── 活动注册表钩子（见 registry.h 的说明）────────────────────────────
namespace {

// 存指针而不是值：注册表由前端持有（Hub / CLI 的局部变量），引擎只借用。
// 用 atomic 是因为解包在**工作线程**里跑（GUI 的 TaskShell），钩子在主线程注入。
std::atomic<const Registry*>& active_slot() {
    static std::atomic<const Registry*> slot{nullptr};
    return slot;
}

// path 是否落在 dir 里（含 dir 自身）？只做**词法**比较（与 `core::path_equals` 同口径：
// 不碰盘、不解析符号链接，Windows 下大小写不敏感）—— 闸门用它认出自己的产物。
bool path_inside_dir(const std::string& path, const std::string& dir) {
    if (path.empty() || dir.empty()) return false;
    const std::filesystem::path target = core::path_from_utf8(dir).lexically_normal();
    std::filesystem::path probe = core::path_from_utf8(path).lexically_normal();
    while (!probe.empty()) {
        if (core::path_equals(core::path_to_utf8(probe), core::path_to_utf8(target))) return true;
        const std::filesystem::path up = probe.parent_path();
        if (up == probe) break;   // 到根了（"/" 的父还是它自己）
        probe = up;
    }
    return false;
}

}  // namespace

void set_active_registry(const Registry* registry) { active_slot().store(registry); }

const Registry* active_registry() { return active_slot().load(); }

namespace {

// ★★ S-05（第三方只读审计 2026-10-18）：解密产物是**运行期中间物**（含游戏资源副本），
//   修前落在 `%TEMP%\xrkit_decrypted\<原文件名>.decrypted` —— **名字可预测**（同机任何进程都能
//   按包名猜到并读取）且**永不清理**（工具一开半天，副本就在盘上躺半天；崩溃时连退出清理都没有）。
//   现在：
//     · 落点 = `%TEMP%\xrkit_decrypted\<pid>-<每次运行随机 token>\` —— 不可预测、按进程隔离；
//     · 文件名 = 源路径的 32 位哈希 ＋ `.decrypted` —— **不泄露包内容清单**（不再带原文件名）；
//     · 退出兜底：`atexit` 删**整棵运行目录**；
//     · 启动清扫：只清「PID 已不存在」的旧运行目录（不碰其它实例正在用的）。
std::string run_token() {
    static const std::string token = [] {
        std::random_device rd;
        std::uint64_t v = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08llx",
                      static_cast<unsigned long long>(v & 0xffffffffULL));
        return std::string(buf);
    }();
    return token;
}

std::string& run_dir_slot() {
    static std::string dir;
    return dir;
}

void cleanup_run_dir() {
    const std::string& dir = run_dir_slot();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(dir), ec);
}

#if defined(_WIN32)
bool pid_alive(unsigned long pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return false;   // 打不开 ⇒ 极可能已退出（权限不足的活进程会被误判，见下）
    ::CloseHandle(h);
    return true;
}
#else
bool pid_alive(unsigned long) { return true; }   // 非 Windows 不扫（保守：宁可不删）
#endif

unsigned long owner_pid_of(const std::string& dirname) {
    const size_t dash = dirname.find('-');
    if (dash == std::string::npos || dash == 0) return 0;
    unsigned long pid = 0;
    for (size_t i = 0; i < dash; ++i) {
        const char ch = dirname[i];
        if (ch < '0' || ch > '9') return 0;
        pid = pid * 10 + static_cast<unsigned long>(ch - '0');
    }
    return pid;
}

}  // namespace

int sweep_stale_decrypt_dirs() {
    std::error_code ec;
    std::filesystem::path root = std::filesystem::temp_directory_path(ec);
    if (ec) return 0;
    root /= "xrkit_decrypted";
    int removed = 0;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code isdir_ec;
        if (!it->is_directory(isdir_ec)) continue;
        const std::string name = it->path().filename().string();
        const unsigned long pid = owner_pid_of(name);
        if (pid == 0) continue;                 // 不是我们的命名空间 ⇒ 不碰
        if (pid == static_cast<unsigned long>(::GetCurrentProcessId())) continue;  // 自己在用
        if (pid_alive(pid)) continue;           // 别的实例还活着 ⇒ 不碰
        std::error_code rm_ec;
        std::filesystem::remove_all(it->path(), rm_ec);
        if (!rm_ec) ++removed;
    }
    return removed;
}

std::string decrypt_output_dir() {
    static const std::string dir = [] {
        std::error_code ec;
        std::filesystem::path root = std::filesystem::temp_directory_path(ec);
        if (ec) root = std::filesystem::path(".");
        root /= "xrkit_decrypted";
        std::error_code ignore;
        std::filesystem::create_directories(root, ignore);
        (void)sweep_stale_decrypt_dirs();       // ★ 启动清扫：只清 PID 已不存在的旧目录
        const std::string dirname = std::to_string(static_cast<unsigned long>(::GetCurrentProcessId())) +
                                    "-" + run_token();
        std::filesystem::path run = root / dirname;
        std::filesystem::create_directories(run, ignore);
        run_dir_slot() = core::path_to_utf8(run);
        std::atexit(cleanup_run_dir);           // ★ 退出兜底：删整棵运行目录
        return run_dir_slot();
    }();
    return dir;
}

namespace {

// ★ S-05 操作级：线程本的会话深度 ＋ 本次操作登记的临时件。
//   用 thread_local：转换/解包跑在工作线程，两个线程的操作各清各的。
int& session_depth() {
    static thread_local int depth = 0;
    return depth;
}

std::vector<std::string>& session_artifacts() {
    static thread_local std::vector<std::string> files;
    return files;
}

void delete_artifacts(std::vector<std::string>& files) {
    for (const std::string& f : files) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(f), ec);
    }
    files.clear();
}

}  // namespace

DecryptSessionScope::DecryptSessionScope() { ++session_depth(); }

DecryptSessionScope::~DecryptSessionScope() {
    int& depth = session_depth();
    if (depth > 0) --depth;
    if (depth == 0) delete_artifacts(session_artifacts());   // ★ 最外层作用域结束 ⇒ 真删
}

void register_decrypt_artifact(const std::string& path) {
    if (path.empty()) return;
    session_artifacts().push_back(path);
}

std::string decrypt_artifact_name(const std::string& source_path) {
    // FNV-1a 32 位：只要"同一源在本进程内稳定、且不泄露原名"就够了（不是安全哈希）
    std::uint32_t h = 2166136261u;
    for (const char ch : source_path) {
        h ^= static_cast<std::uint8_t>(ch);
        h *= 16777619u;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", h);
    return std::string(buf) + ".decrypted";
}

DecryptGateResult gate_active_read(const std::string& path, const Registry* registry) {
    const Registry* active = registry != nullptr ? registry : active_registry();
    DecryptGateResult result;
    result.path = path;
    if (active == nullptr) return result;   // 没有活动注册表 ⇒ 与"没有这段代码"一致
    const std::string out_dir = decrypt_output_dir();
    if (path_inside_dir(path, out_dir)) return result;   // 已是闸门产物 ⇒ 不二次解密
    return gate_db_path(*active, path, out_dir);
}

}  // namespace xrkit::plugin

// ── ★★ 2026-10-06（审计第 4 组）：模块**在编**探针（见 probe_decls.h）──────────
// 本模块被摘掉 ⇒ 链接失败，而不是 `status_line()` 里少报一个数字。
extern "C" const char* xrkit_engine_probe_plugin_tag() { return "xrkit-engine-module-plugin"; }

namespace xrkit::engine {
const char* probe_plugin() { return xrkit_engine_probe_plugin_tag(); }
}  // namespace xrkit::engine

