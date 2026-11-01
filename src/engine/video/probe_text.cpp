#include "xrkit/engine/video/probe_text.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace xrkit::engine::video {
namespace {

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// `NNN kb/s`（流级）。`has` 记"这一行里到底有没有这个数"——缺失和 0 不是一回事。
long long kb_per_sec(const std::string& text, bool& has) {
    has = false;
    const std::string needle = "kb/s";
    size_t at = text.find(needle);
    if (at == std::string::npos) return 0;
    size_t end = at;
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1]))) --begin;
    if (begin == end) return 0;
    long long value = 0;
    for (size_t i = begin; i < end; ++i) value = value * 10 + (text[i] - '0');
    has = true;
    return value * 1000;
}

// `23.98 fps` / `30 tbr` —— `tbn` 是 time base，绝不认。
double labeled_number(const std::string& text, const char* label, bool& has) {
    has = false;
    const std::string needle = label;
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        // 左侧必须是数字（允许一个小数点）
        size_t end = at;
        if (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
            --end;
            size_t begin = end;
            while (begin > 0 && (std::isdigit(static_cast<unsigned char>(text[begin - 1])) != 0 ||
                                 text[begin - 1] == '.')) {
                --begin;
            }
            if (begin < end) {
                const std::string number = text.substr(begin, end - begin);
                char* stop = nullptr;
                const double value = std::strtod(number.c_str(), &stop);
                if (stop != nullptr && *stop == '\0') {
                    has = true;
                    return value;
                }
            }
        }
        at += needle.size();
    }
    return 0.0;
}

// Video 行里的 `320x240`（取第一个满足 WxH 的）。
bool resolution(const std::string& text, int& width, int& height) {
    for (size_t i = 0; i + 2 < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) continue;
        size_t j = i;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) != 0) ++j;
        if (j >= text.size() || text[j] != 'x') continue;
        size_t k = j + 1;
        while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k])) != 0) ++k;
        if (k == j + 1) continue;
        const int w = std::atoi(text.substr(i, j - i).c_str());
        const int h = std::atoi(text.substr(j + 1, k - j - 1).c_str());
        if (w > 0 && h > 0 && w <= 16384 && h <= 16384) {
            width = w;
            height = h;
            return true;
        }
        // 不是分辨率（例如 `0x31637661` 这种 hex）⇒ 继续往后找（修前这里直接 return false）
        i = k;
    }
    return false;
}

int sample_rate(const std::string& text, bool& has) {
    has = false;
    const size_t at = text.find(" Hz");
    if (at == std::string::npos) return 0;
    size_t end = at;
    while (end > 0 && text[end - 1] == ' ') --end;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1])) != 0) --begin;
    if (begin == end) return 0;
    has = true;
    return std::atoi(text.substr(begin, end - begin).c_str());
}

int channels(const std::string& text) {
    if (text.find("mono") != std::string::npos) return 1;
    if (text.find("stereo") != std::string::npos) return 2;
    // `5.1` / `5.1(side)` / `7.1`
    for (const char* layout : {"7.1", "5.1", "2.1"}) {
        if (text.find(layout) != std::string::npos) {
            return layout[0] == '7' ? 8 : (layout[0] == '5' ? 6 : 3);
        }
    }
    return 0;
}

// `Duration: 00:00:01.04` ⇒ 1.04 秒
double duration_seconds(const std::string& text, bool& has) {
    has = false;
    const std::string needle = "Duration:";
    const size_t at = text.find(needle);
    if (at == std::string::npos) return 0.0;
    const size_t begin = at + needle.size();
    const size_t end = text.find(',', begin);
    const std::string stamp = trim(text.substr(begin, end == std::string::npos ? std::string::npos
                                                                            : end - begin));
    int hh = 0, mm = 0;
    double ss = 0.0;
    if (std::sscanf(stamp.c_str(), "%d:%d:%lf", &hh, &mm, &ss) != 3) return 0.0;
    has = true;
    return hh * 3600.0 + mm * 60.0 + ss;
}

// 取 `Video:` / `Audio:` 之后、逗号分隔的**第一个** token（codec 名，去括号）。
std::string codec_after(const std::string& text, const char* kind) {
    const size_t at = text.find(kind);
    if (at == std::string::npos) return std::string();
    size_t begin = at + std::strlen(kind);
    while (begin < text.size() && text[begin] == ' ') ++begin;
    size_t end = begin;
    while (end < text.size() && text[end] != ',' && text[end] != '(') ++end;
    std::string codec = trim(text.substr(begin, end - begin));
    return codec;
}

bool is_stream_line(const std::string& line) { return line.find("Stream #") != std::string::npos; }

}  // namespace

ProbeTextResult parse_ffmpeg_i_stderr(const std::string& text, long long file_size_bytes) {
    ProbeTextResult result;
    bool saw_input = false;
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            const size_t nl = text.find('\n', start);
            lines.push_back(text.substr(start, nl == std::string::npos ? std::string::npos
                                                                      : nl - start));
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    bool duration_ok = false;
    const double duration = duration_seconds(text, duration_ok);

    for (const std::string& raw : lines) {
        const std::string line = trim(raw);
        if (line.find("Input #") != std::string::npos) {
            saw_input = true;
            continue;
        }
        if (!is_stream_line(line)) continue;

        if (line.find("Video:") != std::string::npos) {
            if (result.info.has_video) continue;   // 只取第一条视频流（与 ffprobe 路径一致）
            result.info.has_video = true;
            result.info.video.codec = codec_after(line, "Video:");
            int w = 0, h = 0;
            if (resolution(line, w, h)) {
                result.info.video.width = w;
                result.info.video.height = h;
            }
            bool has_br = false;
            result.info.video.bitrate = kb_per_sec(line, has_br);
            result.has_video_bitrate = has_br;
            // fps(=avg_frame_rate) 优先；无效才回落 tbr(=r_frame_rate)
            bool has_fps = false;
            const double fps = labeled_number(line, "fps", has_fps);
            if (has_fps && fps > 0.0) {
                result.info.video.fps = fps;
            } else {
                bool has_tbr = false;
                const double tbr = labeled_number(line, "tbr", has_tbr);
                result.info.video.fps = (has_tbr && tbr > 0.0) ? tbr : 0.0;
            }
        } else if (line.find("Audio:") != std::string::npos) {
            if (result.info.has_audio) continue;
            result.info.has_audio = true;
            result.info.audio.codec = codec_after(line, "Audio:");
            bool has_sr = false;
            result.info.audio.sample_rate = sample_rate(line, has_sr);
            result.info.audio.channels = channels(line);
            bool has_br = false;
            result.info.audio.bitrate = kb_per_sec(line, has_br);
            result.has_audio_bitrate = has_br;
        }
    }

    if (!saw_input) {
        result.error = "stderr 里没有 `Input #`（不是 ffmpeg -i 的输出，或文件打不开）";
        return result;
    }
    if (!result.info.has_video && !result.info.has_audio) {
        result.error = "stderr 里没有任何可识别的流（只有文件头）";
        return result;
    }
    if (duration_ok && duration > 0.0) result.info.duration = duration;
    if (file_size_bytes > 0) {
        result.info.size_mb = static_cast<double>(file_size_bytes) / (1024.0 * 1024.0);
    }
    result.info.parsed = true;
    return result;
}

}  // namespace xrkit::engine::video
