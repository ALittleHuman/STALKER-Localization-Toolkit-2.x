#pragma once
// xrkit::core —— 宿主内置 HTTP（Windows WinHTTP）。
//
// 联网总开关 `[network] enabled=0` 在这里统一拦截；调用方不需要自己判断。
// 只提供 GET/下载；TLS/证书走系统默认，不关闭校验，不降级到 http。

#include <cstddef>
#include <string>

namespace xrkit::core {

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
    std::size_t bytes = 0;
    bool truncated = false;
};

bool network_enabled();

HttpResult http_get(const std::string& url, int timeout_sec = 15,
                    std::size_t max_bytes = 1u << 20);

// 下载到 path；失败时不留下半文件。
HttpResult http_download(const std::string& url, const std::string& path,
                         int timeout_sec = 120,
                         std::size_t max_bytes = 256u << 20);

}  // namespace xrkit::core
