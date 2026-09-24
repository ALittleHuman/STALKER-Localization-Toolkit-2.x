#pragma once
// xrkit::core —— 自检（selftest）登记表。
//
// 设计口径来自 1.x 的教训：
//   ① 收尾行必须是**纯 ASCII**（冻结 exe 的中文输出在别的进程里是乱码，
//      CI 只能解析 ASCII 行）；
//   ② 断言"假绿"比漏报更贵 —— 所以 SKIP 必须显式（用独立退出码），
//      不许把"没跑"混进"通过"。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::core {

struct CheckContext {
    bool verbose = false;
    std::string current;                 // 由 runner 设置，用于失败定位
    int passed = 0;
    int failed = 0;
    std::vector<std::string> failures;   // "检查名: 原因"
};

using CheckFn = std::function<void(CheckContext&)>;

class CheckRegistry {
public:
    void add(std::string name, CheckFn fn);
    const std::vector<std::pair<std::string, CheckFn>>& items() const { return items_; }

private:
    std::vector<std::pair<std::string, CheckFn>> items_;
};

// 在检查体内使用：不满足时记一条失败（含检查名与原因）
void check(CheckContext& ctx, bool cond, const std::string& detail);

// 跑完整张表。返回进程退出码：0 = 全绿，1 = 有失败。
// ASCII 收尾行：SELFTEST pass=<n> fail=<m>
int run_registry(const CheckRegistry& reg, bool verbose);

}  // namespace xrkit::core
