#include "xrkit/core/log.h"
#include "xrkit/core/path.h"

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
        // ★ H2（全量审计）：`filename().string()` 在 Windows 上按 **CP_ACP** 窄化 ——
        //   非 ACP 可表示的名字（西里尔/中文）会塌成 `?`，导致日志轮转的"当前这一份"判等失配。
        const std::string name = path_to_utf8(it->path().filename());
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
        return path_to_utf8(a.path.filename()) >
               path_to_utf8(b.path.filename());
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

namespace {

std::string read_small_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_small_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

std::string unique_path_for(const std::filesystem::path& want) {
    // ★ 不能用 `.string()`：它按 ACP 窄化，文件名里有中文（`_异常退出`）会抛
    //   "No mapping for the Unicode character…"（自检实测）。
    if (!std::filesystem::exists(want)) return xrkit::core::path_to_utf8(want);
    const std::string stem = xrkit::core::path_to_utf8(want.stem());
    const std::string ext = xrkit::core::path_to_utf8(want.extension());
    for (int i = 2; i < 1000; ++i) {
        const auto cand = want.parent_path() / xrkit::core::path_from_utf8(
                                                 stem + "_" + std::to_string(i) + ext);
        if (!std::filesystem::exists(cand)) return xrkit::core::path_to_utf8(cand);
    }
    return xrkit::core::path_to_utf8(want);
}

}  // namespace

SessionSweep sweep_previous_session(const std::string& dir) {
    SessionSweep out;
    const std::filesystem::path base = xrkit::core::path_from_utf8(dir);
    const std::filesystem::path marker = base / ".session";
    if (!std::filesystem::exists(marker)) return out;
    out.had_marker = true;
    const std::string text = read_small_file(marker);
    // 格式：第一行 log 路径，第二行 state
    const std::size_t nl = text.find('\n');
    const std::string log_path = nl == std::string::npos ? std::string() : text.substr(0, nl);
    const std::string state = nl == std::string::npos ? std::string() : text.substr(nl + 1);
    std::error_code ec;
    if (state.rfind("finished", 0) == 0) {
        // 上一轮正常退出 ⇒ 重置（删掉它那份日志）
        if (!log_path.empty() && std::filesystem::exists(log_path, ec)) {
            std::filesystem::remove(xrkit::core::path_from_utf8(log_path), ec);
            if (!ec) out.removed_log = log_path;
        }
    } else if (!log_path.empty() && std::filesystem::exists(log_path, ec)) {
        // 上一轮没走到正常退出 ⇒ 按命名规则保留
        out.previous_crashed = true;
        const std::filesystem::path src = xrkit::core::path_from_utf8(log_path);
        const auto dest = unique_path_for(
            src.parent_path() /
            xrkit::core::path_from_utf8(xrkit::core::path_to_utf8(src.stem()) + "_abnormal_exit" +
                                        xrkit::core::path_to_utf8(src.extension())));
        std::filesystem::rename(src, dest, ec);
        if (!ec) out.kept_log = xrkit::core::path_to_utf8(dest);
    }
    std::filesystem::remove(marker, ec);
    return out;
}

void mark_session_started(const std::string& dir, const std::string& log_path) {
    std::error_code ec;
    std::filesystem::create_directories(xrkit::core::path_from_utf8(dir), ec);
    const bool ok = write_small_file(xrkit::core::path_from_utf8(dir) / ".session",
                                     log_path + "\nstarted\n");
    if (!ok) xrkit::core::log(Level::Warn, "SESSION warn：写不了 .session（" + dir + "）");
}

void mark_session_finished(const std::string& dir) {
    const std::filesystem::path marker = xrkit::core::path_from_utf8(dir) / ".session";
    std::string text = read_small_file(marker);
    const std::size_t nl = text.find('\n');
    if (nl != std::string::npos) text = text.substr(0, nl + 1) + "finished\n";
    write_small_file(marker, text);
}

}  // namespace xrkit::core
