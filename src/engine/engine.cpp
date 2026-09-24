#include "xrkit/engine/engine.h"

namespace xrkit::engine {

const char* engine_id() { return "xrkit-engine"; }

bool ready() {
    // M0 骨架：fs / font / text / xml / video 五个模块都还没有实现。
    // 这里返回 false 是**如实上报**，不是占位 —— CLI 的 --bench/--dump 会据此
    // 返回退出码 2（显式 SKIP），门禁必须把它记入「本次未覆盖」。
    return false;
}

std::string status_line() {
    return std::string("engine id=") + engine_id() + " ready=0 modules=0/5";
}

}  // namespace xrkit::engine
