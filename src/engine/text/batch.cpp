// xrkit::engine::text —— 编码转换页的批处理层实现（口径与出处见 batch.h 的头注释）。
//
// 这里刻意**没有**任何 `recursive_directory_iterator`：目录遍历全部转调
// `engine::fs::collect_tree()`（仓库里已有的那一份，自带 `WalkReport` 台账）。

#include "xrkit/engine/text/batch.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/overlay.h"
#include "xrkit/engine/text/convert.h"
#include "xrkit/engine/text/encoding.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace xrkit::engine::text {
namespace {

namespace stdfs = std::filesystem;
namespace efs = xrkit::engine::fs;

char lower_ascii_char(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower_ascii(const std::string& text) {
    std::string out = text;
    for (char& c : out) c = lower_ascii_char(c);
    return out;
}

bool ends_with_ci(const std::string& text, const std::string& tail) {
    if (tail.size() > text.size()) return false;
    const size_t off = text.size() - tail.size();
    for (size_t i = 0; i < tail.size(); ++i) {
        if (lower_ascii_char(text[off + i]) != lower_ascii_char(tail[i])) return false;
    }
    return true;
}

std::string basename_of(const std::string& rel) {
    const size_t slash = rel.find_last_of("/\\");
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

std::string dirname_of(const std::string& rel) {
    const size_t slash = rel.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
}

int depth_of(const std::string& rel) {
    int depth = 1;
    for (char c : rel) {
        if (c == '/' || c == '\\') ++depth;
    }
    return depth;
}

// rel 的某一段是否等于 segment（1.x 的 `BACKUP_DIRNAME in relpath.split(os.sep)`）。
bool has_segment(const std::string& rel, const std::string& segment) {
    if (segment.empty()) return false;
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find_first_of("/\\", pos);
        const std::string seg =
            rel.substr(pos, (slash == std::string::npos ? rel.size() : slash) - pos);
        if (seg == segment) return true;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return false;
}

std::vector<std::string> split_patterns(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t comma = text.find(',', pos);
        std::string item =
            text.substr(pos, (comma == std::string::npos ? text.size() : comma) - pos);
        // strip（1.x `[p.strip() for p in ... if p.strip()]`）
        const size_t b = item.find_first_not_of(" \t\r\n");
        const size_t e = item.find_last_not_of(" \t\r\n");
        item = (b == std::string::npos) ? std::string() : item.substr(b, e - b + 1);
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

// ★★ P17-95（审计 `02 F-12`）：路径拼接**一律用 `/`** —— 与 `font/package.cpp:45-49` 那份已改好的
//   join 同口径（`/` 在 Windows 上同样合法，也不会把"分隔符"硬编成一种平台的写法）。
std::string join_path(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    std::string out = dir;
    const char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
    std::string tail = rel;
    for (char& c : tail) {
        if (c == '\\') c = '/';
    }
    return out + tail;
}

// `fnmatch` 的通配符匹配（`*` / `?` / `[seq]`），**大小写不敏感**（Windows 下
// `fnmatch.fnmatch` 走 `os.path.normcase`，1.x 的实际行为就是如此）。
bool fnmatch_ci(const std::string& pat, size_t pi, const std::string& text, size_t ti) {
    while (pi < pat.size()) {
        const char pc = pat[pi];
        if (pc == '*') {
            while (pi < pat.size() && pat[pi] == '*') ++pi;
            if (pi == pat.size()) return true;
            for (size_t k = ti;; ++k) {
                if (fnmatch_ci(pat, pi, text, k)) return true;
                if (k >= text.size()) return false;
            }
        }
        if (pc == '?') {
            if (ti >= text.size()) return false;
            ++pi;
            ++ti;
            continue;
        }
        if (pc == '[') {
            if (ti >= text.size()) return false;
            size_t k = pi + 1;
            bool negate = false;
            if (k < pat.size() && (pat[k] == '!' || pat[k] == '^')) {
                negate = true;
                ++k;
            }
            bool hit = false;
            bool first = true;
            while (k < pat.size() && (pat[k] != ']' || first)) {
                first = false;
                if (k + 2 < pat.size() && pat[k + 1] == '-' && pat[k + 2] != ']') {
                    if (text[ti] >= pat[k] && text[ti] <= pat[k + 2]) hit = true;
                    k += 3;
                } else {
                    if (text[ti] == pat[k]) hit = true;
                    ++k;
                }
            }
            if (k >= pat.size()) {
                // 没有闭合的 `]`：Python 的 fnmatch 把 `[` 当字面量。
                if (text[ti] != pc) return false;
                ++pi;
                ++ti;
                continue;
            }
            ++k;  // 跳过 ']'
            if (hit == negate) return false;
            pi = k;
            ++ti;
            continue;
        }
        if (ti >= text.size() || pc != text[ti]) return false;
        ++pi;
        ++ti;
    }
    return ti == text.size();
}

bool hard_match(const std::string& pattern, const std::string& text) {
    return fnmatch_ci(lower_ascii(pattern), 0, lower_ascii(text), 0);
}

double seconds_since(const std::chrono::steady_clock::time_point& start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

bool glob_match(const std::string& pattern, const std::string& text) {
    // 空 pattern 不匹配任何东西（1.x：空串在 split 后就被丢掉了，到不了这里）。
    if (pattern.empty()) return false;
    return hard_match(pattern, text);
}

bool is_excluded(const std::string& rel_path, const std::string& file_name,
                 const std::string& patterns) {
    const std::vector<std::string> pats = split_patterns(patterns);
    if (pats.empty()) return false;
    std::string rel_bs = rel_path;
    for (char& c : rel_bs) {
        if (c == '/') c = '\\';
    }
    for (const std::string& pattern : pats) {
        if (hard_match(pattern, file_name)) return true;
        if (hard_match(pattern, rel_path)) return true;
        if (rel_bs != rel_path && hard_match(pattern, rel_bs)) return true;
        // ③ 1.x 的 `pattern.replace('*','') in rel_path`（`in` 是**大小写敏感**的，
        //    这里照抄不折大小写；同时喂两种分隔符写法）。
        std::string stripped;
        for (char c : pattern) {
            if (c != '*') stripped.push_back(c);
        }
        if (!stripped.empty() &&
            (rel_path.find(stripped) != std::string::npos ||
             rel_bs.find(stripped) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

std::vector<CollectedFile> collect_files(const std::string& root, const CollectOptions& opt,
                                        CollectReport* report) {
    std::vector<CollectedFile> out;
    if (report != nullptr) *report = CollectReport{};

    efs::WalkReport walk;
    const std::vector<efs::TreeItem> items = efs::collect_tree(root, &walk);
    if (report != nullptr) {
        report->depth = walk.max_depth;
        report->dirs = walk.dirs;
        report->files_seen = walk.files;
        report->errors = walk.errors;
    }

    for (const efs::TreeItem& item : items) {
        if (item.is_dir) continue;
        const std::string rel = item.path;
        const std::string name = basename_of(rel);
        const int depth = depth_of(rel);
        // ① 在备份区里 / ② 本身是 .bak（1.x 在 `os.walk` 那一层就跳过整个 backup
        //    子树 ⇒ 这两条排在"递归/层数"之前，与 1.x 的先后一致）
        if (has_segment(dirname_of(rel), opt.backup_dirname) ||
            ends_with_ci(name, opt.backup_suffix)) {
            if (report != nullptr) ++report->skipped_backup;
            continue;
        }
        // ③ 非递归：只认根下文件（1.x `os.walk` + `break`）
        if (!opt.recursive && depth > 1) {
            if (report != nullptr) ++report->skipped_depth;
            continue;
        }
        // ④ 后缀白名单
        if (!opt.extensions.empty()) {
            bool wanted = false;
            for (const std::string& ext : opt.extensions) {
                if (ends_with_ci(name, ext)) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) {
                if (report != nullptr) ++report->skipped_ext;
                continue;
            }
        }
        // ⑤ 排除模式
        if (is_excluded(rel, name, opt.exclude_patterns)) {
            if (report != nullptr) {
                ++report->skipped_excluded;
                report->excluded.push_back(rel);
            }
            continue;
        }
        out.push_back(CollectedFile{rel, item.source_path});
    }
    if (report != nullptr) report->selected = static_cast<int>(out.size());
    return out;
}

std::string backup_path_for(const std::string& original, const std::string& source_root,
                           const std::string& backup_dirname, const std::string& backup_suffix,
                           std::string* error) {
    if (error != nullptr) error->clear();
    if (source_root.empty()) {
        if (error != nullptr) *error = "未选择源目录";
        return {};
    }
    const stdfs::path root = xrkit::core::path_from_utf8(source_root);
    const stdfs::path orig = xrkit::core::path_from_utf8(original);
    std::error_code ec;
    stdfs::path rel = stdfs::relative(orig, root, ec);
    std::string rel_text;
    if (ec) {
        ec.clear();
        rel_text = xrkit::core::path_to_utf8(orig.filename());
    } else {
        rel_text = xrkit::core::path_to_utf8(rel);
    }
    // 1.x：`rel_path.startswith("..")` ⇒ 退回 basename。
    if (rel_text.empty() || rel_text.rfind("..", 0) == 0) {
        rel_text = xrkit::core::path_to_utf8(orig.filename());
    }
    if (rel_text.empty()) {
        if (error != nullptr) *error = "算不出相对路径：" + original;
        return {};
    }
    return join_path(join_path(source_root, backup_dirname), rel_text) + backup_suffix;
}

BackupOutcome backup_file(const std::string& original, const std::string& source_root,
                          const std::string& backup_dirname, const std::string& backup_suffix) {
    BackupOutcome out;
    std::string error;
    out.path = backup_path_for(original, source_root, backup_dirname, backup_suffix, &error);
    if (out.path.empty()) {
        out.error = error.empty() ? "算不出备份路径" : error;
        return out;
    }
    const stdfs::path target = xrkit::core::path_from_utf8(out.path);
    std::error_code ec;
    if (stdfs::exists(target, ec)) {
        out.ok = true;
        out.existed = true;  // 1.x 的 `[备份跳过]`（不覆盖已有备份）
        out.bytes = stdfs::file_size(target, ec);
        return out;
    }
    ec.clear();
    stdfs::create_directories(target.parent_path(), ec);
    if (ec) {
        out.error = "建备份目录失败：" + ec.message();
        return out;
    }
    ec.clear();
    stdfs::copy_file(xrkit::core::path_from_utf8(original), target, stdfs::copy_options::none, ec);
    if (ec) {
        out.error = "复制失败：" + ec.message();
        return out;
    }
    ec.clear();
    out.bytes = stdfs::file_size(target, ec);
    out.ok = true;
    return out;
}

namespace {

std::string backup_root_of(const BackupTaskOptions& opt) {
    return join_path(opt.source_root, opt.backup_dirname);
}

// 列可恢复/可清空的 .bak（后缀白名单与页面的"扩展名"框同源）。
std::vector<std::pair<std::string, std::string>> list_bak_files(const BackupTaskOptions& opt,
                                                               int* depth,
                                                               std::vector<std::string>* errors) {
    std::vector<std::pair<std::string, std::string>> out;  // {本机路径, 相对备份根的路径}
    const std::string backup_root = backup_root_of(opt);
    efs::WalkReport walk;
    const std::vector<efs::TreeItem> items = efs::collect_tree(backup_root, &walk);
    if (depth != nullptr) *depth = walk.max_depth;
    if (errors != nullptr) *errors = walk.errors;
    for (const efs::TreeItem& item : items) {
        if (item.is_dir) continue;
        if (!opt.recursive && depth_of(item.path) > 1) continue;
        bool wanted = false;
        for (const std::string& ext : opt.extensions) {
            if (ends_with_ci(item.path, ext + opt.backup_suffix)) {
                wanted = true;
                break;
            }
        }
        if (!wanted) continue;
        out.emplace_back(item.source_path, item.path);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

}  // namespace

BackupTaskResult restore_backups(const BackupTaskOptions& opt, const BatchHooks& hooks) {
    BackupTaskResult result;
    auto say = [&hooks](const std::string& text) {
        if (hooks.message) hooks.message(text);
    };
    if (opt.source_root.empty()) {
        result.errors.push_back("未选择源目录");
        say("[恢复失败] 未选择源目录");
        return result;
    }
    const std::string backup_root = backup_root_of(opt);
    std::error_code ec;
    if (!stdfs::is_directory(xrkit::core::path_from_utf8(backup_root), ec)) {
        // 1.x：明确提示"未找到backup目录"，绝不静默报 0。
        result.dir_missing = true;
        say("未找到 backup 目录：" + backup_root);
        return result;
    }
    if (opt.extensions.empty()) {
        result.errors.push_back("请输入文件扩展名，例如 .xml,.txt");
        say("[恢复失败] 请输入文件扩展名，例如 .xml,.txt");
        return result;
    }
    const auto files = list_bak_files(opt, &result.depth, &result.errors);
    for (const std::string& e : result.errors) say("[遍历] " + e);
    result.total = static_cast<int>(files.size());
    if (result.total == 0) {
        say("未找到 .bak 备份文件");
        return result;
    }
    if (hooks.progress) hooks.progress(0, result.total);
    for (const auto& [bak, rel] : files) {
        if (hooks.should_stop && hooks.should_stop()) {
            result.cancelled = true;
            result.stop_reason = "已取消：在文件边界停下（已恢复 " + std::to_string(result.ok) +
                                 " / 共 " + std::to_string(result.total) + " 个，失败 " +
                                 std::to_string(result.failed) + " 个）";
            say(result.stop_reason);
            break;
        }
        if (hooks.gate && !hooks.gate()) {
            result.cancelled = true;
            result.stop_reason = "已取消：在文件边界停下（已恢复 " + std::to_string(result.ok) +
                                 " / 共 " + std::to_string(result.total) + " 个）";
            say(result.stop_reason);
            break;
        }
        BackupItemResult item;
        item.bak = bak;
        // 1.x：`relpath(bak, backup_root)[:-len('.bak')]` ⇒ 原文件 = 源根 / 该相对路径
        std::string inner = rel;
        if (ends_with_ci(inner, opt.backup_suffix)) {
            inner = inner.substr(0, inner.size() - opt.backup_suffix.size());
        }
        item.target = join_path(opt.source_root, inner);
        std::error_code local_ec;
        if (!stdfs::exists(xrkit::core::path_from_utf8(item.target), local_ec)) {
            item.error = "原文件不存在：" + item.target;
            ++result.failed;
            say("[恢复失败] " + basename_of(bak) + " | " + item.error);
            result.items.push_back(std::move(item));
            continue;
        }
        local_ec.clear();
        stdfs::copy_file(xrkit::core::path_from_utf8(bak), xrkit::core::path_from_utf8(item.target),
                         stdfs::copy_options::overwrite_existing, local_ec);
        if (local_ec) {
            item.error = "复制失败：" + local_ec.message();
            ++result.failed;
            say("[恢复失败] " + basename_of(bak) + " | " + item.error);
        } else {
            item.ok = true;
            ++result.ok;
            say("[恢复] " + basename_of(bak) + " → " + item.target);
        }
        result.items.push_back(std::move(item));
        if (hooks.progress) hooks.progress(result.ok + result.failed, result.total);
    }
    if (!result.cancelled) {
        say("恢复结束：成功 " + std::to_string(result.ok) + " / 失败 " +
            std::to_string(result.failed) + " / 候选 " + std::to_string(result.total));
    }
    return result;
}

BackupTaskResult clear_backups(const BackupTaskOptions& opt, const BatchHooks& hooks) {
    BackupTaskResult result;
    auto say = [&hooks](const std::string& text) {
        if (hooks.message) hooks.message(text);
    };
    if (opt.source_root.empty()) {
        result.errors.push_back("未选择源目录");
        say("[清空失败] 未选择源目录");
        return result;
    }
    const std::string backup_root = backup_root_of(opt);
    std::error_code ec;
    if (!stdfs::is_directory(xrkit::core::path_from_utf8(backup_root), ec)) {
        result.dir_missing = true;
        say("未找到 backup 目录：" + backup_root);
        return result;
    }
    if (opt.extensions.empty()) {
        result.errors.push_back("请输入文件扩展名，例如 .xml,.txt");
        say("[清空失败] 请输入文件扩展名，例如 .xml,.txt");
        return result;
    }
    const auto files = list_bak_files(opt, &result.depth, &result.errors);
    for (const std::string& e : result.errors) say("[遍历] " + e);
    result.total = static_cast<int>(files.size());
    if (result.total == 0) {
        say("未找到 .bak 备份文件");
        return result;
    }
    for (const auto& [bak, rel] : files) {
        if (hooks.should_stop && hooks.should_stop()) {
            result.cancelled = true;
            result.stop_reason = "已取消：在文件边界停下（已删除 " + std::to_string(result.ok) +
                                 " / 共 " + std::to_string(result.total) + " 个，失败 " +
                                 std::to_string(result.failed) + " 个）";
            say(result.stop_reason);
            break;
        }
        if (hooks.gate && !hooks.gate()) {
            result.cancelled = true;
            result.stop_reason = "已取消：在文件边界停下（已删除 " + std::to_string(result.ok) +
                                 " / 共 " + std::to_string(result.total) + " 个）";
            say(result.stop_reason);
            break;
        }
        BackupItemResult item;
        item.bak = bak;
        std::error_code local_ec;
        const bool removed = stdfs::remove(xrkit::core::path_from_utf8(bak), local_ec);
        if (local_ec || !removed) {
            item.error = local_ec ? ("删除失败：" + local_ec.message()) : "删除失败：文件不存在";
            ++result.failed;
            say("[删除失败] " + basename_of(bak) + " | " + item.error);
        } else {
            item.ok = true;
            ++result.ok;
            say("[删除备份] " + basename_of(bak) + " → " + bak);
        }
        result.items.push_back(std::move(item));
        if (hooks.progress) hooks.progress(result.ok + result.failed, result.total);
    }
    if (!result.cancelled) {
        say("清空结束：成功 " + std::to_string(result.ok) + " / 失败 " +
            std::to_string(result.failed) + " / 候选 " + std::to_string(result.total));
    }
    return result;
}

std::string format_duration(double seconds) {
    if (seconds < 0) seconds = 0;
    long long total = static_cast<long long>(seconds);
    const long long days = total / 86400;
    total %= 86400;
    const long long hours = total / 3600;
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    char buf[64];
    // Python `str(timedelta)`：`H:MM:SS`；跨天是 `N day, H:MM:SS`（1.x 原样写进 CSV）。
    if (days > 0) {
        std::snprintf(buf, sizeof(buf), "%lld day%s, %lld:%02lld:%02lld", days,
                      days == 1 ? "" : "s", hours, minutes, secs);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", hours, minutes, secs);
    }
    return buf;
}

bool write_stats_csv(const std::string& path, const ConvertStats& stats, std::string* error) {
    std::ofstream out(xrkit::core::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "写不出文件：" + path;
        return false;
    }
    // utf-8-sig：1.x 用 `encoding='utf-8-sig'` ⇒ 三个字节 BOM 在**文件最前面**。
    static const char kBom[] = "\xEF\xBB\xBF";
    out.write(kBom, 3);
    out << "编码转换统计报告\n";
    out << "生成时间," << stats.generated_at << "\n";
    out << "源目录/文件," << stats.source << "\n";
    out << "处理模式," << (stats.single_file ? "单个文件" : "文件夹") << "\n";
    out << "输出目录," << (stats.output_dir.empty() ? "原地转换" : stats.output_dir) << "\n";
    out << "源编码," << stats.source_enc << "\n";
    out << "目标编码," << stats.target_enc << "\n";
    out << "候选文件数," << stats.candidates << "\n";
    out << "已处理," << stats.processed << "\n";
    out << "成功转换," << stats.success << "\n";
    out << "跳过文件," << stats.skipped << "\n";
    out << "失败文件," << stats.failed << "\n";
    out << "耗时," << stats.duration << "\n";
    out.flush();
    if (!out) {
        if (error != nullptr) *error = "写入中途失败：" + path;
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

BatchOutcome convert_batch(const BatchOptions& opt, const BatchHooks& hooks) {
    BatchOutcome out;
    const auto started = std::chrono::steady_clock::now();
    auto say = [&hooks](const std::string& text) {
        if (hooks.message) hooks.message(text);
    };

    const stdfs::path src_path = xrkit::core::path_from_utf8(opt.source);
    std::error_code ec;
    const bool is_file = stdfs::is_regular_file(src_path, ec);
    ec.clear();
    const bool is_dir = !is_file && stdfs::is_directory(src_path, ec);

    std::vector<CollectedFile> files;
    if (is_file) {
        out.single_file = true;
        out.collect.depth = 1;
        out.collect.files_seen = 1;
        const std::string name = xrkit::core::path_to_utf8(src_path.filename());
        if (is_excluded(name, name, opt.exclude_patterns)) {
            ++out.collect.skipped_excluded;
            out.collect.excluded.push_back(name);
            say("[排除] " + name + "（命中排除模式）");
        } else {
            files.push_back(CollectedFile{name, opt.source});
            out.collect.selected = 1;
        }
    } else if (is_dir) {
        CollectOptions co;
        co.extensions = opt.extensions;
        co.recursive = opt.recursive;
        co.exclude_patterns = opt.exclude_patterns;
        co.backup_dirname = opt.backup_dirname;
        co.backup_suffix = opt.backup_suffix;
        files = collect_files(opt.source, co, &out.collect);
    } else {
        out.collect.errors.push_back(opt.source + " | 既不是文件也不是目录");
        say("[错误] 源路径既不是文件也不是目录：" + opt.source);
        out.duration = format_duration(seconds_since(started));
        out.exit_code = 2;
        return out;
    }

    // ★ 台账（零静默）：递归了几层、扫到多少、各自为什么被刷掉 —— 一句都不许省。
    std::string scan = out.single_file
                           ? "扫描：单个文件"
                           : ("扫描：递归 " + std::to_string(out.collect.depth) + " 层，目录 " +
                              std::to_string(out.collect.dirs) + " 个，文件 " +
                              std::to_string(out.collect.files_seen) + " 个");
    scan += " ⇒ 选中 " + std::to_string(out.collect.selected);
    scan += "，后缀不符 " + std::to_string(out.collect.skipped_ext);
    scan += "，命中排除 " + std::to_string(out.collect.skipped_excluded);
    scan += "，备份区跳过 " + std::to_string(out.collect.skipped_backup);
    if (out.collect.skipped_depth > 0) {
        scan += "，只扫顶层挡掉 " + std::to_string(out.collect.skipped_depth);
    }
    if (!out.collect.errors.empty()) {
        scan += "，遍历错误 " + std::to_string(out.collect.errors.size());
    }
    say(scan);
    for (const std::string& e : out.collect.errors) say("[遍历] " + e);
    for (const std::string& e : out.collect.excluded) say("[排除] " + e);

    out.candidates = static_cast<int>(files.size());
    if (hooks.progress) hooks.progress(0, out.candidates);
    if (files.empty()) {
        say("没找到可转换的文件");
        out.duration = format_duration(seconds_since(started));
        out.exit_code = out.collect.errors.empty() ? 0 : 1;
        return out;
    }

    for (const CollectedFile& file : files) {
        // ★ **文件边界**轮询取消：停在这里，已转换的文件都是完整的（不写半个）。
        if (hooks.should_stop && hooks.should_stop()) {
            out.cancelled = true;
            out.stop_reason = "已取消：在文件边界停下（已处理 " + std::to_string(out.processed) +
                              " / " + std::to_string(out.candidates) + " 个文件，失败 " +
                              std::to_string(out.failed) + " 个；已转换的产物都已完整落盘）";
            say(out.stop_reason);
            break;
        }
        if (hooks.gate && !hooks.gate()) {
            out.cancelled = true;
            out.stop_reason = "已取消：在文件边界停下（已处理 " + std::to_string(out.processed) +
                              " / " + std::to_string(out.candidates) + " 个文件）";
            say(out.stop_reason);
            break;
        }
        const std::string name = basename_of(file.rel);

        // 备份（1.x：只在**原地**转换且勾选时做，逐文件、在写盘之前）
        if (opt.output_dir.empty() && opt.backup) {
            const BackupOutcome b =
                backup_file(file.full, opt.source, opt.backup_dirname, opt.backup_suffix);
            if (!b.ok) {
                ++out.backup_failed;
                say("[备份失败] " + name + " | " + b.error);
            } else if (b.existed) {
                ++out.backup_existed;
                say("[备份跳过] " + name + " 已存在备份文件：" + b.path);
            } else {
                ++out.backed_up;
                say("[备份] " + name + " → " + b.path);
            }
        }

        std::string target;
        if (!opt.output_dir.empty()) {
            // 1.x：目录模式保留**相对结构**（`out/output_dir/<rel>`）。扁平化会让不同
            // 子目录的同名文件互相覆盖 —— 那是静默丢产物，必须是相对结构。
            target = join_path(opt.output_dir, file.rel);
            const stdfs::path parent = xrkit::core::path_from_utf8(target).parent_path();
            std::error_code mk;
            stdfs::create_directories(parent, mk);
            if (mk) {
                ++out.failed;
                ++out.processed;
                say("[失败] " + name + " | 建输出目录失败：" + mk.message());
                continue;
            }
        }

        ConvertOptions co;
        co.source_encoding = opt.source_encoding;
        co.target_encoding = opt.target_encoding;
        co.to_output_dir = !opt.output_dir.empty();
        const ConvertOutcome r = convert_file(file.full, target, co);
        if (r.needs_confirm) {
            // ★★ P17-87（§四·9）：**没有窗口可弹 ⇒ 不许猜** —— 当场停下，把问句与诊断
            //   原样交出去（调用方负责报出 + 非零退出）。**这一份与后面的都不转**（不替用户选）。
            //   ★ 不把它算进 `failed`：它不是"转换失败"，是"需要用户先决定"（退码 3）。
            out.needs_confirm = true;
            out.confirm_path = file.full;
            out.confirm_question = r.confirm_question;
            out.confirm_diagnostics = r.confirm_diagnostics;
            say("[需要确认] " + name + " | " + r.step_label +
                "（没有窗口可弹 ⇒ 已停下，未转换任何文件）");
            out.stop_reason = "需要用户确认编码：" + name;
            break;
        }
        if (!r.ok) {
            ++out.failed;
        } else if (r.skipped) {
            ++out.skipped;
        } else {
            ++out.success;
        }
        // 零静默（2026-10-06 用户口径③）：判定的**不确定部分**逐条上屏。
        if (r.low_confidence) ++out.low_confidence;
        if (r.override_decl) ++out.override_decl;
        if (r.used_mismatch) ++out.used_mismatch;
        std::string line = name + " | " + r.step_label;
        if (!r.flags_label.empty()) line += " | " + r.flags_label;
        if (r.skipped) line += " | 已是目标编码";
        line += " → " + opt.target_encoding;
        if (!r.ok) line += " | 失败：" + r.error;
        say(line);
        ++out.processed;
        if (hooks.progress) hooks.progress(out.processed, out.candidates);
    }

    out.duration = format_duration(seconds_since(started));
    // 1.x 的"转换统计"块（逐字段同源 ⇒ 日志与导出的 CSV 不可能分叉）
    say("候选文件总数：" + std::to_string(out.candidates));
    say("本次已处理：" + std::to_string(out.processed));
    say("成功转换：" + std::to_string(out.success));
    say("跳过文件：" + std::to_string(out.skipped));
    say("失败文件：" + std::to_string(out.failed));
    // ★ 判定置信度的汇总报数（**不是失败**，但绝不能静默 —— 上方逐条已留痕）。
    say("低置信判定：" + std::to_string(out.low_confidence) + "（纯猜/无证据，逐条见上方标记）");
    say("统计否决声明：" + std::to_string(out.override_decl));
    say("判定≠实际使用编码：" + std::to_string(out.used_mismatch));
    if (opt.output_dir.empty() && opt.backup) {
        say("备份：新增 " + std::to_string(out.backed_up) + " / 已有 " +
            std::to_string(out.backup_existed) + " / 失败 " + std::to_string(out.backup_failed));
    }
    say("总耗时：" + out.duration);

    // 零静默的收口：**部分失败 ⇒ 退出码非 0**（遍历错误、备份失败同样算失败）。
    // ★★ P17-87（§四·9）：**需要用户确认**单列一档退码 **3**（既不是成功、也不是失败 ——
    //   "没决定"这件事必须与"做坏了"区分得开，否则调用方会把它当成错误重试）。
    const bool bad = out.failed > 0 || out.backup_failed > 0 || !out.collect.errors.empty();
    out.exit_code = out.needs_confirm ? 3 : (bad ? 1 : 0);
    if (!bad && !out.cancelled && !out.needs_confirm) say("全部完成");
    return out;
}

}  // namespace xrkit::engine::text
