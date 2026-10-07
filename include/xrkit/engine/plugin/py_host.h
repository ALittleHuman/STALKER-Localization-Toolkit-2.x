#pragma once
// xrkit::engine::plugin —— **CPython 插件宿主**（插件改装 P2）。
//
// 用户口径（2026-10-07 拍板，选项原文）：「**Python 模块接口 + C ABI 退役**」
//   ⇒ 新插件＝`.py`（`register(api)`），闭源插件走 `.pyd`（Python C-extension）；
//     不再扩展 `plugin/abi.h` 的 `stk_api` 结构表。
//
// 为什么放在**引擎层**且 Qt-free：
//   · 宿主必须能被**两个前端**共用 —— Hub（Qt）与 CLI 都加载同一批 `.py` 插件、走
//     **同一份** `Registry`（用户红线：「GUI 的功能，CLI 肯定要全部拥有啊」）；
//   · 控件门面（L1/L2）留在外壳：本层只管"解释器 + 导入 + 注册"，不碰任何控件。
//
// 口径（三条都能判红）：
//   ① **零静默**：宿主没起来 / 起不来的**理由**必须读得出来（`PyHostStatus::reason`）。
//      绝不允许"插件没生效，而界面上、日志里一片安静"。
//   ② **降级不许装成正常**：构建期没有 Python3 开发文件 ⇒ 编出降级实现
//      （`enabled=false` + 理由），其余功能照常；门禁 `[python-host]` 步在 release 档
//      见 `enabled=0` 即 **FAIL**（不是 SKIP —— 那会变成"一键关掉锁"的后门）。
//   ③ **Python home 只有一份解析口径**：显式参数（`--python-home` / `XRKIT_PYTHON_HOME`）
//      → exe 旁的 `python\`（发行包＝解压出来的 embeddable Python）→ 空串（交给解释器
//      自己按 PATH/注册表找系统安装＝开发态）。见 `resolve_python_home()`。
//
// ⚠ 线程：插件回调可能从**工作线程**里被调用（六页都在 TaskShell 的 worker 里干活）。
//   本层提供的入口必须在调用侧先取得 GIL（`PyGILState_Ensure`）—— P2 的加载/注册阶段
//   全部发生在主线程，回调路径的 GIL 约定随"绑定"那一片一起落地并配判据。

#include <string>

namespace xrkit::plugin {

// 宿主给插件的 API 表版本（`xrkit.api_version`）。表内向后兼容、表外无门（规格 §五）。
constexpr int kPyHostApiVersion = 1;

struct PyHostStatus {
    bool enabled = false;       // 构建期是否带 Python 支持（编译开关 + 开发文件）
    bool initialized = false;   // 解释器是否已经起来
    std::string version;        // 解释器版本（"3.14.6"；未起来为空）
    std::string home;           // **请求**的 home（空 = 没指定，让解释器自己找）
    std::string prefix;         // 解释器**实际**的 base_prefix（未起来为空）
    std::string reason;         // enabled=false / initialized=false 时的理由（零静默）
    int api_version = kPyHostApiVersion;
};

// Python home 的唯一解析口径（见文件头 ③）。返回空串＝"不指定"。
std::string resolve_python_home(const std::string& explicit_home = std::string());

// 起解释器（幂等：已经起来就直接回当前状态）。**不抛**：失败写进 `reason`。
PyHostStatus init_python(const std::string& home = std::string());
// 关解释器（幂等）。调用前必须保证没有存活的 Python 对象（插件句柄等）。
void shutdown_python();
// 当前状态（不启动解释器；纯读取）。
PyHostStatus python_status();

// 自检/探针用的一行摘要（ASCII 键值，门禁直接解析这一行）：
//   `PYHOST enabled=1 initialized=1 version=3.14.6 api=1 home= prefix=… reason=`
std::string python_host_report();

}  // namespace xrkit::plugin
