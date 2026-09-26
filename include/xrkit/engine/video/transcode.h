#pragma once
// xrkit::engine::video —— **转码命令行构造**（用户 2026-09-26 决策：视频转码完整移植 1.x）。
//
// 口径见 `docs/2.0.0-视频模块规格.md`（逐行取证自 1.x `apps/video_ogm_app.py` 的
// `CODEC_MAP` / `_pick_video_encoder` / `convert()`）。判据＝与
// `tests/golden/video_argv.json`（**exec 1.x convert() 本体实调**录下的 argv）逐项相等。
//
// 这里只做"纯计算"：给参考/源两条流信息 + 可用编码器集合 ⇒ 交出 argv。
// 探测（ffprobe）、起进程、读进度、取消都在上层（可自检、可换桩）。

#include <string>
#include <vector>

namespace xrkit::engine::video {

struct VideoStream {
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    long long bitrate = 0;
};

struct AudioStream {
    int sample_rate = 0;
    int channels = 0;
    long long bitrate = 0;
};

struct MediaInfo {
    bool parsed = false;  // ffprobe 解析成功（失败 ⇒ 1.x 会保留音频选项并告警）
    bool has_video = false;
    bool has_audio = false;
    VideoStream video;
    AudioStream audio;
    double duration = 0.0;
};

struct TranscodeRequest {
    std::string ffmpeg = "ffmpeg";
    std::string input;
    std::string output;
    bool has_reference = false;
    MediaInfo reference;
    MediaInfo source;
    std::vector<std::string> available_encoders;  // `ffmpeg -encoders` 扫出来的集合
};

struct TranscodePlan {
    std::vector<std::string> argv;
    std::string video_encoder;
    bool encoder_warning = false;  // 一个可用编码器都没有 ⇒ 回退内置 mpeg4（要告警）
    bool audio_kept_on_probe_failure = false;
    double duration = 0.0;
};

// 参照有编码/码率 ⇒ 匹配；否则默认（见规格 §2）
std::string pick_video_encoder(const std::string& reference_codec,
                               const std::vector<std::string>& available);
TranscodePlan plan_transcode(const TranscodeRequest& request);

}  // namespace xrkit::engine::video
