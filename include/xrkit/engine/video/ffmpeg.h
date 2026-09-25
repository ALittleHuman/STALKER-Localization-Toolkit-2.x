#pragma once
// xrkit::engine::video —— ffmpeg 定位与版本守卫（对照表 ⑧8.1/8.2/8.6/8.7，无 Qt）。
//
// 口径全来自 1.x `apps/video_ogm_app.find_ffmpeg` 与用户原话
// （"ffmpeg不用检不检测，直接读目录下的就行，读不到再跳出窗口让用户手动选择。"）：
//   · **只认两处**：`user.ltx [ffmpeg] path/probe`（用户显式指定，文件必须存在）
//     与 **程序目录**下的 `ffmpeg.exe`/`ffprobe.exe`；
//   · **不搜 PATH、不 import imageio** —— 到处找正是启动变慢（冻结包 ~373 ms）与
//     "到底用了哪个 ffmpeg 说不清"的来源；
//   · 两侧**独立解析**（可能 ffmpeg 来自配置、ffprobe 来自程序目录）；
//   · 8.2 探测是**惰性**的，且**失败也置位**（否则每点一次按钮重弹一次窗）；
//   · 8.5 状态三态：**探测之前不显示 ✗**；
//   · 8.6 版本守卫：`download_ffmpeg.py` 显式拒绝 ffmpeg **8.x**（8.x 的 theora
//     编码器对 STALKER OGM 有 bug）；
//   · 8.7 **只有手动指定的**才写进 `user.ltx`，自动查找到的**不写盘**。

#include <cstdint>
#include <string>

namespace xrkit::engine::video {

// `user.ltx [ffmpeg]` 里的两项（由调用方读出来传进来 —— 便于对拍，不依赖全局状态）
struct FfmpegConfig {
    std::string path;
    std::string probe;
};

struct FfmpegPaths {
    bool found = false;      // ffmpeg 找到了（ffprobe 可能仍缺，8.4 要求如实报）
    bool has_probe = false;
    std::string ffmpeg;
    std::string ffprobe;
    bool ffmpeg_from_config = false;
    bool ffprobe_from_config = false;
};

// 8.1：只认两处。`config` 里的路径**必须真实存在**才算数。
FfmpegPaths find_ffmpeg(const std::string& app_dir, const FfmpegConfig& config);
// 便利版：app_dir = exe 所在目录 + 读 `user.ltx`
FfmpegPaths find_ffmpeg_default();

// 8.5 状态三态：探测之前是 NotProbed（界面**不许**先画 ✗）
enum class ProbeState { NotProbed, Found, Missing };

// 8.6：版本守卫。从 `ffmpeg -version` 的输出里取主版本号；8.x 判为不可用。
int major_version(const std::string& version_output);
bool version_rejected(const std::string& version_output);

// 8.2/8.7：惰性探测 + 手动指定的写盘策略
class FfmpegLocator {
public:
    FfmpegLocator() = default;
    explicit FfmpegLocator(std::string app_dir) : app_dir_(std::move(app_dir)) {}

    const FfmpegPaths& ensure();          // 第一次才真探测；之后返回缓存（含"没找到"）
    ProbeState state() const { return state_; }
    int probe_count() const { return probe_count_; }
    const FfmpegPaths& paths() const { return paths_; }

    // 8.7：手动指定 → 写进 `user.ltx [ffmpeg] path/probe`（文件必须存在，否则拒绝并说明）
    bool set_manual(const std::string& ffmpeg, const std::string& ffprobe, std::string& error);
    // 手动指定后要重新探测才生效（惰性缓存作废）
    void invalidate();

private:
    std::string app_dir_;
    FfmpegPaths paths_;
    ProbeState state_ = ProbeState::NotProbed;
    int probe_count_ = 0;
};

}  // namespace xrkit::engine::video
