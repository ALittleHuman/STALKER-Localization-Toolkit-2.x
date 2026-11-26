// xrkit::core —— 唯一的多段下载管理器。
// 宿主更新、插件同步/异步下载都走这里；多源按候选顺序探测/重试，取消保留分片。

#include "xrkit/core/download.h"

#include "xrkit/core/http.h"
#include "xrkit/core/sha256.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace xrkit::core {
namespace {

constexpr std::uint64_t kMinParallel = 2u << 20;   // 2 MB 以下不折腾多段
constexpr int kMaxWorkers = 12;

std::uint64_t file_size(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return 0;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

bool file_matches_sha(const std::string& path, const std::string& sha) {
    if (sha.empty()) return file_size(path) > 0;
    std::string error;
    return sha256_file(path, error) == sha;
}

bool append_file(std::ofstream& out, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out << in.rdbuf();
    return out.good();
}

std::string segment_path(const std::string& target, int index) {
    return target + ".part." + std::to_string(index);
}

std::string state_path(const std::string& target) {
    return target + ".segments";
}

struct Job {
    DownloadOptions options;
    mutable std::mutex mutex;
    std::condition_variable cv;
    DownloadStatus status;
    std::atomic_bool cancel{false};
    std::chrono::steady_clock::time_point finished_at{};
    bool defer_failure = false;
    std::string last_attempt_error;
    std::thread worker;
};

void set_progress(const std::shared_ptr<Job>& job, std::uint64_t done, std::uint64_t total) {
    std::lock_guard<std::mutex> lock(job->mutex);
    job->status.done = done;
    job->status.total = total;
}

void finish_job(const std::shared_ptr<Job>& job, bool ok, bool cancelled,
                const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (!ok && job->defer_failure) {
            job->last_attempt_error = error;
            return;
        }
        job->status.finished = true;
        job->status.ok = ok;
        job->status.cancelled = cancelled;
        job->status.error = error;
        job->status.path = job->options.target;
        job->finished_at = std::chrono::steady_clock::now();
    }
    job->cv.notify_all();
}

void write_state(const std::string& target, const std::string& url, const std::string& sha,
                 std::uint64_t total, int segments) {
    const std::string path = state_path(target);
    const std::string temp = path + ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "url=" << url << "\n";
    out << "sha256=" << sha << "\n";
    out << "total=" << total << "\n";
    out << "segments=" << segments << "\n";
    out.flush();
    out.close();
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
}

bool state_matches(const std::string& target, const std::string& url, const std::string& sha,
                   std::uint64_t total, int segments) {
    std::ifstream in(state_path(target));
    if (!in) return false;
    std::string line;
    std::string saved_url;
    std::string saved_sha;
    std::uint64_t saved_total = 0;
    int saved_segments = 0;
    while (std::getline(in, line)) {
        if (line.rfind("url=", 0) == 0) saved_url = line.substr(4);
        else if (line.rfind("sha256=", 0) == 0) saved_sha = line.substr(7);
        else if (line.rfind("total=", 0) == 0) {
            try { saved_total = std::stoull(line.substr(6)); } catch (...) { return false; }
        } else if (line.rfind("segments=", 0) == 0) {
            try { saved_segments = std::stoi(line.substr(9)); } catch (...) { return false; }
        }
    }
    // ★ BETA.270：断点键不再绑定 url —— 镜像换源时同一份文件（sha/total 相同）
    //   应当能接着下，而不是从 0 重来。没有 sha 时无法证明"同一份文件"，仍按 url 保守判。
    if (saved_sha.empty() || sha.empty()) {
        return !saved_url.empty() && saved_url == url && saved_sha == sha &&
               saved_total == total && saved_segments == segments;
    }
    return saved_sha == sha && saved_total == total && saved_segments == segments;
}

void remove_parts(const std::string& target, int count) {
    std::error_code ec;
    for (int i = 0; i < count; ++i) std::filesystem::remove(segment_path(target, i), ec);
    std::filesystem::remove(state_path(target), ec);
    std::filesystem::remove(state_path(target) + ".tmp", ec);
}

bool download_single(const std::shared_ptr<Job>& job, const std::string& url,
                     std::uint64_t total) {
    const auto progress = [job](std::size_t done, std::size_t total_bytes) {
        set_progress(job, done, total_bytes);
        return !job->cancel.load();
    };
    const HttpResult result = http_download(url, job->options.target, job->options.timeout_sec,
                                            job->options.max_bytes, progress,
                                            job->options.prefer_direct,
                                            job->options.allow_disabled);
    if (!result.ok) {
        finish_job(job, false, job->cancel.load(), result.error);
        return false;
    }
    if (!file_matches_sha(job->options.target, job->options.sha256)) {
        std::error_code ec;
        std::filesystem::remove(job->options.target, ec);
        std::filesystem::remove(job->options.target + ".part", ec);
        finish_job(job, false, false, "sha256 不匹配");
        return false;
    }
    if (total == 0) total = file_size(job->options.target);
    if (total == 0 && result.bytes > 0) total = result.bytes;
    set_progress(job, total, total);
    finish_job(job, true, false, {});
    return true;
}

bool download_parallel(const std::shared_ptr<Job>& job, const std::string& url,
                       std::uint64_t total, int workers) {
    const int segments = std::max(1, std::min(workers, kMaxWorkers));
    if (!state_matches(job->options.target, url, job->options.sha256, total, segments)) {
        remove_parts(job->options.target, segments);
    }
    write_state(job->options.target, url, job->options.sha256, total, segments);

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    std::uint64_t initial_done = 0;
    const std::uint64_t base = total / static_cast<std::uint64_t>(segments);
    std::vector<std::shared_ptr<std::atomic<std::uint64_t>>> previous_values;
    previous_values.reserve(static_cast<std::size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const std::uint64_t existing = file_size(segment_path(job->options.target, i));
        initial_done += existing;
        previous_values.push_back(std::make_shared<std::atomic<std::uint64_t>>(existing));
    }
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->status.done = initial_done;
        job->status.total = total;
    }
    for (int i = 0; i < segments; ++i) {
        const std::uint64_t start = base * static_cast<std::uint64_t>(i);
        const std::uint64_t end = (i + 1 == segments) ? total - 1 : base * static_cast<std::uint64_t>(i + 1) - 1;
        const auto previous = previous_values[static_cast<std::size_t>(i)];
        threads.emplace_back([job, url, start, end, i, previous, &failures] {
            const auto progress = [job, previous](std::size_t done, std::size_t) {
                const std::uint64_t current = done;
                const std::uint64_t prev = previous->exchange(current);
                if (current > prev) {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->status.done += current - prev;
                }
                return !job->cancel.load();
            };
            const HttpResult result = http_download_range(
                url, segment_path(job->options.target, i), start, end,
                job->options.timeout_sec, progress, job->options.prefer_direct,
                job->options.allow_disabled);
            if (!result.ok) failures.fetch_add(1);
        });
    }
    for (auto& thread : threads) thread.join();
    if (job->cancel.load()) {
        finish_job(job, false, true, "下载已取消");
        return false;
    }
    if (failures.load() > 0) {
        // 服务器不支持 Range 或分片失败：清掉分片，退回单流（仍可 .part 续传）。
        remove_parts(job->options.target, kMaxWorkers);
        return download_single(job, url, total);
    }

    const std::string merged = job->options.target + ".tmp";
    std::ofstream out(merged, std::ios::binary | std::ios::trunc);
    if (!out) {
        finish_job(job, false, false, "无法创建合并文件");
        return false;
    }
    std::uint64_t merged_size = 0;
    for (int i = 0; i < segments; ++i) {
        const std::string part = segment_path(job->options.target, i);
        merged_size += file_size(part);
        if (!append_file(out, part)) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(merged, ec);
            finish_job(job, false, false, "合并分片失败");
            return false;
        }
    }
    out.flush();
    out.close();
    if (merged_size != total) {
        std::error_code ec;
        std::filesystem::remove(merged, ec);
        finish_job(job, false, false, "分片总大小不符");
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(job->options.target, ec);
    std::filesystem::rename(merged, job->options.target, ec);
    if (ec) {
        finish_job(job, false, false, "合并后无法落盘");
        return false;
    }
    if (!file_matches_sha(job->options.target, job->options.sha256)) {
        std::filesystem::remove(job->options.target, ec);
        remove_parts(job->options.target, kMaxWorkers);
        finish_job(job, false, false, "sha256 不匹配");
        return false;
    }
    remove_parts(job->options.target, segments);
    set_progress(job, total, total);
    finish_job(job, true, false, {});
    return true;
}

void run_job(const std::shared_ptr<Job>& job) {
    if (job->options.target.empty() || job->options.urls.empty()) {
        finish_job(job, false, false, "下载参数不完整");
        return;
    }
    std::error_code ec;
    const auto parent = std::filesystem::path(job->options.target).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    // ★ 没有期望 sha 时，目标文件存在不代表内容完整（可能是上次手工留下的半文件）；
    //   不能把“存在且非空”当成下载完成，必须重新下载。
    if (!job->options.sha256.empty() &&
        file_matches_sha(job->options.target, job->options.sha256)) {
        const std::uint64_t size = file_size(job->options.target);
        set_progress(job, size, size);
        finish_job(job, true, false, {});
        return;
    }

    std::string last_error;
    for (const std::string& candidate : job->options.urls) {
        const std::uint64_t total =
            http_probe_size(candidate, std::min(8, job->options.timeout_sec),
                            job->options.prefer_direct, job->options.allow_disabled);
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->status.url = candidate;
            job->defer_failure = true;
            job->last_attempt_error.clear();
        }
        bool ok = false;
        if (job->options.workers <= 1 || total < kMinParallel) {
            ok = download_single(job, candidate, total);
        } else {
            ok = download_parallel(job, candidate, total, job->options.workers);
        }
        if (ok) return;
        if (job->cancel.load()) {
            {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->defer_failure = false;
            }
            finish_job(job, false, true, "下载已取消");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->defer_failure = false;
            last_error = job->last_attempt_error;
        }
        remove_parts(job->options.target, kMaxWorkers);
        std::error_code ec;
        std::filesystem::remove(job->options.target + ".part", ec);
    }
    finish_job(job, false, false,
               last_error.empty() ? "所有下载源都失败" : last_error);
}

}  // namespace

struct DownloadManager::Impl {
    mutable std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs;
    std::atomic<std::uint64_t> next{1};

    // 完成后的 job 保留一段时间给调用方轮询最终状态，超过期限回收；
    // worker 已结束，可安全 join 后释放 std::thread 句柄。
    void reap_finished_locked() {
        const auto now = std::chrono::steady_clock::now();
        constexpr auto kRetention = std::chrono::minutes(10);
        for (auto it = jobs.begin(); it != jobs.end();) {
            bool finished = false;
            bool old = false;
            {
                std::lock_guard<std::mutex> lock(it->second->mutex);
                finished = it->second->status.finished;
                if (finished) old = (now - it->second->finished_at) > kRetention;
            }
            if (!finished || !old) {
                ++it;
                continue;
            }
            if (it->second->worker.joinable()) it->second->worker.join();
            it = jobs.erase(it);
        }
    }
};

DownloadManager& DownloadManager::instance() {
    static DownloadManager manager;
    return manager;
}

DownloadManager::DownloadManager() : impl_(new Impl) {}

DownloadManager::~DownloadManager() {
    for (auto& item : impl_->jobs) {
        item.second->cancel.store(true);
        if (item.second->worker.joinable()) item.second->worker.join();
    }
    delete impl_;
}

std::uint64_t DownloadManager::start(const DownloadOptions& options) {
    auto job = std::make_shared<Job>();
    job->options = options;
    const std::uint64_t id = impl_->next.fetch_add(1);
    try {
        job->worker = std::thread([job] { run_job(job); });
    } catch (const std::exception& e) {
        finish_job(job, false, false, std::string("无法创建下载线程：") + e.what());
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->reap_finished_locked();
        impl_->jobs[id] = job;
    }
    return id;
}

bool DownloadManager::cancel(std::uint64_t job_id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->jobs.find(job_id);
        if (it == impl_->jobs.end()) return false;
        job = it->second;
    }
    job->cancel.store(true);
    return true;
}

void DownloadManager::cancel_owner(const std::string& owner) {
    std::vector<std::shared_ptr<Job>> matched;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& item : impl_->jobs) {
            if (item.second->options.owner == owner) matched.push_back(item.second);
        }
    }
    for (const auto& job : matched) job->cancel.store(true);
}

bool DownloadManager::status(std::uint64_t job_id, DownloadStatus* out) const {
    if (out == nullptr) return false;
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->jobs.find(job_id);
        if (it == impl_->jobs.end()) return false;
        job = it->second;
    }
    std::lock_guard<std::mutex> lock(job->mutex);
    *out = job->status;
    return true;
}

bool DownloadManager::wait(std::uint64_t job_id, DownloadStatus* out, int timeout_ms) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->jobs.find(job_id);
        if (it == impl_->jobs.end()) return false;
        job = it->second;
    }
    std::unique_lock<std::mutex> lock(job->mutex);
    const auto done = [&] { return job->status.finished; };
    if (timeout_ms < 0) {
        job->cv.wait(lock, done);
    } else {
        if (!job->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), done)) return false;
    }
    if (out != nullptr) *out = job->status;
    return true;
}

}  // namespace xrkit::core