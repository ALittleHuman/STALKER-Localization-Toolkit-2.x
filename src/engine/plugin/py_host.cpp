#include "xrkit/engine/plugin/py_host.h"

// ★ P3：控件门面（`api.ui.*` 的实现入口）—— 引擎层只持有外壳注入的表
#include "xrkit/engine/plugin/widget_facade.h"
// ★ P7a：引擎调用门面（`api.engine.*`；用户拍板的方案 3）
#include "xrkit/engine/plugin/engine_facade.h"
// ★ P7b：宿主服务门面（`api.host.*`：偏好读写 / exe 目录 / 系统字体目录）
#include "xrkit/engine/plugin/host_facade.h"

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
// 能力面：注册面（command / menu_item / option_entry / option / **panel / tool**）+ `log`
// + 只读标识 + ★**控件门面**（`api.ui.*`，P3）。
// **控件面不在这里** —— 按规格，控件走宿主门面（L1/L2），那一片在阶段 3。
//
// ★ 前置声明：`PyPluginApi` 的 builder 包装要用 `handle_of_widget`，而 `PyWidget` 与它
//   定义在下面（先声明后定义，同一个匿名命名空间里没问题）。
UiHandle handle_of_widget(const py::object& widget_or_none);

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

    // ★★ P3：插件页 / 面板 —— **控件门面的落点**。
    //   builder 允许**不返回东西**（插件只往宿主给的根里塞控件）；返回值若是 `Widget`
    //   就当作"老写法"交给宿主嵌入。宿主（Hub）已经把"当前面板根"设好了。
    bool register_panel(const std::string& title, py::object builder, const std::string& host,
                        const std::string& area, int order) const {
        PanelBuilder wrapped = wrap_python_builder(builder, "register_panel(" + title + ")");
        if (!wrapped) return false;
        return registry_->register_panel(owner_, title, std::move(wrapped), host, area, {}, order);
    }
    // ★★ P7c：带 `replaces` / `subtitle` 的那一条（样板页迁移用：插件页**顶掉内置页的槽位**）
    bool register_tool(const std::string& name, py::object builder, int order,
                       const std::string& replaces, const std::string& subtitle,
                       const std::string& title) const {
        PanelBuilder wrapped = wrap_python_builder(builder, "register_tool(" + name + ")");
        if (!wrapped) return false;
        return registry_->register_tool(owner_, name, std::move(wrapped), order, replaces,
                                       subtitle, title);
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
    // 把 Python 的 builder 包成引擎要的 `PanelBuilder`：
    //   · 套一层 `UiPanelScope` ⇒ 无论谁调，"当前面板根"都成立（宿主已设过，这里嵌套无害）；
    //   · 返回值允许 `None`（插件只往根里塞控件）；
    //   · ★**异常不往上抛**：宿主调 builder 的地方（Hub 造页/造面板）没有 catch，抛出去就是崩。
    //     这里捕获 → **留痕**（`registry_->add_error` + 引擎日志）→ 返回 nullptr
    //     ⇒ 宿主那边自然走"没造出控件 ⇒ 占位页 + 日志"那条路（零静默，且不崩）。
    PanelBuilder wrap_python_builder(py::object builder, const std::string& what) const {
        if (builder.is_none() != 0 || PyCallable_Check(builder.ptr()) == 0) {
            registry_->add_error(owner_, what + " 的 builder 不是可调用对象");
            return PanelBuilder{};
        }
        py::object kept = builder;
        keepalive()->push_back(kept);
        const std::string owner = owner_;
        Registry* registry = registry_;
        return [kept, what, owner, registry](void* parent) -> void* {
            py::gil_scoped_acquire acquire;
            UiPanelScope scope(parent);
            try {
                py::object result = kept();
                if (result.is_none() != 0) return kNoWidget;
                return handle_of_widget(result);
            } catch (const py::error_already_set& err) {
                const std::string message = what + " 的 builder 抛异常：" + err.what();
                core::log(core::Level::Error, "[" + owner + "] " + message);
                registry->add_error(owner, message);
                return kNoWidget;
            } catch (const std::exception& err) {
                const std::string message = what + " 的 builder 抛异常：" + err.what();
                core::log(core::Level::Error, "[" + owner + "] " + message);
                registry->add_error(owner, message);
                return kNoWidget;
            }
        };
    }

    Registry* registry_ = nullptr;
    std::string owner_;
    std::string dir_;
};

// ── 控件门面在 Python 侧的形态（P3）──────────────────────────────────────────
// 插件**永不接触 Qt**：它拿到的是 `xrkit.Widget`（**只有一个不透明句柄**，没有任何方法能把
// 它解引用成 Qt 对象）；所有操作都回到 `api.ui.*`。★**表外无门** —— 判据
// `tests/check_ui_capability_table.py` 把 `dir(xrkit.Api)/dir(xrkit.Ui)/dir(xrkit.Widget)`
// 与**预期表**逐条对账，多一个方法就红（能力泄漏必须是有意为之）。
class PyWidget {
public:
    explicit PyWidget(UiHandle handle) : handle_(handle) {}
    UiHandle handle() const { return handle_; }
    bool valid() const { return handle_ != kNoWidget; }

private:
    UiHandle handle_ = kNoWidget;
};

// Python 的 `Widget` ↔ 门面句柄（`PyUi` 与 `PyPluginApi` 都要用 ⇒ 提成自由函数）
UiHandle handle_of_widget(const py::object& widget_or_none) {
    if (widget_or_none.is_none() != 0) return kNoWidget;
    if (py::isinstance<PyWidget>(widget_or_none) == 0) return kNoWidget;
    auto* typed = widget_or_none.cast<PyWidget*>();
    return typed == nullptr ? kNoWidget : typed->handle();
}

// ── P6：树的两个入参转换（Python 的 list ↔ 门面的 POD）─────────────────────────
// 列：`[["列1", 0], ["列2", 200]]`（宽度 0 = 宿主按规范给；末列吃剩余宽度）
std::vector<UiFacade::TreeColumn> tree_columns_from_python(const py::object& columns) {
    std::vector<UiFacade::TreeColumn> out;
    if (columns.is_none() != 0) return out;
    for (auto item : py::list(columns)) {
        UiFacade::TreeColumn column;
        py::sequence pair = py::reinterpret_borrow<py::sequence>(item);
        if (py::len(pair) > 0) column.title = py::str(pair[0]).cast<std::string>();
        if (py::len(pair) > 1) column.width = py::cast<int>(pair[1]);
        out.push_back(std::move(column));
    }
    return out;
}

// 节点：`[{"key": "a", "cells": ["a.txt", "12"], "has_children": False,
//          "checkable": True, "checked": False}, …]`（缺的字段取默认）
std::vector<UiFacade::TreeNodeSpec> tree_nodes_from_python(const py::object& nodes) {
    std::vector<UiFacade::TreeNodeSpec> out;
    if (nodes.is_none() != 0) return out;
    for (auto item : py::list(nodes)) {
        py::dict node = py::reinterpret_borrow<py::dict>(item);
        UiFacade::TreeNodeSpec spec;
        if (node.contains("key")) spec.key = py::str(node["key"]).cast<std::string>();
        if (node.contains("cells")) {
            for (auto cell : py::list(node["cells"])) {
                spec.cells.push_back(py::str(cell).cast<std::string>());
            }
        }
        if (node.contains("has_children")) {
            spec.has_children = py::cast<bool>(node["has_children"]);
        }
        if (node.contains("checkable")) spec.checkable = py::cast<bool>(node["checkable"]);
        if (node.contains("checked")) spec.checked = py::cast<bool>(node["checked"]);
        if (node.contains("has_action")) {
            spec.has_action = py::cast<bool>(node["has_action"]);
        }
        // ★★ 2026-10-10（P17-57，接口契约批）：Python 侧也能给 `tooltip` / `matched`
        //   （树节点 dict 的两个新键；缺省＝不设/未命中）。
        if (node.contains("tooltip")) spec.tooltip = py::str(node["tooltip"]).cast<std::string>();
        if (node.contains("matched")) spec.matched = py::cast<bool>(node["matched"]);
        out.push_back(std::move(spec));
    }
    return out;
}

py::list strings_to_python(const std::vector<std::string>& values) {
    py::list out;
    for (const std::string& value : values) out.append(py::str(value));
    return out;
}

// ── P7a：引擎调用门面的入参转换（Python 的 dict → 门面的 POD）─────────────────
FontPackOptions font_options_from_python(const py::object& options) {
    FontPackOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("game", out.game);
    take_string("lang", out.lang);
    take_string("xml_dir", out.xml_dir);
    take_string("font_path", out.font_path);
    take_string("out_dir", out.out_dir);
    take_string("suffix", out.suffix);
    if (data.contains("offset")) out.offset = py::cast<int>(data["offset"]);
    if (data.contains("full_cyrillic")) out.full_cyrillic = py::cast<bool>(data["full_cyrillic"]);
    if (data.contains("only_sizes")) {
        for (auto item : py::list(data["only_sizes"])) {
            out.only_sizes.push_back(py::cast<int>(item));
        }
    }
    return out;
}


// ── ★★ P9a：convert 页那一组的入参转换（dict → 门面 POD）────────────────────
TextBatchOptions text_batch_options_from_python(const py::object& options) {
    TextBatchOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("source", out.source);
    take_string("output_dir", out.output_dir);
    take_string("exclude_patterns", out.exclude_patterns);
    take_string("source_encoding", out.source_encoding);
    take_string("target_encoding", out.target_encoding);
    take_string("backup_dirname", out.backup_dirname);
    take_string("backup_suffix", out.backup_suffix);
    if (data.contains("recursive")) out.recursive = py::cast<bool>(data["recursive"]);
    if (data.contains("backup")) out.backup = py::cast<bool>(data["backup"]);
    if (data.contains("extensions")) {
        for (auto item : py::list(data["extensions"])) {
            out.extensions.push_back(py::str(item).cast<std::string>());
        }
    }
    return out;
}

TextBackupOptions text_backup_options_from_python(const py::object& options) {
    TextBackupOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    if (data.contains("source_root")) out.source_root = py::str(data["source_root"]).cast<std::string>();
    if (data.contains("recursive")) out.recursive = py::cast<bool>(data["recursive"]);
    if (data.contains("extensions")) {
        for (auto item : py::list(data["extensions"])) {
            out.extensions.push_back(py::str(item).cast<std::string>());
        }
    }
    return out;
}

TextStats text_stats_from_python(const py::object& stats) {
    TextStats out;
    if (stats.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(stats);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("generated_at", out.generated_at);
    take_string("source", out.source);
    take_string("output_dir", out.output_dir);
    take_string("source_enc", out.source_enc);
    take_string("target_enc", out.target_enc);
    take_string("duration", out.duration);
    if (data.contains("single_file")) out.single_file = py::cast<bool>(data["single_file"]);
    const auto take_u64 = [&data](const char* key, uint64_t& target) {
        if (data.contains(key)) target = py::cast<uint64_t>(data[key]);
    };
    take_u64("candidates", out.candidates);
    take_u64("processed", out.processed);
    take_u64("success", out.success);
    take_u64("skipped", out.skipped);
    take_u64("failed", out.failed);
    return out;
}

// hooks：引擎会从**工作线程**叫它们 ⇒ 保活 + 取 GIL + 不让 Python 异常掀翻宿主
TextHooks text_hooks_from_python(const py::object& hooks) {
    TextHooks out;
    if (hooks.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(hooks);
    const auto hold = [&data](const char* key) -> py::object {
        if (!data.contains(key) || data[key].is_none() != 0) return py::none();
        py::object kept = data[key];
        keepalive()->push_back(kept);
        return kept;
    };
    py::object stop = hold("cancel");
    if (stop.is_none() == 0) {
        out.should_stop = [stop]() -> bool {
            py::gil_scoped_acquire acquire;
            try {
                return py::cast<bool>(stop());
            } catch (const py::error_already_set&) {
                return false;
            }
        };
    }
    py::object gate = hold("gate");
    if (gate.is_none() == 0) {
        out.gate = [gate]() -> bool {
            py::gil_scoped_acquire acquire;
            try {
                return py::cast<bool>(gate());
            } catch (const py::error_already_set&) {
                return true;   // 回调炸了 ⇒ 别把引擎卡死
            }
        };
    }
    py::object message = hold("message");
    if (message.is_none() == 0) {
        out.message = [message](const std::string& line) {
            py::gil_scoped_acquire acquire;
            try {
                message(py::str(line));
            } catch (const py::error_already_set&) {
            }
        };
    }
    py::object progress = hold("progress");
    if (progress.is_none() == 0) {
        out.progress = [progress](int done, int total) {
            py::gil_scoped_acquire acquire;
            try {
                progress(py::int_(done), py::int_(total));
            } catch (const py::error_already_set&) {
            }
        };
    }
    return out;
}


// ── ★★ P10a：text 页那一组的入参转换（dict → 门面 POD）─────────────────────
TextExtractRequest text_extract_request_from_python(const py::object& request) {
    TextExtractRequest out;
    if (request.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(request);
    if (data.contains("source_dir")) out.source_dir = py::str(data["source_dir"]).cast<std::string>();
    if (data.contains("target_dir")) out.target_dir = py::str(data["target_dir"]).cast<std::string>();
    if (data.contains("prefix")) out.prefix = py::str(data["prefix"]).cast<std::string>();
    return out;
}

TextExtractOptions text_extract_options_from_python(const py::object& options) {
    TextExtractOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    if (data.contains("prefix")) out.prefix = py::str(data["prefix"]).cast<std::string>();
    if (data.contains("id_prefix")) out.id_prefix = py::str(data["id_prefix"]).cast<std::string>();
    if (data.contains("clean")) out.clean = py::cast<bool>(data["clean"]);
    if (data.contains("cancel") && data["cancel"].is_none() == 0) {
        py::object cancel = data["cancel"];
        keepalive()->push_back(cancel);
        out.should_stop = [cancel]() -> bool {
            py::gil_scoped_acquire acquire;
            try {
                return py::cast<bool>(cancel());
            } catch (const py::error_already_set&) {
                return false;   // 回调炸了 ⇒ 别把引擎卡在"取消"上
            }
        };
    }
    return out;
}


// ── ★★ P11a：xml 页那一组的入参/出参转换 ─────────────────────────────────────
py::dict xml_mode_to_python(const XmlMode& mode) {
    py::dict out;
    out["name"] = mode.name;
    out["key"] = mode.key;
    out["order"] = mode.order;
    out["plugin"] = mode.plugin;
    py::list columns;
    for (const XmlColumnSpec& column : mode.columns) {
        py::dict row;
        row["key"] = column.key;
        row["heading"] = column.heading;
        row["width"] = column.width;
        columns.append(row);
    }
    out["columns"] = columns;
    py::list filters;
    for (const XmlFilterSpec& filter : mode.filters) {
        py::dict row;
        row["label"] = filter.label;
        row["row_kinds"] = strings_to_python(filter.row_kinds);
        row["differ_types"] = strings_to_python(filter.differ_types);
        filters.append(row);
    }
    out["filters"] = filters;
    return out;
}

py::dict xml_result_to_python(const XmlCompareResult& result) {
    py::dict out;
    out["rel"] = result.rel;
    out["mode"] = result.mode;
    out["consistent"] = result.consistent;
    out["only_one_side"] = result.only_one_side;
    out["only_side"] = result.only_side;
    out["lines_a"] = result.lines_a;
    out["lines_b"] = result.lines_b;
    out["ids_a"] = result.ids_a;
    out["ids_b"] = result.ids_b;
    out["only_a"] = strings_to_python(result.only_a);
    out["only_b"] = strings_to_python(result.only_b);
    const auto rows_to_python = [](const std::vector<std::vector<std::string>>& rows) {
        py::list list;
        for (const std::vector<std::string>& row : rows) list.append(strings_to_python(row));
        return list;
    };
    out["count_diff"] = rows_to_python(result.count_diff);
    out["text_diff"] = rows_to_python(result.text_diff);
    out["untrusted_a"] = result.untrusted_a;
    out["untrusted_b"] = result.untrusted_b;
    out["warning"] = result.warning;
    out["encoding_a"] = result.encoding_a;
    out["encoding_b"] = result.encoding_b;
    out["error"] = result.error;
    return out;
}

XmlCompareResult xml_result_from_python(const py::object& result) {
    XmlCompareResult out;
    if (result.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(result);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("rel", out.rel);
    take_string("mode", out.mode);
    take_string("only_side", out.only_side);
    take_string("warning", out.warning);
    take_string("encoding_a", out.encoding_a);
    take_string("encoding_b", out.encoding_b);
    take_string("error", out.error);
    if (data.contains("consistent")) out.consistent = py::cast<bool>(data["consistent"]);
    if (data.contains("only_one_side")) out.only_one_side = py::cast<bool>(data["only_one_side"]);
    if (data.contains("untrusted_a")) out.untrusted_a = py::cast<bool>(data["untrusted_a"]);
    if (data.contains("untrusted_b")) out.untrusted_b = py::cast<bool>(data["untrusted_b"]);
    if (data.contains("lines_a")) out.lines_a = py::cast<uint64_t>(data["lines_a"]);
    if (data.contains("lines_b")) out.lines_b = py::cast<uint64_t>(data["lines_b"]);
    if (data.contains("ids_a")) out.ids_a = py::cast<uint64_t>(data["ids_a"]);
    if (data.contains("ids_b")) out.ids_b = py::cast<uint64_t>(data["ids_b"]);
    if (data.contains("only_a")) {
        for (auto item : py::list(data["only_a"])) {
            out.only_a.push_back(py::str(item).cast<std::string>());
        }
    }
    if (data.contains("only_b")) {
        for (auto item : py::list(data["only_b"])) {
            out.only_b.push_back(py::str(item).cast<std::string>());
        }
    }
    const auto rows_from_python = [&data](const char* key,
                                          std::vector<std::vector<std::string>>& target) {
        if (!data.contains(key)) return;
        for (auto row : py::list(data[key])) {
            std::vector<std::string> cells;
            // ★ `row` 是 `py::handle`，得显式借成 list（同文件别处就是这么写的）
            for (auto cell : py::reinterpret_borrow<py::list>(row)) {
                cells.push_back(py::str(cell).cast<std::string>());
            }
            target.push_back(std::move(cells));
        }
    };
    rows_from_python("count_diff", out.count_diff);
    rows_from_python("text_diff", out.text_diff);
    return out;
}

XmlCsvReport xml_csv_report_from_python(const py::object& report) {
    XmlCsvReport out;
    if (report.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(report);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("mode_name", out.mode_name);
    take_string("folder_a", out.folder_a);
    take_string("folder_b", out.folder_b);
    take_string("name_a", out.name_a);
    take_string("name_b", out.name_b);
    take_string("enc_a", out.enc_a);
    take_string("enc_b", out.enc_b);
    take_string("filter_label", out.filter_label);
    take_string("generated_at", out.generated_at);
    take_string("encodings_used", out.encodings_used);
    if (data.contains("columns")) {
        for (auto item : py::list(data["columns"])) {
            out.columns.push_back(py::str(item).cast<std::string>());
        }
    }
    if (data.contains("rows")) {
        for (auto row : py::list(data["rows"])) {
            std::vector<std::string> cells;
            for (auto cell : py::reinterpret_borrow<py::list>(row)) {
                cells.push_back(py::str(cell).cast<std::string>());
            }
            out.rows.push_back(std::move(cells));
        }
    }
    const auto take_u64 = [&data](const char* key, uint64_t& target) {
        if (data.contains(key)) target = py::cast<uint64_t>(data[key]);
    };
    take_u64("consistent", out.consistent);
    take_u64("inconsistent", out.inconsistent);
    take_u64("only_a", out.only_a);
    take_u64("only_b", out.only_b);
    return out;
}


// ── ★★ P12b：宿主服务「跑进程」的入参转换（dict → 门面 POD）──────────────────
// ★★ P12b：子进程的字节流跨到 Python 时**一律"带替换地"解 UTF-8** ——
//   真 ffmpeg 的 stderr 里会出现本地编码的路径，直接 `py::str(std::string)` 会抛
//   UnicodeDecodeError 把宿主掀翻（判据现场就是这么抓到的）。
py::str python_text_from_bytes(const std::string& text) {
    return py::str(PyUnicode_DecodeUTF8(text.data(), static_cast<Py_ssize_t>(text.size()),
                                        "replace"));
}

ProcessRunOptions process_options_from_python(const py::object& options) {
    ProcessRunOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    if (data.contains("program")) out.program = py::str(data["program"]).cast<std::string>();
    if (data.contains("working_dir")) {
        out.working_dir = py::str(data["working_dir"]).cast<std::string>();
    }
    if (data.contains("args")) {
        for (auto item : py::list(data["args"])) {
            out.args.push_back(py::str(item).cast<std::string>());
        }
    }
    if (data.contains("stderr_tail_bytes")) {
        out.stderr_tail_bytes = py::cast<std::size_t>(data["stderr_tail_bytes"]);
    }
    if (data.contains("cancel_wait_ms")) {
        out.cancel_wait_ms = py::cast<int>(data["cancel_wait_ms"]);
    }
    const auto hold = [&data](const char* key) -> py::object {
        if (!data.contains(key) || data[key].is_none() != 0) return py::none();
        py::object kept = data[key];
        keepalive()->push_back(kept);
        return kept;
    };
    py::object stop = hold("should_stop");
    if (stop.is_none() == 0) {
        out.should_stop = [stop]() -> bool {
            py::gil_scoped_acquire acquire;
            try {
                return py::cast<bool>(stop());
            } catch (const py::error_already_set&) {
                return false;   // 回调炸了 ⇒ 别把子进程无故杀掉
            }
        };
    }
    py::object on_stdout = hold("on_stdout");
    if (on_stdout.is_none() == 0) {
        out.on_stdout = [on_stdout](const std::string& line) {
            py::gil_scoped_acquire acquire;
            try {
                on_stdout(python_text_from_bytes(line));
            } catch (const py::error_already_set&) {
            }
        };
    }
    py::object on_stderr = hold("on_stderr");
    if (on_stderr.is_none() == 0) {
        out.on_stderr = [on_stderr](const std::string& line) {
            py::gil_scoped_acquire acquire;
            try {
                on_stderr(python_text_from_bytes(line));
            } catch (const py::error_already_set&) {
            }
        };
    }
    return out;
}

XmlDirDiffOptions xml_dir_options_from_python(const py::object& options) {
    XmlDirDiffOptions out;
    if (options.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(options);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("dir_a", out.dir_a);
    take_string("dir_b", out.dir_b);
    take_string("enc_a", out.enc_a);
    take_string("enc_b", out.enc_b);
    take_string("mode_key", out.mode_key);
    if (data.contains("cancel") && data["cancel"].is_none() == 0) {
        py::object cancel = data["cancel"];
        keepalive()->push_back(cancel);
        out.should_stop = [cancel]() -> bool {
            py::gil_scoped_acquire acquire;
            try {
                return py::cast<bool>(cancel());
            } catch (const py::error_already_set&) {
                return false;
            }
        };
    }
    return out;
}

XmlFilterSpec xml_filter_from_python(const py::object& filter) {
    XmlFilterSpec out;
    if (filter.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(filter);
    if (data.contains("label")) out.label = py::str(data["label"]).cast<std::string>();
    if (data.contains("row_kinds")) {
        for (auto item : py::list(data["row_kinds"])) {
            out.row_kinds.push_back(py::str(item).cast<std::string>());
        }
    }
    if (data.contains("differ_types")) {
        for (auto item : py::list(data["differ_types"])) {
            out.differ_types.push_back(py::str(item).cast<std::string>());
        }
    }
    return out;
}


// ── ★★ P12a：video 页那一组的入参/出参转换 ───────────────────────────────────
VideoMediaInfo video_media_from_python(const py::object& info) {
    VideoMediaInfo out;
    if (info.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(info);
    const auto take_bool = [&data](const char* key, bool& target) {
        if (data.contains(key)) target = py::cast<bool>(data[key]);
    };
    take_bool("parsed", out.parsed);
    take_bool("has_video", out.has_video);
    take_bool("has_audio", out.has_audio);
    const auto take_double = [&data](const char* key, double& target) {
        if (data.contains(key)) target = py::cast<double>(data[key]);
    };
    take_double("duration", out.duration);
    take_double("size_mb", out.size_mb);
    if (data.contains("video")) {
        py::dict video = py::reinterpret_borrow<py::dict>(data["video"]);
        if (video.contains("codec")) out.video.codec = py::str(video["codec"]).cast<std::string>();
        if (video.contains("width")) out.video.width = py::cast<int>(video["width"]);
        if (video.contains("height")) out.video.height = py::cast<int>(video["height"]);
        if (video.contains("fps")) out.video.fps = py::cast<double>(video["fps"]);
        if (video.contains("bitrate")) out.video.bitrate = py::cast<long long>(video["bitrate"]);
    }
    if (data.contains("audio")) {
        py::dict audio = py::reinterpret_borrow<py::dict>(data["audio"]);
        if (audio.contains("codec")) out.audio.codec = py::str(audio["codec"]).cast<std::string>();
        if (audio.contains("sample_rate")) out.audio.sample_rate = py::cast<int>(audio["sample_rate"]);
        if (audio.contains("channels")) out.audio.channels = py::cast<int>(audio["channels"]);
        if (audio.contains("bitrate")) out.audio.bitrate = py::cast<long long>(audio["bitrate"]);
    }
    return out;
}

py::dict video_media_to_python(const VideoMediaInfo& info) {
    py::dict out;
    out["parsed"] = info.parsed;
    out["has_video"] = info.has_video;
    out["has_audio"] = info.has_audio;
    out["duration"] = info.duration;
    out["size_mb"] = info.size_mb;
    py::dict video;
    video["codec"] = info.video.codec;
    video["width"] = info.video.width;
    video["height"] = info.video.height;
    video["fps"] = info.video.fps;
    video["bitrate"] = info.video.bitrate;
    out["video"] = video;
    py::dict audio;
    audio["codec"] = info.audio.codec;
    audio["sample_rate"] = info.audio.sample_rate;
    audio["channels"] = info.audio.channels;
    audio["bitrate"] = info.audio.bitrate;
    out["audio"] = audio;
    return out;
}

VideoRequest video_request_from_python(const py::object& request) {
    VideoRequest out;
    if (request.is_none() != 0) return out;
    py::dict data = py::reinterpret_borrow<py::dict>(request);
    const auto take_string = [&data](const char* key, std::string& target) {
        if (data.contains(key)) target = py::str(data[key]).cast<std::string>();
    };
    take_string("ffmpeg", out.ffmpeg);
    take_string("input", out.input);
    take_string("output", out.output);
    take_string("reference_path", out.reference_path);
    if (data.contains("has_reference")) out.has_reference = py::cast<bool>(data["has_reference"]);
    if (data.contains("reference")) out.reference = video_media_from_python(data["reference"]);
    if (data.contains("source")) out.source = video_media_from_python(data["source"]);
    if (data.contains("available_encoders")) {
        for (auto item : py::list(data["available_encoders"])) {
            out.available_encoders.push_back(py::str(item).cast<std::string>());
        }
    }
    return out;
}

// ── ★★ P14-0a：fs 面第一批的入参/出参转换 ─────────────────────────────────
py::list fs_entries_to_python(const std::vector<FsEntry>& rows) {
    py::list out;
    for (const FsEntry& row : rows) {
        py::dict item;
        item["path"] = row.path;
        item["is_dir"] = row.is_dir;
        item["size"] = row.size;
        out.append(item);
    }
    return out;
}

std::vector<FsEntry> fs_entries_from_python(const py::object& rows) {
    std::vector<FsEntry> out;
    if (rows.is_none() != 0) return out;
    for (auto item : py::list(rows)) {
        py::dict row = py::reinterpret_borrow<py::dict>(item);
        FsEntry entry;
        if (row.contains("path")) entry.path = py::str(row["path"]).cast<std::string>();
        if (row.contains("is_dir")) entry.is_dir = py::cast<bool>(row["is_dir"]);
        if (row.contains("size")) entry.size = py::cast<uint64_t>(row["size"]);
        out.push_back(std::move(entry));
    }
    return out;
}


// ── ★★ P14-0b：fs 面第二批（合并树）的转换 ────────────────────────────────
std::vector<FsTreeSource> fs_tree_sources_from_python(const py::object& sources) {
    std::vector<FsTreeSource> out;
    if (sources.is_none() != 0) return out;
    for (auto item : py::list(sources)) {
        py::dict row = py::reinterpret_borrow<py::dict>(item);
        FsTreeSource one;
        if (row.contains("source")) one.source = py::str(row["source"]).cast<std::string>();
        if (row.contains("entries")) one.entries = fs_entries_from_python(row["entries"]);
        out.push_back(std::move(one));
    }
    return out;
}

std::vector<std::string> fs_strings_from_python(const py::object& values) {
    std::vector<std::string> out;
    if (values.is_none() != 0) return out;
    for (auto item : py::list(values)) out.push_back(py::str(item).cast<std::string>());
    return out;
}


std::vector<FsPackItem> fs_pack_items_from_python(const py::object& items) {
    std::vector<FsPackItem> out;
    if (items.is_none() != 0) return out;
    for (auto item : py::list(items)) {
        py::dict row = py::reinterpret_borrow<py::dict>(item);
        FsPackItem one;
        if (row.contains("path")) one.path = py::str(row["path"]).cast<std::string>();
        if (row.contains("is_dir")) one.is_dir = py::cast<bool>(row["is_dir"]);
        if (row.contains("source_path")) {
            one.source_path = py::str(row["source_path"]).cast<std::string>();
        }
        out.push_back(std::move(one));
    }
    return out;
}

std::vector<PkgEntry> pkg_entries_from_python(const py::object& entries) {
    std::vector<PkgEntry> out;
    if (entries.is_none() != 0) return out;
    for (auto item : py::list(entries)) {
        py::dict row = py::reinterpret_borrow<py::dict>(item);
        PkgEntry one;
        if (row.contains("rel")) one.rel = py::str(row["rel"]).cast<std::string>();
        if (row.contains("source_path")) {
            one.source_path = py::str(row["source_path"]).cast<std::string>();
        }
        if (row.contains("sha256")) one.sha256 = py::str(row["sha256"]).cast<std::string>();
        out.push_back(std::move(one));
    }
    return out;
}

// ── ★★ P16-1c：二进制补丁的**站点表**（Python list[dict] → C++ 值类型）──────────
//   口径（方案 (b)）：表由插件给，引擎只做通用机器。二进制字段一律 **hex 文本**
//   （pybind 的 `std::string` 走 UTF-8，直接传裸字节会被改坏）。
std::vector<xrkit::engine::patch::PatchSite> patch_sites_from_python(const py::object& sites) {
    std::vector<xrkit::engine::patch::PatchSite> out;
    if (sites.is_none() != 0) return out;
    for (auto item : py::list(sites)) {
        py::dict row = py::reinterpret_borrow<py::dict>(item);
        xrkit::engine::patch::PatchSite one;
        auto take = [&](const char* key, std::string& dst) {
            if (row.contains(key)) dst = py::str(row[key]).cast<std::string>();
        };
        take("id", one.id);
        take("kind", one.kind);
        if (row.contains("offset")) one.offset = row["offset"].cast<uint64_t>();
        take("expect_hex", one.expect_hex);
        take("patched_hex", one.patched_hex);
        take("before_hex", one.before_hex);
        take("after_hex", one.after_hex);
        take("helper", one.helper);
        take("tail_hex", one.tail_hex);
        take("note", one.note);
        if (row.contains("layout_delta")) one.layout_delta = row["layout_delta"].cast<uint64_t>();
        take("layout_section", one.layout_section);
        out.push_back(std::move(one));
    }
    return out;
}

// ── ★★ P7a：引擎调用门面在 Python 侧的形态（`api.engine.*`）──────────────────
// 口径（用户拍板的方案 3）：
//   · **值类型 Python 友好**：出入都是 dict / list / str / int，**不漏 C++ 结构体与指针**；
//   · ★**允许在工作线程上调用**（与 `api.ui.*` 相反）：引擎重活本来就该跑在任务壳的 worker
//     线程里（六页都是这么干的）⇒ 这里**不做** UI 线程检查；
//   · 门面没装（CLI/无外壳）⇒ **逐条记账**并返回 None/dict 空壳（零静默）。
class PyEngine {
public:
    explicit PyEngine(std::string owner) : owner_(std::move(owner)) {}

    py::object font_check(const std::string& font_path) const {
        const EngineFacade* facade = available("font_check");
        if (facade == nullptr) return py::none();
        const FontCheckResult result = facade->font_check(font_path);
        py::dict out;
        out["state"] = result.state;
        out["mark"] = result.mark;
        out["missing"] = strings_to_python(result.missing);
        return out;
    }
    py::object font_pack(py::object options) const {
        const EngineFacade* facade = available("font_pack");
        if (facade == nullptr) return py::none();
        const FontPackResult result = facade->font_pack(font_options_from_python(options));
        py::dict out;
        out["ok"] = result.ok;
        out["error"] = result.error;
        out["out_gamedata"] = result.out_gamedata;
        out["fonts_written"] = result.fonts_written;
        out["xml_written"] = result.xml_written;
        out["warnings"] = strings_to_python(result.warnings);
        return out;
    }
    py::object font_state_marks() const {
        const EngineFacade* facade = available("font_state_marks");
        if (facade == nullptr) return py::none();
        const FontStateMarks marks = facade->font_state_marks();
        py::dict out;
        out["ok"] = marks.ok;
        out["japanese"] = marks.japanese;
        out["missing"] = marks.missing;
        out["unknown"] = marks.unknown;
        return out;
    }
    py::object font_size_slots() const {
        const EngineFacade* facade = available("font_size_slots");
        if (facade == nullptr) return py::none();
        py::list out;
        for (int size : facade->font_size_slots()) out.append(py::int_(size));
        return out;
    }

    // ── ★★ P9a：convert 页那一组 ─────────────────────────────────────────────
    py::object text_encodings() const {
        const EngineFacade* facade = available("text_encodings");
        if (facade == nullptr) return py::none();
        const TextEncodings table = facade->text_encodings();
        py::dict out;
        out["manual"] = strings_to_python(table.manual);
        out["target_only"] = strings_to_python(table.target_only);
        return out;
    }
    py::object text_convert_batch(py::object options, py::object hooks) const {
        const EngineFacade* facade = available("text_convert_batch");
        if (facade == nullptr) return py::none();
        const TextConvertOutcome result =
            facade->text_convert_batch(text_batch_options_from_python(options),
                                       text_hooks_from_python(hooks));
        py::dict out;
        out["depth"] = result.depth;
        out["dirs"] = result.dirs;
        out["files_seen"] = result.files_seen;
        out["selected"] = result.selected;
        out["skipped_ext"] = result.skipped_ext;
        out["skipped_excluded"] = result.skipped_excluded;
        out["skipped_backup"] = result.skipped_backup;
        out["skipped_depth"] = result.skipped_depth;
        out["excluded"] = strings_to_python(result.excluded);
        out["collect_errors"] = strings_to_python(result.collect_errors);
        out["single_file"] = result.single_file;
        out["candidates"] = result.candidates;
        out["processed"] = result.processed;
        out["success"] = result.success;
        out["skipped"] = result.skipped;
        out["failed"] = result.failed;
        out["backed_up"] = result.backed_up;
        out["backup_existed"] = result.backup_existed;
        out["backup_failed"] = result.backup_failed;
        out["low_confidence"] = result.low_confidence;
        out["override_decl"] = result.override_decl;
        out["used_mismatch"] = result.used_mismatch;
        out["cancelled"] = result.cancelled;
        out["stop_reason"] = result.stop_reason;
        out["duration"] = result.duration;
        out["exit_code"] = result.exit_code;
        return out;
    }
    py::object text_restore_backups(py::object options, py::object hooks) const {
        return text_backup_call("text_restore_backups", options, hooks);
    }
    py::object text_clear_backups(py::object options, py::object hooks) const {
        return text_backup_call("text_clear_backups", options, hooks);
    }
    py::object text_write_stats_csv(const std::string& path, py::object stats) const {
        const EngineFacade* facade = available("text_write_stats_csv");
        if (facade == nullptr) return py::none();
        const std::string error = facade->text_write_stats_csv(path, text_stats_from_python(stats));
        py::dict out;
        out["ok"] = error.empty();
        out["error"] = error;
        return out;
    }
    py::object text_format_duration(double seconds) const {
        const EngineFacade* facade = available("text_format_duration");
        if (facade == nullptr) return py::none();
        return py::str(facade->text_format_duration(seconds));
    }

    // ── ★★ P10a：text 页那一组（文本提取/改写）──────────────────────────────
    py::object text_default_id_prefix() const {
        const EngineFacade* facade = available("text_default_id_prefix");
        if (facade == nullptr) return py::none();
        return py::str(facade->text_default_id_prefix());
    }
    py::object text_validate_extract(py::object request) const {
        const EngineFacade* facade = available("text_validate_extract");
        if (facade == nullptr) return py::none();
        // 空串 = 通过（引擎口径）；原样交回，页面/插件自己决定怎么显示
        return py::str(facade->text_validate_extract(
            text_extract_request_from_python(request)));
    }
    py::object text_extract_kind(const std::string& dir) const {
        const EngineFacade* facade = available("text_extract_kind");
        if (facade == nullptr) return py::none();
        const TextExtractKindResult result = facade->text_extract_kind(dir);
        py::dict out;
        out["ok"] = result.ok;
        out["kind"] = result.kind;
        out["error"] = result.error;
        return out;
    }
    py::object text_extract_dir(const std::string& source_dir, const std::string& target_dir,
                                const std::string& xml_out_dir, py::object options) const {
        const EngineFacade* facade = available("text_extract_dir");
        if (facade == nullptr) return py::none();
        const TextExtractStats stats = facade->text_extract_dir(
            source_dir, target_dir, xml_out_dir, text_extract_options_from_python(options));
        py::dict out;
        out["ok"] = stats.ok;
        out["error"] = stats.error;
        out["kind"] = stats.kind;
        out["files"] = stats.files;
        out["strings"] = stats.strings;
        out["chars"] = stats.chars;
        out["skipped"] = strings_to_python(stats.skipped);
        out["repl_skipped"] = strings_to_python(stats.repl_skipped);
        out["write_failures"] = stats.write_failures;
        out["texts_xml_name"] = stats.texts_xml_name;
        out["elapsed_ms"] = stats.elapsed_ms;
        out["cancelled"] = stats.cancelled;
        return out;
    }

    // ── ★★ P11a：xml 页那一组 ───────────────────────────────────────────────
    py::object xml_modes() const {
        const EngineFacade* facade = available("xml_modes");
        if (facade == nullptr) return py::none();
        py::list out;
        for (const XmlMode& mode : facade->xml_modes()) out.append(xml_mode_to_python(mode));
        return out;
    }
    py::object xml_find_mode(const std::string& key_or_name) const {
        const EngineFacade* facade = available("xml_find_mode");
        if (facade == nullptr) return py::none();
        const XmlModeLookup lookup = facade->xml_find_mode(key_or_name);
        py::dict out;
        out["found"] = lookup.found;
        out["mode"] = xml_mode_to_python(lookup.mode);
        return out;
    }
    py::object text_read_bytes(const std::string& path, const std::string& encoding) const {
        const EngineFacade* facade = available("text_read_bytes");
        if (facade == nullptr) return py::none();
        const XmlTextRead read = facade->text_read_bytes(path, encoding);
        py::dict out;
        out["ok"] = read.ok;
        out["text"] = read.text;
        out["used_encoding"] = read.used_encoding;
        out["error"] = read.error;
        out["forced"] = read.forced;
        out["used_mismatch"] = read.used_mismatch;
        return out;
    }
    py::object xml_compare_stats(const std::string& rel, const std::string& text_a,
                                 const std::string& text_b, const std::string& enc_a,
                                 const std::string& enc_b) const {
        const EngineFacade* facade = available("xml_compare_stats");
        if (facade == nullptr) return py::none();
        return xml_result_to_python(
            facade->xml_compare_stats(rel, text_a, text_b, enc_a, enc_b));
    }
    py::object xml_compare_text(const std::string& rel, const std::string& text_a,
                                const std::string& text_b, const std::string& enc_a,
                                const std::string& enc_b) const {
        const EngineFacade* facade = available("xml_compare_text");
        if (facade == nullptr) return py::none();
        return xml_result_to_python(facade->xml_compare_text(rel, text_a, text_b, enc_a, enc_b));
    }
    py::object xml_only_one_side(const std::string& rel, const std::string& side) const {
        const EngineFacade* facade = available("xml_only_one_side");
        if (facade == nullptr) return py::none();
        return xml_result_to_python(facade->xml_only_one_side(rel, side));
    }
    py::object xml_inline_detail(py::object result, const std::string& name_a,
                                 const std::string& name_b) const {
        const EngineFacade* facade = available("xml_inline_detail");
        if (facade == nullptr) return py::none();
        return py::str(facade->xml_inline_detail(xml_result_from_python(result), name_a, name_b));
    }
    py::object xml_full_detail(py::object result, const std::string& name_a,
                               const std::string& name_b) const {
        const EngineFacade* facade = available("xml_full_detail");
        if (facade == nullptr) return py::none();
        return py::str(facade->xml_full_detail(xml_result_from_python(result), name_a, name_b));
    }
    py::object xml_full_text_detail(py::object result, const std::string& name_a,
                                    const std::string& name_b) const {
        const EngineFacade* facade = available("xml_full_text_detail");
        if (facade == nullptr) return py::none();
        return py::str(
            facade->xml_full_text_detail(xml_result_from_python(result), name_a, name_b));
    }
    py::object xml_render_csv(py::object report) const {
        const EngineFacade* facade = available("xml_render_csv");
        if (facade == nullptr) return py::none();
        return py::str(facade->xml_render_csv(xml_csv_report_from_python(report)));
    }
    py::object xml_write_csv(const std::string& path, py::object report) const {
        const EngineFacade* facade = available("xml_write_csv");
        if (facade == nullptr) return py::none();
        const std::string error =
            facade->xml_write_csv(path, xml_csv_report_from_python(report));
        py::dict out;
        out["ok"] = error.empty();
        out["error"] = error;
        return out;
    }

    // ── ★★ P11b：目录对比 + 筛选命中 ────────────────────────────────────────
    py::object xml_compare_dirs(py::object options) const {
        const EngineFacade* facade = available("xml_compare_dirs");
        if (facade == nullptr) return py::none();
        const XmlDirDiffReport report =
            facade->xml_compare_dirs(xml_dir_options_from_python(options));
        py::dict out;
        out["total"] = report.total;
        out["cancelled"] = report.cancelled;
        // ★★ P17-10（02 F-05）：**遍历错误也要交给插件**（`DirDiffReport::errors`）——
        //   插件页的"结论不可信"告警与 headless 的 `errors=N` 收尾行都读它；
        //   修前这里只透 total/cancelled/rows ⇒ 插件侧永远看不到"某侧目录没读全"。
        py::list errors;
        for (const std::string& e : report.errors) errors.append(py::str(e));
        out["errors"] = errors;
        out["listings_ok"] = report.errors.empty();
        py::list rows;
        for (const XmlDirDiffRow& row : report.rows) {
            py::dict item = xml_result_to_python(row.result);
            item["kind"] = row.kind;
            rows.append(item);
        }
        out["rows"] = rows;
        return out;
    }
    bool xml_row_matches(py::object filter, const std::string& kind, py::object result) const {
        const EngineFacade* facade = available("xml_row_matches");
        if (facade == nullptr) return true;
        return facade->xml_row_matches(xml_filter_from_python(filter), kind,
                                       xml_result_from_python(result));
    }

    // ── ★★ P12a：video 页那一组 ─────────────────────────────────────────────
    py::object video_find_ffmpeg(const std::string& app_dir, const std::string& ffmpeg_path,
                                 const std::string& ffprobe_path) const {
        const EngineFacade* facade = available("video_find_ffmpeg");
        if (facade == nullptr) return py::none();
        const VideoPaths paths = facade->video_find_ffmpeg(app_dir, ffmpeg_path, ffprobe_path);
        py::dict out;
        out["found"] = paths.found;
        out["has_probe"] = paths.has_probe;
        out["ffmpeg"] = paths.ffmpeg;
        out["ffprobe"] = paths.ffprobe;
        out["ffmpeg_from_config"] = paths.ffmpeg_from_config;
        out["ffprobe_from_config"] = paths.ffprobe_from_config;
        return out;
    }
    int video_major_version(const std::string& version_output) const {
        const EngineFacade* facade = available("video_major_version");
        if (facade == nullptr) return 0;
        return facade->video_major_version(version_output);
    }
    bool video_version_rejected(const std::string& version_output) const {
        const EngineFacade* facade = available("video_version_rejected");
        if (facade == nullptr) return false;
        return facade->video_version_rejected(version_output);
    }
    py::object video_plan_transcode(py::object request) const {
        const EngineFacade* facade = available("video_plan_transcode");
        if (facade == nullptr) return py::none();
        const VideoPlan plan = facade->video_plan_transcode(video_request_from_python(request));
        py::dict out;
        out["argv"] = strings_to_python(plan.argv);
        out["video_encoder"] = plan.video_encoder;
        out["encoder_warning"] = plan.encoder_warning;
        out["audio_kept_on_probe_failure"] = plan.audio_kept_on_probe_failure;
        out["duration"] = plan.duration;
        out["scale_applied"] = plan.scale_applied;
        out["fps_applied"] = plan.fps_applied;
        out["video_bitrate"] = plan.video_bitrate;
        out["audio_bitrate"] = plan.audio_bitrate;
        out["audio_sample_rate"] = plan.audio_sample_rate;
        out["audio_channels"] = plan.audio_channels;
        out["audio_disabled"] = plan.audio_disabled;
        return out;
    }
    py::object video_check_output_conflict(py::object request) const {
        const EngineFacade* facade = available("video_check_output_conflict");
        if (facade == nullptr) return py::none();
        const VideoOutputCheck check =
            facade->video_check_output_conflict(video_request_from_python(request));
        py::dict out;
        out["conflict"] = check.conflict;
        out["reason"] = check.reason;
        return out;
    }
    py::object video_parse_ffprobe(const std::string& json) const {
        const EngineFacade* facade = available("video_parse_ffprobe");
        if (facade == nullptr) return py::none();
        return video_media_to_python(facade->video_parse_ffprobe(json));
    }
    py::object video_parse_encoders(const std::string& output) const {
        const EngineFacade* facade = available("video_parse_encoders");
        if (facade == nullptr) return py::none();
        return strings_to_python(facade->video_parse_encoders(output));
    }
    py::object video_progress_from_line(const std::string& line, double duration_sec) const {
        const EngineFacade* facade = available("video_progress_from_line");
        if (facade == nullptr) return py::none();
        const VideoProgress sample = facade->video_progress_from_line(line, duration_sec);
        py::dict out;
        out["matched"] = sample.matched;
        out["percent"] = sample.percent;
        return out;
    }

    // ── ★★ P14-0a：fs 面第一批 ─────────────────────────────────────────────
    py::object fs_formats() const {
        const EngineFacade* facade = available("fs_formats");
        if (facade == nullptr) return py::none();
        py::list out;
        for (const FsFormatInfo& item : facade->fs_formats()) {
            py::dict row;
            row["key"] = item.key;
            row["label"] = item.label;
            row["is_sqfs"] = item.is_sqfs;
            row["is_auto"] = item.is_auto;
            out.append(row);
        }
        return out;
    }
    py::object fs_format_to_db(const std::string& key) const {
        const EngineFacade* facade = available("fs_format_to_db");
        if (facade == nullptr) return py::none();
        const FsDbLookup native = facade->fs_format_to_db(key);
        py::dict out;
        out["is_db"] = native.is_db;
        out["db_key"] = native.db_key;
        out["db_name"] = native.db_name;
        return out;
    }
    bool fs_format_is_sqfs(const std::string& key) const {
        const EngineFacade* facade = available("fs_format_is_sqfs");
        return facade != nullptr && facade->fs_format_is_sqfs(key);
    }
    bool fs_format_is_auto(const std::string& key) const {
        const EngineFacade* facade = available("fs_format_is_auto");
        return facade != nullptr && facade->fs_format_is_auto(key);
    }
    bool fs_is_package_name(const std::string& path) const {
        const EngineFacade* facade = available("fs_is_package_name");
        return facade != nullptr && facade->fs_is_package_name(path);
    }
    std::string fs_pack_ext_for(const std::string& key) const {
        const EngineFacade* facade = available("fs_pack_ext_for");
        return facade == nullptr ? std::string() : facade->fs_pack_ext_for(key);
    }
    std::string fs_package_folder_name(const std::string& path) const {
        const EngineFacade* facade = available("fs_package_folder_name");
        return facade == nullptr ? std::string() : facade->fs_package_folder_name(path);
    }
    std::string fs_resolve_output_root(const std::string& front, const std::string& current) const {
        const EngineFacade* facade = available("fs_resolve_output_root");
        return facade == nullptr ? std::string() : facade->fs_resolve_output_root(front, current);
    }
    std::string fs_unpack_target_dir(py::object out_root, py::object package_path,
                                     py::object entry_paths) const {
        const EngineFacade* facade = available("fs_unpack_target_dir");
        if (facade == nullptr) return std::string();
        std::vector<std::string> paths;
        if (entry_paths.is_none() == 0) {
            for (auto item : py::list(entry_paths)) {
                paths.push_back(py::str(item).cast<std::string>());
            }
        }
        return facade->fs_unpack_target_dir(py::str(out_root).cast<std::string>(),
                                            py::str(package_path).cast<std::string>(), paths);
    }
    py::object fs_resolve_pack_target(const std::string& typed, const std::string& basename,
                                      const std::string& key) const {
        const EngineFacade* facade = available("fs_resolve_pack_target");
        if (facade == nullptr) return py::none();
        const FsPackTarget native = facade->fs_resolve_pack_target(typed, basename, key);
        py::dict out;
        out["path"] = native.path;
        out["needs_frontend_dialog"] = native.needs_frontend_dialog;
        return out;
    }
    py::object fs_safe_out_path(const std::string& out_dir, const std::string& rel) const {
        const EngineFacade* facade = available("fs_safe_out_path");
        if (facade == nullptr) return py::none();
        const FsOutPath native = facade->fs_safe_out_path(out_dir, rel);
        py::dict out;
        out["ok"] = native.ok;
        out["reason"] = native.reason;
        out["path"] = native.path;
        return out;
    }
    py::object fs_plan_unpack_targets(py::object out_root, py::object sources) const {
        const EngineFacade* facade = available("fs_plan_unpack_targets");
        if (facade == nullptr) return py::none();
        std::vector<std::pair<std::string, std::vector<std::string>>> native;
        if (sources.is_none() == 0) {
            for (auto item : py::list(sources)) {
                py::dict row = py::reinterpret_borrow<py::dict>(item);
                std::string package = row.contains("package")
                                          ? py::str(row["package"]).cast<std::string>()
                                          : std::string();
                std::vector<std::string> entries;
                if (row.contains("entries")) {
                    for (auto one : py::list(row["entries"])) {
                        entries.push_back(py::str(one).cast<std::string>());
                    }
                }
                native.emplace_back(std::move(package), std::move(entries));
            }
        }
        py::list out;
        for (const FsUnpackTarget& item :
             facade->fs_plan_unpack_targets(py::str(out_root).cast<std::string>(), native)) {
            py::dict row;
            row["image"] = item.image;
            row["label"] = item.label;
            row["target"] = item.target;
            out.append(row);
        }
        return out;
    }
    py::object fs_preview_layout(const std::string& package_path, py::object entries) const {
        const EngineFacade* facade = available("fs_preview_layout");
        if (facade == nullptr) return py::none();
        return fs_entries_to_python(
            facade->fs_preview_layout(package_path, fs_entries_from_python(entries)));
    }

    // ── ★★ P14-0b：fs 面第二批（合并树；句柄式）────────────────────────────
    py::object fs_loose_entries(const std::string& root) const {
        const EngineFacade* facade = available("fs_loose_entries");
        if (facade == nullptr) return py::none();
        std::string error;
        const std::vector<FsEntry> entries = facade->fs_loose_entries(root, error);
        py::dict out;
        out["entries"] = fs_entries_to_python(entries);
        out["error"] = error;
        return out;
    }
    py::object fs_sort_sources_by_basename(py::object paths) const {
        const EngineFacade* facade = available("fs_sort_sources_by_basename");
        if (facade == nullptr) return py::none();
        py::list out;
        for (size_t index : facade->fs_sort_sources_by_basename(fs_strings_from_python(paths))) {
            out.append(index);
        }
        return out;
    }
    int fs_tree_build(py::object sources) const {
        const EngineFacade* facade = available("fs_tree_build");
        if (facade == nullptr) return 0;
        return facade->fs_tree_build(fs_tree_sources_from_python(sources));
    }
    void fs_tree_free(int handle) const {
        const EngineFacade* facade = available("fs_tree_free");
        if (facade != nullptr) facade->fs_tree_free(handle);
    }
    py::object fs_tree_sources(int handle) const {
        const EngineFacade* facade = available("fs_tree_sources");
        if (facade == nullptr) return py::none();
        return strings_to_python(facade->fs_tree_sources(handle));
    }
    py::object fs_tree_flatten(int handle) const {
        const EngineFacade* facade = available("fs_tree_flatten");
        if (facade == nullptr) return py::none();
        py::list out;
        for (const FsTreeRow& row : facade->fs_tree_flatten(handle)) {
            py::dict item;
            item["name"] = row.name;
            item["path"] = row.path;
            item["is_dir"] = row.is_dir;
            item["checked"] = row.checked;
            item["size"] = row.size;
            item["source_index"] = row.source_index;
            out.append(item);
        }
        return out;
    }
    void fs_tree_toggle_all(int handle, bool checked) const {
        const EngineFacade* facade = available("fs_tree_toggle_all");
        if (facade != nullptr) facade->fs_tree_toggle_all(handle, checked);
    }
    void fs_tree_toggle_dir(int handle, const std::string& path, bool checked) const {
        const EngineFacade* facade = available("fs_tree_toggle_dir");
        if (facade != nullptr) facade->fs_tree_toggle_dir(handle, path, checked);
    }
    int fs_tree_toggle_source(int handle, int source_index, bool checked) const {
        const EngineFacade* facade = available("fs_tree_toggle_source");
        return facade == nullptr ? 0 : facade->fs_tree_toggle_source(handle, source_index, checked);
    }
    py::object fs_tree_source_state(int handle, int source_index) const {
        const EngineFacade* facade = available("fs_tree_source_state");
        if (facade == nullptr) return py::none();
        const FsSourceState native = facade->fs_tree_source_state(handle, source_index);
        py::dict out;
        out["files"] = native.files;
        out["checked_files"] = native.checked_files;
        return out;
    }
    py::object fs_tree_checked_paths(int handle) const {
        const EngineFacade* facade = available("fs_tree_checked_paths");
        if (facade == nullptr) return py::none();
        return strings_to_python(facade->fs_tree_checked_paths(handle));
    }
    int fs_tree_apply_checked_paths(int handle, py::object paths) const {
        const EngineFacade* facade = available("fs_tree_apply_checked_paths");
        if (facade == nullptr) return 0;
        return facade->fs_tree_apply_checked_paths(handle, fs_strings_from_python(paths));
    }
    int fs_tree_conflict_count(int handle) const {
        const EngineFacade* facade = available("fs_tree_conflict_count");
        return facade == nullptr ? 0 : facade->fs_tree_conflict_count(handle);
    }
    int fs_tree_checked_file_count(int handle) const {
        const EngineFacade* facade = available("fs_tree_checked_file_count");
        return facade == nullptr ? 0 : facade->fs_tree_checked_file_count(handle);
    }
    py::object fs_tree_find(int handle, const std::string& path) const {
        const EngineFacade* facade = available("fs_tree_find");
        if (facade == nullptr) return py::none();
        const FsTreeLookup native = facade->fs_tree_find(handle, path);
        py::dict out;
        out["found"] = native.found;
        out["is_dir"] = native.is_dir;
        out["checked"] = native.checked;
        out["size"] = native.size;
        out["source_index"] = native.source_index;
        return out;
    }

    // ── ★★ P14-0c：fs 面第三批 ─────────────────────────────────────────────
    py::object fs_gate_path(const std::string& path) const {
        const EngineFacade* facade = available("fs_gate_path");
        if (facade == nullptr) return py::none();
        const FsGateResult native = facade->fs_gate_path(path);
        py::dict out;
        out["ok"] = native.ok;
        out["path"] = native.path;
        out["error"] = native.error;
        return out;
    }
    std::string fs_decrypt_output_dir() const {
        const EngineFacade* facade = available("fs_decrypt_output_dir");
        return facade == nullptr ? std::string() : facade->fs_decrypt_output_dir();
    }
    py::object fs_package_entries(const std::string& path, const std::string& db_fallback_key) const {
        const EngineFacade* facade = available("fs_package_entries");
        if (facade == nullptr) return py::none();
        std::string error;
        // ★★ P17-85（01 F-09 收尾）：把"这次是怎么读的"一并露给插件页
        //   （修前只有 entries/error ⇒ 插件页看不到"整包读 N 字节 + 理由"这种中间状态）。
        FsPackageInfo info;
        const std::vector<FsEntry> entries =
            facade->fs_package_entries(path, db_fallback_key, error, &info);
        py::dict out;
        out["entries"] = fs_entries_to_python(entries);
        out["error"] = error;
        out["used_seek"] = info.used_seek;
        out["header_bytes"] = info.header_bytes;
        out["whole_file_reason"] = info.whole_file_reason;
        return out;
    }
    py::object fs_collect_pack_files(const std::string& base, py::object roots) const {
        const EngineFacade* facade = available("fs_collect_pack_files");
        if (facade == nullptr) return py::none();
        const FsPackCollect native =
            facade->fs_collect_pack_files(base, fs_strings_from_python(roots));
        py::dict out;
        py::list items;
        for (const FsPackFile& item : native.items) {
            py::dict row;
            row["rel"] = item.rel;
            row["source_path"] = item.source_path;
            items.append(row);
        }
        out["items"] = items;
        out["skipped_outside_base"] = native.skipped_outside_base;
        out["skipped_package_files"] = native.skipped_package_files;
        return out;
    }
    py::object fs_extract_package(const std::string& image_path, const std::string& out_root,
                                  const std::string& selection, py::object only,
                                  const std::string& db_fallback_key) const {
        const EngineFacade* facade = available("fs_extract_package");
        if (facade == nullptr) return py::none();
        const FsExtractResult native = facade->fs_extract_package(
            image_path, out_root, selection, fs_strings_from_python(only), db_fallback_key);
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["files_written"] = native.files_written;
        out["dirs_created"] = native.dirs_created;
        out["bytes_written"] = native.bytes_written;
        out["duplicate_records"] = native.duplicate_records;
        out["skipped_dirs"] = native.skipped_dirs;
        out["overwritten_count"] = native.overwritten.size();
        out["written_count"] = native.written.size();
        out["failure_count"] = native.failures.size();
        out["failure_note"] = native.failure_note;
        out["elapsed_ms"] = native.elapsed_ms;
        py::list failures;
        for (const FsExtractFailure& item : native.failures) {
            py::dict row;
            row["path"] = item.path;
            row["reason"] = item.reason;
            failures.append(row);
        }
        out["failures"] = failures;
        return out;
    }

    // ── ★★ P14-0d：fs 面第四批（封包）──────────────────────────────────────
    py::object fs_pack_squashfs(const std::string& out_path, py::object items) const {
        const EngineFacade* facade = available("fs_pack_squashfs");
        if (facade == nullptr) return py::none();
        const FsPackResult native = facade->fs_pack_squashfs(out_path, fs_pack_items_from_python(items));
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["bytes_used"] = native.bytes_used;
        out["file_size"] = native.file_size;
        out["inode_count"] = native.inode_count;
        out["data_blocks"] = native.data_blocks;
        out["dedup_blocks"] = native.dedup_blocks;
        out["elapsed_ms"] = native.elapsed_ms;
        return out;
    }
    py::object fs_pack_db(const std::string& out_path, py::object items,
                          const std::string& format_key) const {
        const EngineFacade* facade = available("fs_pack_db");
        if (facade == nullptr) return py::none();
        const FsDbPackResult native =
            facade->fs_pack_db(out_path, fs_pack_items_from_python(items), format_key);
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["file_size"] = native.file_size;
        out["item_count"] = native.item_count;
        py::list rewrites;
        for (const FsNameRewrite& item : native.rewrites) {
            py::dict row;
            row["source"] = item.source;
            row["written"] = item.written;
            rewrites.append(row);
        }
        out["rewrites"] = rewrites;
        return out;
    }
    // ── ★★ P16-1a：统一产物模型（清单/安装/还原）──────────────────────────
    py::object pkg_write_manifest(const std::string& path, py::object entries) const {
        const EngineFacade* facade = available("pkg_write_manifest");
        if (facade == nullptr) return py::none();
        std::string error;
        const bool ok = facade->pkg_write_manifest(path, pkg_entries_from_python(entries), error);
        py::dict out;
        out["ok"] = ok;
        out["error"] = error;
        return out;
    }
    py::object pkg_install(const std::string& target_root, py::object entries,
                           const std::string& backup_root) const {
        const EngineFacade* facade = available("pkg_install");
        if (facade == nullptr) return py::none();
        const PkgInstallResult native =
            facade->pkg_install(target_root, pkg_entries_from_python(entries), backup_root);
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["backup_dir"] = native.backup_dir;
        out["installed"] = native.installed;
        out["overwritten"] = native.overwritten;
        out["failures"] = native.failures;
        return out;
    }
    py::object pkg_restore(const std::string& target_root, const std::string& backup_root,
                           py::object rels) const {
        const EngineFacade* facade = available("pkg_restore");
        if (facade == nullptr) return py::none();
        const PkgRestoreResult native =
            facade->pkg_restore(target_root, backup_root, fs_strings_from_python(rels));
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["restored"] = native.restored;
        out["removed"] = native.removed;
        out["missing"] = native.missing;
        out["failures"] = native.failures;
        return out;
    }

    // ── ★★ P16-1c：二进制补丁（判定半 / 写盘半）──────────────────────────────
    py::object patch_identify(const std::string& path, py::object sites) const {
        const EngineFacade* facade = available("patch_identify");
        if (facade == nullptr) return py::none();
        const xrkit::engine::patch::PatchIdentifyResult native =
            facade->patch_identify(path, patch_sites_from_python(sites));
        py::dict out;
        out["ok"] = native.ok;
        out["error"] = native.error;
        out["verdict_key"] = native.verdict_key;
        out["verdict_text"] = native.verdict_text;
        out["detail"] = native.detail;
        out["thunk_intact"] = native.thunk_intact;
        out["cave_all_zero"] = native.cave_all_zero;
        out["file_size"] = native.file_size;
        out["reference_diff_bytes"] = native.reference_diff_bytes;
        py::dict counts;
        counts["orig"] = native.n_orig;
        counts["patched"] = native.n_patched;
        counts["kept"] = native.n_kept;
        counts["broken"] = native.n_broken;
        counts["other"] = native.n_other;
        counts["context"] = native.n_context;
        counts["range"] = native.n_range;
        out["counts"] = counts;
        py::list items;
        for (const xrkit::engine::patch::PatchItemState& item : native.items) {
            py::dict row;
            row["id"] = item.id;
            row["kind"] = item.kind;
            row["offset"] = item.offset;
            row["state"] = item.state;
            row["expect_hex"] = item.expect_hex;
            row["patched_hex"] = item.patched_hex;
            row["actual_hex"] = item.actual_hex;
            row["context_problem"] = item.context_problem;
            row["call_target"] = item.call_target;
            items.append(row);
        }
        out["items"] = items;
        return out;
    }
    py::object patch_apply(const std::string& src, const std::string& out_path, py::object sites,
                           bool force) const {
        const EngineFacade* facade = available("patch_apply");
        if (facade == nullptr) return py::none();
        const xrkit::engine::patch::PatchApplyResult native =
            facade->patch_apply(src, out_path, patch_sites_from_python(sites), force);
        py::dict out;
        out["ok"] = native.ok;
        out["reason"] = native.reason;
        out["out_path"] = native.out_path;
        out["notes"] = native.notes;
        out["written_bytes"] = native.written_bytes;
        out["refused_input_is_output"] = native.refused_input_is_output;
        out["refused_output_exists"] = native.refused_output_exists;
        return out;
    }

private:

    // 备份两件共用（restore / clear 的门面签名一样）
    py::object text_backup_call(const char* method, py::object options, py::object hooks) const {
        const EngineFacade* facade = available(method);
        if (facade == nullptr) return py::none();
        const TextBackupOptions native = text_backup_options_from_python(options);
        const TextBackupOutcome result =
            std::string(method) == "text_restore_backups"
                ? facade->text_restore_backups(native, text_hooks_from_python(hooks))
                : facade->text_clear_backups(native, text_hooks_from_python(hooks));
        py::dict out;
        out["dir_missing"] = result.dir_missing;
        out["depth"] = result.depth;
        out["total"] = result.total;
        out["ok"] = result.ok;
        out["failed"] = result.failed;
        out["cancelled"] = result.cancelled;
        out["stop_reason"] = result.stop_reason;
        out["errors"] = strings_to_python(result.errors);
        py::list items;
        for (const TextBackupItem& item : result.items) {
            py::dict row;
            row["bak"] = item.bak;
            row["target"] = item.target;
            row["ok"] = item.ok;
            row["error"] = item.error;
            items.append(row);
        }
        out["items"] = items;
        return out;
    }
    const EngineFacade* available(const std::string& method) const {
        if (!engine_facade_has(method)) {
            std::string reason;
            engine_facade_available(&reason);
            core::log(core::Level::Warn,
                      "[" + owner_ + "] 引擎调用门面不可用（engine." + method + "）：" + reason);
            return nullptr;
        }
        return engine_facade();
    }

    std::string owner_;
};

// ── ★★ P7b：宿主服务门面在 Python 侧的形态（`api.host.*`）─────────────────────
// 样板页 fontpack 真的要用这三件（偏好读写 / exe 目录 / 系统字体目录）⇒ 由它反向定义。
// 口径：值类型都是 str / list；**不做线程检查**（偏好是几个字节的文件读写；并发写同一条
// 由插件自己避免 —— 这一条写在 `host_facade.h` 的口径里）。
class PyHost {
public:
    explicit PyHost(std::string owner) : owner_(std::move(owner)) {}

    std::string pref_get(const std::string& section, const std::string& key,
                         const std::string& fallback) const {
        if (const HostFacade* facade = available("pref_get")) {
            return facade->pref_get(section, key, fallback);
        }
        return fallback;
    }
    bool pref_set(const std::string& section, const std::string& key,
                  const std::string& value) const {
        if (const HostFacade* facade = available("pref_set")) {
            return facade->pref_set(section, key, value);
        }
        return false;
    }
    std::string app_dir() const {
        if (const HostFacade* facade = available("app_dir")) return facade->app_dir();
        return std::string();
    }
    py::list system_font_dirs() const {
        if (const HostFacade* facade = available("system_font_dirs")) {
            return strings_to_python(facade->system_font_dirs());
        }
        return py::list();
    }

    // ── ★★ P12b：跑外部进程（宿主服务；语义唯一一份在引擎 `process/run.cpp`）────
    py::object run_process(py::object options) const {
        const HostFacade* facade = available("run_process");
        if (facade == nullptr) return py::none();
        const ProcessRunResult result =
            facade->run_process(process_options_from_python(options));
        py::dict out;
        out["started"] = result.started;
        out["cancelled"] = result.cancelled;
        out["cancel_timeout"] = result.cancel_timeout;
        out["normal_exit"] = result.normal_exit;
        out["exit_code"] = result.exit_code;
        out["error"] = result.error;
        out["stderr_tail"] = python_text_from_bytes(result.stderr_tail);
        out["stdout_lines"] = result.stdout_lines;
        out["stderr_lines"] = result.stderr_lines;
        return out;
    }

private:
    const HostFacade* available(const std::string& method) const {
        if (!host_facade_has(method)) {
            std::string reason;
            host_facade_available(&reason);
            core::log(core::Level::Warn,
                      "[" + owner_ + "] 宿主服务门面不可用（host." + method + "）：" + reason);
            return nullptr;
        }
        return host_facade();
    }

    std::string owner_;
};

class PyUi {
public:
    explicit PyUi(std::string owner) : owner_(std::move(owner)) {}
    py::object label(py::object parent, std::string text, std::string style) const {
        if (const UiFacade* facade = available("label")) {
            return wrap(facade->label(handle_of(parent), text, style));
        }
        return py::none();
    }
    py::object button(py::object parent, std::string text, py::object on_click, int width,
                      std::string style, bool enabled) const {
        const UiFacade* facade = available("button");
        if (facade == nullptr) return py::none();
        std::function<void()> click;
        if (!on_click.is_none()) {
            if (PyCallable_Check(on_click.ptr()) == 0) {
                core::log(core::Level::Warn,
                          "[" + owner_ + "] ui.button 的 on_click 不是可调用对象");
            } else {
                click = wrap_python_handler(on_click, "ui.button");
            }
        }
        return wrap(facade->button(handle_of(parent), text, std::move(click), width, style, enabled));
    }
    py::object row(py::object parent, std::string side, std::string fill, bool expand, int padx,
                   int pady) const {
        if (const UiFacade* facade = available("row")) {
            return wrap(facade->row(handle_of(parent), side, fill, expand, padx, pady));
        }
        return py::none();
    }
    py::object checkbox(py::object parent, std::string text, py::object on_toggle) const {
        const UiFacade* facade = available("checkbox");
        if (facade == nullptr) return py::none();
        std::function<void(bool)> toggle;
        if (!on_toggle.is_none() && PyCallable_Check(on_toggle.ptr()) != 0) {
            py::object kept = on_toggle;
            keepalive()->push_back(kept);
            toggle = [kept](bool on) {
                py::gil_scoped_acquire acquire;
                try {
                    kept(py::bool_(on));
                } catch (const py::error_already_set& err) {
                    core::log(core::Level::Warn,
                              std::string("ui.checkbox 回调抛异常：") + err.what());
                }
            };
        }
        return wrap(facade->checkbox(handle_of(parent), text, std::move(toggle)));
    }
    // ★★ P17-90（接口契约批 · 参数名对账）：**参数名照契约表**（`contract_tables.inc` 的
    //   `kUiMethods[]` 写着 `entry(parent, textvariable, width, show)`，那是"第三方照这两样写"的
    //   契约面）。修前这里叫 `initial` / `password` —— 功能一样、名字对不上 ⇒ 按 1.x 写法
    //   `api.ui.entry(parent, textvariable='…', show=True)` 会 `TypeError`（假缺口真存在）。
    //   ★ 位置参数与既有调用点**完全不变**（仓里 8 处 entry 全是位置传参）。
    py::object entry(py::object parent, std::string textvariable, int width, bool show) const {
        if (const UiFacade* facade = available("entry")) {
            return wrap(facade->entry(handle_of(parent), textvariable, width, show));
        }
        return py::none();
    }
    py::object text(py::object parent, int height, int width, std::string wrap_mode) const {
        if (const UiFacade* facade = available("text")) {
            return wrap(facade->text(handle_of(parent), height, width, wrap_mode));
        }
        return py::none();
    }
    py::object combo(py::object parent, std::vector<std::string> values,
                     py::object on_change) const {
        const UiFacade* facade = available("combo");
        if (facade == nullptr) return py::none();
        std::function<void(const std::string&)> change;
        if (!on_change.is_none() && PyCallable_Check(on_change.ptr()) != 0) {
            py::object kept = on_change;
            keepalive()->push_back(kept);
            change = [kept](const std::string& value) {
                py::gil_scoped_acquire acquire;
                try {
                    kept(py::str(value));
                } catch (const py::error_already_set& err) {
                    core::log(core::Level::Warn, std::string("ui.combo 回调抛异常：") + err.what());
                }
            };
        }
        return wrap(facade->combo(handle_of(parent), values, std::move(change)));
    }
    // ★★ P17-91（接口契约批 · 收账）：参数名与**个数**都照契约表
    //   `drop_box(parent, on_file, placeholder, width)` —— 修前少了 `width`（第三方传的宽度静默丢）。
    py::object drop_box(py::object parent, py::object on_file, std::string placeholder,
                        int width) const {
        const UiFacade* facade = available("drop_box");
        if (facade == nullptr) return py::none();
        std::function<void(const std::string&)> file;
        if (!on_file.is_none() && PyCallable_Check(on_file.ptr()) != 0) {
            py::object kept = on_file;
            keepalive()->push_back(kept);
            file = [kept](const std::string& path) {
                py::gil_scoped_acquire acquire;
                try {
                    kept(py::str(path));
                } catch (const py::error_already_set& err) {
                    core::log(core::Level::Warn,
                              std::string("ui.drop_box 回调抛异常：") + err.what());
                }
            };
        }
        return wrap(facade->drop_box(handle_of(parent), std::move(file), placeholder, width));
    }
    void pack(py::object parent, py::object child, std::string side, std::string fill, bool expand,
              int padx, int pady) const {
        if (const UiFacade* facade = available("pack")) {
            facade->pack(handle_of(parent), handle_of(child), side, fill, expand, padx, pady);
        }
    }
    // ── ★★ P12c：进度条（video 页转码进度）────────────────────────────────────
    py::object progress(py::object parent) const {
        if (const UiFacade* facade = available("progress")) {
            return wrap(facade->progress(handle_of(parent)));
        }
        return py::none();
    }
    bool set_progress(py::object widget, int percent) const {
        if (const UiFacade* facade = available("set_progress")) {
            facade->set_progress(handle_of(widget), percent);
            return true;
        }
        return false;
    }
    int progress_value(py::object widget) const {
        if (const UiFacade* facade = available("progress_value")) {
            return facade->progress_value(handle_of(widget));
        }
        return 0;
    }
    py::object text_view(py::object parent, int height, bool mono) const {
        if (const UiFacade* facade = available("text_view")) {
            return wrap(facade->text_view(handle_of(parent), height, mono));
        }
        return py::none();
    }
    void set_text(py::object widget, std::string text) const {
        if (const UiFacade* facade = available("set_text")) {
            facade->set_text(handle_of(widget), text);
        }
    }
    std::string get_text(py::object widget) const {
        if (const UiFacade* facade = available("get_text")) {
            return facade->get_text(handle_of(widget));
        }
        return std::string();
    }
    std::string combo_value(py::object widget) const {
        if (const UiFacade* facade = available("combo_value")) {
            return facade->combo_value(handle_of(widget));
        }
        return std::string();
    }
    bool checkbox_state(py::object widget) const {
        if (const UiFacade* facade = available("checkbox_state")) {
            return facade->checkbox_state(handle_of(widget));
        }
        return false;
    }
    bool set_checked(py::object widget, bool checked) const {
        if (const UiFacade* facade = available("set_checked")) {
            facade->set_checked(handle_of(widget), checked);
            return true;
        }
        return false;
    }
    // ★★ P11b-2：重建下拉项（第 0 项成为当前项）
    bool combo_set_items(py::object widget, py::object values) const {
        const UiFacade* facade = available("combo_set_items");
        if (facade == nullptr) return false;
        std::vector<std::string> items;
        for (auto item : py::list(values)) items.push_back(py::str(item).cast<std::string>());
        facade->combo_set_items(handle_of(widget), items);
        return true;
    }
    void set_enabled(py::object widget, bool enabled) const {
        if (const UiFacade* facade = available("set_enabled")) {
            facade->set_enabled(handle_of(widget), enabled);
        }
    }
    bool alive(py::object widget) const {
        if (const UiFacade* facade = available("alive")) return facade->alive(handle_of(widget));
        return false;
    }
    void log(const std::string& line) const {
        if (const UiFacade* facade = available("log_append")) facade->log_append(line);
    }
    bool ask_yes_no(std::string title, std::string message) const {
        if (const UiFacade* facade = available("ask_yes_no")) {
            return facade->ask_yes_no(title, message);
        }
        return false;
    }
    // ★★ P17-83（接口契约批「`ask_*` 参数缺口」）：签名**回到 1.x 契约**
    //   （`toolkit_plugin_ui.py:327/:336`）—— 1.x 的 `filetypes` / `initialdir` / `initialfile`
    //   在 2.x 被**静默丢掉**（插件的这些参数收不到、对话框永远从默认目录开）。
    //   `filetypes` 的接受形状＝1.x 那种**两元组列表**（tkinter 口径：`("说明", "*.ext *.ext2")`），
    //   在这里拼成门面认的过滤串（`<说明>|<模式>…`，多条用 `\n` 分隔）。
    //   `parent` 刻意**不进签名**：1.x 那侧只用它找 Tk 顶层窗口；2.x 的宿主对话框天然挂在应用窗口上。
    static std::string filetypes_to_qt(py::object filetypes) {
        if (filetypes.is_none() != 0) return std::string();
        std::string out;
        for (py::handle item : filetypes) {   // 逐条两元组；不认的形状**跳过**（不猜）
            py::tuple pair;
            try {
                pair = py::cast<py::tuple>(item);
            } catch (const py::cast_error&) {
                continue;
            }
            if (pair.size() != 2) continue;
            const std::string label = py::cast<std::string>(pair[0]);
            const std::string patterns = py::cast<std::string>(pair[1]);
            if (patterns.empty()) continue;
            if (!out.empty()) out += '\n';
            out += label + "|" + patterns;
        }
        return out;
    }
    std::string ask_open_file(std::string title, py::object filetypes,
                              const std::string& initialdir) const {
        if (const UiFacade* facade = available("ask_open_file")) {
            return facade->ask_open_file(title, filetypes_to_qt(std::move(filetypes)), initialdir);
        }
        return std::string();
    }
    std::string ask_save_file(std::string title, py::object filetypes,
                              const std::string& initialdir,
                              const std::string& initialfile) const {
        if (const UiFacade* facade = available("ask_save_file")) {
            return facade->ask_save_file(title, filetypes_to_qt(std::move(filetypes)), initialdir,
                                         initialfile);
        }
        return std::string();
    }
    std::string ask_directory(std::string title) const {
        if (const UiFacade* facade = available("ask_directory")) {
            return facade->ask_directory(title);
        }
        return std::string();
    }
    // ★★ P13-2a：右键菜单（宿主渲染；回调给选中项的标签原文）
    bool context_menu(py::object widget, py::object labels, py::object callback) const {
        const UiFacade* facade = available("context_menu");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.context_menu 的回调不是可调用对象");
            return false;
        }
        std::vector<std::string> items;
        for (auto item : py::list(labels)) {
            items.push_back(py::str(item).cast<std::string>());
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->context_menu(handle_of(widget), items, [kept](const std::string& label) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(label));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.context_menu 回调抛异常：") + err.what());
            }
        });
        return true;
    }
    bool open_path(std::string path) const {
        if (const UiFacade* facade = available("open_path")) {
            return facade->open_path(path);
        }
        return false;
    }
    // ★★ P11b-2：写系统剪贴板（xml 页「复制 ID」）
    bool copy_text(std::string text) const {
        if (const UiFacade* facade = available("copy_text")) {
            facade->copy_text(text);
            return true;
        }
        return false;
    }
    // ★★ P8a：字库选择器（宿主唯一一份界面）。回调是**异步**的 ⇒ 把 Python callable
    //   包成 C++ 回调并**进 keepalive**（对话框还活着，回调就可能随时被叫）。
    bool pick_font(std::string current, py::object on_picked) const {
        const UiFacade* facade = available("pick_font");
        if (facade == nullptr) return false;   // 宿主没这个能力 ⇒ 调用方自己退路
        std::function<void(const std::string&)> callback;
        if (!on_picked.is_none()) {
            // `keepalive()` 是"保活表"的入口（**没有返回值**）：先把对象收进去，再拿副本进回调
            py::object held = on_picked;
            keepalive()->push_back(held);
            callback = [held](const std::string& path) {
                py::gil_scoped_acquire acquire;
                try {
                    held(py::str(path));
                } catch (const py::error_already_set&) {
                    // Python 侧异常不该掀翻宿主（与按钮处理器同口径）
                }
            };
        }
        facade->pick_font(current, std::move(callback));
        return true;
    }

    // ── ★★ P4：重型 L2 第一批（虚拟列表 / 日志视图 / 拖放目标 / 任务壳）──────────
    // 口径：渲染/滚动/像素/委托全在宿主；插件只给"数据 + 回调"，回调都在**主线程**上被调
    // （唯一例外：`task_run` 的 worker 在**工作线程**里跑，那两个 sink 是唯一允许的跨线程入口）。
    py::object list(py::object parent, int row_height) const {
        if (const UiFacade* facade = available("list")) {
            return wrap(facade->list(handle_of(parent), row_height));
        }
        return py::none();
    }
    void list_set_row_count(py::object list_widget, int rows) const {
        if (const UiFacade* facade = available("list_set_row_count")) {
            facade->list_set_row_count(handle_of(list_widget), rows);
        }
    }
    // ★L2.5：`provider(row) -> str` 只在**可见行**上被调（虚拟化的承诺）
    void list_set_row_text(py::object list_widget, py::object provider) const {
        const UiFacade* facade = available("list_set_row_text");
        if (facade == nullptr) return;
        if (provider.is_none() != 0) {
            facade->list_set_row_text(handle_of(list_widget), nullptr);
            return;
        }
        if (PyCallable_Check(provider.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.list_set_row_text 的 provider 不是可调用对象");
            return;
        }
        py::object kept = provider;
        keepalive()->push_back(kept);
        facade->list_set_row_text(handle_of(list_widget), [kept](int row) -> std::string {
            py::gil_scoped_acquire acquire;
            try {
                return py::str(kept(row)).cast<std::string>();
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.list_set_row_text 的 provider 抛异常：") + err.what());
                return std::string();
            }
        });
    }
    std::string list_row_text(py::object list_widget, int row) const {
        if (const UiFacade* facade = available("list_row_text")) {
            return facade->list_row_text(handle_of(list_widget), row);
        }
        return std::string();
    }
    // ★★ P16-3b-2：自带滚动的长列表区（控件下限 #3）
    py::object list_self_scrolling(py::object parent, int min_body_height) const {
        const UiFacade* facade = available("list_self_scrolling");
        if (facade == nullptr) return py::none();
        return wrap(facade->list_self_scrolling(handle_of(parent), min_body_height));
    }
    // ── ★★ P16-3c：**按钮左键动作菜单**（控件下限 #1）──────────────────────────
    bool button_on_menu(py::object button, py::object items, py::object callback) const {
        const UiFacade* facade = available("button_on_menu");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.button_on_menu 的回调不是可调用对象");
            return false;
        }
        std::vector<std::string> labels;
        if (items.is_none() == 0) {
            for (auto item : py::list(items)) labels.push_back(py::str(item).cast<std::string>());
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        return facade->button_on_menu(handle_of(button), labels, [kept](int index) {
            py::gil_scoped_acquire acquire;
            try {
                kept(index);
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.button_on_menu 回调抛异常：") + err.what());
            }
        });
    }
    void list_select_row(py::object list_widget, int row) const {
        if (const UiFacade* facade = available("list_select_row")) {
            facade->list_select_row(handle_of(list_widget), row);
        }
    }
    // ★★ P13-2b：列表激活（双击 / 回车）；回调给**行号**
    bool list_on_activate(py::object list_widget, py::object callback) const {
        const UiFacade* facade = available("list_on_activate");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.list_on_activate 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->list_on_activate(handle_of(list_widget), [kept](int row) {
            py::gil_scoped_acquire acquire;
            try {
                kept(row);
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.list_on_activate 回调抛异常：") + err.what());
            }
        });
        return true;
    }
    int list_selected_row(py::object list_widget) const {
        if (const UiFacade* facade = available("list_selected_row")) {
            return facade->list_selected_row(handle_of(list_widget));
        }
        return -1;
    }
    // ★★ P17-82（接口契约批「本体拖入」，用户 2026-10-09 已拍板）：**本机文件/目录拖到列表上**。
    //   回调收**本机路径列表**（`list[str]`；与 `drop_zone_on_drop` 同形，便于插件两边共用一段代码）。
    bool list_on_drop(py::object list_widget, py::object callback) const {
        const UiFacade* facade = available("list_on_drop");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0) {   // None ⇒ 撤销（过滤器仍在，落下什么都不做）
            facade->list_on_drop(handle_of(list_widget), nullptr);
            return true;
        }
        if (PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.list_on_drop 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->list_on_drop(handle_of(list_widget),
                             [kept](const std::vector<std::string>& paths) {
                                 py::gil_scoped_acquire acquire;
                                 try {
                                     kept(strings_to_python(paths));
                                 } catch (const py::error_already_set& err) {
                                     core::log(core::Level::Warn,
                                               std::string("ui.list_on_drop 回调抛异常：") +
                                                   err.what());
                                 }
                             });
        return true;
    }
    // ── ★★ P16-3a：**多选**（控件下限 #2；1.x 参照 `QListWidget::ExtendedSelection`）────
    void list_set_multi(py::object list_widget, bool on) const {
        if (const UiFacade* facade = available("list_set_multi")) {
            facade->list_set_multi(handle_of(list_widget), on);
        }
    }
    py::object list_selected_rows(py::object list_widget) const {
        py::list out;
        const UiFacade* facade = available("list_selected_rows");
        if (facade == nullptr) return out;
        for (int row : facade->list_selected_rows(handle_of(list_widget))) out.append(row);
        return out;
    }
    void list_set_selected_rows(py::object list_widget, py::object rows) const {
        const UiFacade* facade = available("list_set_selected_rows");
        if (facade == nullptr) return;
        std::vector<int> wanted;
        if (rows.is_none() == 0) {
            for (auto item : py::list(rows)) wanted.push_back(py::cast<int>(item));
        }
        facade->list_set_selected_rows(handle_of(list_widget), std::move(wanted));
    }
    int list_content_height(py::object list_widget) const {
        if (const UiFacade* facade = available("list_content_height")) {
            return facade->list_content_height(handle_of(list_widget));
        }
        return 0;
    }
    py::object log_view(py::object parent, int height) const {
        if (const UiFacade* facade = available("log_view")) {
            return wrap(facade->log_view(handle_of(parent), height));
        }
        return py::none();
    }
    void log_view_append(py::object view, const std::string& line) const {
        if (const UiFacade* facade = available("log_view_append")) {
            facade->log_view_append(handle_of(view), line);
        }
    }
    void log_view_clear(py::object view) const {
        if (const UiFacade* facade = available("log_view_clear")) {
            facade->log_view_clear(handle_of(view));
        }
    }
    std::string log_view_text(py::object view) const {
        if (const UiFacade* facade = available("log_view_text")) {
            return facade->log_view_text(handle_of(view));
        }
        return std::string();
    }
    py::object drop_zone(py::object parent, std::string hint) const {
        if (const UiFacade* facade = available("drop_zone")) {
            return wrap(facade->drop_zone(handle_of(parent), hint));
        }
        return py::none();
    }
    void drop_zone_on_drop(py::object zone, py::object on_drop) const {
        const UiFacade* facade = available("drop_zone_on_drop");
        if (facade == nullptr) return;
        if (on_drop.is_none() != 0) {
            facade->drop_zone_on_drop(handle_of(zone), nullptr);
            return;
        }
        if (PyCallable_Check(on_drop.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.drop_zone_on_drop 的回调不是可调用对象");
            return;
        }
        py::object kept = on_drop;
        keepalive()->push_back(kept);
        facade->drop_zone_on_drop(
            handle_of(zone), [kept](const std::vector<std::string>& paths) {
                py::gil_scoped_acquire acquire;
                py::list dropped;
                for (const std::string& path : paths) dropped.append(py::str(path));
                try {
                    kept(dropped);
                } catch (const py::error_already_set& err) {
                    core::log(core::Level::Warn,
                              std::string("ui.drop_zone_on_drop 回调抛异常：") + err.what());
                }
            });
    }
    py::object task_panel(py::object parent, bool with_cancel) const {
        if (const UiFacade* facade = available("task_panel")) {
            return wrap(facade->task_panel(handle_of(parent), with_cancel));
        }
        return py::none();
    }
    // ★★ 唯一允许**跨线程**的入口：worker 在工作线程里跑，收到 `progress(0~100)` / `message(text)`。
    //   两个 sink 都自带 GIL 获取（工作线程里没有 GIL）；worker 里**不许碰别的 ui.* 方法** ——
    //   那由 `on_ui_thread()` 兜住（不在 UI 线程 ⇒ 那些方法返回空 + 记账）。
    bool task_run(py::object panel, std::string title, py::object worker) const {
        const UiFacade* facade = available("task_run");
        if (facade == nullptr) return false;
        if (worker.is_none() != 0 || PyCallable_Check(worker.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.task_run 的 worker 不是可调用对象");
            return false;
        }
        py::object kept = worker;
        keepalive()->push_back(kept);
        return facade->task_run(
            handle_of(panel), title,
            [kept](UiFacade::TaskProgress progress, UiFacade::TaskMessage message) {
                py::gil_scoped_acquire acquire;
                py::object progress_object = py::cpp_function([progress](int percent) {
                    py::gil_scoped_acquire inner;
                    progress(percent);
                });
                py::object message_object = py::cpp_function([message](const std::string& text) {
                    py::gil_scoped_acquire inner;
                    message(text);
                });
                try {
                    kept(progress_object, message_object);
                } catch (const py::error_already_set& err) {
                    const std::string what = err.what();
                    py::gil_scoped_acquire inner;
                    core::log(core::Level::Error,
                              std::string("ui.task_run 的 worker 抛异常：") + what);
                    message(std::string("插件任务抛异常：") + what);   // 零静默：显示在任务壳上
                }
            });
    }
    std::string task_status(py::object panel) const {
        if (const UiFacade* facade = available("task_status")) {
            return facade->task_status(handle_of(panel));
        }
        return std::string();
    }
    void task_cancel(py::object panel) const {
        if (const UiFacade* facade = available("task_cancel")) {
            facade->task_cancel(handle_of(panel));
        }
    }
    // ★★ P12c-2：任务壳的**回主线程**消息回调（回调里可以碰控件；worker 里不许）
    bool task_on_message(py::object panel, py::object handler) const {
        const UiFacade* facade = available("task_on_message");
        if (facade == nullptr) return false;
        if (handler.is_none() != 0 || PyCallable_Check(handler.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.task_on_message 的回调不是可调用对象");
            return false;
        }
        py::object kept = handler;
        keepalive()->push_back(kept);
        facade->task_on_message(handle_of(panel), [kept](const std::string& text) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(text));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.task_on_message 回调抛异常：") + err.what());
            }
        });
        return true;
    }
    bool task_cancel_requested(py::object panel) const {
        if (const UiFacade* facade = available("task_cancel_requested")) {
            return facade->task_cancel_requested(handle_of(panel));
        }
        return false;
    }
    bool on_ui_thread() const {
        if (const UiFacade* facade = available("on_ui_thread")) return facade->on_ui_thread();
        return false;
    }

    // ── ★★ P5：重型 L2 第二批（滚动视口 / 可分栏）──────────────────────────────
    py::object viewport(py::object parent, int min_body_height) const {
        if (const UiFacade* facade = available("viewport")) {
            return wrap(facade->viewport(handle_of(parent), min_body_height));
        }
        return py::none();
    }
    void viewport_fit_content(py::object body, bool content_fit) const {
        if (const UiFacade* facade = available("viewport_fit_content")) {
            facade->viewport_fit_content(handle_of(body), content_fit);
        }
    }
    void viewport_set_min_body_height(py::object body, int min_body_height) const {
        if (const UiFacade* facade = available("viewport_set_min_body_height")) {
            facade->viewport_set_min_body_height(handle_of(body), min_body_height);
        }
    }
    void viewport_sync(py::object body) const {
        if (const UiFacade* facade = available("viewport_sync")) {
            facade->viewport_sync(handle_of(body));
        }
    }
    bool viewport_can_scroll(py::object body) const {
        if (const UiFacade* facade = available("viewport_can_scroll")) {
            return facade->viewport_can_scroll(handle_of(body));
        }
        return false;
    }
    int viewport_scroll_top(py::object body) const {
        if (const UiFacade* facade = available("viewport_scroll_top")) {
            return facade->viewport_scroll_top(handle_of(body));
        }
        return 0;
    }
    py::object split(py::object parent, bool horizontal) const {
        if (const UiFacade* facade = available("split")) {
            return wrap(facade->split(handle_of(parent), horizontal));
        }
        return py::none();
    }

    // ── ★★ P15-1a：标签页（fs 页两标签）────────────────────────────────────
    py::object tabs(py::object parent) const {
        const UiFacade* facade = available("tabs");
        if (facade == nullptr) return py::none();
        return wrap(facade->tabs(handle_of(parent)));
    }
    py::object tabs_add_page(py::object tabs_widget, const std::string& title) const {
        const UiFacade* facade = available("tabs_add_page");
        if (facade == nullptr) return py::none();
        return wrap(facade->tabs_add_page(handle_of(tabs_widget), title));
    }
    int tabs_page_count(py::object tabs_widget) const {
        const UiFacade* facade = available("tabs_page_count");
        return facade == nullptr ? 0 : facade->tabs_page_count(handle_of(tabs_widget));
    }
    void tabs_set_current(py::object tabs_widget, int index) const {
        const UiFacade* facade = available("tabs_set_current");
        if (facade != nullptr) facade->tabs_set_current(handle_of(tabs_widget), index);
    }
    int tabs_current_index(py::object tabs_widget) const {
        const UiFacade* facade = available("tabs_current_index");
        return facade == nullptr ? -1 : facade->tabs_current_index(handle_of(tabs_widget));
    }
    py::object split_add_pane(py::object split_widget, int minsize) const {
        if (const UiFacade* facade = available("split_add_pane")) {
            return wrap(facade->split_add_pane(handle_of(split_widget), minsize));
        }
        return py::none();
    }
    void split_set_max_size(py::object split_widget, int index, int max_size) const {
        if (const UiFacade* facade = available("split_set_max_size")) {
            facade->split_set_max_size(handle_of(split_widget), index, max_size);
        }
    }
    void split_set_collapsed(py::object split_widget, int index, bool collapsed) const {
        if (const UiFacade* facade = available("split_set_collapsed")) {
            facade->split_set_collapsed(handle_of(split_widget), index, collapsed);
        }
    }
    bool split_is_collapsed(py::object split_widget, int index) const {
        if (const UiFacade* facade = available("split_is_collapsed")) {
            return facade->split_is_collapsed(handle_of(split_widget), index);
        }
        return false;
    }
    int split_pane_count(py::object split_widget) const {
        if (const UiFacade* facade = available("split_pane_count")) {
            return facade->split_pane_count(handle_of(split_widget));
        }
        return 0;
    }
    int split_sash_pos(py::object split_widget, int index) const {
        if (const UiFacade* facade = available("split_sash_pos")) {
            return facade->split_sash_pos(handle_of(split_widget), index);
        }
        return -1;
    }

    // ── ★★ P6：树（多列 / 懒建 / 展开态 / 过滤计数 / 三态勾选）──────────────────
    py::object tree(py::object parent, py::object columns) const {
        if (const UiFacade* facade = available("tree")) {
            return wrap(facade->tree(handle_of(parent), tree_columns_from_python(columns)));
        }
        return py::none();
    }
    // ★★ P11b-2：换列声明（xml 页切模式要换表头/列宽）
    void tree_set_columns(py::object tree_widget, py::object columns) const {
        if (const UiFacade* facade = available("tree_set_columns")) {
            facade->tree_set_columns(handle_of(tree_widget), tree_columns_from_python(columns));
        }
    }
    // ★★ P13：行尾动作（fs 页的 ✕）；回调给**行 key**
    bool tree_on_action(py::object tree_widget, std::string mark, py::object callback) const {
        const UiFacade* facade = available("tree_on_action");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn,
                      "[" + owner_ + "] ui.tree_on_action 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_action(handle_of(tree_widget), mark,
                               [kept](const std::string& key) {
                                   py::gil_scoped_acquire acquire;
                                   try {
                                       kept(py::str(key));
                                   } catch (const py::error_already_set& err) {
                                       core::log(core::Level::Warn,
                                                 std::string("ui.tree_on_action 回调抛异常：") +
                                                     err.what());
                                   }
                               });
        return true;
    }
    void tree_set_root(py::object tree_widget, py::object nodes) const {
        if (const UiFacade* facade = available("tree_set_root")) {
            facade->tree_set_root(handle_of(tree_widget), tree_nodes_from_python(nodes));
        }
    }
    void tree_set_children(py::object tree_widget, const std::string& parent_key,
                           py::object nodes) const {
        if (const UiFacade* facade = available("tree_set_children")) {
            facade->tree_set_children(handle_of(tree_widget), parent_key,
                                      tree_nodes_from_python(nodes));
        }
    }
    void tree_clear(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_clear")) {
            facade->tree_clear(handle_of(tree_widget));
        }
    }
    int tree_row_count(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_row_count")) {
            return facade->tree_row_count(handle_of(tree_widget));
        }
        return 0;
    }
    py::list tree_keys(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_keys")) {
            return strings_to_python(facade->tree_keys(handle_of(tree_widget)));
        }
        return py::list();
    }
    void tree_set_expanded(py::object tree_widget, const std::string& key, bool expanded) const {
        if (const UiFacade* facade = available("tree_set_expanded")) {
            facade->tree_set_expanded(handle_of(tree_widget), key, expanded);
        }
    }
    py::list tree_expanded_keys(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_expanded_keys")) {
            return strings_to_python(facade->tree_expanded_keys(handle_of(tree_widget)));
        }
        return py::list();
    }
    void tree_set_checked(py::object tree_widget, const std::string& key, bool checked) const {
        if (const UiFacade* facade = available("tree_set_checked")) {
            facade->tree_set_checked(handle_of(tree_widget), key, checked);
        }
    }
    bool tree_checked(py::object tree_widget, const std::string& key) const {
        if (const UiFacade* facade = available("tree_checked")) {
            return facade->tree_checked(handle_of(tree_widget), key);
        }
        return false;
    }
    py::list tree_checked_keys(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_checked_keys")) {
            return strings_to_python(facade->tree_checked_keys(handle_of(tree_widget)));
        }
        return py::list();
    }
    py::list tree_partial_keys(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_partial_keys")) {
            return strings_to_python(facade->tree_partial_keys(handle_of(tree_widget)));
        }
        return py::list();
    }
    void tree_set_filter(py::object tree_widget, const std::string& text) const {
        if (const UiFacade* facade = available("tree_set_filter")) {
            facade->tree_set_filter(handle_of(tree_widget), text);
        }
    }
    int tree_filter_hits(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_filter_hits")) {
            return facade->tree_filter_hits(handle_of(tree_widget));
        }
        return 0;
    }
    void tree_select(py::object tree_widget, const std::string& key) const {
        if (const UiFacade* facade = available("tree_select")) {
            facade->tree_select(handle_of(tree_widget), key);
        }
    }
    std::string tree_selected_key(py::object tree_widget) const {
        if (const UiFacade* facade = available("tree_selected_key")) {
            return facade->tree_selected_key(handle_of(tree_widget));
        }
        return std::string();
    }
    // ★L2.5 回调：`on_expand(key)`（懒建）、`on_check(key, checked)`、`on_activate(key)`
    void tree_on_expand(py::object tree_widget, py::object callback) const {
        const UiFacade* facade = available("tree_on_expand");
        if (facade == nullptr) return;
        if (callback.is_none() != 0) {
            facade->tree_on_expand(handle_of(tree_widget), nullptr);
            return;
        }
        if (PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.tree_on_expand 的回调不是可调用对象");
            return;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_expand(handle_of(tree_widget), [kept](const std::string& key) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(key));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.tree_on_expand 回调抛异常：") + err.what());
            }
        });
    }
    void tree_on_check(py::object tree_widget, py::object callback) const {
        const UiFacade* facade = available("tree_on_check");
        if (facade == nullptr) return;
        if (callback.is_none() != 0) {
            facade->tree_on_check(handle_of(tree_widget), nullptr);
            return;
        }
        if (PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.tree_on_check 的回调不是可调用对象");
            return;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_check(handle_of(tree_widget),
                              [kept](const std::string& key, bool checked) {
                                  py::gil_scoped_acquire acquire;
                                  try {
                                      kept(py::str(key), py::bool_(checked));
                                  } catch (const py::error_already_set& err) {
                                      core::log(core::Level::Warn,
                                                std::string("ui.tree_on_check 回调抛异常：") +
                                                    err.what());
                                  }
                              });
    }
    void tree_on_activate(py::object tree_widget, py::object callback) const {
        const UiFacade* facade = available("tree_on_activate");
        if (facade == nullptr) return;
        if (callback.is_none() != 0) {
            facade->tree_on_activate(handle_of(tree_widget), nullptr);
            return;
        }
        if (PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.tree_on_activate 的回调不是可调用对象");
            return;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_activate(handle_of(tree_widget), [kept](const std::string& key) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(key));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.tree_on_activate 回调抛异常：") + err.what());
            }
        });
    }

    // ── ★★ P15-0：fs 页缺口三条 ───────────────────────────────────────────
    bool entry_on_change(py::object widget, py::object callback) const {
        const UiFacade* facade = available("entry_on_change");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.entry_on_change 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->entry_on_change(handle_of(widget), [kept](const std::string& text) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(text));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn,
                          std::string("ui.entry_on_change 回调抛异常：") + err.what());
            }
        });
        return true;
    }
    py::object tree_selected_keys(py::object tree_widget) const {
        const UiFacade* facade = available("tree_selected_keys");
        if (facade == nullptr) return py::none();
        py::list out;
        for (const std::string& key : facade->tree_selected_keys(handle_of(tree_widget))) {
            out.append(py::str(key));
        }
        return out;
    }
    bool tree_on_click(py::object tree_widget, py::object callback) const {
        const UiFacade* facade = available("tree_on_click");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0 || PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.tree_on_click 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_click(handle_of(tree_widget), [kept](const std::string& key) {
            py::gil_scoped_acquire acquire;
            try {
                kept(py::str(key));
            } catch (const py::error_already_set& err) {
                core::log(core::Level::Warn, std::string("ui.tree_on_click 回调抛异常：") + err.what());
            }
        });
        return true;
    }
    // ★★ P17-82：**本机文件/目录拖到这棵树上**（与 `list_on_drop` 同一机制、同一份过滤器实现）
    bool tree_on_drop(py::object tree_widget, py::object callback) const {
        const UiFacade* facade = available("tree_on_drop");
        if (facade == nullptr) return false;
        if (callback.is_none() != 0) {   // None ⇒ 撤销
            facade->tree_on_drop(handle_of(tree_widget), nullptr);
            return true;
        }
        if (PyCallable_Check(callback.ptr()) == 0) {
            core::log(core::Level::Warn, "[" + owner_ + "] ui.tree_on_drop 的回调不是可调用对象");
            return false;
        }
        py::object kept = callback;
        keepalive()->push_back(kept);
        facade->tree_on_drop(handle_of(tree_widget),
                             [kept](const std::vector<std::string>& paths) {
                                 py::gil_scoped_acquire acquire;
                                 try {
                                     kept(strings_to_python(paths));
                                 } catch (const py::error_already_set& err) {
                                     core::log(core::Level::Warn,
                                               std::string("ui.tree_on_drop 回调抛异常：") +
                                                   err.what());
                                 }
                             });
        return true;
    }

private:
    // 门面/方法不可用 ⇒ **逐条记账**并返回"空"（零静默：插件作者能立刻看出为什么什么都没出现）
    const UiFacade* available(const std::string& method) const {
        if (!ui_facade_has(method)) {
            std::string reason;
            ui_facade_available(&reason);
            core::log(core::Level::Warn,
                      "[" + owner_ + "] 控件门面不可用（ui." + method + "）：" + reason);
            return nullptr;
        }
        const UiFacade* facade = ui_facade();
        // ★★ 线程纪律：门面方法**只许在宿主 UI 线程**上调用（`task_run` 的那两个 sink 是唯一
        //   例外，它们不走这里）。不在 ⇒ 返回空 + 记账 —— "工作线程里碰 Qt"是最容易崩的一类，
        //   与其让它随机崩，不如让它**每次都失败得看得见**。
        if (facade != nullptr && facade->on_ui_thread && !facade->on_ui_thread()) {
            // ★★ 2026-10-10（P17-1，审计 05 PLG-01 High）：**跨线程只读白名单**。
            //   取消谓词是在插件 worker 线程里被同步轮询的（引擎的 `should_stop`/`cancel`
            //   hook 都在调用线程被调），而修前这里一刀切 ⇒ worker 里读取消位**恒为 false**
            //   ⇒「取消」关不掉、任务照跑到结束、状态栏还谎报"已取消"。
            //   白名单口径在 widget_facade.cpp（只读、不碰控件的方法才准进），两个前端共用。
            if (ui_method_worker_safe(method)) return facade;
            core::log(core::Level::Warn,
                      "[" + owner_ + "] 控件门面只许在宿主 UI 线程调用（ui." + method +
                          " 在别的线程被调用，已拒绝）");
            return nullptr;
        }
        return facade;
    }
    static UiHandle handle_of(const py::object& widget_or_none) {
        return handle_of_widget(widget_or_none);
    }
    static py::object wrap(UiHandle handle) { return py::cast(PyWidget(handle)); }

    std::string owner_;
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
        py::class_<PyHost>(m, "Host")
            .def("pref_get", &PyHost::pref_get, py::arg("section"), py::arg("key"),
                 py::arg("fallback") = "")
            .def("pref_set", &PyHost::pref_set, py::arg("section"), py::arg("key"),
                 py::arg("value"))
            .def("app_dir", &PyHost::app_dir)
            .def("system_font_dirs", &PyHost::system_font_dirs)
            // ★★ P12b：跑外部进程（宿主服务）
            .def("run_process", &PyHost::run_process, py::arg("options"));
        py::class_<PyEngine>(m, "Engine")
            .def("font_check", &PyEngine::font_check, py::arg("font_path"))
            .def("font_pack", &PyEngine::font_pack, py::arg("options"))
            .def("font_state_marks", &PyEngine::font_state_marks)
            .def("font_size_slots", &PyEngine::font_size_slots)
            // ★★ P9a：convert 页那一组（options / hooks 都是 dict；回调进 keepalive）
            .def("text_encodings", &PyEngine::text_encodings)
            .def("text_convert_batch", &PyEngine::text_convert_batch, py::arg("options"),
                 py::arg("hooks") = py::none())
            .def("text_restore_backups", &PyEngine::text_restore_backups, py::arg("options"),
                 py::arg("hooks") = py::none())
            .def("text_clear_backups", &PyEngine::text_clear_backups, py::arg("options"),
                 py::arg("hooks") = py::none())
            .def("text_write_stats_csv", &PyEngine::text_write_stats_csv, py::arg("path"),
                 py::arg("stats"))
            .def("text_format_duration", &PyEngine::text_format_duration, py::arg("seconds"))
            // ★★ P10a：text 页那一组（第三页迁移）
            .def("text_default_id_prefix", &PyEngine::text_default_id_prefix)
            .def("text_validate_extract", &PyEngine::text_validate_extract, py::arg("request"))
            .def("text_extract_kind", &PyEngine::text_extract_kind, py::arg("dir"))
            .def("text_extract_dir", &PyEngine::text_extract_dir, py::arg("source_dir"),
                 py::arg("target_dir"), py::arg("xml_out_dir"), py::arg("options"))
            // ★★ P11a：xml 页那一组（第四页迁移）
            .def("xml_modes", &PyEngine::xml_modes)
            .def("xml_find_mode", &PyEngine::xml_find_mode, py::arg("key_or_name"))
            .def("text_read_bytes", &PyEngine::text_read_bytes, py::arg("path"),
                 py::arg("encoding") = "auto")
            .def("xml_compare_stats", &PyEngine::xml_compare_stats, py::arg("rel"),
                 py::arg("text_a"), py::arg("text_b"), py::arg("enc_a"), py::arg("enc_b"))
            .def("xml_compare_text", &PyEngine::xml_compare_text, py::arg("rel"),
                 py::arg("text_a"), py::arg("text_b"), py::arg("enc_a"), py::arg("enc_b"))
            .def("xml_only_one_side", &PyEngine::xml_only_one_side, py::arg("rel"), py::arg("side"))
            .def("xml_inline_detail", &PyEngine::xml_inline_detail, py::arg("result"),
                 py::arg("name_a"), py::arg("name_b"))
            .def("xml_full_detail", &PyEngine::xml_full_detail, py::arg("result"),
                 py::arg("name_a"), py::arg("name_b"))
            .def("xml_full_text_detail", &PyEngine::xml_full_text_detail, py::arg("result"),
                 py::arg("name_a"), py::arg("name_b"))
            .def("xml_render_csv", &PyEngine::xml_render_csv, py::arg("report"))
            .def("xml_write_csv", &PyEngine::xml_write_csv, py::arg("path"), py::arg("report"))
            // ★★ P11b：目录对比 + 筛选
            .def("xml_compare_dirs", &PyEngine::xml_compare_dirs, py::arg("options"))
            .def("xml_row_matches", &PyEngine::xml_row_matches, py::arg("filter"),
                 py::arg("kind"), py::arg("result"))
            // ★★ P12a：video 页那一组（第五页迁移）
            .def("video_find_ffmpeg", &PyEngine::video_find_ffmpeg, py::arg("app_dir"),
                 py::arg("ffmpeg_path") = "", py::arg("ffprobe_path") = "")
            .def("video_major_version", &PyEngine::video_major_version, py::arg("version_output"))
            .def("video_version_rejected", &PyEngine::video_version_rejected,
                 py::arg("version_output"))
            .def("video_plan_transcode", &PyEngine::video_plan_transcode, py::arg("request"))
            .def("video_check_output_conflict", &PyEngine::video_check_output_conflict,
                 py::arg("request"))
            .def("video_parse_ffprobe", &PyEngine::video_parse_ffprobe, py::arg("json"))
            .def("video_parse_encoders", &PyEngine::video_parse_encoders, py::arg("output"))
            .def("video_progress_from_line", &PyEngine::video_progress_from_line, py::arg("line"),
                 py::arg("duration_sec"))
            // ★★ P14-0a：fs 面第一批
            .def("fs_formats", &PyEngine::fs_formats)
            .def("fs_format_to_db", &PyEngine::fs_format_to_db, py::arg("key"))
            .def("fs_format_is_sqfs", &PyEngine::fs_format_is_sqfs, py::arg("key"))
            .def("fs_format_is_auto", &PyEngine::fs_format_is_auto, py::arg("key"))
            .def("fs_is_package_name", &PyEngine::fs_is_package_name, py::arg("path"))
            .def("fs_pack_ext_for", &PyEngine::fs_pack_ext_for, py::arg("key"))
            .def("fs_package_folder_name", &PyEngine::fs_package_folder_name,
                 py::arg("package_path"))
            .def("fs_resolve_output_root", &PyEngine::fs_resolve_output_root, py::arg("front"),
                 py::arg("current"))
            .def("fs_unpack_target_dir", &PyEngine::fs_unpack_target_dir, py::arg("out_root"),
                 py::arg("package_path"), py::arg("entry_paths"))
            .def("fs_resolve_pack_target", &PyEngine::fs_resolve_pack_target, py::arg("typed"),
                 py::arg("basename"), py::arg("format_key"))
            .def("fs_safe_out_path", &PyEngine::fs_safe_out_path, py::arg("out_dir"), py::arg("rel"))
            .def("fs_plan_unpack_targets", &PyEngine::fs_plan_unpack_targets, py::arg("out_root"),
                 py::arg("sources"))
            .def("fs_preview_layout", &PyEngine::fs_preview_layout, py::arg("package_path"),
                 py::arg("entries"))
            // ★★ P14-0b：fs 面第二批（合并树）
            .def("fs_loose_entries", &PyEngine::fs_loose_entries, py::arg("root"))
            .def("fs_sort_sources_by_basename", &PyEngine::fs_sort_sources_by_basename,
                 py::arg("paths"))
            .def("fs_tree_build", &PyEngine::fs_tree_build, py::arg("sources"))
            .def("fs_tree_free", &PyEngine::fs_tree_free, py::arg("handle"))
            .def("fs_tree_sources", &PyEngine::fs_tree_sources, py::arg("handle"))
            .def("fs_tree_flatten", &PyEngine::fs_tree_flatten, py::arg("handle"))
            .def("fs_tree_toggle_all", &PyEngine::fs_tree_toggle_all, py::arg("handle"),
                 py::arg("checked"))
            .def("fs_tree_toggle_dir", &PyEngine::fs_tree_toggle_dir, py::arg("handle"),
                 py::arg("path"), py::arg("checked"))
            .def("fs_tree_toggle_source", &PyEngine::fs_tree_toggle_source,
                 py::arg("handle"), py::arg("source_index"), py::arg("checked"))
            .def("fs_tree_source_state", &PyEngine::fs_tree_source_state,
                 py::arg("handle"), py::arg("source_index"))
            .def("fs_tree_checked_paths", &PyEngine::fs_tree_checked_paths,
                 py::arg("handle"))
            .def("fs_tree_apply_checked_paths", &PyEngine::fs_tree_apply_checked_paths,
                 py::arg("handle"), py::arg("paths"))
            .def("fs_tree_conflict_count", &PyEngine::fs_tree_conflict_count,
                 py::arg("handle"))
            .def("fs_tree_checked_file_count", &PyEngine::fs_tree_checked_file_count,
                 py::arg("handle"))
            .def("fs_tree_find", &PyEngine::fs_tree_find, py::arg("handle"),
                 py::arg("path"))
            // ★★ P14-0c：fs 面第三批
            .def("fs_gate_path", &PyEngine::fs_gate_path, py::arg("path"))
            .def("fs_decrypt_output_dir", &PyEngine::fs_decrypt_output_dir)
            .def("fs_package_entries", &PyEngine::fs_package_entries, py::arg("path"),
                 py::arg("db_fallback_key") = "")
            .def("fs_collect_pack_files", &PyEngine::fs_collect_pack_files, py::arg("base"),
                 py::arg("roots"))
            .def("fs_extract_package", &PyEngine::fs_extract_package, py::arg("image_path"),
                 py::arg("out_root"), py::arg("selection") = "all",
                 py::arg("only") = py::none(), py::arg("db_fallback_key") = "")
            // ★★ P14-0d：fs 面第四批（封包）
            .def("fs_pack_squashfs", &PyEngine::fs_pack_squashfs, py::arg("out_path"),
                 py::arg("items"))
            .def("fs_pack_db", &PyEngine::fs_pack_db, py::arg("out_path"), py::arg("items"),
                 py::arg("format_key"))
            // ★★ P16-1a：统一产物模型（清单/安装/还原）
            .def("pkg_write_manifest", &PyEngine::pkg_write_manifest, py::arg("path"),
                 py::arg("entries"))
            .def("pkg_install", &PyEngine::pkg_install, py::arg("target_root"),
                 py::arg("entries"), py::arg("backup_root") = "")
            .def("pkg_restore", &PyEngine::pkg_restore, py::arg("target_root"),
                 py::arg("backup_root"), py::arg("rels"))
            // ★★ P16-1c：二进制补丁（表由插件给；二进制字段走 hex 文本）
            .def("patch_identify", &PyEngine::patch_identify, py::arg("path"), py::arg("sites"))
            .def("patch_apply", &PyEngine::patch_apply, py::arg("src"), py::arg("out"),
                 py::arg("sites"), py::arg("force") = false)
;
        py::class_<PyWidget>(m, "Widget")
            .def("__repr__", [](const PyWidget& widget) {
                return std::string("xrkit.Widget(") + (widget.valid() ? "valid" : "null") + ")";
            })
            .def("__bool__", [](const PyWidget& widget) { return widget.valid(); });
        py::class_<PyUi>(m, "Ui")
            .def("label", &PyUi::label, py::arg("parent") = py::none(), py::arg("text") = "",
                 py::arg("style") = "")
            .def("button", &PyUi::button, py::arg("parent") = py::none(), py::arg("text") = "",
                 py::arg("on_click") = py::none(), py::arg("width") = 0, py::arg("style") = "",
                 py::arg("enabled") = true)
            .def("row", &PyUi::row, py::arg("parent") = py::none(), py::arg("side") = "top",
                 py::arg("fill") = "x", py::arg("expand") = false, py::arg("padx") = 0,
                 py::arg("pady") = 0)
            .def("checkbox", &PyUi::checkbox, py::arg("parent") = py::none(), py::arg("text") = "",
                 py::arg("on_toggle") = py::none())
            .def("entry", &PyUi::entry, py::arg("parent") = py::none(), py::arg("textvariable") = "",
                 py::arg("width") = 0, py::arg("show") = false)
            // ★★ P15-0：输入变化回调（fs 搜索框边打边筛）
            .def("entry_on_change", &PyUi::entry_on_change, py::arg("widget"),
                 py::arg("callback") = py::none())
            .def("text", &PyUi::text, py::arg("parent") = py::none(), py::arg("height") = 0,
                 py::arg("width") = 0, py::arg("wrap") = "word")
            .def("combo", &PyUi::combo, py::arg("parent") = py::none(), py::arg("values"),
                 py::arg("on_change") = py::none())
            .def("drop_box", &PyUi::drop_box, py::arg("parent") = py::none(),
                 py::arg("on_file") = py::none(), py::arg("placeholder") = "",
                 py::arg("width") = 0)
            .def("pack", &PyUi::pack, py::arg("parent") = py::none(), py::arg("child"),
                 py::arg("side") = "top", py::arg("fill") = "x", py::arg("expand") = false,
                 py::arg("padx") = 0, py::arg("pady") = 0)
            .def("progress", &PyUi::progress, py::arg("parent") = py::none())
            .def("set_progress", &PyUi::set_progress, py::arg("widget"), py::arg("percent"))
            .def("progress_value", &PyUi::progress_value, py::arg("widget"))
            .def("text_view", &PyUi::text_view, py::arg("parent") = py::none(),
                 py::arg("height") = 0, py::arg("mono") = true)
            .def("set_text", &PyUi::set_text, py::arg("widget"), py::arg("text"))
            .def("get_text", &PyUi::get_text, py::arg("widget"))
            .def("combo_value", &PyUi::combo_value, py::arg("widget"))
            .def("checkbox_state", &PyUi::checkbox_state, py::arg("widget"))
            .def("set_checked", &PyUi::set_checked, py::arg("widget"), py::arg("checked"))
            .def("combo_set_items", &PyUi::combo_set_items, py::arg("widget"),
                 py::arg("values"))
            .def("set_enabled", &PyUi::set_enabled, py::arg("widget"), py::arg("enabled"))
            .def("alive", &PyUi::alive, py::arg("widget"))
            .def("log", &PyUi::log, py::arg("line"))
            .def("ask_yes_no", &PyUi::ask_yes_no, py::arg("title"), py::arg("message"))
            .def("ask_open_file", &PyUi::ask_open_file, py::arg("title"),
                 py::arg("filetypes") = py::none(), py::arg("initialdir") = std::string())
            .def("ask_save_file", &PyUi::ask_save_file, py::arg("title"),
                 py::arg("filetypes") = py::none(), py::arg("initialdir") = std::string(),
                 py::arg("initialfile") = std::string())
            .def("ask_directory", &PyUi::ask_directory, py::arg("title"))
            .def("open_path", &PyUi::open_path, py::arg("path"))
            .def("context_menu", &PyUi::context_menu, py::arg("widget"),
                 py::arg("labels"), py::arg("callback"))
            .def("copy_text", &PyUi::copy_text, py::arg("text"))
            .def("pick_font", &PyUi::pick_font, py::arg("current"),
                 py::arg("on_picked") = py::none())
            // ★ P4：重型 L2 第一批（表外无门 —— 能力表判据按这里的名单对账）
            .def("list", &PyUi::list, py::arg("parent") = py::none(), py::arg("row_height") = 0)
            .def("list_self_scrolling", &PyUi::list_self_scrolling, py::arg("parent") = py::none(),
                 py::arg("min_body_height") = 0)
            // ★★ P16-3c：按钮左键动作菜单（控件下限 #1）
            .def("button_on_menu", &PyUi::button_on_menu, py::arg("button"), py::arg("items"),
                 py::arg("callback"))
            .def("list_set_row_count", &PyUi::list_set_row_count, py::arg("list"), py::arg("rows"))
            .def("list_set_row_text", &PyUi::list_set_row_text, py::arg("list"),
                 py::arg("provider") = py::none())
            .def("list_row_text", &PyUi::list_row_text, py::arg("list"), py::arg("row"))
            .def("list_select_row", &PyUi::list_select_row, py::arg("list"), py::arg("row"))
            .def("list_selected_row", &PyUi::list_selected_row, py::arg("list"))
            // ★★ P16-3a：多选三名（控件下限 #2）
            .def("list_set_multi", &PyUi::list_set_multi, py::arg("list"), py::arg("on"))
            .def("list_selected_rows", &PyUi::list_selected_rows, py::arg("list"))
            .def("list_set_selected_rows", &PyUi::list_set_selected_rows, py::arg("list"),
                 py::arg("rows") = py::none())
            .def("list_on_activate", &PyUi::list_on_activate, py::arg("list"),
                 py::arg("callback"))
            // ★★ P17-82：本机文件/目录拖到插件造的 tree/list 上（「本体拖入」）
            .def("list_on_drop", &PyUi::list_on_drop, py::arg("list"),
                 py::arg("callback") = py::none())
            .def("list_content_height", &PyUi::list_content_height, py::arg("list"))
            .def("log_view", &PyUi::log_view, py::arg("parent") = py::none(),
                 py::arg("height") = 0)
            .def("log_view_append", &PyUi::log_view_append, py::arg("view"), py::arg("line"))
            .def("log_view_clear", &PyUi::log_view_clear, py::arg("view"))
            .def("log_view_text", &PyUi::log_view_text, py::arg("view"))
            .def("drop_zone", &PyUi::drop_zone, py::arg("parent") = py::none(),
                 py::arg("hint") = "")
            .def("drop_zone_on_drop", &PyUi::drop_zone_on_drop, py::arg("zone"),
                 py::arg("on_drop") = py::none())
            .def("task_panel", &PyUi::task_panel, py::arg("parent") = py::none(),
                 py::arg("with_cancel") = true)
            .def("task_run", &PyUi::task_run, py::arg("panel"), py::arg("title"),
                 py::arg("worker"))
            .def("task_status", &PyUi::task_status, py::arg("panel"))
            .def("task_cancel", &PyUi::task_cancel, py::arg("panel"))
            .def("task_cancel_requested", &PyUi::task_cancel_requested, py::arg("panel"))
            .def("task_on_message", &PyUi::task_on_message, py::arg("panel"),
                 py::arg("handler"))
            .def("on_ui_thread", &PyUi::on_ui_thread)
            // ★ P5：重型 L2 第二批（滚动视口 / 可分栏）
            .def("viewport", &PyUi::viewport, py::arg("parent") = py::none(),
                 py::arg("min_body_height") = 0)
            .def("viewport_fit_content", &PyUi::viewport_fit_content, py::arg("body"),
                 py::arg("content_fit") = true)
            .def("viewport_set_min_body_height", &PyUi::viewport_set_min_body_height,
                 py::arg("body"), py::arg("min_body_height"))
            .def("viewport_sync", &PyUi::viewport_sync, py::arg("body"))
            .def("viewport_can_scroll", &PyUi::viewport_can_scroll, py::arg("body"))
            .def("viewport_scroll_top", &PyUi::viewport_scroll_top, py::arg("body"))
            .def("split", &PyUi::split, py::arg("parent") = py::none(),
                 py::arg("horizontal") = true)
            .def("split_add_pane", &PyUi::split_add_pane, py::arg("split"),
                 py::arg("minsize") = 0)
            .def("split_set_max_size", &PyUi::split_set_max_size, py::arg("split"),
                 py::arg("index"), py::arg("max_size"))
            .def("split_set_collapsed", &PyUi::split_set_collapsed, py::arg("split"),
                 py::arg("index"), py::arg("collapsed"))
            .def("split_is_collapsed", &PyUi::split_is_collapsed, py::arg("split"),
                 py::arg("index"))
            .def("split_pane_count", &PyUi::split_pane_count, py::arg("split"))
            .def("split_sash_pos", &PyUi::split_sash_pos, py::arg("split"), py::arg("index"))
            // ★★ P15-1a：标签页（fs 页两标签）
            .def("tabs", &PyUi::tabs, py::arg("parent") = py::none())
            .def("tabs_add_page", &PyUi::tabs_add_page, py::arg("tabs"),
                 py::arg("title"))
            .def("tabs_page_count", &PyUi::tabs_page_count, py::arg("tabs"))
            .def("tabs_set_current", &PyUi::tabs_set_current, py::arg("tabs"),
                 py::arg("index"))
            .def("tabs_current_index", &PyUi::tabs_current_index, py::arg("tabs"))
            // ★ P6：树（多列 / 懒建 / 展开态 / 过滤计数 / 三态勾选）
            .def("tree", &PyUi::tree, py::arg("parent") = py::none(), py::arg("columns") = py::none())
            .def("tree_set_columns", &PyUi::tree_set_columns, py::arg("tree"),
                 py::arg("columns"))
            .def("tree_on_action", &PyUi::tree_on_action, py::arg("tree"),
                 py::arg("mark"), py::arg("callback"))
            .def("tree_set_root", &PyUi::tree_set_root, py::arg("tree"), py::arg("nodes"))
            .def("tree_set_children", &PyUi::tree_set_children, py::arg("tree"),
                 py::arg("parent_key"), py::arg("nodes"))
            .def("tree_clear", &PyUi::tree_clear, py::arg("tree"))
            .def("tree_row_count", &PyUi::tree_row_count, py::arg("tree"))
            .def("tree_keys", &PyUi::tree_keys, py::arg("tree"))
            .def("tree_set_expanded", &PyUi::tree_set_expanded, py::arg("tree"), py::arg("key"),
                 py::arg("expanded"))
            .def("tree_expanded_keys", &PyUi::tree_expanded_keys, py::arg("tree"))
            .def("tree_set_checked", &PyUi::tree_set_checked, py::arg("tree"), py::arg("key"),
                 py::arg("checked"))
            .def("tree_checked", &PyUi::tree_checked, py::arg("tree"), py::arg("key"))
            .def("tree_checked_keys", &PyUi::tree_checked_keys, py::arg("tree"))
            .def("tree_partial_keys", &PyUi::tree_partial_keys, py::arg("tree"))
            .def("tree_set_filter", &PyUi::tree_set_filter, py::arg("tree"), py::arg("text"))
            .def("tree_filter_hits", &PyUi::tree_filter_hits, py::arg("tree"))
            .def("tree_select", &PyUi::tree_select, py::arg("tree"), py::arg("key"))
            .def("tree_selected_key", &PyUi::tree_selected_key, py::arg("tree"))
            // ★★ P15-0：多选读取（按树序）
            .def("tree_selected_keys", &PyUi::tree_selected_keys, py::arg("tree"))
            .def("tree_on_expand", &PyUi::tree_on_expand, py::arg("tree"),
                 py::arg("callback") = py::none())
            .def("tree_on_check", &PyUi::tree_on_check, py::arg("tree"),
                 py::arg("callback") = py::none())
            .def("tree_on_activate", &PyUi::tree_on_activate, py::arg("tree"),
                 py::arg("callback") = py::none())
            // ★★ P15-0：点行身体（宿主只报 key）
            .def("tree_on_click", &PyUi::tree_on_click, py::arg("tree"),
                 py::arg("callback") = py::none())
            // ★★ P17-82：本机文件/目录拖到这棵树上（与 list_on_drop 同一机制）
            .def("tree_on_drop", &PyUi::tree_on_drop, py::arg("tree"),
                 py::arg("callback") = py::none());
        py::class_<PyPluginApi>(m, "Api")
            .def_property_readonly("plugin_file", &PyPluginApi::owner)
            .def_property_readonly("plugin_dir", &PyPluginApi::plugin_dir)
            .def_property_readonly("api_version", [](const PyPluginApi&) { return kPyHostApiVersion; })
            // ★ P3：控件门面（`api.ui.*`）—— 表外无门，见 py_host.h 与能力表判据
            .def_property_readonly("ui", [](const PyPluginApi& api) { return PyUi(api.owner()); })
            // ★ P7a：引擎调用门面（`api.engine.*`；方案 3，表由样板页反向定义）
            .def_property_readonly("engine",
                                   [](const PyPluginApi& api) { return PyEngine(api.owner()); })
            // ★ P7b：宿主服务门面（`api.host.*`：偏好读写 / exe 目录 / 系统字体目录）
            .def_property_readonly("host",
                                   [](const PyPluginApi& api) { return PyHost(api.owner()); })
            .def("log", &PyPluginApi::log, py::arg("message"))
            .def("register_command", &PyPluginApi::register_command, py::arg("action_id"),
                 py::arg("label"), py::arg("handler"), py::arg("when") = py::none())
            .def("register_menu_item", &PyPluginApi::register_menu_item, py::arg("host"),
                 py::arg("location"), py::arg("command_id"), py::arg("order") = 0)
            .def("register_option_entry", &PyPluginApi::register_option_entry, py::arg("host"),
                 py::arg("area"), py::arg("value"), py::arg("when") = py::none())
            .def("register_option", &PyPluginApi::register_option, py::arg("label"),
                 py::arg("choices"), py::arg("callback") = py::none(), py::arg("host") = "fs",
                 py::arg("area") = "top_bar", py::arg("when") = py::none(), py::arg("order") = 0)
            // ★ P3：插件页 / 面板（控件门面的落点）
            .def("register_panel", &PyPluginApi::register_panel, py::arg("title"),
                 py::arg("builder"), py::arg("host") = "fs", py::arg("area") = "top_bar",
                 py::arg("order") = 0)
            .def("register_tool", &PyPluginApi::register_tool, py::arg("name"), py::arg("builder"),
                 py::arg("order") = 0, py::arg("replaces") = "", py::arg("subtitle") = "",
                 py::arg("title") = "");
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
// ★★ P7d：真正干活的实现 —— 公共入口与 `--py-plugin-call` 探针**共用这一份**。
//   `modules_out` 非空时把 (owner, module) 交出来：探针要按 owner 找插件的无界面入口，
//   而加载器**有意不把模块放进 `sys.modules`**（插件之间不许靠 import 互相串）⇒
//   探针没法靠模块名去 `sys.modules` 里找（实测取证：`modules=` 是空的）。
std::vector<RegistrationError> load_python_plugins_impl(
    const std::string& dir, Registry& registry, PyLoadSummary* summary,
    std::vector<std::pair<std::string, py::object>>* modules_out);

std::vector<RegistrationError> load_python_plugins_from_dir(const std::string& dir, Registry& registry,
                                                            PyLoadSummary* summary) {
    return load_python_plugins_impl(dir, registry, summary, nullptr);
}

std::vector<RegistrationError> load_python_plugins_impl(
    const std::string& dir, Registry& registry, PyLoadSummary* summary,
    std::vector<std::pair<std::string, py::object>>* modules_out) {
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
            if (modules_out != nullptr) modules_out->emplace_back(owner, module);
            // ★★ P7b（上一轮答"插件间兼容性"时承诺的判据）：**插件声明它按哪一版插件 API 写的**。
            //   声明 `PLUGIN_API = <int>`（缺省＝按当前版本）。**比宿主新 ⇒ 拒绝加载**（并留痕）：
            //   那种插件会调用宿主还不认识的能力，"进去之后调一半才炸"比"当场拒绝"难查得多；
            //   比宿主旧 ⇒ 照常加载（宿主契约只增不删，老插件必须还能跑）。
            //   ★ 检查放在 `register(api)` **之前** ⇒ 拒绝时**一条注册都不会留下**（不需要回滚，
            //   但下面那条回滚路径照旧兜底）。
            if (py::hasattr(module, "PLUGIN_API") != 0) {
                py::object declared = module.attr("PLUGIN_API");
                if (py::isinstance<py::int_>(declared) == 0) {
                    throw std::runtime_error("PLUGIN_API 必须是整数（声明插件按哪一版 API 写的）");
                }
                const int wanted = declared.cast<int>();
                if (wanted > kPyHostApiVersion) {
                    throw std::runtime_error(
                        "插件要求插件 API v" + std::to_string(wanted) + "，而本宿主只到 v" +
                        std::to_string(kPyHostApiVersion) + "（拒绝加载：请升级宿主或改用旧接口）");
                }
            }
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
    bool want_ui = false;
    bool want_plugin_call = false;
    std::string want_plugin_call_json;
    std::string want_plugin_call_plugin;
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
        if (a == "--ui-facade") {
            want_ui = true;
            continue;
        }
        if (a == "--py-plugin-call") {
            want_plugin_call = true;
            continue;
        }
        // ★ P9b：显式选插件（`--plugin <文件名>`）—— 插件目录里不止一枚时必须用它，
        //   否则"第一个导出 headless 的"一改就会静默换掉判据对象。
        if (want_plugin_call && a == "--plugin" && i + 1 < args.size()) {
            want_plugin_call_plugin = args[++i];
            continue;
        }
        if (want_plugin_call && want_plugin_call_json.empty() && a.size() > 0 &&
            a[0] == '{') {
            want_plugin_call_json = a;
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
    if (!want_host && !want_plugins && !want_ui && !want_plugin_call) {
        emit("PYHOST error reason=用法：--python-host | --py-plugins [目录] | --ui-facade | "
             "--py-plugin-call [目录] [JSON]");
        return 3;
    }

#ifdef XRKIT_WITH_PYTHON
    if (want_ui) {
        // ★★ P3 **能力表探针**：把 `xrkit` 模块里**实际暴露**的名字逐条打出来 ——
        //   判据（`tests/check_ui_capability_table.py`）拿它与**预期表**逐条对账，
        //   **多一个就红**（规格 §二「能力表：表外无门，静态判红」）。
        const PyHostStatus host = init_python(home);
        if (!host.initialized) {
            emit("UIFACADE host=failed reason=" + one_line(host.reason));
            return host.enabled ? 1 : 2;
        }
        std::string reason;
        const bool available = ui_facade_available(&reason);
        std::string methods;
        for (const std::string& name : ui_facade_method_names()) {
            if (!methods.empty()) methods += ",";
            methods += name;
        }
        emit(std::string("UIFACADE available=") + (available ? "1" : "0") + " methods=" + methods +
             " reason=" + one_line(reason));
        py::gil_scoped_acquire acquire;
        py::module_ module = py::module_::import("xrkit");
        py::object dir_fn = py::module_::import("builtins").attr("dir");
        for (const char* cls : {"Api", "Ui", "Widget", "Engine", "Host"}) {
            std::vector<std::string> names;
            for (auto item : py::list(dir_fn(module.attr(cls)))) {
                const std::string name = py::str(item).cast<std::string>();
                if (!name.empty() && name[0] == '_') continue;   // 下划线开头（dunder/内部）不算能力
                names.push_back(name);
            }
            std::sort(names.begin(), names.end());
            std::string joined;
            for (const std::string& name : names) {
                if (!joined.empty()) joined += ",";
                joined += name;
            }
            emit(std::string("UIFACADE ") + cls + "=" + joined);
        }
        return 0;
    }
#endif

    if (want_host) {
        const PyHostStatus started = init_python(home);
        emit(python_host_report());
        const bool enabled = started.enabled;
        const bool initialized = started.initialized;
        shutdown_python();
        if (!enabled) return 2;
        return initialized ? 0 : 1;
    }

    // ★★ P7d：`--py-plugin-call <目录> <json>` —— **无界面调用插件的非 UI 入口**。
    //   契约：插件导出模块级 `headless(api, args)`（args 是 JSON 解析出来的 dict），
    //   返回值用 `json.dumps` 打成一行交回来。样板页的"产物逐字节一致"就靠这条通道判
    //   （门禁里不能开 GUI）。`api.ui.*` 在这条路上不可用（没有界面），`api.engine.*` /
    //   `api.host.*` 可用。
    if (want_plugin_call) {
        const std::string call_dir = dir.empty() ? (xrkit::core::app_dir() + "/plugins") : dir;
        const PyHostStatus call_host = init_python(home);
        if (!call_host.enabled) {
            emit("PYPLUGINCALL reason=" + call_host.reason);
            return 2;
        }
        if (!call_host.initialized) {
            emit("PYPLUGINCALL error reason=解释器起不来：" + one_line(call_host.reason));
            return 1;
        }
        // ★ 这里**不能**套 `gil_scoped_acquire`：宿主 `init_python()` 用 `PyEval_SaveThread()`
        //   放开了 GIL，而 `load_python_plugins_from_dir()` 自己会取（与 `--py-plugins` 同口径）。
        //   外面再套一层 = 两套记账（`PyEval_SaveThread` 与 pybind11 的 `PyGILState_Ensure`）
        //   叠加 ⇒ 访问违例（本批实测：三个探针插件全崩 0xC0000005）。GIL 在下面**只取一次**。
        Registry call_registry;
        PyLoadSummary call_summary;
        // ★ 用**内部实现**多要一份"导入出来的模块"（加载器有意不放进 sys.modules）
        std::vector<std::pair<std::string, py::object>> call_modules;
        const std::vector<RegistrationError> call_errors =
            load_python_plugins_impl(call_dir, call_registry, &call_summary, &call_modules);
        emit("PYPLUGINCALL files=" + std::to_string(call_summary.files) +
             " loaded=" + std::to_string(call_summary.loaded) +
             " errors=" + std::to_string(call_errors.size()));
        if (call_summary.loaded == 0) {
            emit("PYPLUGINCALL error reason=目录里没有可加载的插件（" + one_line(call_dir) + "）");
            return 1;
        }
        // ★ GIL：**只取一次**，且是在加载器返回之后（见上面的注释 —— 嵌套会崩）。
        py::gil_scoped_acquire call_acquire;
        // 找**导出 headless 的那个插件**：遍历加载器交出来的模块表（加载序与注册一致）。
        //   ★ 不去查 `sys.modules`：加载器**有意不把插件模块放进去**（插件之间不许靠 import 互相串）
        //     —— 本批实测：`sys.modules` 里一个 `xrkit_plugin_*` 都没有。
        py::object entry;
        std::string entry_name;
        for (const auto& loaded : call_modules) {
            if (!want_plugin_call_plugin.empty() &&
                loaded.first != want_plugin_call_plugin) {
                continue;   // 显式点了名就只认它
            }
            py::object module = loaded.second;
            if (py::hasattr(module, "headless") == 0) continue;
            entry = module.attr("headless");
            entry_name = loaded.first;
            break;
        }
        // ★★ 判"没找到"必须用 `!entry`（**空** pybind11 对象），不能用 `entry.is_none()`：
        //   后者比的是 `m_ptr == Py_None`，对空指针返回 **false** ⇒ 判据放过 ⇒ 后面
        //   `entry(api, args)` 调了个空指针 ⇒ 0xC0000005（本批实测：三个探针插件全崩，
        //   而且崩点看着像在插件里 —— 其实是这一行判据的锅）。
        if (!entry) {
            // ★ 报账要**可诊断**：把"加载器交出来的插件"打出来（本批定位这条路径就靠它 ——
            //   光说一句"没有导出"什么也查不出来）。
            std::string loaded_names;
            for (const auto& loaded : call_modules) loaded_names += loaded.first + " ";
            emit("PYPLUGINCALL error reason=目录里没有插件导出 headless(api, args) loaded=" +
                 one_line(loaded_names) +
                 (want_plugin_call_plugin.empty() ? std::string()
                                                  : "（点名要：" + want_plugin_call_plugin + "）"));
            call_modules.clear();   // ★ 释放 Python 引用**必须在持锁时**（见文件头那条）
            return 1;
        }
        py::object args;
        try {
            args = want_plugin_call_json.empty()
                       ? py::dict()
                       : py::module_::import("json").attr("loads")(want_plugin_call_json);
        } catch (const py::error_already_set&) {
            emit("PYPLUGINCALL error reason=第二个操作数不是合法 JSON");
            call_modules.clear();
            return 3;
        }
        PyPluginApi* raw = new PyPluginApi(&call_registry, entry_name, call_dir);
        py::object api = py::cast(raw, py::return_value_policy::take_ownership);
        try {
            py::object result = entry(api, args);
            py::object dumped = py::module_::import("json").attr("dumps")(
                result, py::arg("ensure_ascii") = false, py::arg("sort_keys") = true);
            emit("PYPLUGINCALL plugin=" + entry_name +
                 " result=" + one_line(py::str(dumped).cast<std::string>()));
        } catch (const py::error_already_set& err) {
            emit("PYPLUGINCALL plugin=" + entry_name + " error reason=" + one_line(err.what()));
            call_modules.clear();
            return 1;
        }
        for (const std::string& line : take_python_log_lines()) {
            emit("PYPLUGINCALL log " + one_line(line));
        }
        // ★★ **释放模块引用要在持锁时做**：`call_modules` 里是 `py::object`，若在 GIL 之外析构
        //   （函数返回时）就是访问违例 —— 本批实测：crash 在"调用成功之后"，看着像巧合。
        call_modules.clear();
        return 0;
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
