// xrkit::plugin —— **宿主服务门面的默认表**（2026-10-07 P7b）。
//
// ★ 住在**引擎**层：这四件（偏好读写 / exe 目录 / 系统字体目录）只依赖引擎与操作系统，
//   不依赖 Qt、也不依赖外壳 ⇒ CLI（`xrkit_cli.exe --py-plugins`）与 GUI 两个宿主都能用。
//   （同 P7a 的教训：把只依赖引擎的东西放外壳，CLI 那个宿主就用不到。）

#include "xrkit/engine/process/run.h"
#include "xrkit/engine/plugin/host_facade.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/path.h"
#include "xrkit/core/version.h"
#include "xrkit/engine/font/system_dirs.h"

#include <string>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace xrkit::plugin {
namespace {

namespace core = xrkit::core;
namespace efont = xrkit::engine::font;
namespace eprocess = xrkit::engine::process;
namespace fs = std::filesystem;

std::string safe_owner(const std::string& owner) {
    std::string out;
    for (char c : owner) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((std::isalnum(u) != 0 || c == '_' || c == '-' || c == '.') ? c : '_');
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    return out.empty() ? std::string("plugin") : out;
}

fs::path plugin_data_root(const std::string& owner) {
    const char* local = std::getenv("LOCALAPPDATA");
    fs::path base = (local != nullptr && *local != '\0')
                        ? fs::u8path(local) / "xrkit" / "plugins"
                        : fs::u8path(core::app_dir()) / "plugins" / ".data";
    return base / safe_owner(owner);
}

std::string ensure_dir(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) return std::string();
    return core::path_to_utf8(path);
}

#ifdef _WIN32
std::wstring to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

HANDLE background_job() {
    static HANDLE job = [] {
        HANDLE handle = CreateJobObjectW(nullptr, nullptr);
        if (handle != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &info,
                                    sizeof(info));
        }
        return handle;
    }();
    return job;
}
#endif

const HostFacade kHostFacade = {
    /* pref_get：读 `user.ltx` 的 `[section] key`（缺失返回 fallback） */
    [](const std::string& section, const std::string& key, const std::string& fallback) {
        return core::load_user_value(section, key, fallback);
    },
    /* pref_set：只写点名的那一项（不整块覆盖别的键） */
    [](const std::string& section, const std::string& key, const std::string& value) {
        return core::save_user_value(section, key, value);
    },
    /* app_dir：exe 所在目录（发行版放 ffmpeg/ffprobe 的地方、`user.ltx` 的落点） */
    []() { return core::app_dir(); },
    /* data_dir：per-plugin 持久目录（插件更新不删除） */
    [](const std::string& owner) { return ensure_dir(plugin_data_root(owner)); },
    /* cache_dir：per-plugin 可清理缓存 */
    [](const std::string& owner) { return ensure_dir(plugin_data_root(owner) / "cache"); },
    /* system_font_dirs：唯一一份（内置页与插件共用；逐条照 1.x） */
    []() { return efont::system_font_dirs(); },
    /* run_process：**唯一一份**进程语义（CREATE_NO_WINDOW / 真停 / 如实退码 / stderr 尾巴） */
    [](const ProcessRunOptions& options) {
        eprocess::RunOptions native;
        native.program = options.program;
        native.args = options.args;
        native.working_dir = options.working_dir;
        native.stderr_tail_bytes = options.stderr_tail_bytes;
        native.cancel_wait_ms = options.cancel_wait_ms;
        native.should_stop = options.should_stop;
        native.on_stdout = options.on_stdout;
        native.on_stderr = options.on_stderr;
        const eprocess::RunResult native_result = eprocess::run(native);
        ProcessRunResult out;
        out.started = native_result.started;
        out.cancelled = native_result.cancelled;
        out.cancel_timeout = native_result.cancel_timeout;
        out.normal_exit = native_result.normal_exit;
        out.exit_code = native_result.exit_code;
        out.error = native_result.error;
        out.stderr_tail = native_result.stderr_tail;
        out.stdout_lines = native_result.stdout_lines;
        out.stderr_lines = native_result.stderr_lines;
        return out;
    },
    /* start_process：长驻/后台进程；无管道，日志重定向到文件；Job kill-on-close */
    [](const ProcessStartOptions& options) {
        ProcessStartResult out;
#ifdef _WIN32
        if (options.program.empty()) {
            out.error = "program 为空";
            return out;
        }
        const std::wstring program = to_wide(options.program);
        std::wstring command = to_wide(eprocess::build_command_line(options.program, options.args));
        std::wstring working;
        if (!options.working_dir.empty()) working = to_wide(options.working_dir);
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        const std::wstring log_path = options.log_path.empty() ? L"NUL" : to_wide(options.log_path);
        HANDLE log_handle = CreateFileW(log_path.c_str(), GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log_handle == INVALID_HANDLE_VALUE || nul_in == INVALID_HANDLE_VALUE) {
            if (log_handle != INVALID_HANDLE_VALUE) CloseHandle(log_handle);
            if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
            out.error = "无法打开日志/NUL 句柄";
            return out;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul_in;
        si.hStdOutput = log_handle;
        si.hStdError = log_handle;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(program.c_str(), command.data(), &sa, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       working.empty() ? nullptr : working.c_str(), &si, &pi);
        CloseHandle(log_handle);
        CloseHandle(nul_in);
        if (!ok) {
            out.error = "CreateProcess 失败，错误码 " + std::to_string(GetLastError());
            return out;
        }
        HANDLE job = background_job();
        if (job != nullptr) AssignProcessToJobObject(job, pi.hProcess);
        if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
            out.error = "ResumeThread 失败";
            TerminateProcess(pi.hProcess, 1);
        } else {
            out.started = true;
            out.pid = static_cast<unsigned long>(pi.dwProcessId);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
#else
        out.error = "当前构建不支持后台进程";
#endif
        return out;
    },
    /* stop_process：终止并最多等 5 秒 */
    [](unsigned long pid) {
#ifdef _WIN32
        HANDLE handle = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (handle == nullptr) return false;
        const bool ok = TerminateProcess(handle, 1) != 0;
        WaitForSingleObject(handle, 5000);
        CloseHandle(handle);
        return ok;
#else
        (void)pid;
        return false;
#endif
    },
    /* process_alive */
    [](unsigned long pid) {
#ifdef _WIN32
        HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    static_cast<DWORD>(pid));
        if (handle == nullptr) return false;
        const DWORD waited = WaitForSingleObject(handle, 0);
        CloseHandle(handle);
        return waited == WAIT_TIMEOUT;
#else
        (void)pid;
        return false;
#endif
    },
    /* restart：GUI 外壳注册处理器；CLI/无壳宿主返回 false */
    []() { return host_restart(); },
    /* quit：GUI 外壳注册处理器；CLI/无壳宿主返回 false */
    []() { return host_quit(); },
    /* version：本体版本字符串（插件检查更新用） */
    []() { return xrkit::core::version_string(); },
    /* pid：当前宿主进程号（本体 updater 等待退出用） */
    []() -> unsigned long {
#ifdef _WIN32
        return static_cast<unsigned long>(GetCurrentProcessId());
#else
        return static_cast<unsigned long>(getpid());
#endif
    },
};

}  // namespace

const HostFacade* builtin_host_facade() {
    static const HostFacade instance = kHostFacade;
    return &instance;
}

}  // namespace xrkit::plugin
