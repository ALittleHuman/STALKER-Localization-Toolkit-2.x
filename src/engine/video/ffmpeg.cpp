#include "xrkit/engine/video/ffmpeg.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

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
    const bool local_ffmpeg_ok = is_file(local_ffmpeg);
    const bool local_probe_ok = is_file(local_probe);

    if (manual_ffmpeg) {
        out.ffmpeg = config.path;
        out.ffmpeg_from_config = true;
    } else if (local_ffmpeg_ok) {
        out.ffmpeg = local_ffmpeg;  // 自动找到的**不写盘**（8.7）
    }
    if (manual_probe) {
        out.ffprobe = config.probe;
        out.ffprobe_from_config = true;
    } else if (local_probe_ok) {
        out.ffprobe = local_probe;
    }
    out.found = !out.ffmpeg.empty();
    out.has_probe = !out.ffprobe.empty();
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
    return major_version(version_output) == 8;
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
