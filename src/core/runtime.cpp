#include "xrkit/core/runtime.h"

#include "xrkit/core/path.h"

#include <filesystem>
#include <fstream>

namespace xrkit::core {
namespace {

namespace fs = std::filesystem;

bool exists_any(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    // 依赖可能是**目录**（如 `tools/fontlib`），所以"存在"就认。
    return fs::exists(path_from_utf8(path), ec);
}

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// 只认 `[runtime]` 段的极小 ltx 读取：`<app_dir>/xrkit_user.ltx`。
// （为什么不复用 `load_user_value`：它绑死在**本进程**的 app_dir 上，
//   而自检要拿一个临时 app 目录来验路由。）
std::string read_runtime_value(const std::string& app_dir, const std::string& key) {
    const fs::path file = path_from_utf8(app_dir) / "xrkit_user.ltx";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::string();
    std::ifstream in(file);
    if (!in) return std::string();
    bool in_runtime = false;
    std::string line;
    while (std::getline(in, line)) {
        std::string text = trim(line);
        if (text.empty()) continue;
        if (text[0] == ';' || text[0] == '#') continue;
        if (text[0] == '[') {
            const size_t close = text.find(']');
            const std::string section =
                close == std::string::npos ? text.substr(1) : text.substr(1, close - 1);
            in_runtime = (section == "runtime");
            continue;
        }
        if (!in_runtime) continue;
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        if (trim(text.substr(0, eq)) != key) continue;
        return trim(text.substr(eq + 1));
    }
    return std::string();
}

}  // namespace

const char* runtime_source_name(RuntimeSource source) {
    switch (source) {
        case RuntimeSource::Explicit: return "explicit";
        case RuntimeSource::Root: return "root";
        case RuntimeSource::AppDefault: return "app";
        case RuntimeSource::Missing: break;
    }
    return "missing";
}

std::string expand_runtime_tokens(const std::string& value, const std::string& app_dir,
                                 const std::string& root_value) {
    std::string out = value;
    const auto replace_all = [](std::string& target, const std::string& from,
                                const std::string& to) {
        if (from.empty()) return;
        size_t at = 0;
        while ((at = target.find(from, at)) != std::string::npos) {
            target.replace(at, from.size(), to);
            at += to.size();
        }
    };
    replace_all(out, "@APP@", app_dir);
    replace_all(out, "${root}", root_value);
    return out;
}

std::string runtime_root(const std::string& app_dir) {
    const std::string raw = read_runtime_value(app_dir, "root");
    if (raw.empty()) return std::string();
    // root 自己也可能写 `@APP@`（正式版就是它）。
    return expand_runtime_tokens(raw, app_dir, std::string());
}

RuntimeDep resolve_runtime_dep(const std::string& app_dir, const std::string& key,
                               const std::string& under_root, const std::string& under_app) {
    RuntimeDep dep;
    const std::string root = runtime_root(app_dir);

    // ① 单项显式路径
    if (const std::string explicit_value = read_runtime_value(app_dir, key);
        !explicit_value.empty()) {
        const std::string expanded = expand_runtime_tokens(explicit_value, app_dir, root);
        if (exists_any(expanded)) {
            dep.path = expanded;
            dep.source = RuntimeSource::Explicit;
            return dep;
        }
        // 显式写了却不存在 ⇒ 不静默换别的（交给调用方报错）；
        // 但仍允许 ③ 的工具目录兜底，避免"用户指了旧路径"直接把功能打死。
    }

    // ② root + 约定子路径
    if (!root.empty() && !under_root.empty()) {
        const std::string candidate =
            (fs::path(path_from_utf8(root)) / path_from_utf8(under_root)).string();
        if (exists_any(candidate)) {
            dep.path = candidate;
            dep.source = RuntimeSource::Root;
            return dep;
        }
    }

    // ③ 工具目录默认
    if (!under_app.empty()) {
        const std::string candidate =
            (fs::path(path_from_utf8(app_dir)) / path_from_utf8(under_app)).string();
        if (exists_any(candidate)) {
            dep.path = candidate;
            dep.source = RuntimeSource::AppDefault;
            return dep;
        }
    }
    return dep;   // ④ 空 ⇒ 调用方必须报错
}

}  // namespace xrkit::core
