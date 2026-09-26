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

// ── 进度与取消（1.x `convert()` 340–372 行，逐句取证）────────────────
//
// 1.x 在**每一行 stderr** 上做两件事，顺序不能颠倒：
//   ① 先看 `self._cancelled` —— 取消就 `terminate()` + `wait()` + **删掉输出文件** +
//      返回 `(False, "已取消")`（所以取消**不留半成品**）；
//   ② 再用 `time=(\d+):(\d+):(\d+)\.(\d+)` 匹配，命中且 duration>0 才算进度：
//      `cur = h*3600 + mi*60 + s + cs/100.0`（第 4 组按**厘秒**）、
//      `percent = min(cur/duration*100, 99.5)` —— **上限 99.5、没有下限**（1.x 原文如此）。
// **`convert()` 不清零 `_cancelled`**（只由 `reset_cancel()` 清）—— 这条要照抄，
// 否则"取消后重跑"的行为和 1.x 不一样。
struct ProgressSample {
    bool matched = false;   // 这一行里有 `time=`（且 duration>0）
    double percent = 0.0;   // 上限 99.5（1.x 的 min(..., 99.5)）
};

ProgressSample progress_from_line(const std::string& line, double duration_sec);

class CancelToken {
public:
    void cancel() { cancelled_ = true; }
    void reset() { cancelled_ = false; }   // 只有它能清零（1.x `reset_cancel`）
    bool cancelled() const { return cancelled_; }

private:
    bool cancelled_ = false;
};

}  // namespace xrkit::engine::video
