#pragma once
// xrkit::engine::video::probe_text —— `ffmpeg -i` stderr 的文本解析（无 ffprobe 时的降级探测）。
//
// 为什么要有它（P-798）
// --------------------
// 发行包若不再带 `bin\ffprobe.exe`，探测只能靠 `ffmpeg -hide_banner -nostdin -i <file>` 的
// stderr 文本。它是**给人看的**，不是稳定接口 ⇒ 只取我们能从实测里确认的那几样：
//
//   · 流级行：`Stream #0:0...: Video: <codec>, ..., 320x240 ..., 204 kb/s, 23.98 fps, 23.98 tbr`
//   · 音频行：`Stream #0:1...: Audio: <codec>, 44100 Hz, mono, fltp, 70 kb/s`
//   · 时长：  `Duration: 00:00:01.00, start: ..., bitrate: 284 kb/s`   ← **容器级**，码率不收
//
// 口径（两份复核 2026-11-01 收敛）：
//   · `fps` 标签 = `avg_frame_rate`；`tbr` 标签 = `r_frame_rate`；优先 `fps`，其无效才回落 `tbr`；
//     `tbn` 是 time base，**绝不**当帧率；
//   · 视频码率**只**取流级 `NNN kb/s`；容器级（Duration 行）的 `bitrate:` 必须忽略 ——
//     拿它填流码率会凭空造出 `-b:v`，与 `-q:v 7` 是结构性差异；
//   · `0/0` / `N/A` / `0 kb/s` ⇒ 0；「缺失」与「0」在语义上不同，用 `has_video_bitrate` 区分；
//   · `size_mb` 不从文本取（文本里没有文件大小）⇒ 由调用方给 `file_size_bytes`。

#include "xrkit/engine/video/transcode.h"

#include <string>

namespace xrkit::engine::video {

struct ProbeTextResult {
    bool parsed = false;          // 看到 `Input #` 且至少一条流
    MediaInfo info;
    bool has_video_bitrate = false;   // 流级码率**存在**（区分"缺失"与"0"）
    bool has_audio_bitrate = false;
    std::string error;            // 解析失败原因（parsed=false 时非空）
};

// `text` = `ffmpeg -hide_banner -nostdin -i <file>` 的完整 stderr；
// `file_size_bytes` = 调用方用 filesystem 取的真实大小（<0 表示未知）。
ProbeTextResult parse_ffmpeg_i_stderr(const std::string& text, long long file_size_bytes);

}  // namespace xrkit::engine::video
