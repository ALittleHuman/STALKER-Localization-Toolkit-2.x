#pragma once
// xrkit::core —— **带上限的整文件读**（H5 统一读闸门，2026-11-20）。
//
// 用户 2026-11-20 裁定：多路径整文件读必须有统一上限（取 512 MB），不能各自 `istreambuf_iterator`。
// 本文件提供**唯一**实现；读超限时返回 false 并给出**点名原因**（不是静默截断）。

#include <cstddef>
#include <string>

namespace xrkit::core {

// 512 MB —— 与 `untrusted_limits` 的包级上限同量级；真实配置文件远小于此。
inline constexpr std::size_t kMaxWholeFileBytes = 512ull * 1024ull * 1024ull;

// 按 UTF-8 路径整读文件到 `out`（二进制）。超过 `max_bytes` ⇒ 失败并清空 `out`。
// `error` 非空时写入点名原因（含实际大小与上限）。
bool read_file_capped(const std::string& path_utf8, std::size_t max_bytes, std::string& out,
                      std::string& error);

// 便捷重载：用一个按 UTF-8 的路径 + 默认上限。
bool read_file_capped(const std::string& path_utf8, std::string& out, std::string& error);

}  // namespace xrkit::core
