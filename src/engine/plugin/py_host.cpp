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

private:
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
    py::object entry(py::object parent, std::string initial, int width, bool password) const {
        if (const UiFacade* facade = available("entry")) {
            return wrap(facade->entry(handle_of(parent), initial, width, password));
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
    py::object drop_box(py::object parent, py::object on_file, std::string placeholder) const {
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
        return wrap(facade->drop_box(handle_of(parent), std::move(file), placeholder));
    }
    void pack(py::object parent, py::object child, std::string side, std::string fill, bool expand,
              int padx, int pady) const {
        if (const UiFacade* facade = available("pack")) {
            facade->pack(handle_of(parent), handle_of(child), side, fill, expand, padx, pady);
        }
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
    std::string ask_open_file(std::string title) const {
        if (const UiFacade* facade = available("ask_open_file")) {
            return facade->ask_open_file(title);
        }
        return std::string();
    }
    std::string ask_save_file(std::string title) const {
        if (const UiFacade* facade = available("ask_save_file")) {
            return facade->ask_save_file(title);
        }
        return std::string();
    }
    std::string ask_directory(std::string title) const {
        if (const UiFacade* facade = available("ask_directory")) {
            return facade->ask_directory(title);
        }
        return std::string();
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
    void list_select_row(py::object list_widget, int row) const {
        if (const UiFacade* facade = available("list_select_row")) {
            facade->list_select_row(handle_of(list_widget), row);
        }
    }
    int list_selected_row(py::object list_widget) const {
        if (const UiFacade* facade = available("list_selected_row")) {
            return facade->list_selected_row(handle_of(list_widget));
        }
        return -1;
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
            .def("system_font_dirs", &PyHost::system_font_dirs);
        py::class_<PyEngine>(m, "Engine")
            .def("font_check", &PyEngine::font_check, py::arg("font_path"))
            .def("font_pack", &PyEngine::font_pack, py::arg("options"))
            .def("font_state_marks", &PyEngine::font_state_marks)
            .def("font_size_slots", &PyEngine::font_size_slots);
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
            .def("entry", &PyUi::entry, py::arg("parent") = py::none(), py::arg("initial") = "",
                 py::arg("width") = 0, py::arg("password") = false)
            .def("text", &PyUi::text, py::arg("parent") = py::none(), py::arg("height") = 0,
                 py::arg("width") = 0, py::arg("wrap") = "word")
            .def("combo", &PyUi::combo, py::arg("parent") = py::none(), py::arg("values"),
                 py::arg("on_change") = py::none())
            .def("drop_box", &PyUi::drop_box, py::arg("parent") = py::none(),
                 py::arg("on_file") = py::none(), py::arg("placeholder") = "")
            .def("pack", &PyUi::pack, py::arg("parent") = py::none(), py::arg("child"),
                 py::arg("side") = "top", py::arg("fill") = "x", py::arg("expand") = false,
                 py::arg("padx") = 0, py::arg("pady") = 0)
            .def("text_view", &PyUi::text_view, py::arg("parent") = py::none(),
                 py::arg("height") = 0, py::arg("mono") = true)
            .def("set_text", &PyUi::set_text, py::arg("widget"), py::arg("text"))
            .def("get_text", &PyUi::get_text, py::arg("widget"))
            .def("combo_value", &PyUi::combo_value, py::arg("widget"))
            .def("checkbox_state", &PyUi::checkbox_state, py::arg("widget"))
            .def("set_enabled", &PyUi::set_enabled, py::arg("widget"), py::arg("enabled"))
            .def("alive", &PyUi::alive, py::arg("widget"))
            .def("log", &PyUi::log, py::arg("line"))
            .def("ask_yes_no", &PyUi::ask_yes_no, py::arg("title"), py::arg("message"))
            .def("ask_open_file", &PyUi::ask_open_file, py::arg("title"))
            .def("ask_save_file", &PyUi::ask_save_file, py::arg("title"))
            .def("ask_directory", &PyUi::ask_directory, py::arg("title"))
            // ★ P4：重型 L2 第一批（表外无门 —— 能力表判据按这里的名单对账）
            .def("list", &PyUi::list, py::arg("parent") = py::none(), py::arg("row_height") = 0)
            .def("list_set_row_count", &PyUi::list_set_row_count, py::arg("list"), py::arg("rows"))
            .def("list_set_row_text", &PyUi::list_set_row_text, py::arg("list"),
                 py::arg("provider") = py::none())
            .def("list_row_text", &PyUi::list_row_text, py::arg("list"), py::arg("row"))
            .def("list_select_row", &PyUi::list_select_row, py::arg("list"), py::arg("row"))
            .def("list_selected_row", &PyUi::list_selected_row, py::arg("list"))
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
            // ★ P6：树（多列 / 懒建 / 展开态 / 过滤计数 / 三态勾选）
            .def("tree", &PyUi::tree, py::arg("parent") = py::none(), py::arg("columns") = py::none())
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
            .def("tree_on_expand", &PyUi::tree_on_expand, py::arg("tree"),
                 py::arg("callback") = py::none())
            .def("tree_on_check", &PyUi::tree_on_check, py::arg("tree"),
                 py::arg("callback") = py::none())
            .def("tree_on_activate", &PyUi::tree_on_activate, py::arg("tree"),
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
    if (!want_host && !want_plugins && !want_ui) {
        emit("PYHOST error reason=用法：--python-host | --py-plugins [目录] | --ui-facade");
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
