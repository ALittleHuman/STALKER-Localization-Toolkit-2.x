#include "xrkit/engine/video/transcode.h"

#include <cmath>
#include <map>
#include <string>

namespace xrkit::engine::video {
namespace {

// 1.x `CODEC_MAP`：mpeg4/xvid/msmpeg4v3 → libxvid；theora → libtheora；表外 → libtheora
std::string preferred_encoder(const std::string& reference_codec) {
    static const std::map<std::string, std::string> kMap = {
        {"mpeg4", "libxvid"},
        {"xvid", "libxvid"},
        {"msmpeg4v3", "libxvid"},
        {"theora", "libtheora"},
    };
    const auto it = kMap.find(reference_codec);
    return it == kMap.end() ? std::string("libtheora") : it->second;
}

bool contains(const std::vector<std::string>& values, const std::string& needle) {
    for (const std::string& value : values) {
        if (value == needle) return true;
    }
    return false;
}

std::string number(long long value) { return std::to_string(value); }

// 1.x 用 f"{ref_fps}" 拼字符串：整数帧率写成 "25.0"（Python float 的 repr），别写成 "25"
std::string fps_text(double fps) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", fps);
    std::string text(buffer);
    if (text.find('.') == std::string::npos && text.find('e') == std::string::npos &&
        text.find("inf") == std::string::npos && text.find("nan") == std::string::npos) {
        text += ".0";
    }
    return text;
}

}  // namespace

std::string pick_video_encoder(const std::string& reference_codec,
                               const std::vector<std::string>& available) {
    const std::string preferred = preferred_encoder(reference_codec);
    if (contains(available, preferred)) return preferred;
    for (const char* fallback : {"libtheora", "libxvid", "mpeg4"}) {
        if (contains(available, fallback)) return fallback;
    }
    return "mpeg4";  // 内置编码器总可用；调用方据此告警
}

TranscodePlan plan_transcode(const TranscodeRequest& request) {
    TranscodePlan plan;

    // 参考参数（缺省值就是 1.x 的缺省）
    std::string ref_vcodec;
    int ref_w = 0;
    int ref_h = 0;
    double ref_fps = 0.0;
    long long ref_vbitrate = 0;
    long long ref_ar = 48000;
    int ref_ac = 2;
    long long ref_abitrate = 160000;
    if (request.has_reference) {
        if (request.reference.has_video) {
            ref_vcodec = request.reference.video.codec;
            ref_w = request.reference.video.width;
            ref_h = request.reference.video.height;
            ref_fps = request.reference.video.fps;
            ref_vbitrate = request.reference.video.bitrate;
        }
        if (request.reference.has_audio) {
            const AudioStream& audio = request.reference.audio;
            ref_ar = audio.sample_rate > 0 ? audio.sample_rate : 48000;
            ref_ac = audio.channels > 0 ? audio.channels : 2;
            ref_abitrate = audio.bitrate > 0 ? audio.bitrate : 160000;
        }
    }

    plan.video_encoder = pick_video_encoder(ref_vcodec, request.available_encoders);
    plan.encoder_warning = request.available_encoders.empty() ||
                           (!contains(request.available_encoders, "libtheora") &&
                            !contains(request.available_encoders, "libxvid") &&
                            !contains(request.available_encoders, "mpeg4"));

    const MediaInfo& src = request.source;
    if (src.parsed && src.duration > 0) plan.duration = src.duration;

    std::vector<std::string>& cmd = plan.argv;
    cmd.push_back(request.ffmpeg);
    cmd.push_back("-y");
    cmd.push_back("-i");
    cmd.push_back(request.input);

    // 视频编码
    if (plan.video_encoder == "libxvid" || plan.video_encoder == "mpeg4") {
        const long long bitrate = ref_vbitrate > 0 ? ref_vbitrate : 2500000;
        cmd.insert(cmd.end(), {"-c:v", plan.video_encoder, "-b:v", number(bitrate), "-pix_fmt",
                               "yuv420p"});
    } else {
        if (ref_vbitrate > 0) {
            cmd.insert(cmd.end(),
                       {"-c:v", "libtheora", "-b:v", number(ref_vbitrate), "-pix_fmt", "yuv420p"});
        } else {
            cmd.insert(cmd.end(),
                       {"-c:v", "libtheora", "-q:v", "7", "-pix_fmt", "yuv420p", "-g", "30"});
        }
    }

    // 缩放 / 帧率（1.x：源与参考同宽高 ⇒ 取消 scale；fps 差 < 0.01 ⇒ 取消 fps）
    bool need_scale = ref_w > 0 && ref_h > 0;
    bool need_fps = ref_fps > 0;
    if ((need_scale || need_fps) && src.parsed && src.has_video) {
        if (need_scale && src.video.width == ref_w && src.video.height == ref_h) need_scale = false;
        if (need_fps && std::fabs(src.video.fps - ref_fps) < 0.01) need_fps = false;
    }
    std::string filter;
    if (need_scale) filter += "scale=" + number(ref_w) + ":" + number(ref_h);
    if (need_fps) {
        if (!filter.empty()) filter += ",";
        filter += "fps=" + fps_text(ref_fps);
    }
    if (!filter.empty()) cmd.insert(cmd.end(), {"-vf", filter});

    // 音频：只有"源确认无音轨"才 -an；ffprobe 失败 ⇒ 保留音频选项（并置位告警）
    if (!src.parsed) {
        plan.audio_kept_on_probe_failure = src.parsed == false && request.has_reference;
        cmd.insert(cmd.end(), {"-c:a", "libvorbis", "-b:a", number(ref_abitrate), "-ar",
                               number(ref_ar), "-ac", number(ref_ac)});
    } else if (src.has_audio) {
        cmd.insert(cmd.end(), {"-c:a", "libvorbis", "-b:a", number(ref_abitrate), "-ar",
                               number(ref_ar), "-ac", number(ref_ac)});
    } else {
        cmd.push_back("-an");
    }

    cmd.push_back("-f");
    cmd.push_back("ogg");
    cmd.push_back(request.output);
    return plan;
}

ProgressSample progress_from_line(const std::string& line, double duration_sec) {
    ProgressSample sample;
    if (duration_sec <= 0) return sample;
    // 1.x 的 `time=(\d+):(\d+):(\d+)\.(\d+)`：手写扫描，语义等价（`search` = 取第一处）
    const std::string key = "time=";
    size_t at = line.find(key);
    while (at != std::string::npos) {
        size_t i = at + key.size();
        auto digits = [&](int& value) {
            const size_t start = i;
            long long acc = 0;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                acc = acc * 10 + (line[i] - '0');
                ++i;
            }
            value = static_cast<int>(acc);
            return i > start;
        };
        int h = 0, mi = 0, sec = 0, cs = 0;
        if (digits(h) && i < line.size() && line[i] == ':' && ++i != 0 && digits(mi) &&
            i < line.size() && line[i] == ':' && ++i != 0 && digits(sec) && i < line.size() &&
            line[i] == '.' && ++i != 0 && digits(cs)) {
            const double cur = h * 3600.0 + mi * 60.0 + sec + cs / 100.0;
            sample.matched = true;
            const double percent = cur / duration_sec * 100.0;
            sample.percent = percent < 99.5 ? percent : 99.5;  // 1.x: min(cur/duration*100, 99.5)
            return sample;
        }
        at = line.find(key, at + key.size());
    }
    return sample;
}

}  // namespace xrkit::engine::video
