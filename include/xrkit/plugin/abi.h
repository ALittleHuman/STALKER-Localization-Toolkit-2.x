#pragma once
// xrkit 插件 ABI —— **纯 C**（对照表 ⑫12.1/12.2/12.3）。
//
// 设计口径（用户 2026-09-24 拍板，原话："B2需要全部支持。这样也利好后续维护。"）：
//   · 1.x 的**全部注册点都要在 2.0 支持**（解密器/格式/命令/菜单项/下拉条目/新建下拉/
//     面板/Hub 页/比较模式）；
//   · 能力面＝宿主渲染的"**纯数据 + 回调**"：插件**永不接触 Qt 类型** ——
//     控件一律是不透明的 `stk_widget*` 句柄（12.2：只有 `api.widgets`、`register_panel`
//     的 builder、`register_tool` 的 builder 三处与工具包强耦合，其余全是数据）；
//   · `stk_ui` 的 **16 个方法**（9 造控件 + 7 改控件/取输入）**名字与参数名就是第三方契约**
//     —— 由 `tools/gen_plugin_contract.py` 从 1.x `toolkit_plugin_ui.py` 按 AST 读出并锁住
//     （见 `engine/plugin/contract.h`：`ui_method_count()` == 16）。
//
// 为什么用 C ABI：插件可以是任意语言/编译器产物（C/C++/Rust/…），只要它填得出这张函数表；
// 宿主这边（Qt）负责把 `stk_widget*` 变成真控件。**失败的注册一律返回 0 并让宿主留痕**
//（⑫12.4：非法寻址写 `registration_errors`，绝不静默丢弃）。

#include <stddef.h>
#include <stdint.h>

// ★★ 决策 1 第一刀：控件门面契约搬到了独立头。
//   ★ 必须在 `#ifdef __cplusplus` **之外** include —— 这个头会被 `tools/test_plugin/*.c`
//     以 **C** 方式包含，放在 C++ 条件里会让 C 编译看不到 `stk_widget`（本刀第二个坑）。
#include "xrkit/engine/plugin/facade_api.h"

#ifdef __cplusplus
// ★★ 决策 1 第一刀：控件门面契约搬到了独立头（**必须在 `extern "C"` 之前 include** ——
//   在 extern "C" 里 include 任何 C++ 标准库头都会炸 `STL1003`，这是本刀第一个坑）。
//   ★ 文件名**不能叫 `ui_*`**：Qt 的 autouic 会把 `ui_xxx.h` 当成 uic 生成物，
//     直接报 `AutoUic error: includes the uic file …`（本刀第三个坑）—— 所以叫 facade_api.h。

extern "C" {
#endif

#define STK_ABI_VERSION 1

// 导出宏：插件入口**必须**导出，否则宿主 GetProcAddress 拿不到
// （实测踩过：忘了它，症状是"DLL 里没有入口 stk_plugin_init"）
#if defined(_WIN32)
#define STK_EXPORT __declspec(dllexport)
#else
#define STK_EXPORT __attribute__((visibility("default")))
#endif

// ★★ 决策 1（C ABI 退净）第一刀：控件门面契约（`stk_ui` 16 方法 ＋ 不透明句柄）
//   已搬到 `xrkit/engine/plugin/facade_api.h` —— 它**不是** C 插件专属（外壳控件门面与
//   Python 绑定共用同一张表），所以不能跟着本头一起删。
//   这里只保留 C 插件 ABI（`stk_api` ＋ 入口）本身。

// ── 注册面：10 个注册点（⑫12.1）────────────────────────────────────
// 全部返回 int：1=成功，0=被拒（宿主会把理由记进 registration_errors 并写日志）。
// `when_*` 用**纯数据**表达 12.6 的三形态：
//   · when_key_count == 0 且 when_fn == NULL ⇒ 恒真（对应 None / 空 dict）；
//   · when_key_count > 0 ⇒ 要求宿主 context 里这些键**相等**；`when_values[i] == NULL`
//     表示期望值是 None（照 1.x `ctx.get(k) == v`：缺键也算命中）；
//   · when_fn != NULL ⇒ 调它，返回非 0 为满足；**抛异常/出错一律按不满足处理**。
typedef int (*stk_when_fn)(void* user, const char* const* keys, const char* const* values,
                           int count);

typedef struct stk_when {
    const char* const* keys;
    const char* const* values;  // 与 keys 等长；某位为 NULL 表示期望 None
    int count;
    stk_when_fn fn;
    void* user;
} stk_when;

typedef int (*stk_decrypt_check_fn)(void* user, const char* path);
typedef int (*stk_decrypt_fn)(void* user, const char* path, void** out_data, size_t* out_size);
typedef int (*stk_format_fn)(void* user, const char* path);
typedef void (*stk_command_fn)(void* user);
typedef stk_widget* (*stk_builder_fn)(void* user, stk_widget* parent);

// 比较模式的回调（12.1 的第 10 个注册点）：结果用**宿主提供的 sink** 回传 ——
// 插件不持有任何内存，宿主**当场把每个字符串拷走**，跨 ABI 没有所有权问题。
//   sink(sink_user, "consistent", "1"|"0")
//   sink(..., "only_a", id) / ("only_b", id)          —— 可重复
//   sink(..., "text_diff", "id\tA 文本\tB 文本")      —— 可重复
//   sink(..., "count_diff", "id\tA 次数\tB 次数")     —— 可重复
//   sink(..., "detail", 完整明细) / ("row_kind", "same|diff|only_a|only_b")
typedef void (*stk_result_sink)(void* sink_user, const char* key, const char* value);
typedef void (*stk_compare_fn)(void* user, const char* rel, const char* text_a,
                               const char* text_b, const char* enc_a, const char* enc_b,
                               stk_result_sink sink, void* sink_user);

typedef struct stk_api {
    int abi_version;
    stk_host* host;
    const stk_ui* ui;   // 16 方法（12.3）

    /* ① 解密器 */ int (*register_decryptor)(stk_host*, const char* name,
                                             stk_decrypt_check_fn, stk_decrypt_fn, void* user);
    /* ② 格式   */ int (*register_format)(stk_host*, const char* name, stk_format_fn,
                                           void* user);
    /* ③ 命令   */ const char* (*register_command)(stk_host*, const char* action_id,
                                                   const char* label, stk_command_fn,
                                                   void* user, const stk_when* when);
    /* ④ 挂载点 */ int (*register_menu_item)(stk_host*, const char* host_name,
                                             const char* location, const char* command_id,
                                             int order);
    /*   └ `location` 传 **NULL ⇒ 落到 1.x 的默认值 `"context"`**（右键菜单槽；
     *      `toolkit_plugins.py:361` 的 `location="context"`）。 */
    /* ⑤ 下拉条目 */ int (*register_option_entry)(stk_host*, const char* host_name,
                                                  const char* area, const char* value,
                                                  const stk_when* when);
    /* ⑥ 新建下拉 */ int (*register_option)(stk_host*, const char* label,
                                             const char* const* choices, int choice_count,
                                             stk_command_fn callback, void* user,
                                             const char* host_name, const char* area,
                                             const stk_when* when, int order);
    /* ⑦ 面板   */ int (*register_panel)(stk_host*, const char* title, stk_builder_fn,
                                          void* user, const char* host_name, const char* area,
                                          const stk_when* when, int order);
    /* ⑧ Hub 页 */ int (*register_tool)(stk_host*, const char* name, stk_builder_fn, void* user,
                                        int order);
    /* ⑨ 比较模式 */ int (*register_compare_mode)(stk_host*, const char* name, const char* key,
                                                  int order, const char* const* column_keys,
                                                  const char* const* column_headings,
                                                  const int* column_widths, int column_count,
                                                  const char* const* filter_labels,
                                                  int filter_count, const char* const* filter_kinds,
                                                  stk_compare_fn compare, void* user);
    /* ⑩ 留痕   */ void (*add_error)(stk_host*, const char* message);
} stk_api;

// ── 插件入口 ─────────────────────────────────────────────────────────
// 插件 DLL 必须导出这个符号（名字固定）：
//     extern "C" __declspec(dllexport) int stk_plugin_init(const stk_api* api);
// 返回 0 表示初始化失败（宿主只记录，**不阻塞启动**）。
#define STK_PLUGIN_ENTRY "stk_plugin_init"
typedef int (*stk_plugin_init_fn)(const stk_api* api);

#ifdef __cplusplus
}  // extern "C"
#endif
