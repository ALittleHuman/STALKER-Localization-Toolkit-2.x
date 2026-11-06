#include "xrkit/engine/font/ixr.h"

#include "xrkit/core/path.h"
#include "xrkit/core/runtime.h"
#include "xrkit/core/theme.h"   // app_dir()
#include "xrkit/engine/process/run.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

namespace fs = std::filesystem;

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
}

}  // namespace

IxrTools ixr_resolve_tools() {
    IxrTools tools;
    const fs::path app = core::path_from_utf8(core::app_dir());

    // ① python：显式环境变量 → 包内 python/python.exe → PATH 里的 python
    const std::string env_python = env_or_empty("XRKIT_IXR_PYTHON");
    if (!env_python.empty()) {
        tools.python = env_python;
    } else if (const xrkit::core::RuntimeDep dep = xrkit::core::resolve_runtime_dep(
                   core::path_to_utf8(app), "python", "python/python.exe", "python/python.exe");
               !dep.path.empty()) {
        tools.python = dep.path;   // ★ runtime 路由（迭代期共享 root / 正式版 @APP@）
    } else if (const fs::path packaged = app / "python" / "python.exe"; is_file(packaged)) {
        tools.python = core::path_to_utf8(packaged);
    } else {
        // ★ 2026-11-06（DSH）：**不许回落到 PATH 上的 python** —— Windows 上那常是
        //   WindowsApps 占位符（exit 9009），错误还发生在生成器里，很难归因。
        tools.error = "找不到包内 python（试过：XRKIT_IXR_PYTHON、<exe>/python/python.exe、"
                      "runtime 路由）—— 发行包应自带；开发态请显式设置 XRKIT_IXR_PYTHON";
        return tools;
    }

    // ② 启动器：显式环境变量 → 包内 tools/ixr_run.py
    const std::string env_run = env_or_empty("XRKIT_IXR_RUN");
    if (!env_run.empty()) {
        tools.launcher = env_run;
    } else if (const xrkit::core::RuntimeDep dep = xrkit::core::resolve_runtime_dep(
                   core::path_to_utf8(app), "ixr_run", "tools/ixr_run.py", "tools/ixr_run.py");
               !dep.path.empty()) {
        tools.launcher = dep.path;   // ★ runtime 路由
    } else if (const fs::path packaged = app / "tools" / "ixr_run.py"; is_file(packaged)) {
        tools.launcher = core::path_to_utf8(packaged);
    } else {
        tools.error = "找不到 IX-Ray 生成器启动器（tools/ixr_run.py）——"
                      "发行包里应当自带；开发态用环境变量 XRKIT_IXR_RUN 指过去";
        return tools;
    }
    if (!is_file(core::path_from_utf8(tools.launcher))) {
        tools.error = "启动器不存在：" + tools.launcher;
        return tools;
    }
    return tools;
}

IxrBuildResult ixr_build(const IxrBuildOptions& options) {
    IxrBuildResult out;
    const IxrTools tools = ixr_resolve_tools();
    out.python = tools.python;
    out.launcher = tools.launcher;
    if (!tools.error.empty()) {
        out.error = tools.error;
        return out;
    }

    process::RunOptions run;
    run.program = tools.python;
    run.args.push_back(tools.launcher);
    run.args.push_back("--pack");
    run.args.push_back(options.pack);
    run.args.push_back("--font");
    run.args.push_back(options.font);
    run.args.push_back("--out");
    run.args.push_back(options.out);
    if (!options.langs.empty()) {
        run.args.push_back("--langs");
        run.args.push_back(options.langs);
    }
    if (options.all_langs) run.args.push_back("--all-langs");
    if (options.split_output) run.args.push_back("--split-output");
    if (!options.text_root.empty()) {
        run.args.push_back("--text-root");
        run.args.push_back(options.text_root);
    }
    if (!options.plan.empty()) {
        run.args.push_back("--plan");
        run.args.push_back(options.plan);
    }
    if (!options.sizes.empty()) {
        run.args.push_back("--sizes");
        run.args.push_back(options.sizes);
    }
    if (!options.merge_glyphs.empty()) {
        run.args.push_back("--merge-glyphs");
        run.args.push_back(options.merge_glyphs);
    }
    run.on_stdout = [&out](const std::string& line) {
        out.stdout_tail += line;
        out.stdout_tail += '\n';
        if (out.stdout_tail.size() > 65536) {
            out.stdout_tail.erase(0, out.stdout_tail.size() - 65536);
        }
    };
    run.on_stderr = [&out](const std::string& line) {
        out.stderr_tail += line;
        out.stderr_tail += '\n';
        if (out.stderr_tail.size() > 65536) {
            out.stderr_tail.erase(0, out.stderr_tail.size() - 65536);
        }
    };

    const process::RunResult result = process::run(run);
    out.exit_code = result.exit_code;
    if (!result.started) {
        out.error = "生成器起不来（python=" + tools.python + "）：" + result.error;
        return out;
    }
    if (result.exit_code != 0) {
        // 零静默：把助手自己说的话带出去（它已经会写 "IXRGEN error reason=…"）
        out.error = "生成器失败（exit=" + std::to_string(result.exit_code) + "）";
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace xrkit::engine::font
