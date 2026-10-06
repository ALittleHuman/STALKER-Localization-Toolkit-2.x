#include "xrkit/engine/engine.h"

#include "probe_decls.h"

#include <algorithm>
#include <array>
#include <set>

namespace xrkit::engine {
namespace {

// ── 模块表：**名字 ↔ 探针**一一对应（见 `probe_decls.h` 与 `engine.h` 的口径）──
// 为什么两样都要：名字是给人看/给自检核对的（要与 `src/engine/` 的目录一一对应），
// 探针是"真的链上来了"的证据（摘掉模块 ⇒ 链接失败，不是静默少报）。
struct ModuleEntry {
    const char* name;
    const char* (*probe)();
};

constexpr std::array<ModuleEntry, 6> kModules{{
    {"fs", &probe_fs},
    {"font", &probe_font},
    {"text", &probe_text},
    {"xml", &probe_xml},
    {"video", &probe_video},
    {"plugin", &probe_plugin},
}};

// 被 `set_module_subset` 收窄后的"当前引擎用到的模块集"（默认 = 全部）。
// 用 `set<string>` 存名字：判据要的是"这一轮到底用了几块"，不是"清单里写了几块"。
std::set<std::string>& active_subset() {
    static std::set<std::string> subset;
    return subset;
}

// ★ 为什么不能只用 `subset.empty()` 判"收窄过没有"：`--module-subset` 传**空清单**
//   是一个合法的探针场景（"一块都不生效"），而"没收窄"与"收窄成空集"在
//   `subset.empty()` 上长得一模一样 ⇒ 那个探针会静默变成"全生效"。所以另记一个标志位。
bool& subset_set_flag() {
    static bool flag = false;
    return flag;
}

bool subset_narrowed() { return subset_set_flag(); }

bool active(const ModuleEntry& entry) {
    if (!subset_narrowed()) return true;   // 没收窄 ⇒ 全部生效（产品路径）
    return active_subset().count(entry.name) != 0;
}

}  // namespace

const char* engine_id() { return "xrkit-engine"; }

bool ready() {
    // 2026-09-26：M0 的占位已经过时 —— fs / font / text / xml / video 五个模块都已实现，
    // 所以这里如实返回 true。**不许再留着 ready()==false**：CLI 的 --bench 之类会据此
    // 退 2（显式 SKIP），而门禁把 SKIP 记进「本次未覆盖」—— 一个恒 false 的占位会让
    // 门禁长期维护一个错误状态（HanaAgent 2026-09-25 审计提出）。
    // ★★ 2026-10-06（审计第 4 组）：改成**由模块表推出来** —— 一块都没链上就不许说 ready。
    return module_count() > 0 && module_ready_count() == module_count();
}

int module_count() {
    int total = 0;
    for (const ModuleEntry& entry : kModules) {
        if (active(entry)) ++total;
    }
    return total;
}

int module_ready_count() {
    int on = 0;
    for (const ModuleEntry& entry : kModules) {
        if (!active(entry)) continue;
        if (entry.probe != nullptr && entry.probe() != nullptr) ++on;
    }
    return on;
}

int all_module_count() { return static_cast<int>(kModules.size()); }

std::vector<std::string> module_names() {
    std::vector<std::string> out;
    out.reserve(kModules.size());
    for (const ModuleEntry& entry : kModules) out.emplace_back(entry.name);
    return out;
}

void set_module_subset(const std::vector<std::string>& names) {
    std::set<std::string>& subset = active_subset();
    subset.clear();
    for (const std::string& name : names) {
        // 未知名字**照样收进来**（`kModules` 里找不到 ⇒ 一块都不生效）——
        // 这里刻意不做"过滤成已知名"：那会把"名字写错了"悄悄变成"没写"，正是自报的温床。
        subset.insert(name);
    }
    subset_set_flag() = true;
}

std::string status_line() {
    // ★★ 2026-10-06（审计第 4 组）：**一个数字都不是字面量** —— 全部从模块表算出来。
    //   `modules=<能跑>/<当前用到>`：摘掉/收窄模块集，这一串必须跟着变（否则就是常量自报）。
    const int total = module_count();
    const int on = module_ready_count();
    return std::string("engine id=") + engine_id() + " ready=" + (ready() ? "1" : "0") +
           " modules=" + std::to_string(on) + "/" + std::to_string(total);
}

}  // namespace xrkit::engine
