#pragma once
// xrkit::core —— 宿主内置 HTTP（Windows WinHTTP）。
//
// 联网总开关 `[network] enabled=0` 在这里统一拦截；调用方不需要自己判断。
// 只提供 GET/下载；TLS/证书走系统默认，不关闭校验，不降级到 http。

#include <cstddef>
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
                    std::size_t max_bytes = 1u << 20, bool prefer_direct = false);

// 简化 POST：body 原样发送；content_type 空时按 application/json。
HttpResult http_post(const std::string& url, const std::string& body,
                     const std::string& content_type = "application/json",
                     int timeout_sec = 20, std::size_t max_bytes = 1u << 20);

// 下载到 path；失败时不留下半文件。
HttpResult http_download(const std::string& url, const std::string& path,
                         int timeout_sec = 120,
                         std::size_t max_bytes = 256u << 20,
                         const HttpProgress& progress = {},
                         bool prefer_direct = false);

}  // namespace xrkit::core
