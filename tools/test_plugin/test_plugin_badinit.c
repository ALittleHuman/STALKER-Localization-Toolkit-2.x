/* xrkit 测试插件（**坏 init 夹具**）—— 纯 C，与 `test_plugin.c` **同一份写法**，
 * 只有一处不同（见末尾那段 `register_compare_mode`）：最后一句注册**必然失败**。
 *
 * 为什么要单独一枚 DLL（而不是改 `test_plugin.c`）：`test_plugin.c` 是"加载**成功**"
 * 那条判据的夹具（自检 ⑫12.4 那段会逐条断言它的 6 条注册真的进了表），改它就毁了那条判据。
 *
 * 它专门提供 S1-2 的**触发形态**：`init` 期间**先成功注册了 6 项**（命令 + 挂载点 + 下拉条目
 * + 格式 + 解密器 + 面板），走到最后一条 `register_compare_mode` 时失败 ⇒ 按官方夹具的写法
 * `return 0` ⇒ 宿主走到"入口返回 0"那条路。修前宿主只 `FreeLibrary`、**不回滚**这 6 条注册
 * ⇒ 表里留着指向**已 unmap 代码段**的裸函数指针（点一下就 AV）。
 *
 * ★ 判据要求 `init` 真的返回 0（而不是"注册被拒但宿主不管"）：宿主如实报的失败理由里
 *   必须写明"入口返回 0"；门禁再按 **owner 名**逐容器断言表里零残留。
 */
#include "xrkit/plugin/abi.h"

#include <string.h>

static const stk_api* g_api = 0;
static int g_clicks = 0;

static void on_hello(void* user) {
    (void)user;
    g_clicks += 1;
}

static void on_click_emit(stk_result_sink sink, void* sink_user, const char* key,
                          const char* value) {
    sink(sink_user, key, value);
}

/* ★ 本夹具**刻意没有**比较回调：最后一条注册就是"缺 compare"。
 *   （不是漏写 —— 写了它就是"未引用的静态函数"，MSVC /W4 会报 C4505；
 *     而这个夹具的要害恰恰是"这条注册一定失败"。） */

static int demo_format(void* user, const char* path) {
    (void)user;
    return strstr(path, ".demo") != 0 ? 1 : 0;
}

static int demo_decrypt_check(void* user, const char* path) {
    (void)user;
    return (path != 0 && strstr(path, ".stkenc") != 0) ? 1 : 0;
}

static int demo_decrypt(void* user, const char* path, void** out_data, size_t* out_size) {
    (void)user;
    (void)path;
    (void)out_data;
    (void)out_size;
    return 0; /* 解不开（这条夹具要验的是"可见失败"） */
}

static stk_widget* demo_builder(void* user, stk_widget* parent) {
    (void)user;
    (void)parent;
    return 0;
}

STK_EXPORT int stk_plugin_init(const stk_api* api) {
    if (api == 0 || api->abi_version != STK_ABI_VERSION) return 0;
    g_api = api;

    static const char* when_keys[] = {"mode"};
    static const char* when_values[] = {"demo"};
    stk_when when;
    when.keys = when_keys;
    when.values = when_values;
    when.count = 1;
    when.fn = 0;
    when.user = 0;

    /* ── 下面 6 条在修前是**真的注册进表**了（这就是残留的来源）────────────────── */
    const char* cid = api->register_command(api->host, "hello", "打个招呼", on_hello, 0, &when);
    if (cid == 0) return 0;
    if (api->register_menu_item(api->host, "xml", "toolbar", cid, 3) == 0) return 0;
    if (api->register_option_entry(api->host, "xml", "filter", "demo 筛选项", 0) == 0) return 0;
    if (api->register_format(api->host, "demo", demo_format, 0) == 0) return 0;
    if (api->register_decryptor(api->host, "demo 加密包", demo_decrypt_check, demo_decrypt, 0) ==
        0) {
        return 0;
    }
    if (api->register_panel(api->host, "demo 面板", demo_builder, 0, "xml", "top_bar", 0, 9) == 0) {
        return 0;
    }

    /* ── ★ 与官方夹具的唯一差别：compare 传 0（"缺 compare" ⇒ 宿主拒绝 ⇒ 返回 0）────
     *   官方夹具在这里传 `demo_compare`（注册成功、init 返回 1）。
     *   于是本夹具走到这一句时：**前 6 条已经在表里**，而 `init` 返回 0。 */
    static const char* col_keys[] = {"rel", "status", "detail"};
    static const char* col_headings[] = {"文件", "状态", "差异"};
    static const int col_widths[] = {280, 60, 400};
    static const char* filter_labels[] = {"全部"};
    static const char* filter_kinds[] = {"same|diff|only_a|only_b"};
    if (api->register_compare_mode(api->host, "demo 模式", "demo_mode", 40, col_keys, col_headings,
                                   col_widths, 3, filter_labels, 1, filter_kinds, 0 /* 缺 compare */,
                                   0) == 0) {
        return 0;
    }
    return 1;
}

/* 与官方夹具同名的核对符号（门禁不读它，保留是为了"同一份写法"一目了然） */
int stk_test_plugin_clicks(void) { return g_clicks; }
