#include "xrkit/engine/fs/extract_tree.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/squashfs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace xrkit::engine::fs {
namespace {

namespace stdfs = std::filesystem;

constexpr const char* kRejectEmpty = "条目路径为空（或不是字符串）";
constexpr const char* kRejectAbsolute = "绝对路径 / UNC，拒绝写出";
constexpr const char* kRejectDrive = "带盘符，拒绝写出";
constexpr const char* kRejectDotDot = "分段含 ..（试图逃出输出目录）";
constexpr const char* kRejectFallback = "复核后仍不在输出目录下（含跨盘）";

bool ascii_iequal(char a, char b) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
    return lower(a) == lower(b);
}

// Windows 下路径比较**大小写不敏感**（1.x 的 `os.path.normcase`）；
// 其它平台按字节比。
bool component_equal(const std::string& a, const std::string& b) {
#ifdef _WIN32
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!ascii_iequal(a[i], b[i])) return false;
    }
    return true;
#else
    return a == b;
#endif
}

std::vector<std::string> split_segments(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// ⑤ 的兜底复核：joined 必须**逐组件**以 root 开头（不是字符串前缀比对 ——
// `C:\out` 与 `C:\out2` 是字符串前缀关系但不是同一棵树）。
bool is_within(const stdfs::path& root, const stdfs::path& joined) {
    std::vector<std::string> r;
    std::vector<std::string> j;
    for (const auto& part : root) r.push_back(xrkit::core::path_to_utf8(part));
    for (const auto& part : joined) j.push_back(xrkit::core::path_to_utf8(part));
    if (j.size() < r.size()) return false;
    for (size_t i = 0; i < r.size(); ++i) {
        if (!component_equal(r[i], j[i])) return false;
    }
    return true;
}

}  // namespace

OutPathResult safe_out_path(const std::string& out_dir, const std::string& rel) {
    OutPathResult result;
    if (rel.empty()) {
        result.reason = kRejectEmpty;
        return result;
    }

    std::string normalized = rel;
    for (char& c : normalized) {
        if (c == '\\') c = '/';  // ② 先归一，再判绝对
    }
    if (normalized.front() == '/') {
        result.reason = kRejectAbsolute;
        return result;
    }
    if (normalized.size() >= 2 && normalized[1] == ':') {
        result.reason = kRejectDrive;
        return result;
    }

    // ④ 拆段时**丢掉空段与 `.`**（1.x 这么做的，所以 `a//b`、`a/./b` 都是合法的
    //    "同一件事"，不能拒；拒的只有真正的 `..`）。
    std::vector<std::string> parts;
    for (const std::string& seg : split_segments(normalized, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            result.reason = kRejectDotDot;
            return result;
        }
        // 段里带**根名/根目录**的（Windows 上是 `D:` 这种盘符相对路径）：
        // 1.x 靠 `os.path.join` 的语义变化 + `commonpath` 的 ValueError 拦下它，
        // 这里显式拦 —— 判据是同一个（"拼完不再位于输出目录下"），只是不再依赖
        // "join 悄悄换根"这个副作用。
        const stdfs::path seg_path = xrkit::core::path_from_utf8(seg);
        if (seg_path.has_root_name() || seg_path.has_root_directory()) {
            result.reason = kRejectFallback;
            return result;
        }
        parts.push_back(seg);
    }
    if (parts.empty()) {
        result.reason = kRejectEmpty;
        return result;
    }

    // ⑤ 兜底复核。逐段拼（不用 `operator/`：Windows 上 rhs 带根名会**替换**整条路径，
    //    那正是"盘符相对路径"逃逸的形态，必须让它留在复核里被拦，而不是悄悄换根）。
    const stdfs::path root = xrkit::core::path_from_utf8(out_dir).lexically_normal();
    std::string joined_utf8 = xrkit::core::path_to_utf8(root);
    for (const std::string& seg : parts) {
        if (!joined_utf8.empty() && joined_utf8.back() != '/') joined_utf8 += '/';
        joined_utf8 += seg;
    }
    const stdfs::path joined = xrkit::core::path_from_utf8(joined_utf8).lexically_normal();
    if (!is_within(root, joined)) {
        result.reason = kRejectFallback;
        return result;
    }

    result.ok = true;
    result.path = xrkit::core::path_to_utf8(joined);
    return result;
}

std::string ExtractTreeResult::failure_note() const {
    if (failures.empty()) return std::string();
    const uint64_t total = files_written + failures.size();
    std::string note = std::to_string(failures.size()) + "/" + std::to_string(total) + " 项失败：";
    const size_t shown = std::min<size_t>(3, failures.size());
    for (size_t i = 0; i < shown; ++i) {
        if (i != 0) note += "；";
        note += failures[i].path + "：" + failures[i].reason;
    }
    if (failures.size() > shown) note += "…";
    return note;
}

ExtractTreeResult extract_tree(const std::string& image_path, const std::string& out_root,
                               const ExtractTreeOptions& options) {
    ExtractTreeResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&result, started]() {
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    };

    // 6.10：空选择 ≠ 全部。1.x 的 `if not files:` 同时覆盖 `None` 与 `[]`，
    // 于是"什么都没勾"会退化成整镜像导出、还被记为成功。2.0 在这里显式拒绝。
    if (options.selection == Selection::Listed && options.only.empty()) {
        result.error = "选中集为空：拒绝执行（1.x 会因此导出整镜像，2.0 不再退化成「全部」）";
        return finish();
    }
    if (out_root.empty()) {
        result.error = "输出根为空";
        return finish();
    }

    const ListResult listed = list_entries(image_path);
    if (!listed.ok) {
        result.error = listed.error;
        return finish();
    }
    result.ok = true;  // 从这里往后：单项失败算单项，不算整批失败

    // 选出本次要处理的条目（保持**清单顺序**：镜像列出的顺序已是路径字节序，
    // Listed 顺序则按调用方给的顺序，便于"谁失败"可复现）。
    std::vector<const Entry*> todo;
    if (options.selection == Selection::All) {
        todo.reserve(listed.entries.size());
        for (const Entry& e : listed.entries) todo.push_back(&e);
    } else {
        for (const std::string& want : options.only) {
            const Entry* found = nullptr;
            for (const Entry& e : listed.entries) {
                if (e.path == want) {
                    found = &e;
                    break;
                }
            }
            if (found == nullptr) {
                result.failures.push_back({want, "归档里没有该条目"});
                continue;
            }
            todo.push_back(found);
        }
    }

    for (const Entry* entry : todo) {
        // 6.7：每条都过安全路径 —— 目录项也不例外（否则 `..` 能靠"建目录"逃出去）
        const OutPathResult target = safe_out_path(out_root, entry->path);
        if (!target.ok) {
            result.failures.push_back({entry->path, target.reason});
            continue;
        }

        if (entry->is_dir) {
            // 1.x：**选中**模式下目录项直接跳过（目录由文件的父目录顺带建出来）；
            // 整镜像模式（--unpack-path）才建目录。这里的差别是照代码来的。
            if (options.selection == Selection::Listed) {
                ++result.skipped_dirs;
                continue;
            }
            std::error_code ec;
            stdfs::create_directories(xrkit::core::path_from_utf8(target.path), ec);
            if (ec) {
                result.failures.push_back({entry->path, "建目录失败：" + ec.message()});
                continue;
            }
            ++result.dirs_created;
            result.written.push_back(entry->path);
            continue;
        }

        const FileData data = read_file(image_path, entry->path);
        if (!data.ok) {
            result.failures.push_back({entry->path, data.error});
            continue;
        }
        // 6.8：空文件是**合法条目** —— 写出 0 字节并计入成功。
        //（1.x 的旧代码 `rc == 0 and r.stdout` 把空文件当成"解不出来"，已修。）
        std::string error;
        if (!xrkit::core::write_file_atomic(target.path, data.data, options.atomic, error)) {
            result.failures.push_back({entry->path, error});
            continue;
        }
        ++result.files_written;
        result.bytes_written += data.data.size();
        result.written.push_back(entry->path);
    }

    return finish();
}

ExtractTreeResult extract_package(const std::string& image_path, const std::string& out_root,
                                  const ExtractTreeOptions& options,
                                  std::optional<DbFormat> db_fallback) {
    // ① SquashFS：先只读 96 字节超块判 magic（大镜像不能整包进内存）⇒ 命中就走既有那条
    SuperBlock super;
    std::string super_error;
    if (read_superblock(image_path, super, super_error)) {
        return extract_tree(image_path, out_root, options);
    }

    // ② X-Ray DB 六格式：条目表在尾部 ⇒ 整文件读
    ExtractTreeResult result;
    const auto begin = std::chrono::steady_clock::now();
    const auto finish = [&result, begin]() {
        result.elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                .count();
        return result;
    };
    if (out_root.empty()) {
        result.error = "输出根为空";
        return finish();
    }
    // ② X-Ray DB 六格式：条目表在尾部 ⇒ **seek 式**只读"块头 + 尾部那一块"；
    //    逐条目解包时也只读那一条的那一段（旧实现整包读进内存：1900 MB 的包光读盘 8 s，
    //    而且整个镜像常驻内存）。两条路的产物逐值一致（共用 `decode_entry_bytes`）。
    const DbListing listed = list_db_seek(image_path, db_fallback);
    if (!listed.ok) {
        result.error = listed.error;
        return finish();
    }
    result.ok = true;   // 从这里往后：单项失败算单项（与 SquashFS 分支同口径）

    const bool want_all = options.selection == Selection::All;
    // Listed 模式下按 1.x **跳过目录项**（6.10 的同族口径）
    for (const DbEntry& entry : listed.entries) {
        if (!want_all) {
            const bool wanted =
                std::find(options.only.begin(), options.only.end(), entry.path) != options.only.end();
            if (!wanted || entry.is_dir) continue;
        }
        const OutPathResult safe = safe_out_path(out_root, entry.path);
        if (!safe.ok) {
            result.failures.push_back({entry.path, safe.reason});
            continue;
        }
        if (entry.is_dir) {
            std::error_code ec;
            stdfs::create_directories(core::path_from_utf8(safe.path), ec);
            if (ec) {
                result.failures.push_back({entry.path, "建目录失败：" + ec.message()});
                continue;
            }
            ++result.dirs_created;
            continue;
        }
        const DbExtract data = extract_entry_seek(image_path, entry);
        if (!data.ok) {
            result.failures.push_back({entry.path, data.error});
            continue;
        }
        std::string write_error;
        if (!core::write_file_atomic(safe.path, data.data, options.atomic, write_error)) {
            result.failures.push_back({entry.path, write_error});
            continue;
        }
        // 空文件是**合法条目**（写出 0 字节）—— 与"解不出来"分开（6.8）
        ++result.files_written;
        result.bytes_written += data.data.size();
        result.written.push_back(entry.path);
    }
    return finish();
}

}  // namespace xrkit::engine::fs
