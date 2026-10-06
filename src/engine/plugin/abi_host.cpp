#include "xrkit/engine/plugin/abi_host.h"

#include <array>
#include <cstring>
#include <map>
#include <sstream>

namespace xrkit::plugin {
namespace {

// 把 "\t" 分隔的三段还原成 (id, a, b)
std::array<std::string, 3> split3(const std::string& text) {
    std::array<std::string, 3> out;
    size_t field = 0;
    std::string current;
    for (char c : text) {
        if (c == '\t' && field < 2) {
            out[field++] = current;
            current.clear();
            continue;
        }
        current += c;
    }
    out[field] = current;
    return out;
}

}  // namespace

WhenSpec when_from_abi(const stk_when* when) {
    WhenSpec spec;
    if (when == nullptr) return spec;  // 恒真
    for (int i = 0; i < when->count; ++i) {
        const char* key = (when->keys != nullptr) ? when->keys[i] : nullptr;
        if (key == nullptr) continue;
        const char* value = (when->values != nullptr) ? when->values[i] : nullptr;
        if (value == nullptr) {
            spec.match.push_back({key, ContextValue{}});  // 期望 None（缺键也算命中）
        } else {
            spec.match.push_back({key, std::string(value)});
        }
    }
    if (when->fn != nullptr) {
        // 把裸函数指针 + user 包成 lambda；异常由 Registry::invoke/when_ok 兜住
        const stk_when_fn fn = when->fn;
        void* user = when->user;
        const std::vector<std::pair<std::string, ContextValue>> match = spec.match;
        spec.predicate = [fn, user, match](const Context& context) {
            std::vector<const char*> keys;
            std::vector<const char*> values;
            std::vector<std::string> storage;
            keys.reserve(match.size());
            values.reserve(match.size());
            storage.reserve(match.size());
            for (const auto& [key, value] : match) {
                keys.push_back(key.c_str());
                if (!value.has_value()) {
                    values.push_back(nullptr);
                } else {
                    storage.push_back(*value);
                    values.push_back(storage.back().c_str());
                }
            }
            // 宿主 context 也一并交给插件（keys/values 是当次调用的快照）
            (void)context;
            return fn(user, keys.data(), values.data(), static_cast<int>(keys.size())) != 0;
        };
    }
    return spec;
}

PluginApiAdapter::PluginApiAdapter(Registry& registry, std::string plugin_name, const stk_ui* ui)
    : registry_(registry), plugin_name_(std::move(plugin_name)), ui_(ui) {
    api_.abi_version = STK_ABI_VERSION;
    api_.host = reinterpret_cast<stk_host*>(this);
    api_.ui = ui_;  // nullptr = 无 Qt（自检路径）
    api_.register_decryptor = &PluginApiAdapter::on_decryptor;
    api_.register_format = &PluginApiAdapter::on_format;
    api_.register_command = &PluginApiAdapter::on_command;
    api_.register_menu_item = &PluginApiAdapter::on_menu_item;
    api_.register_option_entry = &PluginApiAdapter::on_option_entry;
    api_.register_option = &PluginApiAdapter::on_option;
    api_.register_panel = &PluginApiAdapter::on_panel;
    api_.register_tool = &PluginApiAdapter::on_tool;
    api_.register_compare_mode = &PluginApiAdapter::on_compare_mode;
    api_.add_error = &PluginApiAdapter::on_add_error;
}

PluginApiAdapter* PluginApiAdapter::self(stk_host* host) {
    return reinterpret_cast<PluginApiAdapter*>(host);
}

const char* PluginApiAdapter::keep(std::string text) {
    // ★★ 2026-10-06（F1）：这里返回的指针是**跨 ABI 交给插件的长期指针**
    //   （`on_command` 把它当命令 id 交回给 `register_menu_item`），所以容器必须保证
    //   "插入不搬家"。`owned_` 是 `std::deque` ⇒ 标准保证两端插入**不使既有元素的
    //   指针/引用失效** ⇒ `c_str()` 在整个宿主生命周期内有效（选型理由见头文件）。
    //   判红锁：CLI `--selftest` 里"注册 3 个命令后逐个校验早期 id 仍可读且内容正确"
    //   （改回 `std::vector` 立刻红 —— 那道锁会先堆上涂抹同尺寸块再读早期指针）。
    owned_.push_back(std::move(text));
    return owned_.back().c_str();
}

int PluginApiAdapter::on_decryptor(stk_host* host, const char* name, stk_decrypt_check_fn check,
                                   stk_decrypt_fn decrypt, void* user) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || check == nullptr || decrypt == nullptr) return 0;
    const bool ok = self_ptr->registry_.register_decryptor(
        self_ptr->plugin_name_,
        [check, user](const std::string& path) { return check(user, path.c_str()) != 0; },
        [decrypt, user](const std::string& path, std::string& out) {
            void* data = nullptr;
            size_t size = 0;
            if (decrypt(user, path.c_str(), &data, &size) == 0) return false;
            if (data != nullptr && size > 0) {
                out.assign(static_cast<const char*>(data), size);
            }
            return true;
        });
    (void)name;
    return ok ? 1 : 0;
}

int PluginApiAdapter::on_format(stk_host* host, const char* name, stk_format_fn handler,
                                void* user) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || name == nullptr || handler == nullptr) return 0;
    return self_ptr->registry_.register_format(
               self_ptr->plugin_name_, name,
               [handler, user](const std::string& path) { return handler(user, path.c_str()) != 0; })
               ? 1
               : 0;
}

const char* PluginApiAdapter::on_command(stk_host* host, const char* action_id, const char* label,
                                         stk_command_fn handler, void* user,
                                         const stk_when* when) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || action_id == nullptr || handler == nullptr) return nullptr;
    const auto id = self_ptr->registry_.register_command(
        self_ptr->plugin_name_, action_id, label != nullptr ? label : "",
        [handler, user] { handler(user); }, when_from_abi(when));
    if (!id.has_value()) return nullptr;
    return self_ptr->keep(*id);
}

int PluginApiAdapter::on_menu_item(stk_host* host, const char* host_name, const char* location,
                                   const char* command_id, int order) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || host_name == nullptr || command_id == nullptr) return 0;
    // ★★ 2026-10-06（①）：**1.x 口径的默认槽位** —— `toolkit_plugins.py:361` 的签名是
    //   `register_menu_item(..., location="context", ...)`，即"不写 location ⇒ 右键菜单槽"。
    //   C ABI 里"不写"就是 **NULL** ⇒ 落到同一个默认值；修前 NULL 会被 `_bad_slot` 当成
    //   非法槽位**拒掉**（照 1.x 写的插件在这里就失效了，而 C 里传 NULL 是很自然的事）。
    const std::string loc = (location != nullptr) ? location : "context";
    return self_ptr->registry_.register_menu_item(self_ptr->plugin_name_, host_name, loc,
                                                  command_id, order)
               ? 1
               : 0;
}

int PluginApiAdapter::on_option_entry(stk_host* host, const char* host_name, const char* area,
                                      const char* value, const stk_when* when) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || host_name == nullptr || value == nullptr) return 0;
    return self_ptr->registry_.register_option_entry(self_ptr->plugin_name_, host_name,
                                                     area != nullptr ? area : "", value,
                                                     when_from_abi(when))
               ? 1
               : 0;
}

int PluginApiAdapter::on_option(stk_host* host, const char* label, const char* const* choices,
                                int count, stk_command_fn callback, void* user,
                                const char* host_name, const char* area, const stk_when* when,
                                int order) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || label == nullptr || host_name == nullptr) return 0;
    std::vector<std::string> values;
    for (int i = 0; i < count; ++i) {
        if (choices != nullptr && choices[i] != nullptr) values.push_back(choices[i]);
    }
    const auto cb = callback != nullptr ? CommandHandler([callback, user] { callback(user); })
                                        : CommandHandler{};
    return self_ptr->registry_.register_option(self_ptr->plugin_name_, label, std::move(values),
                                               cb, host_name, area != nullptr ? area : "",
                                               when_from_abi(when), order)
               ? 1
               : 0;
}

int PluginApiAdapter::on_panel(stk_host* host, const char* title, stk_builder_fn builder,
                               void* user, const char* host_name, const char* area,
                               const stk_when* when, int order) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || title == nullptr || builder == nullptr || host_name == nullptr) {
        return 0;
    }
    PanelBuilder wrapped = [builder, user](void* parent) -> void* { return builder(user, nullptr); };
    return self_ptr->registry_.register_panel(self_ptr->plugin_name_, title, std::move(wrapped),
                                              host_name, area != nullptr ? area : "",
                                              when_from_abi(when), order)
               ? 1
               : 0;
}

int PluginApiAdapter::on_tool(stk_host* host, const char* name, stk_builder_fn builder, void* user,
                              int order) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || name == nullptr || builder == nullptr) return 0;
    PanelBuilder wrapped = [builder, user](void* parent) -> void* { return builder(user, nullptr); };
    return self_ptr->registry_.register_tool(self_ptr->plugin_name_, name, std::move(wrapped), order)
               ? 1
               : 0;
}

int PluginApiAdapter::on_compare_mode(stk_host* host, const char* name, const char* key, int order,
                                      const char* const* column_keys,
                                      const char* const* column_headings,
                                      const int* column_widths, int column_count,
                                      const char* const* filter_labels, int filter_count,
                                      const char* const* filter_kinds, stk_compare_fn compare,
                                      void* user) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || name == nullptr || key == nullptr || compare == nullptr) return 0;
    CompareModeReg mode;
    mode.spec.name = name;
    mode.spec.key = key;
    mode.spec.order = order;
    mode.spec.plugin = self_ptr->plugin_name_;
    for (int i = 0; i < column_count; ++i) {
        engine::xml::XmlColumn column;
        column.key = (column_keys != nullptr && column_keys[i] != nullptr) ? column_keys[i] : "";
        column.heading =
            (column_headings != nullptr && column_headings[i] != nullptr) ? column_headings[i] : "";
        column.width = (column_widths != nullptr) ? column_widths[i] : 0;
        mode.spec.columns.push_back(std::move(column));
    }
    for (int i = 0; i < filter_count; ++i) {
        engine::xml::XmlFilter filter;
        filter.label = (filter_labels != nullptr && filter_labels[i] != nullptr)
                           ? filter_labels[i]
                           : "";
        if (filter_kinds != nullptr && filter_kinds[i] != nullptr) {
            // 形如 "same|diff|only_a"：`|` 分隔的行类别
            std::string kinds = filter_kinds[i];
            size_t start = 0;
            while (start <= kinds.size()) {
                const size_t bar = kinds.find('|', start);
                const std::string one =
                    kinds.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                if (!one.empty()) filter.row_kinds.push_back(one);
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        }
        mode.spec.filters.push_back(std::move(filter));
    }
    mode.compare = [compare, user](const std::string& rel, const std::string& text_a,
                                   const std::string& text_b, const std::string& enc_a,
                                   const std::string& enc_b) {
        engine::xml::XmlCompareResult result;
        result.rel = rel;
        result.mode = "plugin";
        const auto sink = [](void* sink_user, const char* key, const char* value) {
            auto* target = static_cast<engine::xml::XmlCompareResult*>(sink_user);
            if (key == nullptr || value == nullptr) return;
            const std::string k = key;
            if (k == "consistent") {
                target->consistent = (std::string(value) == "1");
            } else if (k == "only_a") {
                target->only_a.push_back(value);
            } else if (k == "only_b") {
                target->only_b.push_back(value);
            } else if (k == "text_diff") {
                target->text_diff.push_back(split3(value));
            } else if (k == "count_diff") {
                target->count_diff.push_back(split3(value));
            } else if (k == "detail") {
                target->error = value;  // 复用字段：插件给的完整明细
            }
        };
        compare(user, rel.c_str(), text_a.c_str(), text_b.c_str(), enc_a.c_str(), enc_b.c_str(),
                sink, &result);
        result.ids_a = result.id_text_a.size();
        result.ids_b = result.id_text_b.size();
        return result;
    };
    return self_ptr->registry_.register_compare_mode(self_ptr->plugin_name_, std::move(mode)) ? 1
                                                                                              : 0;
}

void PluginApiAdapter::on_add_error(stk_host* host, const char* message) {
    PluginApiAdapter* self_ptr = self(host);
    if (self_ptr == nullptr || message == nullptr) return;
    // 插件主动上报的错误也走同一条留痕通道（Hub 启动时可见）
    self_ptr->registry_.invoke(self_ptr->plugin_name_, "插件自报",
                               [message] { throw std::runtime_error(message); });
}

}  // namespace xrkit::plugin
