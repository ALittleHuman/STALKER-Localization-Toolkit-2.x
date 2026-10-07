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
    // 1.x `_fmt` 的音频行列的是 `a.codec`（如 vorbis）⇒ 解析结果里必须有它。
    // 转码命令行本身不用它（只用声道/采样率/码率），是**信息面板**要显示。
    std::string codec;
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
    // 1.x `VideoInfo.size_mb = float(format.size) / 1024 / 1024`（信息面板的"大小"行）
    double size_mb = 0.0;
};

struct TranscodeRequest {
    std::string ffmpeg = "ffmpeg";
    std::string input;
    std::string output;
    bool has_reference = false;
    MediaInfo reference;
    // ★ 2026-10-07（审计补充 F1-3）：参考 OGM 的**磁盘路径**（`MediaInfo` 里没有路径）。
    //   空 = 调用方没给参考路径 ⇒ `check_output_conflict` 就不判"输出撞参考"这一条。
    std::string reference_path;
    MediaInfo source;
    std::vector<std::string> available_encoders;  // `ffmpeg -encoders` 扫出来的集合
};

struct TranscodePlan {
    std::vector<std::string> argv;
    std::string video_encoder;
    bool encoder_warning = false;  // 一个可用编码器都没有 ⇒ 回退内置 mpeg4（要告警）
    bool audio_kept_on_probe_failure = false;
    double duration = 0.0;

    // ★ 界面「视频参数」面板要显示的**正是这些值**（1.x `_refresh_display` 的对比行）。
    //   它们全由本函数决定 ⇒ 面板直接读，**不许**在页面里把阈值/默认值再判一遍
    //   （否则 23.98 vs 24.00 那类边界会出现"面板说 OK、argv 里却插了 fps 滤镜"）。
    bool scale_applied = false;   // 真的写了 `scale=`（1.x 的「将缩放 / OK」）
    bool fps_applied = false;     // 真的写了 `fps=`（1.x 的「将调整 / OK」）
    long long video_bitrate = 0;  // 实际写进 `-b:v` 的值（0 = 没写，如 theora 的 `-q:v` 路线）
    long long audio_bitrate = 0;  // 实际写进 `-b:a` 的值
    int audio_sample_rate = 0;    // 实际写进 `-ar` 的值
    int audio_channels = 0;       // 实际写进 `-ac` 的值
    bool audio_disabled = false;  // 源**确认无音轨** ⇒ `-an`
};

// 参照有编码/码率 ⇒ 匹配；否则默认（见规格 §2）
std::string pick_video_encoder(const std::string& reference_codec,
                               const std::vector<std::string>& available);
TranscodePlan plan_transcode(const TranscodeRequest& request);

// ── ★ 2026-10-07（审计补充 F1-2 / F1-3）：输出目标的**起跑前守卫** ──────────────
//
// `plan_transcode` 照抄 1.x，argv 里**恒有 `-y`**（`transcode.cpp` 的 `cmd.push_back("-y")`）
// ⇒ 覆盖是**静默的**：ffmpeg 不问、页面也不问。所以"输出已存在"与"输出就是输入"这两件
// 必须在**起跑前**由引擎判定出来，调用方再据此**拒绝或明确询问**（零静默）。
//
// 判定只做两件事：**规范化路径比较**（`core::path_equals`：大小写 / 正反斜杠 / `.` `..` 段）
// 与**存在性**（`std::filesystem::exists`）。不读文件内容，也不做别的推断。
enum class OutputConflict {
    None,             // 既不是输入、磁盘上也不存在 ⇒ 直接开跑
    SameAsSource,     // 输出 == `input`（源文件）⇒ 叠上 `-y` 就是**截断/覆盖源文件**
    SameAsReference,  // 输出 == `reference_path`（参考 OGM）⇒ 它也是输入，同样会毁掉
    Exists,           // 输出是别的已存在文件 ⇒ 覆盖前必须**经用户明确确认**
};

// 判定顺序：SameAsSource → SameAsReference → Exists（前两条最危险，优先报）。
// `output` 为空 ⇒ None（"没填输出"由调用方自己报，不是本函数的事）。
OutputConflict check_output_conflict(const TranscodeRequest& request);

// 给人看的理由（零静默：非 None 的每一种都必须有一句话能贴到界面上）
std::string output_conflict_reason(OutputConflict conflict, const TranscodeRequest& request);

// ── ffprobe / ffmpeg 输出的**纯解析段**（1.x `parse_video_info` / `_ensure_encoders`）──
//
// 只做"文本 → 结构"（不起进程、不碰 Qt），所以判据可以拿**录下来的真输出**逐字段对拍
// （见 `tests/golden_video_probe.py`：1.x 的同名函数吃同一份字节，两边比字段）。
//
// `parse_ffprobe_json`：吃 `ffprobe -v quiet -print_format json -show_format -show_streams`
// 的 stdout。逐字段照 1.x：`format.duration`/`format.size`（字符串数字）、
// 第一个 `codec_type=="video"` 的 `codec_name/width/height/avg_frame_rate|r_frame_rate/
// bit_rate`、第一个 `codec_type=="audio"` 的 `codec_name/channels/sample_rate/bit_rate`。
// 不是合法 JSON ⇒ 返回 `parsed=false`（调用方据此走 1.x"保留音频选项 + 告警"的分支）。
MediaInfo parse_ffprobe_json(const std::string& json);

// `parse_encoder_list`：吃 `ffmpeg -encoders` 的 stdout。1.x 只认以 ` V`/` A` 开头的行、
// 取该行第 2 列（`V....D libtheora ...` ⇒ `libtheora`）。返回**按出现顺序**（去重）。
std::vector<std::string> parse_encoder_list(const std::string& output);

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
