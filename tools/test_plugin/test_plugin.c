/* xrkit 测试插件 —— **纯 C**（故意用 .c 编译：证明这套 ABI 真的是 C ABI）。
 *
 * 它把 10 个注册点里的几个真的用起来，供门禁/自检判断"加载 + 注册"整条链路：
 *   · 命令 + 挂载点（带数据化 when）
 *   · 下拉条目
 *   · 一个比较模式（结果用宿主 sink 回传）
 * 入口符号固定为 stk_plugin_init（见 xrkit/plugin/abi.h）。
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

/* 比较模式：把两侧文本里出现的 id 直接当差异报出去（只为了能被验证是它跑的） */
static void demo_compare(void* user, const char* rel, const char* text_a, const char* text_b,
                         const char* enc_a, const char* enc_b, stk_result_sink sink,
                         void* sink_user) {
    (void)user;
    (void)enc_a;
    (void)enc_b;
    if (strstr(text_a, "SAME") != 0 && strstr(text_b, "SAME") != 0) {
        on_click_emit(sink, sink_user, "consistent", "1");
        return;
    }
    on_click_emit(sink, sink_user, "consistent", "0");
    on_click_emit(sink, sink_user, "only_a", rel);
}

static int demo_format(void* user, const char* path) {
    (void)user;
    return strstr(path, ".demo") != 0 ? 1 : 0;
}

/* ★ 2026-10-04（用户口径「GUI 的功能，CLI 肯定要全部拥有啊」）：注册一条"**这个包需要解密**"
 * 的判定 —— 供 CLI 黑盒判据用（引擎读入口的闸门必须在 CLI 上也生效）。
 *
 * 判据刻意挑**文件名里的 `.stkenc` 标记**（确定性：不看内容、不碰盘、不依赖样本），
 * 解密**故意永远失败** —— 它锁的是"命中却解不开 ⇒ **可见失败**、绝不回落成普通包读"
 * 那条分支：
 *   · `xrkit_cli --plugins <本 DLL 所在目录> --dump x.stkenc.db`
 *     ⇒ 必须**拒绝 + 与 GUI 同一条理由 + 非零退出**；
 *   · 同名文件不加 `--plugins` ⇒ 照旧放行（没有注册表就没有闸门，与修前一致）。
 */
static int demo_decrypt_check(void* user, const char* path) {
    (void)user;
    return (path != 0 && strstr(path, ".stkenc") != 0) ? 1 : 0;
}

static int demo_decrypt(void* user, const char* path, void** out_data, size_t* out_size) {
    (void)user;
    (void)path;
    (void)out_data;
    (void)out_size;
    return 0; /* 解不开（这条夹具就是要验"可见失败"） */
}

static stk_widget* demo_builder(void* user, stk_widget* parent) {
    (void)user;
    (void)parent;
    return 0; /* 无 Qt 时宿主也没法真造控件；返回 0 表示"没造出来" */
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

    static const char* col_keys[] = {"rel", "status", "detail"};
    static const char* col_headings[] = {"文件", "状态", "差异"};
    static const int col_widths[] = {280, 60, 400};
    static const char* filter_labels[] = {"全部"};
    static const char* filter_kinds[] = {"same|diff|only_a|only_b"};
    if (api->register_compare_mode(api->host, "demo 模式", "demo_mode", 40, col_keys, col_headings,
                                   col_widths, 3, filter_labels, 1, filter_kinds, demo_compare,
                                   0) == 0) {
        return 0;
    }
    return 1;
}

/* 让门禁能核对"插件真的被调过"（自检读这个计数） */
int stk_test_plugin_clicks(void) { return g_clicks; }
