#pragma once
// xrkit::engine —— 引擎门面（SquashFS / DB / 字库 / 编码 / XML / 视频）。
//
// 本层同样**不许依赖 Qt**：黄金语料差分要用 CLI 黑盒驱动引擎，
// 而不是把测试塞进 GUI 进程里。

#include <string>

namespace xrkit::engine {

const char* engine_id();

// 各子模块是否已就绪。M0 阶段全部为 false —— CLI 据此把 --bench/--dump
// 记成**显式 SKIP**（退出码 2），而不是假绿。
bool ready();

// 纯 ASCII，供 CLI 与门禁解析。
std::string status_line();

}  // namespace xrkit::engine
