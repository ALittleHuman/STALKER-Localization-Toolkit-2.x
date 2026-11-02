#pragma once
// 视频输入的**能力清单**（唯一来源）。
//
// 为什么要有它（HA/DSH 2026-11-02）：同一份"支持哪些容器/编码"曾被抄在
// build_ffmpeg_min.sh、check_ffmpeg_min.py、UI 文案三处 ⇒ 先腐坏一处（P-174）。
// 现在：引擎常量 → 产品页（文件过滤器 + 错误文案）/ 插件页（经门面）/ 判据，都从这里取。
#include <string>
#include <vector>

namespace xrkit::engine::video {

inline constexpr const char* kSupportedContainers[] = {
    "mp4", "mov", "m4v", "mkv", "avi", "webm", "ogm"};
inline constexpr const char* kSupportedVideoCodecs[] = {
    "h264", "mpeg4", "mpeg2video", "vp8", "vp9", "theora"};
inline constexpr const char* kSupportedAudioCodecs[] = {
    "aac", "mp3", "opus", "vorbis"};

inline std::vector<std::string> supported_containers() {
    return std::vector<std::string>(std::begin(kSupportedContainers), std::end(kSupportedContainers));
}
inline std::vector<std::string> supported_video_codecs() {
    return std::vector<std::string>(std::begin(kSupportedVideoCodecs), std::end(kSupportedVideoCodecs));
}
inline std::vector<std::string> supported_audio_codecs() {
    return std::vector<std::string>(std::begin(kSupportedAudioCodecs), std::end(kSupportedAudioCodecs));
}

// "mp4 · mov · …"，报错文案用（分隔符统一在这里，别再抄一份）。
inline std::string join_dot(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += " · ";
        out += items[i];
    }
    return out;
}
inline std::string containers_text() { return join_dot(supported_containers()); }

}  // namespace xrkit::engine::video
