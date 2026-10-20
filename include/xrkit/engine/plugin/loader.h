#pragma once
// xrkit::engine::plugin —— 插件加载入口（**只有 Python**）。
//
// ★★ 决策 1（用户 2026-10-18/19「C ABI 退干净」）：C 插件这条路**整体删除** ——
//   没有 `stk_api`、没有 `stk_plugin_init`、没有 `LoadLibrary` 插件路径、没有 DLL 句柄表。
//   保留的是：**两个前端（Hub / CLI）唯一的那一个加载入口**（C-01/C-03 的静态锁还在它身上），
//   以及 Python 插件（`py_host.h`，新契约）。
//
//   历史（只作决策记录，不是可执行说明）：修前这里还有 `load_plugin(dll, adapter)`、
//   `load_plugins_from_dir`、`is_plugin_dll_name`、`unload_all`、`loaded_count`，
//   以及"先撤表再 FreeLibrary"的顺序纪律（S1-2/S-04）。C ABI 退净后这些一并消失 ——
//   Python 插件没有"卸载"这回事（解释器活到进程退出）。

#include "xrkit/engine/plugin/registry.h"

#include <string>
#include <vector>

namespace xrkit::plugin {

// 一次插件加载的结果（Python 插件也用它）。
struct LoadResult {
    bool ok = false;
    std::string error;
    std::string plugin_name;  // 文件名（注册前缀）
    // 这次加载带来的注册条数；**失败时 = 本次已被撤回的注册条数**（零静默：回滚了几条也要看得见）。
    size_t registrations = 0;
};

// ★ **两个前端唯一的那一个加载入口**：扫目录里的 `*.py` 逐个加载。
//   `dir` 不存在 / 空目录 ⇒ 返回空错误表（"没有插件"不是错误）。
//   返回**全部**错误（同时也会进 `registry.registration_errors()`）。
//   ★ 收在一个函数里是为了让"两边加载的是同一批东西、进的是同一份 Registry"变成结构性事实。
std::vector<RegistrationError> load_all_plugins_from_dir(const std::string& dir, Registry& registry);

}  // namespace xrkit::plugin
