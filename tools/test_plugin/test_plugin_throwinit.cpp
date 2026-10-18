/* xrkit 测试插件（**init 抛异常夹具**）—— 这一枚必须用 **C++** 编译（`throw` 不是 C）。
 *
 * 用途（S-03，第三方只读审计 2026-10-18）：
 *   插件的 `init` 里抛 C++ 异常时，宿主必须**拦下并报错**，不许让异常穿过 `LoadLibrary`
 *   边界冒到 `Hub` 构造路径（Qt 不接异常 ⇒ 进程终止）。
 *   注册期回调有 `Registry::invoke` 兜底，**入口本身没有** —— 本夹具就是那条路的证据。
 *
 * 与 `test_plugin_badinit.c` 的区别（**两条不同的宿主分支**）：
 *   · 那个是 "init **返回 0**" ⇒ 宿主走返回值分支；
 *   · 这个是 "init **直接抛**" ⇒ 宿主走异常分支（`catch`），善后要做同一套：
 *     按 owner 回滚这次 init 期间的全部注册 → `FreeLibrary` → 把理由写进 `LoadResult`。
 *   ⇒ 所以判据也要分开：本夹具要能证明"异常前注册的那条被回滚掉了"。
 *
 * ★ 命名刻意不匹配打包脚本的 `xrkit_*_plugin.dll` glob（`_throwinit` 结尾）——
 *   一枚"注定 init 抛异常"的插件不该被塞进发行/开发包的 `plugins\` 里。
 */
#include "xrkit/plugin/abi.h"

#include <stdexcept>

namespace {

void on_boom(void* /*user*/) {}

}  // namespace

extern "C" STK_EXPORT int stk_plugin_init(const stk_api* api) {
    if (api == nullptr || api->abi_version != STK_ABI_VERSION) return 0;

    // ① **先真的注册一条**：这条进了表之后才抛 ⇒ 宿主若不回滚，表里就留下一个指向
    //    本 DLL 代码段的裸函数指针（`FreeLibrary` 之后就是已 unmap 的地址）。
    static const char* when_keys[] = {"mode"};
    static const char* when_values[] = {"boom"};
    stk_when when;
    when.keys = when_keys;
    when.values = when_values;
    when.count = 1;
    when.fn = nullptr;
    when.user = nullptr;
    const char* cid =
        api->register_command(api->host, "boom", "抛异常夹具", on_boom, nullptr, &when);
    if (cid == nullptr) return 0;   // 注册本身就失败 ⇒ 走"返回 0"那条（不是本夹具的要点）

    // ② 然后抛 —— 宿主必须在这里拦下
    throw std::runtime_error("夹具：init 故意抛 C++ 异常（S-03）");
}
