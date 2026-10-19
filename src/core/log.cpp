#include "xrkit/core/log.h"

#include <cstdio>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
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

void log_block(std::string_view text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file.is_open()) return;   // 没挂文件就什么都不做（stderr 不给：UI 行不该污染控制台）
    g_file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (text.empty() || text.back() != '\n') g_file.put('\n');
    g_file.flush();                  // ★ 一次 flush（跟着 LogBox 的 80 ms 批量走）
}

PruneResult prune_log_files(const std::string& dir, int keep, const std::string& protect_path) {
    PruneResult res;
    if (dir.empty() || keep <= 0) return res;
    std::error_code ec;
    std::filesystem::path root = std::filesystem::path(dir);
    if (!std::filesystem::is_directory(root, ec)) return res;

    struct Item {
        std::filesystem::path path;
        std::filesystem::file_time_type when;
    };
    std::vector<Item> items;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        const std::string name = it->path().filename().string();
        // 只认 log_*.txt（同秒后缀 _2/_3 也算），别误删别的文件
        if (name.rfind("log_", 0) != 0) continue;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) continue;
        std::error_code tec;
        const auto when = it->last_write_time(tec);
        items.push_back({it->path(), tec ? std::filesystem::file_time_type{} : when});
    }
    if (static_cast<int>(items.size()) <= keep) return res;

    // 新的在前。★ **并列时按文件名倒序**兜底：同一秒内的 `log_..._2.txt`、`_3.txt` 以及
    //   刚写出来的一批文件，mtime 精度/复制都可能抹平成同一个值 —— 只比时间的话"谁该被删"
    //   会退化成目录迭代顺序（随机器漂移）。文件名就是我们自己的时间戳命名，倒序＝新的在前。
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.when != b.when) return a.when > b.when;
        return a.path.filename().string() > b.path.filename().string();
    });
    const std::filesystem::path protect = std::filesystem::path(protect_path);
    int kept = 0;
    for (const Item& item : items) {
        // ★ "当前这份"用**两条**判等的或：整路径相等 **或** 文件名相等。
        //   只比整路径是不够的 —— 调用方传进来的字符串与迭代器给的路径可能在不同环节被规范化
        //   （大小写 / 短名 / 分隔符），我第一版只比整路径，结果把正在写的那份删了，
        //   自检当场抓出来（情形 B：deleted=1，本该 0）。
        const bool is_protect =
            !protect.empty() && (item.path == protect ||
                                 (!protect.filename().empty() &&
                                  item.path.filename() == protect.filename()));
        if (kept < keep || is_protect) {
            ++kept;                        // 前 keep 份留下；当前这份无论排第几都留
            continue;
        }
        std::error_code rec;
        std::filesystem::remove(item.path, rec);
        if (rec) ++res.failed; else ++res.deleted;
    }
    return res;
}

}  // namespace xrkit::core
