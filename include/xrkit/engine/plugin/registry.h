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
    // ★★ 2026-10-06（审计处理批·追加②）：非空 ⇒ 这条 when 的**参数非法**（例如 ABI 的
    //   `count` 是负数/超大值 —— 除 `count` 外 ABI 没有第二个长度字段，宿主只能靠域校验兜住）。
    //   口径：**不允许**把它当成"恒假/恒真"混过去 —— 注册点必须**拒绝注册并把理由留痕**
    //   （零静默），否则插件拿到的是"注册成功了但永不生效"。
    std::string invalid_reason;

    bool always() const { return match.empty() && !predicate && invalid_reason.empty(); }
    bool invalid() const { return !invalid_reason.empty(); }
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
    // ★★ P7c（样板页迁移）：规格 §五 的**显式覆盖** —— 新 id + `replaces` 指向被替换的页 id
    //   （禁静默顶掉；被拒时留痕、内置页照旧在）。空 = 不覆盖任何页。
    //   同时给页一个副标题（内置页有；插件页此前没有 —— 迁移时要能表达同样的东西）。
    std::string replaces;
    std::string subtitle;
    // ★ P7c：标签上的**显示名**（空 ⇒ 用 id）。迁移后的页 id 与原名不同
    //   （覆盖要求新 id），但标题仍要显示原名。
    std::string title;
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

// 内置的两套比较模式也**走注册表**（⑫12.1：内置与插件共用一张表），
// 由 `xml::builtin_modes()` 的声明 + xml 模块的比较/明细函数装好。
std::vector<CompareModeReg> builtin_compare_modes();

// ── ★★ S1-2（2026-10-06 只读审计）：注册表的"**按 owner 记账**"与"按 owner 回滚" ──
//
// 缺陷（修前）：`init(&api)` 返回 0 时宿主只 `FreeLibrary`、**不回滚**它这次 init 期间
// 已经进表的注册 ⇒ 表里留着指向**已 unmap 代码段**的裸函数指针（命令 handler / 菜单项 /
// 面板 builder / 解密器 / 比较模式 …）⇒ 之后用户点到它注册的按钮就是 AV
// （`Registry::invoke` 只 catch C++ 异常，**接不住** AV）。
//
// 为什么必须**按 owner 逐容器**、不能按一个总数：`registration_count()` 是 9 个容器的
// **总和**（含 `compare_modes_`），"总量相等"完全可能是"撤错了别家的、留了自己的"
// （计数是**总量**，不是**归属**）⇒ 判据与回滚都得按 **owner 名**（插件 dll 文件名）做。
//
// ★ `errors_` **不在**回滚/零残留的范围里：那是"留痕/诊断"通道，只存字符串（不可能悬垂），
//   而零静默要求"init 返回 0""哪条注册被拒"这些理由**必须看得见**。
//   所以 `residue_for` 把它单独列出来（可见），`rollback_plugin` **不碰**它。
//   （本批对判据草案②的复核修正，见批报告。）
// ★ `compare_modes_` 里内置模式的 owner 是 `engine::xml::kBuiltinPlugin`（唯一哨兵）⇒
//   按 owner 过滤**天然**碰不到出厂模式 —— 这正是**不能**复用 `scan_clear()` 的原因
//   （它刻意不清 `compare_modes_`，那是重扫口径，不是回滚）。
struct PluginResidue {
    std::string plugin;  // owner（插件 dll 文件名）
    size_t decryptors = 0;
    size_t formats = 0;
    size_t commands = 0;
    size_t menu_items = 0;
    size_t option_entries = 0;
    size_t options = 0;
    size_t panels = 0;
    size_t tools = 0;
    size_t compare_modes = 0;
    size_t errors = 0;  // 留痕条数：**不算**注册残留，但必须逐项可见（零静默）

    // 9 个注册容器之和（**不含** errors_）
    size_t registrations() const {
        return decryptors + formats + commands + menu_items + option_entries + options + panels +
               tools + compare_modes;
    }
    bool clean() const { return registrations() == 0; }
    // 逐项 `k=v` 一行报出来（判据脚本/自检直接解析这一行，不去猜哪个容器脏了）
    std::string describe() const;
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
    // ★★ P7c：显式覆盖（规格 §五）：`replaces` 指向被替换的页 id（空 = 不覆盖）；
    //   失败（自替换 / 已被别人覆盖）⇒ 拒绝并留痕，内置页照旧在。
    bool register_tool(const std::string& plugin, const std::string& name, PanelBuilder builder,
                       int order, const std::string& replaces, const std::string& subtitle,
                       const std::string& title = "");
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
    // 渲染端要按 area 把条目塞进对应的下拉（`option_entries_for` 只回值，不含 area）
    const std::vector<OptionEntryReg>& option_entry_regs() const {
        return option_entries_;
    }
    const std::vector<RegistrationError>& registration_errors() const { return errors_; }
    // 宿主侧（加载器 / 插件自报）也往**同一条**留痕通道写 —— Hub 启动时统一显示
    void add_error(const std::string& plugin, const std::string& message) {
        reject(plugin, message);
    }
    // 12.9：**调插件回调**的统一入口（异常在这里被吃掉并记账，绝不冒到 UI 层）
    void invoke(const std::string& plugin, const std::string& what, const std::function<void()>& fn);

    // 12.7：`scan()` 清掉插件贡献，但**不动** compare_modes（内置与插件共用一张表）
    void scan_clear(const std::vector<RegistrationError>& carried_errors = {});

    // ★★ S1-2：**按 owner 逐容器**只读报账（判据的唯一一份实现：9 个容器 + errors_ 逐项计数）
    PluginResidue residue_for(const std::string& plugin) const;
    // ★★ S1-2：**按 owner 逐容器**撤掉它注册的一切（"全成功才生效"的 abort 面）。
    //   返回撤掉的**注册**条数（不含 errors_）。刻意不做"按总数回滚"（见 PluginResidue 上方）。
    size_t rollback_plugin(const std::string& plugin);

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

// ⑫12.1 的 **decryptor 消费端**（用户 2026-09-26 决策：NLC 解密是外部闭源插件，
// 走 `register_decryptor`/`find_decryptor`，工具内**不放任何解密逻辑、无模组名分支**）：
// 加载 DB 之前问一遍插件解密器 ——
//   · 没有解密器命中 ⇒ `ok=true, used_plugin=false, path=原路径`（行为与不做这件事完全一致）；
//   · 命中且解密成功 ⇒ 把明文**原子写**到 `out_dir` 下，返回那个路径（`used_plugin=true`）；
//   · 命中但解密失败 ⇒ `ok=false` + 明确理由（**不许静默回落到"当普通包读"**）。
struct DecryptGateResult {
    bool ok = true;
    bool used_plugin = false;
    std::string path;
    std::string error;
};

DecryptGateResult gate_db_path(const Registry& registry, const std::string& path,
                               const std::string& out_dir);

// ── 活动注册表钩子（★ 2026-10-04：把闸门从"只有外壳过"抬到**引擎读入口**）──────
// 用户口径（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。**」
// 修前的违例：闸门只在外壳（`FsPage::gate_source`）里，CLI 的动作路径**从不建注册表** ⇒
// 加密包被 CLI 当普通包读 —— 同一个引擎、两个前端两套行为（前端分叉）。
//
// 所以钩子必须是**引擎侧的全局约定**，两个前端各自注入自己那份 `Registry`：
//   · `set_active_registry(nullptr)` / 从未注入 ⇒ `gate_active_read` 立刻原样放行，
//     读入口的行为与"没有这段代码"**逐字节一致**（shell/CLI 都不注入时就是今天）；
//   · 两个前端注的是**同一份东西**（目录都是 `app_dir()/plugins`）⇒ 判据不会分叉。
//
// ⚠ 生命周期：存的是**指针**。注入方（Hub / FsPage / CLI）必须在注册表析构前复位
//   （`set_active_registry(nullptr)`），否则钩子会指向已死对象。
void set_active_registry(const Registry* registry);
const Registry* active_registry();

// RAII 版注入：作用域内钩子指向 `registry`，析构时收回（置 `nullptr`）。
// 给"注册表是**局部对象**、出口很多"的前端用（CLI 的 `run_cli` 就是那种形状）——
// 任何一条 return 路径上钩子都不会指向已死的注册表。
struct ActiveRegistryScope {
    explicit ActiveRegistryScope(const Registry* registry) { set_active_registry(registry); }
    ~ActiveRegistryScope() { set_active_registry(nullptr); }
    ActiveRegistryScope(const ActiveRegistryScope&) = delete;
    ActiveRegistryScope& operator=(const ActiveRegistryScope&) = delete;
};

// 解密产物的落盘目录（引擎侧唯一一份口径）：`<系统临时目录>/xrkit_decrypted`。
// 为什么放引擎：闸门现在在引擎里跑（CLI 也走它），产物落点不能再由外壳的 `QDir::temp()` 说了算；
// 外壳从同一处取值 ⇒ 两边认出"这是闸门自己的产物"，不会二次解密。目录不存在时**建出来**。
std::string decrypt_output_dir();

// 引擎读入口统一走的**活动闸门**：
//   · `registry` 显式传入者优先，为空 ⇒ 用 `active_registry()`；两者都空 ⇒ 原样放行；
//   · 路径已在 `decrypt_output_dir()` 里 ⇒ 原样放行（那是闸门自己的产物，二次解密会把
//     已经解开的包再解一遍 —— 外壳先过一遍 `gate_source` 再交给引擎，必然会碰到这一条）；
//   · 其余情况 = `gate_db_path(*registry, path, decrypt_output_dir())`（三种结局一字不改）。
DecryptGateResult gate_active_read(const std::string& path, const Registry* registry = nullptr);

}  // namespace xrkit::plugin
