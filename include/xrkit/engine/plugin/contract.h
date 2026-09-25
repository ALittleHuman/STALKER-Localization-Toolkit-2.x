#pragma once
// xrkit::engine::plugin —— 插件契约的**只读视图**（对照表 ⑫12.1/12.3/12.4）。
//
// 槽位表与 `api.ui` 的 16 方法**不是手抄的**：由 `tools/gen_plugin_contract.py`
// 从 1.x 源码（`toolkit_plugins.py` / `toolkit_plugin_ui.py`）**按 AST** 读出，
// 生成 `contract_tables.inc`，并由门禁 `--check` 逐字节锁住。
// ⑫12.3 的原话：契约清单**从源码 AST 读，读不到就报错，不许静默降级** ——
// 所以这里除了访问器，还有一个 `contract_ok()` 把"表是不是齐全/条数对不对"如实报出来。
//
// 本层**不许 include Qt**（J5 锁）：插件永不接触 Qt 类型，宿主负责渲染；
// 定位在 `engine/plugin/` 就是为了让 J5 锁把它一起管住。

#include <string>

namespace xrkit::plugin {

struct SlotSpec {
    const char* key;
    const char* label;
};

struct UiMethodSpec {
    const char* name;
    const char* params;  // 参数名（逗号分隔，无类型）—— 名字与参数名都是第三方契约
};

// ── 槽位表（1.x HOSTS / LOCATIONS / AREA_*）──────────────────────────
int host_count();
const SlotSpec& host(int index);
bool known_host(const std::string& key);
const char* host_label(const std::string& key);  // 不认识返回 nullptr

int location_count();
const SlotSpec& location(int index);
bool known_location(const std::string& key);

int area_count();
const SlotSpec& area(int index);
bool known_area(const std::string& key);

// ── api.ui 契约（16 方法：9 造控件 + 7 改控件/取输入）──────────────
int ui_method_count();
const UiMethodSpec& ui_method(int index);
const UiMethodSpec* find_ui_method(const std::string& name);  // 找不到返回 nullptr

// 契约自检：表非空、条数符合预期（hosts=6 / locations=2 / ui_methods=16）。
// 失败时把原因写进 `why` —— **不静默降级**。
bool contract_ok(std::string& why);

}  // namespace xrkit::plugin
