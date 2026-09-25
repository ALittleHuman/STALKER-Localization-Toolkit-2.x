#pragma once
// xrkit::engine::plugin —— 插件注册表（对照表 ⑫12.1/12.4/12.5/12.6/12.7/12.9，无 Qt）。
//
// 1.x `toolkit_plugins.PluginManager` + `Api` 的 C++ 等价物：**10 个注册点**
//（9 个 `api.register_*` + `register_compare_mode` 这个"贡献点"），
// 能力面一律是"**纯数据 + 回调**"——宿主负责渲染，插件永不接触 Qt 类型。
//
// 逐条照 1.x 的代码：
//   · 12.4 槽位**精确匹配**（host ∈ HOSTS、location ∈ LOCATIONS）；非法寻址**注册时**被拒
//     并写 `registration_errors`（绝不静默丢弃——那会表现成"注册了却不生效"）；
//   · 12.5 `order` 升序 + **注册顺序**稳定排序；`option_entries_for` **不排序**、按注册
//     顺序保序去重；`compare_modes_for` 同名时**内置优先**（与注册顺序无关）；
//   · 12.6 `when`：None/空 dict 恒真；dict 缺键即不满足（注意 Python 的 `ctx.get(k) == v`
//     在 v 为 None 时"缺键也算命中"）；可调用抛异常 ⇒ False；
//   · 12.7 `scan()` **不清空** compare_modes（内置与插件共用同一张表）；
//   · 12.9 插件回调抛异常**绝不静默、也绝不冒到 UI 层**：这里捕获并记一条 registration_error
//     级别的诊断日志（宿主侧统一上报）。
//
// ★ 12.8（1.x 缺陷）：`compare_modes_for` 用 `"<builtin>"`、`xml_modes` 用 `"<builtin:xml>"`
//   —— 两套哨兵。2.0 统一成 `engine::xml::kBuiltinPlugin`（唯一哨兵），所以这里判"内置"只认它。

#include "xrkit/engine/xml/compare.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace xrkit::plugin {

// ── when（12.6）───────────────────────────────────────────────────────
// 值用 `optional`：`nullopt` 对应 Python 的 `None`（缺键也命中，照 1.x 的 `.get(k) == v`）
using ContextValue = std::optional<std::string>;
using Context = std::map<std::string, ContextValue>;

struct WhenSpec {
    std::vector<std::pair<std::string, ContextValue>> match;
    std::function<bool(const Context&)> predicate;

    bool always() const { return match.empty() && !predicate; }
};

bool when_ok(const WhenSpec& when, const Context& context);

// ── 注册项 ───────────────────────────────────────────────────────────

using CommandHandler = std::function<void()>;
using PanelBuilder = std::function<void*(void* parent)>;  // 返回不透明控件句柄（M2-7c 接 C ABI）
using FormatHandler = std::function<bool(const std::string& path)>;
using DecryptCheck = std::function<bool(const std::string& path)>;
using DecryptFn = std::function<bool(const std::string& path, std::string& out)>;

// 比较模式（12.1 的第 10 个注册点）：模式声明 + 三个回调
using CompareFn = std::function<engine::xml::XmlCompareResult(
    const std::string& rel, const std::string& text_a, const std::string& text_b,
    const std::string& enc_a, const std::string& enc_b)>;
using RowBuilderFn = std::function<std::string(const engine::xml::XmlCompareResult&)>;
using DetailBuilderFn = std::function<std::string(const engine::xml::XmlCompareResult&,
                                                  const std::string&, const std::string&)>;

struct RegistrationError {
    std::string plugin;
    std::string message;
};

struct CommandReg {
    std::string id;       // "<插件>:<动作名>"
    std::string action;
    std::string label;
    CommandHandler handler;
    WhenSpec when;
    std::string plugin;
};

struct MenuItemReg {
    std::string plugin;
    std::string command_id;
    std::string host;
    std::string location;
    int order = 0;
};

struct OptionEntryReg {
    std::string plugin;
    std::string host;
    std::string area;
    std::string value;
    WhenSpec when;
};

struct OptionReg {
    std::string plugin;
    std::string label;
    std::vector<std::string> choices;
    CommandHandler callback;
    std::string host;
    std::string area;
    WhenSpec when;
    int order = 0;
};

struct PanelReg {
    std::string plugin;
    std::string title;
    PanelBuilder builder;
    std::string host;
    std::string area;
    WhenSpec when;
    int order = 0;
};

struct ToolReg {
    std::string plugin;
    std::string name;
    PanelBuilder builder;
    int order = 0;
};

struct FormatReg {
    std::string plugin;
    std::string name;
    FormatHandler handler;
};

struct DecryptorReg {
    std::string plugin;
    DecryptCheck check;
    DecryptFn decrypt;
};

struct CompareModeReg {
    engine::xml::XmlModeSpec spec;  // 名字/列/筛选/order/哨兵
    CompareFn compare;
    RowBuilderFn row_builder;
    DetailBuilderFn detail_builder;  // 可空 ⇒ 用通用摘要
};

// ── 注册表 ───────────────────────────────────────────────────────────

class Registry {
public:
    // 10 个注册点。失败 = false/`nullopt`，理由进 registration_errors()（**不静默**）。
    bool register_decryptor(const std::string& plugin, DecryptCheck check, DecryptFn decrypt);
    bool register_format(const std::string& plugin, const std::string& name, FormatHandler handler);
    std::optional<std::string> register_command(const std::string& plugin,
                                                const std::string& action_id,
                                                const std::string& label, CommandHandler handler,
                                                WhenSpec when = {});
    bool register_menu_item(const std::string& plugin, const std::string& host,
                            const std::string& location, const std::string& command_id,
                            int order = 0);
    bool register_option_entry(const std::string& plugin, const std::string& host,
                               const std::string& area, const std::string& value,
                               WhenSpec when = {});
    bool register_option(const std::string& plugin, const std::string& label,
                         std::vector<std::string> choices, CommandHandler callback,
                         const std::string& host, const std::string& area, WhenSpec when = {},
                         int order = 0);
    bool register_panel(const std::string& plugin, const std::string& title, PanelBuilder builder,
                        const std::string& host, const std::string& area, WhenSpec when = {},
                        int order = 0);
    bool register_tool(const std::string& plugin, const std::string& name, PanelBuilder builder,
                       int order = 0);
    bool register_compare_mode(const std::string& plugin, CompareModeReg mode);

    // ── 消费端（每条注册都必须**真的被看见**，否则就是"注册成功但无人消费"＝FAIL）──
    std::vector<const CommandReg*> menu_items_for(const std::string& host,
                                                  const std::string& location,
                                                  const Context& context = {}) const;
    std::vector<const OptionReg*> options_for(const std::string& host,
                                              const std::string& area = std::string(),
                                              const Context& context = {}) const;
    std::vector<std::string> option_entries_for(const std::string& host,
                                                const std::string& area = std::string(),
                                                const Context& context = {}) const;
    std::vector<const PanelReg*> panels_for(const std::string& host,
                                            const std::string& area = std::string(),
                                            const Context& context = {}) const;
    std::vector<const ToolReg*> tools_sorted() const;               // order 升序 + 注册顺序
    std::vector<const CompareModeReg*> compare_modes_for() const;   // order 升序 + 内置优先
    const CompareModeReg* compare_mode(const std::string& name) const;
    const DecryptorReg* find_decryptor(const std::string& path) const;  // check 抛异常 ⇒ 跳过

    const std::vector<FormatReg>& formats() const { return formats_; }
    const std::vector<RegistrationError>& registration_errors() const { return errors_; }
    // 12.9：**调插件回调**的统一入口（异常在这里被吃掉并记账，绝不冒到 UI 层）
    void invoke(const std::string& plugin, const std::string& what, const std::function<void()>& fn);

    // 12.7：`scan()` 清掉插件贡献，但**不动** compare_modes（内置与插件共用一张表）
    void scan_clear(const std::vector<RegistrationError>& carried_errors = {});

    size_t command_count() const { return commands_.size(); }
    size_t registration_count() const;
    bool has_command(const std::string& id) const { return commands_.count(id) != 0; }

private:
    void reject(const std::string& plugin, const std::string& message);
    bool bad_slot(const std::string& plugin, const std::string& what, const std::string& host,
                  const std::string& location = std::string());

    std::vector<DecryptorReg> decryptors_;
    std::vector<FormatReg> formats_;
    std::map<std::string, CommandReg> commands_;
    std::vector<MenuItemReg> menu_items_;
    std::vector<OptionEntryReg> option_entries_;
    std::vector<OptionReg> options_;
    std::vector<PanelReg> panels_;
    std::vector<ToolReg> tools_;
    std::vector<CompareModeReg> compare_modes_;
    std::vector<RegistrationError> errors_;
};

}  // namespace xrkit::plugin
