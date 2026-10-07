#include "xrkit/engine/plugin/py_host.h"

#include "xrkit/core/log.h"
// `app_dir()`（exe 所在目录）住在 core/theme.h —— **不是** path.h：
// 实测按 path.h 写会报 `"app_dir": 不是 "xrkit::core" 的成员`（本片第一次编译就踩到）。
#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>

#ifdef XRKIT_WITH_PYTHON
#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

#ifdef XRKIT_WITH_PYTHON
namespace py = pybind11;

// 主线程的 thread state（`PyEval_SaveThread` 的返回值）：
// 起来之后**必须把 GIL 放开**（见 `init_python` 末尾）—— 否则主线程一直攥着 GIL，
// 工作线程里回调进 Python（六页都在 TaskShell 的 worker 里干活）会死等。
PyThreadState*& main_thread_state() {
    static PyThreadState* state = nullptr;
    return state;
}

// 交给插件的 Python 可调用对象（命令回调 / when 谓词）必须活到进程结束 —— 与 C ABI 那条
// 「不卸载正在被引用的模块」同一条纪律。
// ★ 刻意用 `new` 出去、**永不析构**：静态对象会在 `Py_FinalizeEx` 之后才析构，
//   那时再减引用计数就是踩已死的解释器。
std::vector<py::object>* keepalive() {
    static std::vector<py::object>* instance = new std::vector<py::object>();
    return instance;
}

// `api.log()` 的近期行（探针/门禁用它证明"插件的日志通道真的通"；**上界面**（Hub 日志栏）
// 随阶段 3 的宿主门面一起接）。上限 200 行，满了丢最旧的 —— 它是诊断缓冲，不是日志系统。
std::vector<std::string>& plugin_log_buffer() {
    static std::vector<std::string> instance;
    return instance;
}

size_t& loaded_python_plugins() {
    static size_t instance = 0;
    return instance;
}

// 裸 embeddable 布局的判据：`<home>/python3XX.zip` 在**根**（标准库=.zip，扩展模块=.pyd 也在根）。
// 为什么需要它（P2c）：embeddable 包是**扁平**的，既不认 `Lib/`、也不把 .pyd 放进 `DLLs/`
// ⇒ 只设 `PyConfig.home` 时解释器按"标准安装"布局推路径，照样 `Failed to import encodings module`。
std::filesystem::path find_embed_stdlib_zip(const std::string& home) {
    if (home.empty()) return {};
    std::error_code ec;
    for (std::filesystem::directory_iterator it(xrkit::core::path_from_utf8(home), ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = xrkit::core::path_to_utf8(it->path().filename());
        if (name.rfind("python3", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".zip") == 0) {
            return it->path();
        }
    }
    return {};
}

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

// `when`：**逐条照 1.x `toolkit_plugins._when_ok`**（行为基准是 1.x）：
//   falsy（None / {} / False）⇒ 恒真；dict ⇒ 要求 context 对应键相等（期望值 None 时缺键也算命中）；
//   可调用 ⇒ 调它（抛异常按不满足）；其余真值 ⇒ 恒真。
WhenSpec when_from_python(const py::object& when) {
    WhenSpec spec;
    bool truthy = true;
    try {
        truthy = when.cast<bool>();
    } catch (const py::error_already_set&) {
        truthy = true;   // 连 truthiness 都求不出来 ⇒ 按 1.x 的兜底"恒真"处理
    }
    if (!truthy) return spec;
    if (py::isinstance<py::dict>(when)) {
        for (auto item : py::reinterpret_borrow<py::dict>(when)) {
            const std::string key = py::str(item.first).cast<std::string>();
            ContextValue value;
            if (!item.second.is_none()) value = py::str(item.second).cast<std::string>();
            spec.match.emplace_back(key, value);
        }
        return spec;
    }
    if (PyCallable_Check(when.ptr()) != 0) {
        py::object kept = when;
        keepalive()->push_back(kept);
        spec.predicate = [kept](const Context& context) -> bool {
            py::gil_scoped_acquire acquire;   // 谓词可能从工作线程被问到
            try {
                py::dict ctx;
                for (const auto& [key, value] : context) {
                    ctx[py::str(key)] = value.has_value() ? py::cast(*value) : py::none();
                }
                return kept(ctx).cast<bool>();
            } catch (const py::error_already_set&) {
                return false;   // 12.6：可调用抛异常 ⇒ False（与 C ABI 同口径）
            }
        };
        return spec;
    }
    return spec;   // 其余真值：恒真（1.x 的兜底 `return True`）
}

// 把 Python 可调用对象包成引擎要的 `CommandHandler`：异常**转成 C++ 异常**，
// 交给 `Registry::invoke` 记账（零静默），绝不冒到 Qt 事件循环里；
// 同时保证回调不管从哪个线程来都先拿到 GIL。
CommandHandler wrap_python_handler(py::object callable, const std::string& label) {
    py::object kept = callable;
    keepalive()->push_back(kept);
    return [kept, label]() {
        py::gil_scoped_acquire acquire;
        try {
            kept();
        } catch (const py::error_already_set& err) {
            throw std::runtime_error("Python 回调「" + label + "」抛异常：" + std::string(err.what()));
        }
    };
}

// ── 给插件的 API 对象（第一版 API 表）────────────────────────────────────────
// 能力面：注册面（command / menu_item / option_entry / option）+ `log` + 只读标识。
// **控件面不在这里** —— 按规格，控件走宿主门面（L1/L2），那一片在阶段 3。
class PyPluginApi {
public:
    PyPluginApi(Registry* registry, std::string owner, std::string plugin_dir)
        : registry_(registry), owner_(std::move(owner)), dir_(std::move(plugin_dir)) {}

    const std::string& owner() const { return owner_; }
    const std::string& plugin_dir() const { return dir_; }

    void log(const std::string& message) const {
        const std::string line = "[" + owner_ + "] " + message;
        core::log(core::Level::Info, line);
        std::vector<std::string>& buffer = plugin_log_buffer();
        buffer.push_back(line);
        if (buffer.size() > 200) buffer.erase(buffer.begin());
    }

    py::object register_command(const std::string& action_id, const std::string& label,
                               py::object handler, py::object when) const {
        if (PyCallable_Check(handler.ptr()) == 0) {
            registry_->add_error(owner_, "register_command(" + action_id + ") 的 handler 不是可调用对象");
            return py::none();
        }
        const std::optional<std::string> id = registry_->register_command(
            owner_, action_id, label, wrap_python_handler(handler, label), when_from_python(when));
        if (!id.has_value()) return py::none();
        return py::str(*id);
    }

    bool register_menu_item(const std::string& host, const std::string& location,
                            const std::string& command_id, int order) const {
        return registry_->register_menu_item(owner_, host, location, command_id, order);
    }

    bool register_option_entry(const std::string& host, const std::string& area,
                               const std::string& value, py::object when) const {
        return registry_->register_option_entry(owner_, host, area, value, when_from_python(when));
    }

    bool register_option(const std::string& label, const std::vector<std::string>& choices,
                         py::object callback, const std::string& host, const std::string& area,
                         py::object when, int order) const {
        CommandHandler handler;
        if (!callback.is_none()) {
            if (PyCallable_Check(callback.ptr()) == 0) {
                registry_->add_error(owner_, "register_option(" + label + ") 的 callback 不是可调用对象");
                return false;
            }
            handler = wrap_python_handler(callback, label);
        }
        return registry_->register_option(owner_, label, choices, handler, host, area,
                                          when_from_python(when), order);
    }

private:
    Registry* registry_ = nullptr;
    std::string owner_;
    std::string dir_;
};

// 让 `import xrkit` 能用。走 `sys.modules` 注册而**不**用 `PyImport_AppendInittab`：
// 后者与 `PyConfig` 初始化路径的交互随版本变（pybind11 的 `PYBIND11_EMBEDDED_MODULE`
// 也是那条路），这里走一条**确定性**的路：建一个 module 对象塞进 `sys.modules`，
// 之后插件里 `import xrkit` 直接命中它。
void ensure_api_module() {
    py::dict modules = py::module_::import("sys").attr("modules");
    if (modules.contains("xrkit")) return;
    py::module_ m = py::reinterpret_steal<py::module_>(PyModule_New("xrkit"));
    m.attr("api_version") = kPyHostApiVersion;
    // ★ 类型注册**一次性**：`shutdown_python()` 之后不再支持重新起解释器
    //   （见头文件对 `shutdown_python` 的说明）。
    static bool class_registered = false;
    if (!class_registered) {
        py::class_<PyPluginApi>(m, "Api")
            .def_property_readonly("plugin_file", &PyPluginApi::owner)
            .def_property_readonly("plugin_dir", &PyPluginApi::plugin_dir)
            .def_property_readonly("api_version", [](const PyPluginApi&) { return kPyHostApiVersion; })
            .def("log", &PyPluginApi::log, py::arg("message"))
            .def("register_command", &PyPluginApi::register_command, py::arg("action_id"),
                 py::arg("label"), py::arg("handler"), py::arg("when") = py::none())
            .def("register_menu_item", &PyPluginApi::register_menu_item, py::arg("host"),
                 py::arg("location"), py::arg("command_id"), py::arg("order") = 0)
            .def("register_option_entry", &PyPluginApi::register_option_entry, py::arg("host"),
                 py::arg("area"), py::arg("value"), py::arg("when") = py::none())
            .def("register_option", &PyPluginApi::register_option, py::arg("label"),
                 py::arg("choices"), py::arg("callback") = py::none(), py::arg("host") = "fs",
                 py::arg("area") = "top_bar", py::arg("when") = py::none(), py::arg("order") = 0);
        class_registered = true;
    }
    modules["xrkit"] = m;
}

std::string sanitize_module_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_');
    }
    return out;
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
    // ③ **扁平** embed 形态：`python3XX.zip` 就在 exe 旁边（embeddable 包直接解在包根）。
    //    这条与 ② 都是"发行包自带"，只是落点不同 —— 两种都认，判据就看 zip 在不在。
    if (!find_embed_stdlib_zip(xrkit::core::app_dir()).empty()) {
        return xrkit::core::app_dir();
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
    // ★★ P2c：裸 embeddable 布局（`<home>/python3XX.zip`）必须**显式给模块搜索路径** ——
    //   扁平包里标准库是 .zip、扩展模块 .pyd 也在根，CPython 按"标准安装"布局推路径找不到
    //   ⇒ 只设 home 仍然 `Failed to import encodings module`（本片第一版就是这样红的）。
    std::vector<std::string> embed_paths;
    if (!wanted_home.empty()) {
        const std::filesystem::path embed_zip = find_embed_stdlib_zip(wanted_home);
        if (!embed_zip.empty()) {
            embed_paths.push_back(xrkit::core::path_to_utf8(embed_zip));   // 标准库
            embed_paths.push_back(wanted_home);                            // 根上的 .pyd
            std::error_code dir_ec;
            for (const char* sub : {"DLLs", "Lib"}) {   // 完整安装形态（有人把 home 指过去）也认
                const std::filesystem::path dir = xrkit::core::path_from_utf8(wanted_home) / sub;
                if (std::filesystem::is_directory(dir, dir_ec)) {
                    embed_paths.push_back(xrkit::core::path_to_utf8(dir));
                }
            }
        }
    }
    if (!embed_paths.empty()) {
        config.module_search_paths_set = 1;
        for (const std::string& path : embed_paths) {
            const std::filesystem::path native_path = xrkit::core::path_from_utf8(path);
            const std::wstring wide_path = native_path.wstring();
            const PyStatus appended =
                PyWideStringList_Append(&config.module_search_paths, wide_path.c_str());
            if (PyStatus_Exception(appended)) {
                PyConfig_Clear(&config);
                status.initialized = false;
                status.reason = appended.err_msg != nullptr
                                    ? appended.err_msg
                                    : "追加模块搜索路径失败（未给理由）";
                return status;
            }
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
    // 把给插件的 `xrkit` 模块先备好（此时 GIL 还在手上）
    ensure_api_module();
    // ★★ 放开 GIL：主线程一直攥着它的话，工作线程里回调进 Python 会死等。
    //   （`gil=released` 是门禁 `[python-host]` 步的判据之一：改回"不放"即红。）
    main_thread_state() = PyEval_SaveThread();
    status.gil_released = main_thread_state() != nullptr;
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
        if (status.gil_released) {
            PyEval_RestoreThread(main_thread_state());
            main_thread_state() = nullptr;
            status.gil_released = false;
        }
        const int rc = Py_FinalizeEx();
        status.initialized = false;
        status.version.clear();
        status.prefix.clear();
        status.reason = rc < 0 ? "Py_FinalizeEx 返回失败（有未清理的对象或 I/O）" : std::string();
    }
#endif
    status.initialized = false;
    status.gil_released = false;
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
           " gil=" + (status.initialized ? (status.gil_released ? "released" : "held") : "-") +
           " version=" + (status.version.empty() ? "-" : status.version) +
           " api=" + std::to_string(status.api_version) +
           " home=" + (status.home.empty() ? "-" : status.home) +
           " prefix=" + (status.prefix.empty() ? "-" : status.prefix) +
           " reason=" + status.reason;
}

// ── 命名判据：`*.py`（大小写不敏感，唯一一份）────────────────────────────────
bool is_plugin_py_name(const std::string& filename) {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".py") == 0;
}

// ── P2b：从目录加载 Python 插件 ──────────────────────────────────────────────
std::vector<RegistrationError> load_python_plugins_from_dir(const std::string& dir, Registry& registry,
                                                            PyLoadSummary* summary) {
    std::vector<RegistrationError> errors;
    PyLoadSummary local;
    PyLoadSummary& acc = summary != nullptr ? *summary : local;

    // ① 扫目录：**不依赖 Python**（没有宿主时也要能如实说出"有几个 .py 没被加载"）
    std::error_code ec;
    const std::filesystem::path root = xrkit::core::path_from_utf8(dir);
    if (!std::filesystem::is_directory(root, ec)) {
        errors.push_back({"<py-host>", "插件目录不存在：" + dir});
        // 目录不存在**不算启动失败** —— 只记录，宿主照常起来（与 DLL 加载器同口径）
        return errors;
    }
    std::vector<std::string> files;
    for (std::filesystem::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = xrkit::core::path_to_utf8(it->path().filename());
        // 命名判据只许一份（大小写不敏感）：`HELLO.PY` 与 `hello.py` 是同一个文件
        if (is_plugin_py_name(name)) files.push_back(xrkit::core::path_to_utf8(it->path()));
    }
    std::sort(files.begin(), files.end());
    acc.files = files.size();
    if (files.empty()) return errors;   // 目录里没有 .py ⇒ 一条都不记（不制造噪声）

#ifndef XRKIT_WITH_PYTHON
    const std::string why = python_status().reason;
    const std::string message = "发现 " + std::to_string(files.size()) +
                                " 个 .py 插件，但本构建没有 CPython 宿主 ⇒ Python 插件不可用（" +
                                why + "）";
    errors.push_back({"<py-host>", message});
    registry.add_error("<py-host>", message);
    return errors;
#else
    const PyHostStatus status = init_python();
    if (!status.initialized) {
        const std::string message = "发现 " + std::to_string(files.size()) +
                                    " 个 .py 插件，但 CPython 宿主没起来 ⇒ Python 插件不可用（" +
                                    status.reason + "）";
        errors.push_back({"<py-host>", message});
        registry.add_error("<py-host>", message);
        return errors;
    }

    py::gil_scoped_acquire acquire;
    ensure_api_module();
    py::object spec_from = py::module_::import("importlib.util").attr("spec_from_file_location");
    py::object module_from_spec = py::module_::import("importlib.util").attr("module_from_spec");

    for (const std::string& file : files) {
        const std::string owner = base_name(file);
        const std::string module_name = "xrkit_plugin_" + sanitize_module_name(owner);
        std::string failure;
        try {
            py::object spec = spec_from(module_name, file);
            if (spec.is_none()) {
                throw std::runtime_error("spec_from_file_location 返回 None（文件/路径不对？）");
            }
            py::object module = module_from_spec(spec);
            spec.attr("loader").attr("exec_module")(module);
            if (py::hasattr(module, "register") == 0) {
                throw std::runtime_error("插件没有导出 register(api)（第一版 API 表要求它）");
            }
            py::object register_fn = module.attr("register");
            if (PyCallable_Check(register_fn.ptr()) == 0) {
                throw std::runtime_error("register 不是可调用对象");
            }
            PyPluginApi* raw = new PyPluginApi(&registry, owner, dir);
            py::object api = py::cast(raw, py::return_value_policy::take_ownership);
            py::object result = register_fn(api);
            if (py::isinstance<py::bool_>(result) != 0 && result.cast<bool>() == false) {
                throw std::runtime_error("register(api) 明确返回 False（插件自报失败）");
            }
        } catch (const py::error_already_set& err) {
            failure = err.what();
        } catch (const std::exception& err) {
            failure = err.what();
        }
        if (!failure.empty()) {
            // ★ 先**按 owner 回滚**再记账：半个插件的注册留在表里＝指向坏插件的回调
            //   （C ABI 那条 S1-2 的血账，这里同一套口径、同一个实现）。
            const size_t undone = registry.rollback_plugin(owner);
            const std::string message =
                failure + "（已按 owner 回滚 " + std::to_string(undone) + " 条注册）";
            errors.push_back({owner, message});
            registry.add_error(owner, message);
            core::log(core::Level::Warn, "Python 插件加载失败：" + owner + "：" + failure);
            continue;
        }
        ++acc.loaded;
        acc.registrations += registry.residue_for(owner).registrations();
        ++loaded_python_plugins();
        core::log(core::Level::Info, "Python 插件已加载：" + owner);
    }
    return errors;
#endif
}

size_t loaded_python_plugin_count() {
#ifdef XRKIT_WITH_PYTHON
    return loaded_python_plugins();
#else
    return 0;
#endif
}

// 插件日志缓冲（诊断用；探针/自检读它证明"插件日志通道真的通"）。
// ★ 不放进 `PyHostStatus`：它不是"宿主状态"，而是**每次加载/回调都可能多出来的行**。
std::vector<std::string> take_python_log_lines() {
    std::vector<std::string> out;
#ifdef XRKIT_WITH_PYTHON
    out.swap(plugin_log_buffer());
#endif
    return out;
}

// ── 宿主探针的唯一实现（CLI 与 GUI exe 共用；见头文件）──────────────────────────
namespace {

// 键值协议**一行一条**：Python 异常的 traceback 是多行的，直接打会把 `plugin=` 之后的内容
// 跳行、解析器只能猜 ⇒ 折成 ` ⏎ `（口径同 `--shot` 的 `LBL` 行）。
std::string one_line(const std::string& text) {
    std::string out = text;
    for (size_t at = out.find('\n'); at != std::string::npos; at = out.find('\n', at)) {
        out.replace(at, 1, " \xE2\x8F\x8E ");   // ⏎（UTF-8）
    }
    return out;
}

}  // namespace

int run_python_host_probe(const std::vector<std::string>& args, const LineSink& sink) {
    const auto emit = [&sink](const std::string& line) {
        if (sink) sink(line);
    };
    bool want_host = false;
    bool want_plugins = false;
    std::string home;
    std::string dir;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--python-host") {
            want_host = true;
            continue;
        }
        if (a == "--py-plugins") {
            want_plugins = true;
            continue;
        }
        if (a == "--python-home" && i + 1 < args.size()) {
            home = args[++i];
            continue;
        }
        if (a == "--python-home") {
            emit("PYHOST error reason=用法：--python-home <目录>");
            return 3;
        }
        if (!a.empty() && a[0] == '-') continue;   // 其余开关不由本探针管
        if (dir.empty()) dir = a;                   // 第一个操作数 = 插件目录
    }
    if (!want_host && !want_plugins) {
        emit("PYHOST error reason=用法：--python-host | --py-plugins [目录]");
        return 3;
    }

    if (want_host) {
        const PyHostStatus started = init_python(home);
        emit(python_host_report());
        const bool enabled = started.enabled;
        const bool initialized = started.initialized;
        shutdown_python();
        if (!enabled) return 2;
        return initialized ? 0 : 1;
    }

    // `--py-plugins`：起宿主 → 扫目录 → import → register(api) → 进注册表 →
    // **真消费一次**（fs 工具栏那条命令）→ 报账。退出码 0=通 / 1=有失败 / 2=构建期没带 Python。
    const std::string plugin_dir = dir.empty() ? (xrkit::core::app_dir() + "/plugins") : dir;
    const PyHostStatus host = init_python(home);
    emit(std::string("PYPLUGINS host=") +
         (host.initialized ? "ok" : (host.enabled ? "failed" : "disabled")) +
         " version=" + (host.version.empty() ? "-" : host.version) + " dir=" + plugin_dir);
    if (!host.enabled) {
        emit("PYPLUGINS reason=" + host.reason);
        return 2;
    }
    Registry registry;
    PyLoadSummary summary;
    const std::vector<RegistrationError> errors =
        load_python_plugins_from_dir(plugin_dir, registry, &summary);
    emit("PYPLUGINS files=" + std::to_string(summary.files) +
         " loaded=" + std::to_string(summary.loaded) +
         " registrations=" + std::to_string(summary.registrations) +
         " errors=" + std::to_string(errors.size()));
    for (const RegistrationError& e : errors) {
        emit("PYPLUGINS error plugin=" + e.plugin + " reason=" + one_line(e.message));
        // ★ 回滚判据的可见面：坏插件这次注册进表的东西**必须为 0**（按 owner 逐容器报账）。
        emit("PYPLUGINS residue plugin=" + e.plugin +
             " value=" + std::to_string(registry.residue_for(e.plugin).registrations()));
    }
    const std::vector<const CommandReg*> items = registry.menu_items_for("fs", "toolbar", {});
    if (!items.empty()) {
        const CommandReg* first = items.front();
        emit("PYPLUGINS consume plugin=" + first->plugin + " label=" + first->label +
             " id=" + first->id);
        registry.invoke(first->plugin, first->label, first->handler);
        emit("PYPLUGINS residue plugin=" + first->plugin +
             " value=" + std::to_string(registry.residue_for(first->plugin).registrations()));
    } else {
        emit("PYPLUGINS consume result=none");
    }
    for (const std::string& line : take_python_log_lines()) {
        emit("PYPLUGINS log " + one_line(line));
    }
    if (!host.initialized) return 1;
    return (summary.loaded > 0 && errors.empty()) ? 0 : 1;
}

}  // namespace xrkit::plugin
