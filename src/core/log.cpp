#include "xrkit/core/log.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace xrkit::core {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
bool g_active = false;

const char* level_tag(Level level) {
    switch (level) {
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERR ";
    }
    return "????";
}

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
    localtime_s(&tm_buf, &t);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

}  // namespace

void set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_active = false;
    if (path.empty()) return;
    g_file.open(path, std::ios::app | std::ios::binary);
    g_active = g_file.is_open();  // 打不开就只走 stderr，绝不因日志失败中断程序
}

bool log_file_active() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active;
}

void log(Level level, std::string_view msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string line;
    line.reserve(msg.size() + 32);
    line += "[";
    line += now_stamp();
    line += "] ";
    line += level_tag(level);
    line += " ";
    line.append(msg.data(), msg.size());
    line += "\n";

    std::fwrite(line.data(), 1, line.size(), stderr);
    if (g_file.is_open()) {
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
    }
}

}  // namespace xrkit::core
