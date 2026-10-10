#pragma once
// xrkit::engine::process —— **跑外部进程**的唯一一份实现（2026-10-08 P12b）。
//
// 为什么要下沉到引擎（而不是让插件自己 `subprocess`）：
//   · "取消要**真停**"、"退出码要**如实**报"、"stderr 尾巴要**看得见**"、"**不许弹控制台**"
//     —— 这四条是**产品语义**（C++ 视频页逐条实现过：`CREATE_NO_WINDOW` / kill+等 5 秒 /
//     超时要如实说"取消超时" / 退出码原文报出）。
//     插件侧再写一份 `subprocess` 就是第二份实现 ⇒ 本仓门禁反复拦的就是这个形态。
//   · 门禁 `tests/check_no_console.py` 按文件判"产品代码里不许出现会弹控制台的启动方式" ⇒
//     新实现必须自己带 `CREATE_NO_WINDOW`（本文件里有）。
//
// 语义（逐条照 `video_page.cpp` 的现有实现）：
//   ① 起进程带 `CREATE_NO_WINDOW`；
//   ② stdout/stderr 都要读（**双管道不读就会死锁** ⇒ stderr 走一个线程专门读）；
//   ③ 取消：先 `TerminateProcess`，再等 `cancel_wait_ms`（默认 5000）；**没死透要如实报**
//      `cancel_timeout=true`（不假装停了）；
//   ④ `exit_code` / `normal_exit` 一律如实（不把"起不来"混进"退出码非 0"）；
//   ⑤ `stderr_tail` 只留**尾巴**（默认 8 KiB）—— 界面显示的就是它。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xrkit::engine::process {

struct RunOptions {
    std::string program;                     // 例如 "ffmpeg"（按调用方给的原样传给系统）
    std::vector<std::string> args;           // 不含 argv[0]
    std::string working_dir;                 // 空 = 继承当前目录
    std::function<bool()> should_stop;       // 取消请求（每次读到行/每 20 ms 问一次）
    std::function<void(const std::string&)> on_stdout;   // 逐行（进度解析用它）
    std::function<void(const std::string&)> on_stderr;   // 逐行
    std::size_t stderr_tail_bytes = 8192;    // 只留尾巴
    int cancel_wait_ms = 5000;               // 取消后最多等这么久
};

struct RunResult {
    bool started = false;        // 起不来 ⇒ false（`error` 里给理由）
    bool cancelled = false;      // 收到取消请求并**真停**了
    bool cancel_timeout = false; // 取消后 5 秒没死透（**如实报，不假装停了**）
    bool normal_exit = false;    // 正常退出（不是崩溃/被信号带走）
    int exit_code = -1;
    std::string error;           // 起不来 / 等待失败的原因
    std::string stderr_tail;     // stderr 的尾巴（界面显示的就是它）
    uint64_t stdout_lines = 0;
    uint64_t stderr_lines = 0;
};

// 同步跑：**调用方在工作线程里用**（界面那条路是任务壳的 worker）。
RunResult run(const RunOptions& options);

// ★★ 2026-10-10（P17-11，审计 01 F-04 Medium）：把「参数表 → Windows 命令行」这一步**暴露出来**，
//   好让自检能在不真起进程的情况下逐字节判它（修前这一手只藏在匿名命名空间里，
//   于是"参数以反斜杠结尾时收尾引号被吃掉"这条缺陷没有任何判据盯得住）。
//   规则与 Python `subprocess.list2cmdline` 逐字节一致（1.x 拼命令行用的就是它）；
//   `run()` 内部走的就是这一个实现（唯一一份，不许分叉）。
std::string build_command_line(const std::string& program, const std::vector<std::string>& args);

}  // namespace xrkit::engine::process
