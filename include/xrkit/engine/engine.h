#pragma once
// xrkit::engine —— 引擎门面（SquashFS / DB / 字库 / 编码 / XML / 视频）。
//
// 本层同样**不许依赖 Qt**：黄金语料差分要用 CLI 黑盒驱动引擎，
// 而不是把测试塞进 GUI 进程里。

#include <string>
#include <vector>

namespace xrkit::engine {

const char* engine_id();

// 各子模块是否已就绪。M0 阶段全部为 false —— CLI 据此把 --bench/--dump
// 记成**显式 SKIP**（退出码 2），而不是假绿。
bool ready();

// ── ★★ 2026-10-06（审计第 4 组）：模块清单**不再自报常量** ─────────────────────
// 审计原文：「`engine::status_line()` 的**常量自报**」。
// 修前 `status_line()` 直接返回字面量 `" ready=1 modules=5/5"` —— 它和"到底编进来
// 几个模块"毫无关系（实测 `src/engine` 下有 6 个模块，报告里还是 5/5）。
//
// 现在的口径：
//   · 模块**清单**（`module_names()`）是这一份字面量列表 —— 它必须与 `src/engine/`
//     下的模块目录一一对应（自检逐名核对，名字对不上即红）；
//   · 模块**计数**（`module_count()`）来自"真链上的探针"（每个模块在自己的 TU 里
//     定义 `probe_<模块>()`，见 `probe_decls.h`）—— 摘掉一个模块它就少一个；
//   · `--module-subset <a,b>`（CLI 诊断开关）能把"当前引擎用到的模块集"真的改小 ⇒
//     `status_line()` 里的数字**必须跟着变**，否则就是自报常量（判红锁见 `tests/run_ci.py`
//     的 `engine-status` 步）。
int module_count();           // 当前引擎**用到**的模块数（可能被 `module_subset` 收窄）
int module_ready_count();     // 其中真能跑（探针返回非空）的个数
int all_module_count();       // 编译进来的模块总数（不受收窄影响）
std::vector<std::string> module_names();   // 清单原始名字（供自检/诊断）

// 诊断用（**不改产品行为**）：把"当前引擎用到的模块集"收窄成给定清单里的那些。
// 传空清单 = 一个都不用（探针用）。未知名字忽略（计数由它自己掉下来）。
// 只有 CLI 的 `--module-subset` 会调；产品路径不调。
void set_module_subset(const std::vector<std::string>& names);

// 纯 ASCII，供 CLI 与门禁解析。**内容从模块表算出来**（不是字面量）。
std::string status_line();

}  // namespace xrkit::engine
