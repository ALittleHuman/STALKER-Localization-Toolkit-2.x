#pragma once
// xrkit::plugin —— **宿主服务门面**（插件改装的"宿主面"第三块：注册面 / 控件面 / 引擎面之外的
// 那些"宿主才有的东西"）。2026-10-07 P7b。
//
// 为什么单独一张表（而不是塞进 `UiFacade` 或 `EngineFacade`）：
//   · 它既不是"控件"（插件不构造任何东西），也不是"引擎能力"（不碰解包/出包那类业务）；
//     它是**宿主替插件办的杂事**：记用户偏好、报"系统字体在哪"、报 exe 目录。
//   · 样板页 fontpack 真的要用这三件（`load/save_user_value`、`platform_font_dirs`、`app_dir`）
//     ⇒ **由样板页反向定义**，不是猜。
//
// 三条口径（与前两张表一致）：
//   ① 门面由**外壳**注入；引擎自带**默认表**（这几件只依赖引擎，CLI 与 GUI 两个宿主都用得上）；
//   ② ★**表外无门**（能力表判据：多一个即泄漏、少一个即削能力）；
//   ③ 值类型 Python 友好（str / list），不漏 C++ 结构体与指针。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::plugin {


// ── ★★ P12b：**跑外部进程**（宿主服务；语义唯一一份在 `engine/process/run.cpp`）────
//   为什么要它：视频页要跑 ffprobe / `ffmpeg -encoders` / ffmpeg 转码，而"取消要真停、
//   退出码如实报、stderr 尾巴可见、不许弹控制台"是**产品语义** —— 插件侧自己写
//   `subprocess` 就是第二份实现（那正是本仓门禁反复拦的形态）。

struct ProcessRunOptions {
    std::string program;
    std::vector<std::string> args;
    std::string working_dir;
    std::size_t stderr_tail_bytes = 8192;
    int cancel_wait_ms = 5000;
    std::function<bool()> should_stop;
    std::function<void(const std::string&)> on_stdout;
    std::function<void(const std::string&)> on_stderr;
};

struct ProcessRunResult {
    bool started = false;
    bool cancelled = false;
    bool cancel_timeout = false;
    bool normal_exit = false;
    int exit_code = -1;
    std::string error;
    std::string stderr_tail;
    uint64_t stdout_lines = 0;
    uint64_t stderr_lines = 0;
};

struct HostFacade {
    // 用户偏好（`user.ltx` 里 `[section] key=value`）：**只读写点名的那一项**，不整块覆盖
    std::function<std::string(const std::string& section, const std::string& key,
                              const std::string& fallback)>
        pref_get;
    std::function<bool(const std::string& section, const std::string& key,
                       const std::string& value)>
        pref_set;
    // exe 所在目录（发行版放 ffmpeg/ffprobe 的地方，也是 `user.ltx` 的落点）
    std::function<std::string()> app_dir;
    // 系统字体目录（唯一一份：内置页与插件共用）
    std::function<std::vector<std::string>()> system_font_dirs;
    // ★★ P16-0：字段名与三处登记名（`kMethodNames` / `has_impl` / pybind 的 `.def("run_process")`
    //   与 `PyHost::run_process`）**统一成 `run_process`**。修前这里叫 `host_run_process`
    //   ⇒ 门面自查（`host_facade_has("run_process")`）恒 false，而插件可见的名字正是
    //   `run_process`：能力"看着在、实际拿不到"。改的是**内部字段名**，插件可见契约一字未变。
    std::function<ProcessRunResult(const ProcessRunOptions&)> run_process;
};

void set_host_facade(const HostFacade* facade);
const HostFacade* host_facade();

bool host_facade_available(std::string* reason = nullptr);
bool host_facade_has(const std::string& name);
std::vector<std::string> host_facade_method_names();

}  // namespace xrkit::plugin
