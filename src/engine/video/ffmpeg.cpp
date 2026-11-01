#include "xrkit/engine/video/ffmpeg.h"

#include "xrkit/engine/process/run.h"

#include "xrkit/core/runtime.h"

#include "probe_decls.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <map>

namespace xrkit::engine::video {
namespace {

namespace stdfs = std::filesystem;

bool is_file(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return stdfs::is_regular_file(core::path_from_utf8(path), ec);
}

std::string join_exe(const std::string& app_dir, const char* name) {
    if (app_dir.empty()) return name;
    std::string base = app_dir;
    if (base.back() != '/' && base.back() != '\\') base += '/';
    return base + name;
}

}  // namespace

FfmpegPaths find_ffmpeg(const std::string& app_dir, const FfmpegConfig& config) {
    FfmpegPaths out;
    // 8.1：**只有这两处** —— 配置里的路径必须存在才算数
    const bool manual_ffmpeg = is_file(config.path);
    const bool manual_probe = is_file(config.probe);
    const std::string local_ffmpeg = join_exe(app_dir, "ffmpeg.exe");
    const std::string local_probe = join_exe(app_dir, "ffprobe.exe");
    // ★★ 决策 4（用户 2026-10-18/19）：发行包按类别收拢 ⇒ ffmpeg/ffprobe 挪进 `bin\`。
    //   查找顺序保持"配置 → 根 → `bin\`"，**根目录优先**：老包（散在根）与临时手放的
    //   `ffmpeg.exe` 都还照旧能用；`bin\` 只是新增的**第二个自动落点**。
    // ★★ 2026-11-01（用户：迭代期依赖路由到统一 runtime，正式版 root=@APP@）：
    //   顺序 = `[ffmpeg] path/probe`（旧显式键，兼容）→ `[runtime]`（显式项 → root → 工具目录）。
    //   **不搜 PATH**（旧纪律）。
    const xrkit::core::RuntimeDep rt_ffmpeg = xrkit::core::resolve_runtime_dep(
        app_dir, "ffmpeg", "bin/ffmpeg.exe", "bin/ffmpeg.exe");
    const xrkit::core::RuntimeDep rt_probe = xrkit::core::resolve_runtime_dep(
        app_dir, "ffprobe", "bin/ffprobe.exe", "bin/ffprobe.exe");
    const std::string bin_ffmpeg = join_exe(app_dir, "bin/ffmpeg.exe");
    const std::string bin_probe = join_exe(app_dir, "bin/ffprobe.exe");
    const bool local_ffmpeg_ok = is_file(local_ffmpeg);
    const bool local_probe_ok = is_file(local_probe);
    const bool bin_ffmpeg_ok = is_file(bin_ffmpeg);
    const bool bin_probe_ok = is_file(bin_probe);

    if (manual_ffmpeg) {
        out.ffmpeg = config.path;
        out.ffmpeg_from_config = true;
    } else if (!rt_ffmpeg.path.empty() &&
               rt_ffmpeg.source != xrkit::core::RuntimeSource::AppDefault) {
        // ★ runtime 路由（显式项 / root）优先
        out.ffmpeg = rt_ffmpeg.path;
    } else if (local_ffmpeg_ok) {
        out.ffmpeg = local_ffmpeg;  // 自动找到的**不写盘**（8.7）
    } else if (bin_ffmpeg_ok) {
        out.ffmpeg = bin_ffmpeg;    // ★ 决策 4：发行包的 `bin\ffmpeg.exe`
    }
    if (manual_probe) {
        out.ffprobe = config.probe;
        out.ffprobe_from_config = true;
    } else if (!rt_probe.path.empty() &&
               rt_probe.source != xrkit::core::RuntimeSource::AppDefault) {
        out.ffprobe = rt_probe.path;   // ★ runtime 路由（显式 / root）
    } else if (local_probe_ok) {
        out.ffprobe = local_probe;
    } else if (bin_probe_ok) {
        out.ffprobe = bin_probe;    // ★ 决策 4：发行包的 `bin\ffprobe.exe`
    }
    out.found = !out.ffmpeg.empty();
    out.has_probe = !out.ffprobe.empty();
    // ★ P-739：取一次 `ffmpeg -version`（版本闸门输入）；起不来只让 version 为空，
    //   不改变"找到/没找到"的结论。
    if (out.found) {
        // ★ P-780（复核提醒）：探测会 spawn 一个进程，而插件 4 处 + 引擎默认路径都会调
        //   find_ffmpeg ⇒ 按「路径 + 大小 + 写入时间」做进程内缓存，一个进程只探一次。
        static std::mutex cache_mutex;
        static std::map<std::string, std::string> version_cache;
        std::error_code stat_ec;
        const auto stamp = std::filesystem::last_write_time(core::path_from_utf8(out.ffmpeg), stat_ec);
        const auto bytes = std::filesystem::file_size(core::path_from_utf8(out.ffmpeg), stat_ec);
        const std::string cache_key = out.ffmpeg + "|" + std::to_string(stat_ec ? 0 : bytes) + "|" +
                                     std::to_string(stat_ec ? 0 : static_cast<long long>(stamp.time_since_epoch().count()));
        {
            std::lock_guard<std::mutex> guard(cache_mutex);
            const auto hit = version_cache.find(cache_key);
            if (hit != version_cache.end()) {
                out.version = hit->second;
                return out;
            }
        }
        process::RunOptions run_options;
        run_options.program = out.ffmpeg;
        run_options.args = {"-hide_banner", "-version"};
        std::string captured;
        run_options.on_stdout = [&captured](const std::string& line) {
            if (captured.size() < 512) captured += line + "\n";
        };
        const process::RunResult rr = process::run(run_options);
        if (rr.started && !captured.empty()) out.version = captured;
        {
            std::lock_guard<std::mutex> guard(cache_mutex);
            version_cache[cache_key] = out.version;
        }
    }
    return out;
}

FfmpegPaths find_ffmpeg_default() {
    FfmpegConfig config;
    config.path = core::load_user_value("ffmpeg", "path");
    config.probe = core::load_user_value("ffmpeg", "probe");
    return find_ffmpeg(core::app_dir(), config);
}

int major_version(const std::string& version_output) {
    // 认这些形态：`ffmpeg version 7.1-essentials_build`、`ffmpeg version n8.0`、
    // `ffmpeg version 8.0.1 Copyright`。取 "version" 之后的第一串数字。
    const std::string lowered_text = [&version_output] {
        std::string text = version_output;
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }
        return text;
    }();
    size_t pos = lowered_text.find("version");
    if (pos == std::string::npos) return -1;
    pos += 7;
    while (pos < lowered_text.size() &&
           !std::isdigit(static_cast<unsigned char>(lowered_text[pos]))) {
        // 跳过空白、'n'、'v' 之类的修饰（`version n8.0`）
        if (std::isalpha(static_cast<unsigned char>(lowered_text[pos])) == 0 &&
            std::isspace(static_cast<unsigned char>(lowered_text[pos])) == 0 &&
            lowered_text[pos] != '-') {
            break;
        }
        ++pos;
    }
    if (pos >= lowered_text.size() ||
        std::isdigit(static_cast<unsigned char>(lowered_text[pos])) == 0) {
        return -1;
    }
    int value = 0;
    while (pos < lowered_text.size() &&
           std::isdigit(static_cast<unsigned char>(lowered_text[pos])) != 0) {
        value = value * 10 + (lowered_text[pos] - '0');
        ++pos;
    }
    return value;
}

bool version_rejected(const std::string& version_output) {
    // 8.6：8.x 的 theora 编码器对 STALKER OGM 有 bug ⇒ 显式拒绝
    // ★ P-739/P-584：认不出版本也拒绝（fail-closed）；修前空串会放行 8.x
    const int major = major_version(version_output);
    return major < 0 || major == 8;
}

const FfmpegPaths& FfmpegLocator::ensure() {
    if (state_ != ProbeState::NotProbed) return paths_;  // 惰性：探过一次就不再来
    ++probe_count_;
    FfmpegConfig config;
    config.path = core::load_user_value("ffmpeg", "path");
    config.probe = core::load_user_value("ffmpeg", "probe");
    paths_ = find_ffmpeg(app_dir_.empty() ? core::app_dir() : app_dir_, config);
    // 8.2：**失败也置位** —— 否则"没找到"这个结论不会被记住，每点一次按钮重弹一次窗
    state_ = paths_.found ? ProbeState::Found : ProbeState::Missing;
    return paths_;
}

bool FfmpegLocator::set_manual(const std::string& ffmpeg, const std::string& ffprobe,
                               std::string& error) {
    error.clear();
    if (!is_file(ffmpeg)) {
        error = "指定的 ffmpeg 不存在：" + ffmpeg;
        return false;
    }
    if (!ffprobe.empty() && !is_file(ffprobe)) {
        error = "指定的 ffprobe 不存在：" + ffprobe;
        return false;
    }
    // 8.7：**手动指定的**记进 user.ltx（自动找到的不写盘）
    if (!core::save_user_value("ffmpeg", "path", ffmpeg)) {
        error = "写不进 user.ltx 的 [ffmpeg] path";
        return false;
    }
    if (!ffprobe.empty() && !core::save_user_value("ffmpeg", "probe", ffprobe)) {
        error = "写不进 user.ltx 的 [ffmpeg] probe";
        return false;
    }
    invalidate();
    return true;
}

void FfmpegLocator::invalidate() { state_ = ProbeState::NotProbed; }

}  // namespace xrkit::engine::video

// ── ★★ 2026-10-06（审计第 4 组）：模块**在编**探针（见 `probe_decls.h`）────────
// 本模块被摘掉 ⇒ 链接失败，而不是 `status_line()` 里少报一个数字。
extern "C" const char* xrkit_engine_probe_video_tag() { return "xrkit-engine-module-video"; }

namespace xrkit::engine {
const char* probe_video() { return xrkit_engine_probe_video_tag(); }
}  // namespace xrkit::engine

