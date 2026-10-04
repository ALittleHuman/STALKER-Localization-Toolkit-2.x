// xrkit_fake_ffmpeg —— 离屏自检用的**假 ffmpeg**（真 ffmpeg 不保证在门禁机器上按 ⑧8.1
// 的口径可见：只认 user.ltx 与程序目录两处、不搜 PATH）。
//
// 为什么要有个真进程：进度条、取消（**终止进程 + 删半成品**）、跑完收干净进程 这三条的
// 判据都落在"进程真的在跑 / 真的没了"上，拿桩函数替不掉。本夹具同时替三种角色
// （页面只通过"ffmpeg 路径"这一个入口拿到它，所以三个都走它）：
//   · `-encoders`            ⇒ 打印一份编码器表（**连图例行那种垃圾一起**，1.x 会把它收下）；
//   · `-print_format json`   ⇒ 打印一份 ffprobe JSON（转换完成后重新解析产物那一屏）；
//   · 其余（转码）            ⇒ 造出输出文件 + 往 stderr 打 `time=` 进度行；
//                              默认几百毫秒后退出；`XRKIT_FAKE_FFMPEG_MODE=loop` 时一直打，
//                              等着被"取消"杀（用来判"取消要真停"）。
//
// 它**不进发行包**（package.ps1 只拷 xrkit_app.exe 与依赖），只是构建产物。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void emit(const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

const char* kEncoders =
    "Encoders:\n"
    " V..... = Video\n"
    " A..... = Audio\n"
    " S..... = Subtitle\n"
    " ------\n"
    " V....D libtheora            libtheora (Theora)\n"
    " V....D libxvid              libxvid (Xvid)\n"
    " V....D mpeg4                MPEG-4 part 2\n"
    " A....D libvorbis            libvorbis (Vorbis)\n";

const char* kProbeJson =
    "{\"streams\":[{\"index\":0,\"codec_name\":\"theora\",\"codec_type\":\"video\","
    "\"width\":640,\"height\":480,\"avg_frame_rate\":\"25/1\",\"bit_rate\":\"800000\"},"
    "{\"index\":1,\"codec_name\":\"vorbis\",\"codec_type\":\"audio\",\"sample_rate\":\"44100\","
    "\"channels\":2,\"bit_rate\":\"128000\"}],"
    "\"format\":{\"filename\":\"OUT.ogm\",\"format_name\":\"ogg\",\"duration\":\"10.000000\","
    "\"size\":\"1048576\"}}\n";

bool has_flag(const std::vector<std::string>& args, const char* needle) {
    for (const std::string& arg : args) {
        if (arg == needle) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (has_flag(args, "-encoders")) {
        std::fwrite(kEncoders, 1, std::strlen(kEncoders), stdout);
        std::fflush(stdout);
        return 0;
    }
    if (has_flag(args, "-print_format")) {
        std::fwrite(kProbeJson, 1, std::strlen(kProbeJson), stdout);
        std::fflush(stdout);
        return 0;
    }

    // 转码：输出 = 最后一个参数（`plan_transcode` 的形态：… -f ogg <输出>）
    const std::string output = args.empty() ? std::string() : args.back();
    if (!output.empty()) {
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        out << "fake-ogm";
        out.flush();
    }
    const char* mode = std::getenv("XRKIT_FAKE_FFMPEG_MODE");
    const bool loop = mode != nullptr && std::strcmp(mode, "loop") == 0;
    if (mode != nullptr && std::strcmp(mode, "fail") == 0) {
        emit("boom-invalid-input: fake ffmpeg refuses this file\n");
        return 3;
    }
    char line[160];
    for (int tick = 1; loop || tick <= 8; ++tick) {
        const int centis = tick * 25;  // 每跳 0.25 秒
        std::snprintf(line, sizeof(line),
                      "frame=%d fps=25 q=0.0 size=0kB time=00:00:%02d.%02d bitrate=0.0kbits/s\n",
                      tick, centis / 100, centis % 100);
        emit(line);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return 0;
}
