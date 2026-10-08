// xrkit::engine::process —— 跑外部进程的唯一实现（语义见 `run.h`）。
//
// ★★ 2026-10-08（P12b）：第一版用"stdout 主循环 + stderr 读线程"，判据当场跑挂（>10 分钟）——
//   真因是**终止 `cmd` 不终止它的孙进程**：孙进程还攥着管道写端 ⇒ 读端永不 EOF ⇒
//   读线程 `join()` 永远等下去。修法三条（每条都对应一个"产品语义"）：
//     ① **作业对象**（Job Object + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）：取消时
//        `TerminateJobObject` ⇒ **整棵树**停；收尾 `CloseHandle(job)` ⇒ 残留孙进程一起收；
//     ② **不再起读线程**：两条管道都用 `PeekNamedPipe` 轮询（同一循环里读），
//        ⇒ 没有"等线程"这一步，也就不可能被卡住；
//     ③ 进程已退出且两条管道都空了 ⇒ 立刻收尾；另有**兜底**（退出后最多再等 2 秒排空）。
//
// Windows：`CreateProcessW` + `CREATE_NO_WINDOW`（门禁 `check_no_console.py` 认这一条）。
// 其他平台：**如实说没实现**（不糊一个没测过的路径过去）。

#include "xrkit/engine/process/run.h"

#include "xrkit/core/path.h"

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace xrkit::engine::process {
namespace {

void append_tail(std::string& tail, const std::string& text, std::size_t limit) {
    tail += text;
    if (limit > 0 && tail.size() > limit) {
        tail.erase(0, tail.size() - limit);   // ★ 只留尾巴（界面显示的就是它）
    }
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// 命令行引号规则照 Windows 的规矩（`CommandLineToArgvW` 的反函数）：带空格/引号的参数要包起来。
std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (char ch : arg) {
        if (ch == '"') out += "\\\"";
        else out += ch;
    }
    out += "\"";
    return out;
}

// 逐行切（`\r\n` / `\n` 都认；最后一段不完整也交出去，宁可多一条行也不吞字节）
void feed_lines(std::string& pending, const char* data, size_t size,
                const std::function<void(const std::string&)>& sink) {
    if (sink == nullptr) {
        pending.clear();
        return;
    }
    pending.append(data, size);
    size_t start = 0;
    while (true) {
        const size_t pos = pending.find('\n', start);
        if (pos == std::string::npos) break;
        std::string line = pending.substr(start, pos - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        sink(line);
        start = pos + 1;
    }
    if (start > 0) pending.erase(0, start);
}

// 把一条管道里**当前可读**的都读出来（`PeekNamedPipe` 说没有就不读 ⇒ 不阻塞）
void drain_pipe(HANDLE pipe, std::string& pending, std::size_t tail_limit,
                std::string* tail, uint64_t* lines,
                const std::function<void(const std::string&)>& sink) {
    char buffer[4096];
    while (true) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == 0) return;
        if (available == 0) return;
        DWORD read = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) return;
        const auto sink_wrapper = [&](const std::string& line) {
            if (tail != nullptr) append_tail(*tail, line + "\n", tail_limit);
            if (lines != nullptr) ++(*lines);
            if (sink) sink(line);
        };
        feed_lines(pending, buffer, read, sink_wrapper);
    }
}

}  // namespace

RunResult run(const RunOptions& options) {
    RunResult result;
    if (options.program.empty()) {
        result.error = "程序名为空";
        return result;
    }

    std::string command = quote_arg(options.program);
    for (const std::string& arg : options.args) {
        command += " ";
        command += quote_arg(arg);
    }

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    if (CreatePipe(&out_read, &out_write, &attributes, 0) == 0) {
        result.error = "建 stdout 管道失败";
        return result;
    }
    if (CreatePipe(&err_read, &err_write, &attributes, 0) == 0) {
        CloseHandle(out_read);
        CloseHandle(out_write);
        result.error = "建 stderr 管道失败";
        return result;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    // ★ 作业对象：`KILL_ON_JOB_CLOSE` ⇒ 取消/收尾时**整棵树**一起收（孙进程也跑不掉）
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = out_write;
    startup.hStdError = err_write;
    PROCESS_INFORMATION info{};

    std::wstring command_line = widen(command);
    std::wstring working = widen(options.working_dir);
    // ★★ `CREATE_NO_WINDOW`：门禁 `check_no_console.py` 按文件判这一条（缺了就弹 cmd 窗口）
    const DWORD flags = CREATE_NO_WINDOW;
    const BOOL created = CreateProcessW(
        nullptr, command_line.empty() ? nullptr : command_line.data(), nullptr, nullptr, TRUE,
        flags, nullptr, working.empty() ? nullptr : working.c_str(), &startup, &info);
    // 父进程侧的两个写端要立刻关掉，否则读端永远等不到 EOF
    CloseHandle(out_write);
    CloseHandle(err_write);
    if (created == 0) {
        CloseHandle(out_read);
        CloseHandle(err_read);
        if (job != nullptr) CloseHandle(job);
        result.error = "起不来（CreateProcess 失败，错误码 " + std::to_string(GetLastError()) + "）";
        return result;
    }
    result.started = true;
    CloseHandle(info.hThread);
    if (job != nullptr) AssignProcessToJobObject(job, info.hProcess);

    std::string out_pending;
    std::string err_pending;
    bool stop_requested = false;
    bool exited = false;
    auto exited_at = std::chrono::steady_clock::now();
    while (true) {
        if (!stop_requested && options.should_stop && options.should_stop()) {
            stop_requested = true;
            if (job != nullptr) {
                TerminateJobObject(job, 1);   // ★ 整棵树
            } else {
                TerminateProcess(info.hProcess, 1);
            }
            if (WaitForSingleObject(info.hProcess, static_cast<DWORD>(
                                                        std::max(0, options.cancel_wait_ms))) ==
                WAIT_TIMEOUT) {
                result.cancel_timeout = true;   // ★ 没死透就如实说，不假装停了
            }
        }
        drain_pipe(out_read, out_pending, 0, nullptr, &result.stdout_lines, options.on_stdout);
        drain_pipe(err_read, err_pending, options.stderr_tail_bytes, &result.stderr_tail,
                   &result.stderr_lines, options.on_stderr);
        if (!exited && WaitForSingleObject(info.hProcess, 0) == WAIT_OBJECT_0) {
            exited = true;
            exited_at = std::chrono::steady_clock::now();
        }
        if (exited) {
            // 退出后再给 300 ms 把管道排空（孙进程可能还写着一点），最多兜底 2 秒
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - exited_at)
                                    .count();
            DWORD out_available = 0;
            DWORD err_available = 0;
            PeekNamedPipe(out_read, nullptr, 0, nullptr, &out_available, nullptr);
            PeekNamedPipe(err_read, nullptr, 0, nullptr, &err_available, nullptr);
            if ((out_available == 0 && err_available == 0 && waited > 300) || waited > 2000) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 尾段（最后一段没有换行的内容也要交出去）
    if (!out_pending.empty()) {
        if (options.on_stdout) options.on_stdout(out_pending);
        ++result.stdout_lines;
    }
    if (!err_pending.empty()) {
        append_tail(result.stderr_tail, err_pending, options.stderr_tail_bytes);
        ++result.stderr_lines;
        if (options.on_stderr) options.on_stderr(err_pending);
    }

    DWORD exit_code = 0;
    if (GetExitCodeProcess(info.hProcess, &exit_code) != 0) {
        result.exit_code = static_cast<int>(exit_code);
        result.normal_exit = !result.cancel_timeout && exit_code != STILL_ACTIVE;
    }
    result.cancelled = stop_requested;
    CloseHandle(info.hProcess);
    CloseHandle(out_read);
    CloseHandle(err_read);
    // ★ 收尾关作业对象 ⇒ 残留的孙进程一起收（也就不会再攥着管道）
    if (job != nullptr) CloseHandle(job);
    return result;
}

#else

RunResult run(const RunOptions& options) {
    RunResult result;
    (void)options;
    result.error = "引擎的进程运行器当前只在 Windows 上实现（如实报，不糊一条没测过的路径）";
    return result;
}

#endif

}  // namespace xrkit::engine::process
