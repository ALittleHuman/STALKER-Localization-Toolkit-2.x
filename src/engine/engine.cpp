#include "xrkit/engine/engine.h"

namespace xrkit::engine {

const char* engine_id() { return "xrkit-engine"; }

bool ready() {
    // 2026-09-26：M0 的占位已经过时 —— fs / font / text / xml / video 五个模块都已实现，
    // 所以这里如实返回 true。**不许再留着 ready()==false**：CLI 的 --bench 之类会据此
    // 退 2（显式 SKIP），而门禁把 SKIP 记进「本次未覆盖」—— 一个恒 false 的占位会让
    // 门禁长期维护一个错误状态（HanaAgent 2026-09-25 审计提出）。
    return true;
}

std::string status_line() {
    return std::string("engine id=") + engine_id() + " ready=1 modules=5/5";
}

}  // namespace xrkit::engine
