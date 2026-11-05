#pragma once
// xrkit::core —— 极简日志（不依赖 Qt）。
//
// 口径（照 1.x）：重要结果进 GUI 日志栏并落盘；纯明细只落盘。
// 该分层由上层决定，这里只提供落盘与标准错误两条通道。

#include <string>
#include <string_view>

namespace xrkit::core {

enum class Level { Info, Warn, Error };

// 追加一行（std::cerr + 可选文件）。msg 里的中文按 UTF-8 写出。
void log(Level level, std::string_view msg);

// 空字符串 = 关闭文件输出。目录不存在时静默降级（绝不因日志失败中断程序）。
void set_log_file(const std::string& path);

// 供 --selftest 用：当前是否已挂上文件
bool log_file_active();

// ★★ C-13（第三方只读审计 2026-10-18；用户 2026-10-19 决策）：**只写文件、不写 stderr**。
//   用途：把界面上那一块日志（原本只进 LogBox、一条都不落盘）**镜像**到运行日志文件里。
//   为什么按"块"而不是逐行：LogBox 本来就是 80 ms 批量，写文件跟着批量走 ——
//   解包 3.9 万条覆盖时不会退化成 3.9 万次 flush。
//   `text` 原样写入（末尾补一个 `\n`），**一次 flush**。
void log_block(std::string_view text);

// ★ C-13 §5.6：运行日志**保留 50 份**（按最后写入时间，新的留）。
//   · 只认 `log_*.txt`（含同秒的 `_2`、`_3` 后缀）；
//   · `protect_path` 那一份**永不删**（当前正在写的这份）；
//   · 删不掉（被别的实例占用）**不静默**：返回**删掉的份数**，失败的份数也返回，
//     由调用方决定要不要提示；本函数自己不打印（core 层不碰 UI）。
//   返回：{deleted, failed}（用结构体是为了让"删了几份/几份失败"都能被断言）。
struct PruneResult {
    int deleted = 0;
    int failed = 0;
};
PruneResult prune_log_files(const std::string& dir, int keep, const std::string& protect_path);


// ★ 2026-11-05（用户口径）：**日志按"正常退出才重置"管理**。
//   修前：每次启动都开新文件、旧文件按"保留 50 份"轮转 —— 崩溃那次的日志和正常那次的
//   混在一起，想看"上次为什么崩"要先猜哪一份。
//   现在：
//     · `sweep_previous_session(dir)`：读 `<dir>/.session`（上一轮留下的）；
//         - 上一轮**正常退出**（finished）⇒ 删掉它那份日志（= 重置）；
//         - 上一轮**没标记**（= 崩溃/强杀）⇒ 把那份日志重命名成 `<原名>_abnormal_exit.txt` **保留**，
//           写在返回值里，由上层提示"上次异常退出，日志已保留：…"；
//     · `mark_session_started(dir, log_path)`：本轮开始（写 `.session`，state=started）；
//     · `mark_session_finished(dir)`：正常退出（state=finished）。
//   命名规则：`<原名>_abnormal_exit.txt`；同一秒重复 ⇒ 加 `_2`、`_3`。
struct SessionSweep {
    bool had_marker = false;
    bool previous_crashed = false;     // true = 上一轮没走到正常退出
    std::string kept_log;              // 保留（改名）后的路径；空 = 没有可保留的
    std::string removed_log;           // 正常退出而被重置掉的那份
};
SessionSweep sweep_previous_session(const std::string& dir);
void mark_session_started(const std::string& dir, const std::string& log_path);
void mark_session_finished(const std::string& dir);

}  // namespace xrkit::core
