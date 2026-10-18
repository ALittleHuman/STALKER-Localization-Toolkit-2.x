#pragma once
// xrkit::engine::plugin —— 插件 DLL 加载（M2-7c 后半）。
//
// 纪律（用户拍板 + ⑫12.4）：
//   · **加载失败只记录，绝不阻塞启动**（1.x 也是这个口径）；
//   · 每个 DLL 一份 `stk_api`（命令 id 前缀 = 文件名），注册期的非法寻址由 `Registry` 留痕；
//   · **不卸载正在被引用的模块**：注册表里存着插件回调的函数指针，所以句柄要活到
//     `unload_all()`（进程退出/重扫前才放）—— 提前 `FreeLibrary` 会让回调变成野指针。

#include "xrkit/engine/plugin/registry.h"
// ★ S1-2：`load_plugin` 的第二参是 `PluginApiAdapter`（注册目标与回滚目标是同一份东西）
//   ⇒ 这个头文件需要适配器的**完整声明**（`abi_host.h` 本身就带 `registry.h` + `abi.h`）。
#include "xrkit/engine/plugin/abi_host.h"
#include "xrkit/plugin/abi.h"

#include <string>
#include <vector>

namespace xrkit::plugin {

struct LoadResult {
    bool ok = false;         // 库加载 + 入口调用都成功
    std::string error;
    std::string plugin_name;  // 文件名（注册前缀）
    int init_rc = 0;          // 插件入口的返回值（0 = 它自己说失败）
    // 这次加载带来的注册条数；**失败时 = 本次已被撤回的注册条数**（零静默：回滚了几条也要看得见）
    // ★★ 2026-10-06（审计第 4 组：「`LoadResult::registrations` **死字段**」）：修前只有失败
    //   分支写它，成功加载时**恒为 0**（成功路径从不填）—— 字段名义上是"这次加载带来的注册
    //   条数"，实际只在失败那一路有值，消费端（CLI 探针）读到的一直是 0。现在两条路都按
    //   **同一个来源**填：`Registry::residue_for(owner).registrations()`（按 owner 逐容器报账）。
    size_t registrations = 0;
};

// 加载一个 DLL 并调 `stk_plugin_init`。**任何失败都只填 error 返回**（不抛、不崩）。
//
// ★★ S1-2（2026-10-06 审计）：`init` 返回 0 ⇒ **先按 owner 回滚它这次 init 期间的全部注册，
//   然后才 `FreeLibrary`**。修前只做 `FreeLibrary` ⇒ 表里留着指向**已 unmap 代码段**的裸函数
//   指针（命令 handler / 菜单项 / 面板 builder / 解密器 / 比较模式 …）⇒ 之后用户点到它注册的
//   按钮就是 AV（实测 `0xC0000005`；`Registry::invoke` 只 catch C++ 异常，接不住 AV）。
//   顺序不能反：**先撤表、再放库**（库一放，那些指针就已经悬空了）。
// ★ 签名收成"调用方必须交出**适配器**"（而不是裸 `stk_api`）：注册目标（registry + owner 名）
//   就在适配器自己身上 ⇒ 调用方**不可能**把"回调指针的落点"与"回滚的目标"传成两份不同的东西。
//   （也不给"默认不回滚"的参数 —— 那正是修前的形状。）
LoadResult load_plugin(const std::string& dll_path, const PluginApiAdapter& adapter);

// 扫目录里的 `*.dll`，逐个加载（顺序：文件名升序 —— 与 1.x 的加载顺序一致，可复现）。
// `ui` 由外壳提供（Qt 实现的 16 方法）；无 Qt 时传 nullptr。
// 返回本次加载产生的错误（也会进 `registry.registration_errors()`）。
std::vector<RegistrationError> load_plugins_from_dir(const std::string& dir, Registry& registry,
                                                     const stk_ui* ui = nullptr);

// ★★ P2b（插件改装，用户 2026-10-07 拍板「Python 模块接口 + C ABI 退役」）：
//   **两个前端（Hub / CLI）唯一的那一个加载入口** —— 先扫 `*.dll`（C ABI，退役中），
//   再扫 `*.py`（新契约，见 py_host.h）。收在一个函数里是为了让"两边加载的是同一批东西、
//   进的是同一份 Registry"变成结构性事实，而不是两条各写一遍的路径。
//   返回**全部**错误（两条路合并，顺序：DLL 在前、Python 在后）。
std::vector<RegistrationError> load_all_plugins_from_dir(const std::string& dir, Registry& registry,
                                                         const stk_ui* ui = nullptr);

// ★★ S2-2（B 批，审计 §2）：**插件文件的命名判据只许这一份**。
//   `*.dll` 的后缀判定必须**大小写不敏感**（Windows 文件名本来就不分大小写：`NLC.DLL` 是同一个
//   文件）。修前 `load_plugins_from_dir` 判的是**原名**的 `.dll`，而给它做小写化的
//   `std::transform` 结果此后再未使用（死代码）⇒ `NLC.DLL` 这类**静默漏载**：不报错、不进
//   `errors`、也不计数，用户只看到"插件好像没生效"。CLI 的 `--plugin-init-rollback` 探针
//   犯的是同一个错（两处各抄一份判定＝两份语义）。现在两处都调这一个函数。
bool is_plugin_dll_name(const std::string& filename);

// 释放所有已加载的插件库。
//
// ★★ S-04（第三方只读审计 2026-10-18）：**现在会先清表再卸库** —— 对每个已加载插件，
//   按它的 owner（`load_plugin` 时记下的 `adapter.plugin_name()`）**逐容器回滚**注册，
//   然后才 `FreeLibrary`。顺序不能反：表里存的是**插件代码里的函数指针**，先卸库就成了悬垂。
//   · `registry`：显式给的优先；**不给**就用活动注册表（`active_registry()`，外壳与 CLI 都走它）。
//   · 两者都没有时**只能** FreeLibrary —— 那种调用方手里没有表，也就谈不上清表；
//     这一档仍要遵守旧纪律（**确定没人再调那些回调**时才调）—— 误用会**回调悬垂**。
void unload_all(Registry* registry = nullptr);
size_t loaded_count();

}  // namespace xrkit::plugin
