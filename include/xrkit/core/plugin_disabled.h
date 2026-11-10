#pragma once
// ★ 2026-11-10（用户：插件管理器独立进程）：**禁用清单**的单一读写实现。
//
// 文件：`<插件目录>/plugins.disabled`（UTF-8 文本；一行一条，字段用 TAB 分隔）：
//
//     # 注释 / 空行忽略
//     bad_plugin.py<TAB>xrkit.bad<TAB>启动崩溃，已手动禁用
//
// 为什么不用 JSON：宿主（CLI/GUI）与独立管理器都要读，工程里没有通用 JSON 解析器；
// 行格式足够、可手改、坏行能跳过。**只按文件名禁用**：管理器不需要 import 插件就能写入，
// 宿主在 import **之前**跳过 ⇒ 坏插件根本不会执行。
#include <string>
#include <vector>

namespace xrkit::plugin {

struct DisabledPluginEntry {
    std::string file;    // 相对插件目录的文件名（如 bad_plugin.py）
    std::string id;      // 清单 id（可空；管理器写入时填，便于人读）
    std::string reason;  // 原因（可空）
};

// 读 `<dir>/plugins.disabled`；文件不存在返回空表（不是错误）。
std::vector<DisabledPluginEntry> read_disabled_plugins(const std::string& dir);

// 原子写回（临时文件 → rename）；父目录不存在时先建。失败返回 false 并填 error。
bool write_disabled_plugins(const std::string& dir,
                            const std::vector<DisabledPluginEntry>& entries,
                            std::string& error);

// 文件名是否在禁用表里（大小写不敏感；只比 file 字段）。
bool is_file_disabled(const std::vector<DisabledPluginEntry>& entries,
                      const std::string& file);

}  // namespace xrkit::plugin
