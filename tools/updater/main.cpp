// xrkit_updater.exe —— 本体更新事务的外部执行器。
//
// 用法：
//   xrkit_updater.exe --pid <进程号> --target <安装目录> --staged <新版本目录> \
//                     --transaction <临时事务目录> [--exe <安装目录\xrkit_app.exe>] [--wait-ms 10000]
//
// 流程：等旧进程退出 → 备份/替换（临时事务）→ 启动新版本 → 观察 wait-ms：
//   · 新进程还活着 ⇒ commit（删除备份）
//   · 启动失败/提前退出 ⇒ rollback（恢复旧目录）
//
// 只依赖 core，不链接 Qt；退出码：0=成功 / 1=回滚 / 3=用法错误。

#include "xrkit/core/update_transaction.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::string to_utf8(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
    return out;
#else
    return std::string(value.begin(), value.end());
#endif
}

bool wait_pid(unsigned long pid) {
#ifdef _WIN32
    if (pid == 0) return true;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (handle == nullptr) return true;  // 已经退出
    const DWORD result = WaitForSingleObject(handle, 60000);
    CloseHandle(handle);
    return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
#else
    (void)pid;
    return true;
#endif
}

bool launch_and_check(const std::wstring& exe, const std::wstring& workdir,
                      unsigned long wait_ms) {
#ifdef _WIN32
    if (exe.empty()) return true;  // 无 exe：只做文件事务
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
    buffer.push_back(L'\0');
    const BOOL ok = CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0,
                                   nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si,
                                   &pi);
    if (!ok) {
        std::printf("UPDATER detail=CreateProcess failed err=%lu exe=%ls\n",
                    static_cast<unsigned long>(GetLastError()), exe.c_str());
        return false;
    }
    const DWORD waited = WaitForSingleObject(pi.hProcess, wait_ms);
    if (waited == WAIT_TIMEOUT) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;  // 新进程仍活着
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    // 双保险：某些环境下句柄等待会提前返回 signaled，但进程仍活着；
    // 以 PID 再问一次，活着就算启动成功（避免误回滚）。
    HANDLE alive = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                               pi.dwProcessId);
    if (alive != nullptr) {
        CloseHandle(alive);
        std::printf("UPDATER detail=new process alive pid=%lu (wait=%lu code=%lu)\n",
                    static_cast<unsigned long>(pi.dwProcessId),
                    static_cast<unsigned long>(waited), static_cast<unsigned long>(code));
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    std::printf("UPDATER detail=new process exited code=%lu wait=%lu\n",
                static_cast<unsigned long>(code), static_cast<unsigned long>(waited));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;  // 提前正常退出也算可用；崩溃/非 0 回滚
#else
    (void)exe;
    (void)workdir;
    (void)wait_ms;
    return true;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::string target, staged, transaction, exe;
    unsigned long pid = 0;
    unsigned long wait_ms = 10000;
    unsigned long parent_pid = 0;
    bool from_temp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string* out) {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        if (arg == "--pid") {
            std::string value;
            if (!next(&value)) return 3;
            try {
                pid = std::stoul(value);
            } catch (...) {
                return 3;
            }
        } else if (arg == "--target") {
            if (!next(&target)) return 3;
        } else if (arg == "--staged") {
            if (!next(&staged)) return 3;
        } else if (arg == "--transaction") {
            if (!next(&transaction)) return 3;
        } else if (arg == "--exe") {
            if (!next(&exe)) return 3;
        } else if (arg == "--wait-ms") {
            std::string value;
            if (!next(&value)) return 3;
            try {
                wait_ms = std::stoul(value);
            } catch (...) {
                return 3;
            }
        } else if (arg == "--from-temp") {
            from_temp = true;
        } else if (arg == "--parent-pid") {
            std::string value;
            if (!next(&value)) return 3;
            try {
                parent_pid = std::stoul(value);
            } catch (...) {
                return 3;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: xrkit_updater --pid N --target DIR --staged DIR --transaction DIR "
                        "[--exe APP] [--wait-ms N]\n");
            return 0;
        } else {
            return 3;
        }
    }
    if (target.empty() || staged.empty() || transaction.empty()) return 3;

#ifdef _WIN32
    // updater 自己通常就在被替换的安装目录里；先复制到事务目录再让子进程执行，
    // 否则 remove_all(target) 会撞到正在运行的 updater.exe。
    if (!from_temp) {
        std::error_code ec;
        std::filesystem::create_directories(transaction, ec);
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::filesystem::path copy =
            std::filesystem::u8path(transaction + ".runner.exe");
        std::filesystem::copy_file(self, copy, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (!ec) {
            std::wstring cmd = L"\"" + copy.wstring() + L"\"";
            for (int i = 1; i < argc; ++i) {
                cmd += L" \"" + std::filesystem::u8path(argv[i]).wstring() + L"\"";
            }
            cmd += L" --from-temp";
            cmd += L" --parent-pid " + std::to_wstring(GetCurrentProcessId());
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
            buffer.push_back(L'\0');
            if (CreateProcessW(copy.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
                               nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                std::printf("UPDATER handoff ok transaction=%s\n", transaction.c_str());
                return 0;
            }
        }
        // 复制/拉起失败：继续尝试原地执行（可能因自身被锁而失败，会走回滚）。
    }
#endif

    if (!wait_pid(parent_pid)) {
        std::printf("UPDATER error reason=父 updater 未退出\n");
        return 1;
    }
    if (!wait_pid(pid)) {
        std::printf("UPDATER error reason=旧进程未退出\n");
        return 1;
    }
    xrkit::core::UpdatePaths paths{target, staged, transaction};
    xrkit::core::UpdateResult result = xrkit::core::update_prepare(paths);
    if (result.ok) result = xrkit::core::update_replace(paths);
    if (!result.ok) {
        std::printf("UPDATER error reason=替换失败：%s\n", result.error.c_str());
        xrkit::core::update_rollback(paths);
        return 1;
    }

    const std::filesystem::path target_path = std::filesystem::u8path(target);
    std::wstring exe_w;
    if (!exe.empty()) {
        exe_w = std::filesystem::u8path(exe).wstring();
    } else {
        const std::filesystem::path candidate = target_path / "xrkit_app.exe";
        if (std::filesystem::exists(candidate)) exe_w = candidate.wstring();
    }
    const bool launched = launch_and_check(exe_w, target_path.wstring(), wait_ms);
    if (!launched) {
        std::printf("UPDATER error reason=新版本启动失败，回滚\n");
        xrkit::core::update_rollback(paths);
        return 1;
    }
    result = xrkit::core::update_commit(paths);
    if (!result.ok) {
        std::printf("UPDATER error reason=提交失败：%s\n", result.error.c_str());
        return 1;
    }
    std::printf("UPDATER ok target=%s\n", target.c_str());
    return 0;
}
