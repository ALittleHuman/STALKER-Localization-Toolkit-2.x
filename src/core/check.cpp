#include "xrkit/core/check.h"

#include <exception>
#include <iostream>
#include <utility>

namespace xrkit::core {

void CheckRegistry::add(std::string name, CheckFn fn) {
    items_.emplace_back(std::move(name), std::move(fn));
}

void check(CheckContext& ctx, bool cond, const std::string& detail) {
    if (cond) {
        ++ctx.passed;
    } else {
        ++ctx.failed;
        ctx.failures.push_back(ctx.current + ": " + detail);
    }
}

int run_registry(const CheckRegistry& reg, bool verbose) {
    CheckContext ctx;
    ctx.verbose = verbose;

    for (const auto& item : reg.items()) {
        ctx.current = item.first;
        const int failed_before = ctx.failed;
        try {
            item.second(ctx);
        } catch (const std::exception& e) {
            ++ctx.failed;
            ctx.failures.push_back(ctx.current + ": 抛出异常 " + e.what());
        } catch (...) {
            ++ctx.failed;
            ctx.failures.push_back(ctx.current + ": 抛出未知异常");
        }
        if (verbose) {
            // 纯 ASCII，供别的进程解析（1.x：中文行在别的进程里是乱码）
            std::cout << (ctx.failed == failed_before ? "  PASS " : "  FAIL ") << item.first
                      << "\n";
        }
    }

    for (const auto& f : ctx.failures) {
        std::cout << "FAIL " << f << "\n";
    }
    std::cout << "SELFTEST pass=" << ctx.passed << " fail=" << ctx.failed << "\n";
    return ctx.failed == 0 ? 0 : 1;
}

}  // namespace xrkit::core
