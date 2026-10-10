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

// 命令行引号规则照 Windows 的规矩（`CommandLineToArgvW` 的反函数）。
// ★★ 2026-10-10（P17-11，审计 01 F-04 Medium）：**修前漏了"反斜杠翻倍"** ——
//   旧实现只把 `"` 转义成 `\"`，反斜杠原样输出 ⇒ 参数**以反斜杠结尾**时（`C:\dir with space\`）
//   得到 `"C:\dir with space\"`：那个收尾反斜杠把**收尾引号**吃掉了 ⇒ `CommandLineToArgvW`
//   把后面的参数全并进这一个参数（实测口径见下），命令直接跑歪或跑不起来。
//   Windows 的真规则（也是 Python `subprocess.list2cmdline` 的规则 —— 1.x 就是用它拼命令行的）：
//     · 需要外层引号 ⇔ 参数**为空**或含**空格/制表符**（只含 `"` 的不需要）；
//     · 连续 N 个反斜杠后面跟一个 `"` ⇒ 那 N 个要写成 2N 个，再写 `\"`；
//     · 参数**以 N 个反斜杠结尾且要加外层引号** ⇒ 那 N 个要写成 2N 个，否则收尾引号被吃掉。
//   判据：`xrkit_app --smoke` 里的引号表（期望值由 `subprocess.list2cmdline` 逐条生成；
//   另在 4000 条随机串上做过逐字节对拍，见批报告）。
std::string quote_arg(const std::string& arg) {
    const bool need_quotes = arg.empty() || arg.find_first_of(" \t") != std::string::npos;
    std::string out;
    size_t pending_backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == '"') {
            // 2N 个反斜杠 + 转义引号（N 是紧邻它前面那一串反斜杠的个数）
            out.append(pending_backslashes * 2 + 1, '\\');
            out += '"';
            pending_backslashes = 0;
            continue;
        }
        out.append(pending_backslashes, '\\');
        pending_backslashes = 0;
        out += ch;
    }
    if (need_quotes) {
        out.insert(out.begin(), '"');
        out.append(pending_backslashes * 2, '\\');
        out += '"';
    } else {
        out.append(pending_backslashes, '\\');
    }
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

// ★★ P17-11（01 F-04）：命令行拼装的**唯一一份实现**（`run()` 与自检都走它）。
//   与 Python `subprocess.list2cmdline` 逐字节同规则 ⇒ 1.x 拼出来的命令行与我们一致。
std::string build_command_line(const std::string& program, const std::vector<std::string>& args) {
    std::string command = quote_arg(program);
    for (const std::string& arg : args) {
        command += " ";
        command += quote_arg(arg);
    }
    return command;
}

RunResult run(const RunOptions& options) {
    RunResult result;
    if (options.program.empty()) {
        result.error = "程序名为空";
        return result;
    }

    const std::string command = build_command_line(options.program, options.args);

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
