#pragma once
// xrkit::core —— 多段下载管理器（无 Qt，供宿主更新与插件宿主共用）。
//
// 设计边界：
//   · 目标文件 + <target>.part.N + <target>.segments；
//   · 1/2/4 段（默认 4），服务器不支持 Range 时回退单流；
//   · 取消只置位，正在阻塞的 WinHTTP 最多等到当前超时；
//   · 管理线程负责状态/拼接/校验，worker 只写自己的分片。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

struct DownloadOptions {
    std::string target;
    std::vector<std::string> urls;
    std::string sha256;
    std::size_t max_bytes = 8u << 30;
    int timeout_sec = 300;
    int workers = 4;
    bool prefer_direct = true;
};

struct DownloadStatus {
    bool finished = false;
    bool ok = false;
    bool cancelled = false;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
    std::string path;
};

class DownloadManager {
public:
    static DownloadManager& instance();

    std::uint64_t start(const DownloadOptions& options);
    bool cancel(std::uint64_t job);
    bool status(std::uint64_t job, DownloadStatus* out) const;
    // 供同步调用方等待；timeout_ms < 0 表示无限等待。
    bool wait(std::uint64_t job, DownloadStatus* out, int timeout_ms = -1);

private:
    DownloadManager();
    ~DownloadManager();
    struct Impl;
    Impl* impl_;
};

}  // namespace xrkit::core