#include "xrkit/engine/video/transcode.h"

#include "xrkit/core/path.h"

#include <cctype>
#include <charconv>   // ★ P17-9（02 F-03）：to_chars = 最短往返表示（与 Python repr 同语义）
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

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

// 1.x 用 f"{ref_fps}" 拼字符串（`video_ogm_app.py:302` 的 `vf_parts.append(f"fps={ref_fps}")`）：
// Python 的 f-string 对 float 走的是 **repr**（最短但能往返的十进制表示）——
//   · 整数帧率写成 "25.0"（不是 "25"）；
//   · 非整数比写成 24000/1001 = "23.976023976023978"、30000/1001 = "29.97002997002997"。
// ★★ 2026-10-10（P17-9，审计 02 F-03 Medium）：修前这里用 `%.6g` ——
//   对 23.976023976023978 只给 "23.976" ⇒ `-vf fps=` 与参考 argv **不等**（门禁 [26] 的
//   "逐项相等"在非整数帧率样本上必红），产物的时基也跟着变。
//   现在改用 `std::to_chars`（**最短往返**表示，与 Python repr 同语义）；
//   尾部再补 ".0" 的老逻辑保持不变（to_chars 对 25.0 给 "25"，Python repr 给 "25.0"）。
std::string fps_text(double fps) {
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer) - 1, fps);
    std::string text;
    if (converted.ec == std::errc()) {
        text.assign(buffer, converted.ptr);
    } else {
        // 兜底：to_chars 失败（理论不可达）仍给一个能看的十进制表示，别返回空串。
        std::snprintf(buffer, sizeof(buffer), "%.17g", fps);
        text.assign(buffer);
    }
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

// ── ★ 2026-10-07（审计补充 F1-2 / F1-3）：输出目标的起跑前守卫 ────────────────
namespace {

// 规范化路径比较（`core::path_equals` 做 lexically_normal + 大小写 + 正反斜杠）。
bool same_path(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    return core::path_equals(a, b);
}

}  // namespace

OutputConflict check_output_conflict(const TranscodeRequest& request) {
    if (request.output.empty()) return OutputConflict::None;
    // ① 输出就是源文件：源本身是 .ogm 时默认输出名恰好落在它身上（审计 F1-3 的现场）。
    //    手打的输出框同理 —— **不区分**是不是默认名。
    if (same_path(request.output, request.input)) return OutputConflict::SameAsSource;
    // ② 输出就是参考 OGM：参考也是**输入**，`-y` 会把它毁掉。
    if (same_path(request.output, request.reference_path)) return OutputConflict::SameAsReference;
    // ③ 输出是别的已存在文件：覆盖前必须问一次（原来不问就开始 `-y`）。
    std::error_code ec;
    const bool exists = std::filesystem::exists(core::path_from_utf8(request.output), ec);
    if (exists && !ec) return OutputConflict::Exists;
    return OutputConflict::None;
}

std::string output_conflict_reason(OutputConflict conflict, const TranscodeRequest& request) {
    switch (conflict) {
        case OutputConflict::SameAsSource:
            return "输出路径就是**源文件**本身（" + request.output +
                   "）：命令行里带 `-y`，跑下去会把源文件覆盖/截断，必须先改输出名";
        case OutputConflict::SameAsReference:
            return "输出路径就是**参考 OGM** 本身（" + request.output +
                   "）：它也是这次转换的**输入**，跑下去会把参考文件覆盖/截断，必须先改输出名";
        case OutputConflict::Exists:
            return "输出文件已经存在（" + request.output +
                   "）：命令行里带 `-y`，不问一声就会**静默覆盖**它";
        case OutputConflict::None:
            break;
    }
    return std::string();
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
        plan.video_bitrate = bitrate;
        cmd.insert(cmd.end(), {"-c:v", plan.video_encoder, "-b:v", number(bitrate), "-pix_fmt",
                               "yuv420p"});
    } else {
        if (ref_vbitrate > 0) {
            plan.video_bitrate = ref_vbitrate;
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
    // 面板读的是"真的做了什么"，不是"打算做什么" ⇒ 与插入滤镜同一个判据
    plan.scale_applied = need_scale;
    plan.fps_applied = need_fps;

    // 音频：只有"源确认无音轨"才 -an；ffprobe 失败 ⇒ 保留音频选项（并置位告警）
    // 摘要里的音频值 = **真的写进 argv 的那几个数**（-an 时是 0 ⇒ 面板也就不显示音频行，
    // 与 1.x 一致：1.x 的 `if sa and ra:` 在源无音轨时同样什么都不打印）。
    if (!src.parsed) {
        plan.audio_kept_on_probe_failure = src.parsed == false && request.has_reference;
        plan.audio_bitrate = ref_abitrate;
        plan.audio_sample_rate = static_cast<int>(ref_ar);
        plan.audio_channels = ref_ac;
        cmd.insert(cmd.end(), {"-c:a", "libvorbis", "-b:a", number(ref_abitrate), "-ar",
                               number(ref_ar), "-ac", number(ref_ac)});
    } else if (src.has_audio) {
        plan.audio_bitrate = ref_abitrate;
        plan.audio_sample_rate = static_cast<int>(ref_ar);
        plan.audio_channels = ref_ac;
        cmd.insert(cmd.end(), {"-c:a", "libvorbis", "-b:a", number(ref_abitrate), "-ar",
                               number(ref_ar), "-ac", number(ref_ac)});
    } else {
        plan.audio_disabled = true;
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

// ═══════════════════════════════════════════════════════════════════════════
// ffprobe / ffmpeg 输出解析（1.x `parse_video_info` 的解析段 + `_ensure_encoders`）
//
// 为什么这两段在引擎里、而不是在页面里：它们是**纯计算**（文本 → 结构），
// 放在引擎才能拿"录下来的真输出"直接对拍（页面里就得起进程才能验）。
// ═══════════════════════════════════════════════════════════════════════════
namespace {

// ffprobe JSON 需要的那点结构：对象 / 数组 / 字符串 / 数字 / 布尔 / null。
// 刻意不引第三方 JSON 库（引擎依赖表里没有它，也不为一个字段解析拖进来）。
struct JValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JValue> items;
    std::vector<std::pair<std::string, JValue>> members;

    const JValue* find(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }
};

class JParser {
public:
    explicit JParser(const std::string& input) : text_(input) {}

    bool parse(JValue& out) {
        skip_space();
        if (!parse_value(out)) return false;
        skip_space();
        return pos_ >= text_.size();  // 尾部不许有垃圾（"不是合法 JSON"要如实报）
    }

private:
    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    bool parse_value(JValue& out) {
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out.kind = JValue::Kind::String;
            return parse_string(out.text);
        }
        if (literal("true")) {
            out.kind = JValue::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = JValue::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null")) {
            out.kind = JValue::Kind::Null;
            return true;
        }
        return parse_number(out);
    }

    bool parse_object(JValue& out) {
        out.kind = JValue::Kind::Object;
        ++pos_;  // '{'
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (pos_ >= text_.size() || text_[pos_] != '"' || !parse_string(key)) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') return false;
            ++pos_;
            skip_space();
            JValue value;
            if (!parse_value(value)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(JValue& out) {
        out.kind = JValue::Kind::Array;
        ++pos_;  // '['
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (true) {
            skip_space();
            JValue value;
            if (!parse_value(value)) return false;
            out.items.push_back(std::move(value));
            skip_space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
    }

    bool parse_string(std::string& out) {
        ++pos_;  // 开引号
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) return false;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    // UTF-8 编码（代理对不做合并：ffprobe 的字段里没有这类字符）
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parse_number(JValue& out) {
        const size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ == start) return false;
        out.kind = JValue::Kind::Number;
        out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    const std::string& text_;
    size_t pos_ = 0;
};

// 1.x 一律 `float(...)` / `int(...)`：字段可能是 JSON 数字（`channels`）也可能是
// **字符串数字**（`bit_rate`、`sample_rate`、`duration`、`size`）⇒ 两种都要认。
bool as_double(const JValue* value, double& out) {
    if (value == nullptr) return false;
    if (value->kind == JValue::Kind::Number) {
        out = value->number;
        return true;
    }
    if (value->kind == JValue::Kind::String) {
        if (value->text.empty()) return false;
        out = std::strtod(value->text.c_str(), nullptr);
        return true;
    }
    return false;
}

long long as_int(const JValue* value) {
    double number = 0.0;
    if (!as_double(value, number)) return 0;
    return static_cast<long long>(number);
}

std::string as_string(const JValue* value) {
    if (value == nullptr) return std::string();
    if (value->kind == JValue::Kind::String) return value->text;
    return std::string();
}

// 1.x：`avg_frame_rate` 优先，缺了用 `r_frame_rate`（再缺才是 "0/1"）；
// 只认 "num/den" 形态、den != 0 —— 别的形态 1.x 不改 fps（保持 0.0）。
double fps_from(const JValue* stream) {
    const JValue* value = stream->find("avg_frame_rate");
    if (value == nullptr) value = stream->find("r_frame_rate");
    std::string text = as_string(value);
    if (text.empty()) text = "0/1";
    const size_t slash = text.find('/');
    if (slash == std::string::npos) return 0.0;
    const double num = std::strtod(text.substr(0, slash).c_str(), nullptr);
    const double den = std::strtod(text.substr(slash + 1).c_str(), nullptr);
    if (den == 0.0) return 0.0;
    return num / den;
}

}  // namespace

MediaInfo parse_ffprobe_json(const std::string& json) {
    MediaInfo info;
    JValue root;
    JParser parser(json);
    if (!parser.parse(root) || root.kind != JValue::Kind::Object) return info;  // parsed=false
    info.parsed = true;

    if (const JValue* format = root.find("format"); format != nullptr) {
        double duration = 0.0;
        if (as_double(format->find("duration"), duration)) info.duration = duration;
        double size = 0.0;
        if (as_double(format->find("size"), size)) info.size_mb = size / (1024.0 * 1024.0);
    }

    const JValue* streams = root.find("streams");
    if (streams == nullptr || streams->kind != JValue::Kind::Array) return info;
    for (const JValue& stream : streams->items) {
        if (stream.kind != JValue::Kind::Object) continue;
        const std::string type = as_string(stream.find("codec_type"));
        if (type == "video" && !info.has_video) {
            info.has_video = true;
            info.video.codec = as_string(stream.find("codec_name"));
            info.video.width = static_cast<int>(as_int(stream.find("width")));
            info.video.height = static_cast<int>(as_int(stream.find("height")));
            info.video.fps = fps_from(&stream);
            info.video.bitrate = as_int(stream.find("bit_rate"));
        } else if (type == "audio" && !info.has_audio) {
            info.has_audio = true;
            info.audio.codec = as_string(stream.find("codec_name"));
            info.audio.channels = static_cast<int>(as_int(stream.find("channels")));
            info.audio.sample_rate = static_cast<int>(as_int(stream.find("sample_rate")));
            info.audio.bitrate = as_int(stream.find("bit_rate"));
        }
    }
    return info;
}

std::vector<std::string> parse_encoder_list(const std::string& output) {
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= output.size()) {
        const size_t nl = output.find('\n', start);
        const std::string line =
            output.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // 1.x：`if line.startswith(" V") or line.startswith(" A")`
        if (line.size() >= 2 && line[0] == ' ' && (line[1] == 'V' || line[1] == 'A')) {
            // 1.x `line.split()` ⇒ 任意空白切分，取第 2 列
            size_t i = 0;
            int field = 0;
            std::string second;
            while (i < line.size()) {
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                const size_t begin = i;
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                if (i == begin) break;
                if (field == 1) {
                    second = line.substr(begin, i - begin);
                    break;
                }
                ++field;
            }
            if (!second.empty()) {
                bool seen = false;
                for (const std::string& name : names) {
                    if (name == second) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) names.push_back(second);
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return names;
}

}  // namespace xrkit::engine::video
