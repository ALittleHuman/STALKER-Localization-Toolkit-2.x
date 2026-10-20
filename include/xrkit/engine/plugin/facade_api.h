#pragma once
// xrkit::engine::plugin —— **控件门面契约**（`stk_ui` 16 方法）。
// ★ 文件名不叫 `ui_facade.h`：Qt autouic 把 `ui_*.h` 当 uic 生成物（会直接 build fail）。
//
// ★★ 决策 1（用户 2026-10-18/19「C ABI 退干净」）第一刀：
//   这张表原来定义在 `include/xrkit/plugin/abi.h` 里 —— 那个头是**C 插件 ABI**（`stk_api` /
//   `stk_plugin_init` / `STK_PLUGIN_ENTRY`）的契约，随 C ABI 一起要删掉。
//   但 `stk_ui` **不是** C 插件专属：外壳的控件门面（`widget_facade_qt.cpp`）实现它、
//   Python 绑定（`py_host`）与门面共用同一张表 —— 也就是说它是**宿主内部的控件工厂契约**，
//   只是历史上和 C ABI 写在同一个文件里。
//   ⇒ 先把它搬到独立头文件（本文件），`abi.h` 之后整份删除时就不会把门面一起带走。
//
// 口径不变：参数名与 1.x `TkUiFactory` 一致（第三方就是照那套写的）；
// 16 方法＝9 造控件 ＋ 7 改控件/取输入；`parent`/返回的 `stk_widget*` 都是**不透明句柄**。

#ifdef __cplusplus
extern "C" {
#endif

// 不透明句柄：插件（与门面调用方）只认识指针，永远不知道背后是 Qt 还是别的
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

#ifdef __cplusplus
}  // extern "C"
#endif
