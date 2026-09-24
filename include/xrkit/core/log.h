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

}  // namespace xrkit::core
