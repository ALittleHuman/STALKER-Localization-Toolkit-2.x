#pragma once
// xrkit::core —— 路径与字符串编码。
//
// 口径：**对外一律 UTF-8**（CLI 参数、归档内路径、日志），只有真正碰文件系统的
// 那一层才转成本地宽字符。Windows 上 `char*` 路径会被当成 ANSI 代码页，
// 西里尔/中文名会直接找不到文件（实测踩过：ASCII 路径通、西里尔目录不通）。

#include <filesystem>
#include <string>

namespace xrkit::core {

// UTF-8 → 本地路径（Windows 下走 UTF-16）
std::filesystem::path path_from_utf8(const std::string& utf8);

// 本地路径 → UTF-8（'/' 分隔）
std::string path_to_utf8(const std::filesystem::path& path);

// 两个路径是否指向**同一个位置**：只做词法规范化（不碰盘、不解析符号链接），
// **Windows 下大小写不敏感**（与 Python `os.path.normcase` 同语义）。
// 用途：守卫"输出目录会不会原地覆盖源目录"这类判据 —— 1.x 用的正是 normcase 比较。
bool path_equals(const std::string& a, const std::string& b);

}  // namespace xrkit::core
