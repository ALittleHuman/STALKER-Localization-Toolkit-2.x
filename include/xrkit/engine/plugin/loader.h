#pragma once
// xrkit::engine::plugin —— 插件 DLL 加载（M2-7c 后半）。
//
// 纪律（用户拍板 + ⑫12.4）：
//   · **加载失败只记录，绝不阻塞启动**（1.x 也是这个口径）；
//   · 每个 DLL 一份 `stk_api`（命令 id 前缀 = 文件名），注册期的非法寻址由 `Registry` 留痕；
//   · **不卸载正在被引用的模块**：注册表里存着插件回调的函数指针，所以句柄要活到
//     `unload_all()`（进程退出/重扫前才放）—— 提前 `FreeLibrary` 会让回调变成野指针。

#include "xrkit/engine/plugin/registry.h"
#include "xrkit/plugin/abi.h"

#include <string>
#include <vector>

namespace xrkit::plugin {

struct LoadResult {
    bool ok = false;         // 库加载 + 入口调用都成功
    std::string error;
    std::string plugin_name;  // 文件名（注册前缀）
    int init_rc = 0;          // 插件入口的返回值（0 = 它自己说失败）
    size_t registrations = 0;  // 这次加载带来的注册条数（便于自检/日志）
};

// 加载一个 DLL 并调 `stk_plugin_init`。**任何失败都只填 error 返回**（不抛、不崩）。
LoadResult load_plugin(const std::string& dll_path, const stk_api& api);

// 扫目录里的 `*.dll`，逐个加载（顺序：文件名升序 —— 与 1.x 的加载顺序一致，可复现）。
// `ui` 由外壳提供（Qt 实现的 16 方法）；无 Qt 时传 nullptr。
// 返回本次加载产生的错误（也会进 `registry.registration_errors()`）。
std::vector<RegistrationError> load_plugins_from_dir(const std::string& dir, Registry& registry,
                                                     const stk_ui* ui = nullptr);

// 释放所有已加载的插件库（只在确定没人再调它们的回调时用）
void unload_all();
size_t loaded_count();

}  // namespace xrkit::plugin
