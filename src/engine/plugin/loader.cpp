#include "xrkit/engine/plugin/loader.h"

// P2b：唯一入口把 `.py`（新契约）拉进来 —— 命名判据与加载实现都在 py_host。
#include "xrkit/engine/plugin/py_host.h"

#include <string>
#include <vector>

// ★★ 决策 1（用户 2026-10-18/19「C ABI 退干净」）：本文件原来还包含
//   `is_plugin_dll_name` / `load_plugin`（LoadLibrary ＋ `stk_plugin_init` ＋ S-03 异常护栏 ＋
//   S1-2 回滚）/ `unload_all`（S-04：先撤表再 FreeLibrary）/ `loaded_count`。
//   这些随 C 插件 ABI 一起**整段删除**；现在只剩"两个前端唯一的那一个入口"：
//   扫 `*.py` 并把错误合并返回（C-01/C-03 的静态锁仍然锁在这个函数上）。

namespace xrkit::plugin {

std::vector<RegistrationError> load_all_plugins_from_dir(const std::string& dir, Registry& registry) {
    // ★ 只有一条路：`*.py`（新契约）。错误全部进**同一份** `registry.registration_errors()`
    //   （零静默），返回的就是这份清单，调用方按同一顺序显示。
    return load_python_plugins_from_dir(dir, registry);
}

}  // namespace xrkit::plugin
