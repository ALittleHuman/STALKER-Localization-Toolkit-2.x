#pragma once
// xrkit::engine::plugin —— 把 `Registry` 包成**插件视角的 C ABI 表**（M2-7c）。
//
// 分工：**注册面**在这里实现（纯 C ABI，Qt-free）；`api.ui` 的 16 个方法由**外壳**注入
//（那是唯一需要 Qt 的地方，12.2 说的三处强耦合之一）。`ui == nullptr` 时那 16 个方法
// 返回空/0 —— 自检就靠这个在无 Qt 进程里驱动整套注册。
//
// 每个插件实例一份 `stk_api`：命令 id 的前缀是**插件名**（1.x 是 `<文件名>:<动作名>`），
// 注册失败一律返回 0/NULL 并让 `Registry` 留痕（⑫12.4）。

#include "xrkit/engine/plugin/registry.h"
#include "xrkit/plugin/abi.h"

#include <deque>
#include <string>
#include <vector>

namespace xrkit::plugin {

class PluginApiAdapter {
public:
    // `ui` 由外壳提供（Qt 实现）；无 Qt 时传 nullptr
    PluginApiAdapter(Registry& registry, std::string plugin_name, const stk_ui* ui = nullptr);

    const stk_api& api() const { return api_; }
    const std::string& plugin_name() const { return plugin_name_; }
    // 注册期返回过的字符串（命令 id 等）—— 保证指针在宿主生命周期内有效。
    //
    // ★★ 2026-10-06（F1 命令 id 悬空；判红锁＝CLI `--selftest` 的"命令 id 指针稳定性"）：
    //   这条承诺**曾经是假的** —— 容器是 `std::vector<std::string>`，注册第 2 个命令就会
    //   扩容搬走旧元素，而命令 id 形如 `"<插件名>:<动作名>"` 多半落在 MSVC `std::string`
    //   的 SSO 阈值（15 字节）内 ⇒ 字符数据内联在对象里 ⇒ 早先 `keep()` 返回的 `c_str()`
    //   指向**已被释放的旧数组**（plugin 若"先注册全部命令、再统一挂 menu item"就踩上）。
    //   换成 `std::deque` 的理由（三条里选它）：
    //     ① 标准保证 deque 在**两端插入**时"不使既有元素的指针/引用失效"（vector 没有这条）；
    //     ② 没有容量前提 —— `reserve(N)` 只是把问题推到第 N+1 个命令，而插件能注册多少
    //        个命令**宿主不知道**；
    //     ③ 每串一次堆分配（`unique_ptr<std::string>` 那条路要两次），也不改调用方的用法。
    const std::deque<std::string>& owned_strings() const { return owned_; }

private:
    static PluginApiAdapter* self(stk_host* host);
    const char* keep(std::string text);

    // 10 个注册点的 trampoline（签名必须与 `stk_api` 里的函数指针一致）
    static int on_decryptor(stk_host*, const char* name, stk_decrypt_check_fn, stk_decrypt_fn,
                            void* user);
    static int on_format(stk_host*, const char* name, stk_format_fn, void* user);
    static const char* on_command(stk_host*, const char* action_id, const char* label,
                                 stk_command_fn, void* user, const stk_when* when);
    static int on_menu_item(stk_host*, const char* host_name, const char* location,
                            const char* command_id, int order);
    static int on_option_entry(stk_host*, const char* host_name, const char* area,
                               const char* value, const stk_when* when);
    static int on_option(stk_host*, const char* label, const char* const* choices, int count,
                         stk_command_fn callback, void* user, const char* host_name,
                         const char* area, const stk_when* when, int order);
    static int on_panel(stk_host*, const char* title, stk_builder_fn, void* user,
                        const char* host_name, const char* area, const stk_when* when, int order);
    static int on_tool(stk_host*, const char* name, stk_builder_fn, void* user, int order);
    static int on_compare_mode(stk_host*, const char* name, const char* key, int order,
                               const char* const* column_keys, const char* const* column_headings,
                               const int* column_widths, int column_count,
                               const char* const* filter_labels, int filter_count,
                               const char* const* filter_kinds, stk_compare_fn compare,
                               void* user);
    static void on_add_error(stk_host*, const char* message);

    Registry& registry_;
    std::string plugin_name_;
    const stk_ui* ui_ = nullptr;
    // ★ 容器**必须是"两端插入不搬家"的那种**（见 `owned_strings()` 上方）：
    //   `keep()` 把元素的 `c_str()` 交出去当**跨 ABI 的长期指针**，扩容搬家就等于悬空。
    std::deque<std::string> owned_;
    stk_api api_{};
};

// 把 `stk_when` 转成引擎的 `WhenSpec`（12.6 的三形态；`fn != NULL` 走 lambda 包装）
WhenSpec when_from_abi(const stk_when* when);

}  // namespace xrkit::plugin
