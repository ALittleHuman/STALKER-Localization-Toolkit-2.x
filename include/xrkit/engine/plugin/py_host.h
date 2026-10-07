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
//   ③ **Python home 只有一份解析口径**（`resolve_python_home`）：
//      显式参数（`--python-home` / `XRKIT_PYTHON_HOME`）
//      → exe 旁 `python\`（发行包＝子目录形态）
//      → **exe 旁有 `python3XX.zip`**（发行包＝裸 embeddable 直接解在包根，扁平形态，P2c 新增）
//      → 构建期烘焙的开发态 home（dev 构建）
//      → 空串（交给解释器自己按 PATH/注册表找系统安装）。
//      ★ 发行包的两种形态都认，判据就是"zip 在不在"。
//
// ⚠ 线程：插件回调可能从**工作线程**里被调用（六页都在 TaskShell 的 worker 里干活）。
//   本层提供的入口必须在调用侧先取得 GIL（`PyGILState_Ensure`）—— P2 的加载/注册阶段
//   全部发生在主线程，回调路径的 GIL 约定随"绑定"那一片一起落地并配判据。

#include <string>
#include <vector>

#include <functional>

#include "xrkit/engine/plugin/registry.h"

namespace xrkit::plugin {

// 宿主给插件的 API 表版本（`xrkit.api_version`）。表内向后兼容、表外无门（规格 §五）。
constexpr int kPyHostApiVersion = 1;

struct PyHostStatus {
    bool enabled = false;       // 构建期是否带 Python 支持（编译开关 + 开发文件）
    bool initialized = false;   // 解释器是否已经起来
    bool gil_released = false;  // 主线程是否已把 GIL 放开（工作线程才能安全回调进 Python）
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
//   `PYHOST enabled=1 initialized=1 gil=released version=3.14.6 api=1 home= prefix= reason=`
std::string python_host_report();

// ── P2b：从目录加载 **Python 插件**（`*.py`）────────────────────────────────────
// 与 C ABI 的 `load_plugins_from_dir` **共用同一份 `Registry`**（用户红线：能力不许两套表）。
//
// 契约（第一版 API 表 = `register(api)`，api 是宿主给的 `xrkit.Api` 实例）：
//   · 插件 = 目录里的一个 `.py` 文件，**文件名就是它的 owner**（命令 id 形如 `hello.py:greet`）；
//   · **零静默 + 可回退**：导入失败 / 没有 `register` / `register` 不是可调用 / `register` 抛异常 /
//     `register` 明确返回 `False` ⇒ 逐条进 `RegistrationError`，并**按 owner 回滚**它注册过的一切
//     （复用 `Registry::rollback_plugin` ⇒ 不留指向坏插件的回调）；
//   · 目录不存在 ⇒ 一条留痕（与 DLL 加载器同口径），**不阻塞启动**；
//   · 宿主没起来（构建期没带 Python / `init_python` 失败）⇒ 只要目录里**有 `.py`** 就留一条**带理由**
//     的留痕（"插件没生效"不许静默）；目录里没有 `.py` 则一条都不记（不制造噪声）。
struct PyLoadSummary {
    size_t files = 0;          // 扫到的 `*.py` 个数
    size_t loaded = 0;         // `register(api)` 正常跑完的插件数
    size_t registrations = 0;  // 这些插件合计注册进 `Registry` 的条数
};

std::vector<RegistrationError> load_python_plugins_from_dir(const std::string& dir, Registry& registry,
                                                            PyLoadSummary* summary = nullptr);
// 进程内已成功加载的 Python 插件数（探针用）。
size_t loaded_python_plugin_count();

// 取走并清空**插件日志缓冲**（`api.log()` 的近期行）。
// 为什么单独一条通道：插件日志是"每次加载/回调都可能多出来的行"，不是宿主状态；
// 探针/自检用它证明"插件日志真的通到宿主"（上界面＝阶段 3 的宿主门面）。
std::vector<std::string> take_python_log_lines();

// ★ 命名判据**只此一份**（与 `is_plugin_dll_name` 同一条纪律：Windows 文件名不分大小写，
//   `HELLO.PY` 与 `hello.py` 是同一个文件 ⇒ 必须大小写不敏感，否则静默漏载）。
bool is_plugin_py_name(const std::string& filename);

// ── 宿主探针的**唯一实现**（CLI 与 GUI exe 的同名开关都调它）────────────────────
// 为什么收在引擎里、而且收成一个函数：**发行包里只有 `xrkit_app.exe`（没有 CLI）**，
// 而"包内自带的 Python 到底能不能起来、能不能加载 `.py`"必须在**包内**判（见 `scripts/package.ps1`）
// ⇒ GUI exe 也要能跑这套诊断；两处各写一遍就是两份会分叉的实现（本仓最贵的一类返工）。
//
// 参数：`args` = **去掉 exe 名之后的完整参数表**（本函数自己挑 `--python-host` /
// `--py-plugins [目录]` / `--python-home <目录>`，其余开关一律忽略）。
// 输出：逐行交给 `sink`（引擎层**不碰 stdout** —— 谁来打印由前端决定）。
// 返回：0=通  1=有失败  2=构建期没带 Python（显式跳过）  3=用法错误。
using LineSink = std::function<void(const std::string&)>;
int run_python_host_probe(const std::vector<std::string>& args, const LineSink& sink);

}  // namespace xrkit::plugin
