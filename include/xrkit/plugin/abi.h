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

#ifdef __cplusplus
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

// 不透明句柄：插件只认识指针，永远不知道背后是 Qt 还是别的
typedef struct stk_widget stk_widget;
typedef struct stk_host stk_host;

// ── api.ui：16 方法（9 造控件 + 7 改控件/取输入）─────────────────────
// 参数名与 1.x `TkUiFactory` 一致（第三方就是照那套写的）：
//   label(parent, text, style) / button(parent, text, on_click, width, style, state)
//   row(parent, side, fill, expand, padx, pady) / checkbox(parent, text, variable, on_toggle)
//   entry(parent, textvariable, width, show) / text(parent, height, width, wrap)
//   combo(parent, values, variable, width, on_change) / drop_box(parent, on_file, placeholder, width)
//   pack(widget, side, fill, expand, padx, pady, anchor)
//   set_text(widget, text) / set_state(widget, state) / text_view(parent, height, mono)
//   alive(widget) / ask_yes_no(title, message) / ask_open_file(title) / ask_save_file(title)
typedef void (*stk_click_fn)(void* user);
typedef void (*stk_change_fn)(void* user, const char* value);
typedef void (*stk_file_fn)(void* user, const char* path);

typedef struct stk_ui {
    /* 1 */ stk_widget* (*label)(stk_host* h, stk_widget* parent, const char* text,
                                 const char* style);
    /* 2 */ stk_widget* (*button)(stk_host* h, stk_widget* parent, const char* text,
                                  stk_click_fn on_click, void* user, const char* style,
                                  int state);
    /* 3 */ stk_widget* (*row)(stk_host* h, stk_widget* parent, const char* side,
                              const char* fill, int expand, int padx, int pady);
    /* 4 */ stk_widget* (*checkbox)(stk_host* h, stk_widget* parent, const char* text,
                                    stk_change_fn on_toggle, void* user);
    /* 5 */ stk_widget* (*entry)(stk_host* h, stk_widget* parent, const char* initial,
                                 int width, const char* show);
    /* 6 */ stk_widget* (*text)(stk_host* h, stk_widget* parent, int height, int width,
                                const char* wrap);
    /* 7 */ stk_widget* (*combo)(stk_host* h, stk_widget* parent, const char* const* values,
                                 int count, stk_change_fn on_change, void* user);
    /* 8 */ stk_widget* (*drop_box)(stk_host* h, stk_widget* parent, stk_file_fn on_file,
                                    void* user, const char* placeholder, int width);
    /* 9 */ void (*pack)(stk_host* h, stk_widget* parent, stk_widget* child, const char* side,
                         const char* fill, int expand, int padx, int pady);
    /* 10 */ void (*set_text)(stk_host* h, stk_widget* widget, const char* text);
    /* 11 */ void (*set_state)(stk_host* h, stk_widget* widget, int state);
    /* 12 */ stk_widget* (*text_view)(stk_host* h, stk_widget* parent, int height, int mono);
    /* 13 */ int (*alive)(stk_host* h, stk_widget* widget);
    /* 14 */ int (*ask_yes_no)(stk_host* h, const char* title, const char* message);
    /* 15 */ const char* (*ask_open_file)(stk_host* h, const char* title);
    /* 16 */ const char* (*ask_save_file)(stk_host* h, const char* title);
} stk_ui;

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
