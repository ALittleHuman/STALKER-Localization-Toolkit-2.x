#pragma once
// xrkit::core —— 多段下载管理器（无 Qt，供宿主更新与插件宿主共用）。
//
// 设计边界：
//   · 目标文件 + <target>.part.N + <target>.segments；
//   · 默认 4 段，最多 6 段；服务器不支持 Range 时回退单流；
//   · 取消只置位，正在阻塞的 WinHTTP 最多等到当前超时；分片保留，可续传；
//   · 每个 job 一个管理线程；worker 只写自己的分片。宿主更新和插件 sync/async 共用本类。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

struct DownloadOptions {
    std::string target;
    std::vector<std::string> urls;
    std::string sha256;
    std::string owner;  // 可选：插件 owner；卸载时按 owner 取消
    std::size_t max_bytes = 8u << 30;
    int timeout_sec = 300;
    int workers = 4;
    bool prefer_direct = true;
    // 联网开关关闭时仍允许本次下载（插件经用户弹窗授权后由 py_host 置 true）。
    bool allow_disabled = false;
};

struct DownloadStatus {
    bool finished = false;
    bool ok = false;
    bool cancelled = false;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
    std::string path;
    std::string url;
};

class DownloadManager {
public:
    static DownloadManager& instance();

    std::uint64_t start(const DownloadOptions& options);
    bool cancel(std::uint64_t job);
    // 取消某个 owner 名下的全部任务（插件 unload / reload 时调用）。
    void cancel_owner(const std::string& owner);
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