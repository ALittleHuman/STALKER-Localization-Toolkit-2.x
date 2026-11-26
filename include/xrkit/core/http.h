#pragma once
// xrkit::core —— 宿主内置 HTTP（Windows WinHTTP）。
//
// 联网总开关 `[network] enabled=0` 在这里统一拦截；调用方不需要自己判断。
// 提供 GET/POST/文件下载/Range 分片；TLS/证书走系统默认，不关闭校验，不降级到 http。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace xrkit::core {

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    std::size_t bytes = 0;
    bool truncated = false;
    // ★ 2026-11-19：本次响应"应该是多少字节"（200 取 Content-Length；206 取
    //   Content-Range 的总长）。0 = 响应没给，无法判断完整性（不做校验）。
    std::size_t expected_bytes = 0;
};

bool network_enabled();

// 下载进度回调：bytes=本次已收，total=响应总长（0=未知）；返回 false 表示取消。
using HttpProgress = std::function<bool(std::size_t bytes, std::size_t total)>;

HttpResult http_get(const std::string& url, int timeout_sec = 15,
                    std::size_t max_bytes = 1u << 20, bool prefer_direct = false,
                    bool allow_disabled = false);

// 只用 Range 0-0 探测总大小；拿不到返回 0。
std::uint64_t http_probe_size(const std::string& url, int timeout_sec = 8,
                              bool prefer_direct = false, bool allow_disabled = false);

// 简化 POST：body 原样发送；content_type 空时按 application/json。
HttpResult http_post(const std::string& url, const std::string& body,
                     const std::string& content_type = "application/json",
                     int timeout_sec = 20, std::size_t max_bytes = 1u << 20,
                     bool allow_disabled = false);

// 下载到 path；失败时不留下半文件。
HttpResult http_download(const std::string& url, const std::string& path,
                         int timeout_sec = 120,
                         std::size_t max_bytes = 256u << 20,
                         const HttpProgress& progress = {},
                         bool prefer_direct = false, bool allow_disabled = false);

// 下载一个闭区间 [start,end] 到分片文件；已有内容按 Range 续传。
// 服务器忽略 Range 返回 200 时返回失败并删除分片，调用方应回退单流。
HttpResult http_download_range(const std::string& url, const std::string& path,
                               std::uint64_t start, std::uint64_t end,
                               int timeout_sec = 300,
                               const HttpProgress& progress = {},
                               bool prefer_direct = false, bool allow_disabled = false);

}  // namespace xrkit::core
