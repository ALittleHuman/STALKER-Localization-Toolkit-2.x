#include "xrkit/engine/plugin/py_host.h"

// `app_dir()`（exe 所在目录）住在 core/theme.h —— **不是** path.h：
// 实测按 path.h 写会报 `"app_dir": 不是 "xrkit::core" 的成员`（本片第一次编译就踩到）。
#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifdef XRKIT_WITH_PYTHON
// pybind11 只用于**读宿主自己的状态**（`sys.version_info` / `sys.base_prefix`），
// 证明 vendored 头与运行期都对得上；给插件的 API 表（`xrkit` 模块）随后一片接上。
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <Python.h>
#endif

namespace xrkit::plugin {
namespace {

std::mutex& host_mutex() {
    static std::mutex instance;
    return instance;
}

PyHostStatus& status_storage() {
    static PyHostStatus instance;
    return instance;
}

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

void refresh_reason_if_disabled(PyHostStatus& status) {
#ifndef XRKIT_WITH_PYTHON
    status.enabled = false;
    status.initialized = false;
    status.reason = "构建期未启用 CPython 宿主（CMake 选项 XRKIT_WITH_PYTHON 关掉，"
                    "或找不到 Python3 开发文件）—— 插件宿主不可用；请在构建日志里查"
                    "「CPython 插件宿主」那行";
#else
    status.enabled = true;
    if (!status.initialized && status.reason.empty()) {
        status.reason = "解释器尚未启动（还没调用 init_python）";
    }
#endif
}

#ifdef XRKIT_WITH_PYTHON
namespace py = pybind11;

// 取一个 ASCII 键值行要用的值：任何 Python 异常都退化成空串（**不许**因为读状态把宿主弄崩）。
std::string py_str_or_empty(const char* module_attr) {
    try {
        py::object value = py::module_::import("sys").attr(module_attr);
        return py::str(value).cast<std::string>();
    } catch (const py::error_already_set&) {
        return std::string();
    }
}

std::string version_from_interpreter() {
    try {
        py::object info = py::module_::import("sys").attr("version_info");
        return std::to_string(info.attr("major").cast<int>()) + "." +
               std::to_string(info.attr("minor").cast<int>()) + "." +
               std::to_string(info.attr("micro").cast<int>());
    } catch (const py::error_already_set&) {
        return std::string();
    }
}
#endif  // XRKIT_WITH_PYTHON

}  // namespace

std::string resolve_python_home(const std::string& explicit_home) {
    if (!explicit_home.empty()) return explicit_home;
    // ① 环境变量（**不设持久环境变量的规矩**针对的是"跑测试要预置环境"，
    //    这里是给用户/打包链一个显式入口，缺省为空 ⇒ 行为与不设它完全一致）
    const std::string from_env = env_or_empty("XRKIT_PYTHON_HOME");
    if (!from_env.empty()) return from_env;
    // ② exe 旁的 `python\`：发行包形态（embeddable Python 解压在那里）。
    //    ★ 必须排在 ③ 之前：发行包在自己的机器上跑时，**包内那份**才是唯一能保证版本一致的。
    const std::filesystem::path bundled =
        xrkit::core::path_from_utf8(xrkit::core::app_dir()) / "python";
    std::error_code ec;
    if (std::filesystem::is_directory(bundled, ec)) {
        return xrkit::core::path_to_utf8(bundled);
    }
#ifdef XRKIT_PYTHON_HOME_DEFAULT
    // ③ 构建期烘焙的开发态 home（构建时那个 Python 安装）。
    //    为什么必须有它（实测）：嵌入解释器不设 home 时会按**宿主 exe 所在目录**推 prefix，
    //    于是去 `build/…/tools/cli/Lib` 找标准库 ⇒ `Failed to import encodings module`。
    return std::string(XRKIT_PYTHON_HOME_DEFAULT);
#else
    // ④ 都不适用：交给解释器自己找（只有"没烘焙过路径"的构建才会走到这里）
    return std::string();
#endif
}

PyHostStatus init_python(const std::string& home) {
    std::lock_guard<std::mutex> lock(host_mutex());
    PyHostStatus& status = status_storage();
    refresh_reason_if_disabled(status);
    if (!status.enabled) return status;
    if (status.initialized) return status;

    const std::string wanted_home = resolve_python_home(home);
    status.home = wanted_home;
    status.reason.clear();

#ifdef XRKIT_WITH_PYTHON
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    // 宿主是 GUI/CLI 进程：**不让 Python 抢信号处理**（否则 Ctrl+C / 关窗行为会变）
    config.install_signal_handlers = 0;
    // 先设 home（发行包＝exe 旁 python/），再初始化 —— 分两步写是为了**每一步的失败都有
    // 自己的 `func`/`err_msg`**（PyStatus 不带默认构造的哨兵值，不猜它的初值形态）。
    if (!wanted_home.empty()) {
        // 用**宽字符**接口传路径（Windows 上路径可能含非 ASCII；`PyConfig_SetBytesString`
        // 按 locale 解释字节，遇到中文目录会走样）。
        const std::filesystem::path native_home = xrkit::core::path_from_utf8(wanted_home);
        const std::wstring wide_home = native_home.wstring();
        const PyStatus set_home = PyConfig_SetString(&config, &config.home, wide_home.c_str());
        if (PyStatus_Exception(set_home)) {
            PyConfig_Clear(&config);
            status.initialized = false;
            status.reason = set_home.err_msg != nullptr ? set_home.err_msg
                                                        : "设置 PyConfig.home 失败（未给理由）";
            return status;
        }
    }
    const PyStatus started = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(started)) {
        // 零静默：把解释器给的原话留下（err_msg 可能是空 ⇒ 退化成"未给理由"，
        // 但**绝不**返回一个 initialized=true 的假状态）
        status.initialized = false;
        status.reason = started.err_msg != nullptr ? started.err_msg
                                                   : "Py_InitializeFromConfig 失败（未给理由）";
        if (status.reason.empty()) status.reason = "Py_InitializeFromConfig 失败（err_msg 为空）";
        return status;
    }
    // pybind11 的 embedded 模块表在 Py_Initialize 时就位；`xrkit` 模块本身随后一片接上。
    status.initialized = Py_IsInitialized() != 0;
    if (!status.initialized) {
        status.reason = "Py_InitializeFromConfig 没报错，但 Py_IsInitialized() 为假";
        return status;
    }
    status.version = version_from_interpreter();
    status.prefix = py_str_or_empty("base_prefix");
    if (status.version.empty() || status.prefix.empty()) {
        // 读状态失败本身要可见（但宿主仍算起来了：能 import 才是硬判据）
        status.reason = "解释器已起来，但读取 sys.version_info/base_prefix 失败";
    }
#else
    status.reason = "构建期未启用 CPython 宿主（见 python_status().reason 的说明）";
#endif
    return status;
}

void shutdown_python() {
    std::lock_guard<std::mutex> lock(host_mutex());
    PyHostStatus& status = status_storage();
#ifdef XRKIT_WITH_PYTHON
    if (status.initialized) {
        // 口径：先确保没有存活的 pybind11 对象（调用方责任，见头文件的线程说明），
        // 再关解释器；`Py_FinalizeEx` 的返回值如实反映出来。
        const int rc = Py_FinalizeEx();
        status.initialized = false;
        status.version.clear();
        status.prefix.clear();
        status.reason = rc < 0 ? "Py_FinalizeEx 返回失败（有未清理的对象或 I/O）" : std::string();
    }
#endif
    status.initialized = false;
    (void)status;
}

PyHostStatus python_status() {
    std::lock_guard<std::mutex> lock(host_mutex());
    PyHostStatus status = status_storage();
    refresh_reason_if_disabled(status);
    return status;
}

std::string python_host_report() {
    const PyHostStatus status = python_status();
    return "PYHOST enabled=" + std::to_string(status.enabled ? 1 : 0) +
           " initialized=" + std::to_string(status.initialized ? 1 : 0) +
           " version=" + (status.version.empty() ? "-" : status.version) +
           " api=" + std::to_string(status.api_version) +
           " home=" + (status.home.empty() ? "-" : status.home) +
           " prefix=" + (status.prefix.empty() ? "-" : status.prefix) +
           " reason=" + status.reason;
}

}  // namespace xrkit::plugin
