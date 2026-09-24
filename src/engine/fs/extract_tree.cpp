#include "xrkit/engine/fs/extract_tree.h"

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

bool write_file_atomic(const std::string& final_path, const std::vector<uint8_t>& data,
                       bool atomic, std::string& error) {
    const stdfs::path target = xrkit::core::path_from_utf8(final_path);
    std::error_code ec;
    const stdfs::path dir = target.parent_path();
    if (!dir.empty()) {
        stdfs::create_directories(dir, ec);
        if (ec) {
            error = "建目录失败：" + ec.message();
            return false;
        }
    }
    const stdfs::path temp =
        atomic ? stdfs::path(target.string() + ".part") : target;

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "打不开输出文件";
            return false;
        }
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(data.size()));
        }
        out.close();
        if (!out) {
            error = "写盘失败（磁盘满/被占用）";
            // 落半截就删掉：宁可没有，也不要留一个**看起来存在**的坏文件
            if (atomic) stdfs::remove(temp, ec);
            return false;
        }
    }

    if (!atomic) return true;

    // 6.9 的"逐字节复核"：从临时文件读回来比一遍 ——
    // 写盘被截断（磁盘满、被占用）只在**读回**时看得见，`ofstream` 的 `!out` 未必报。
    {
        std::ifstream in(temp, std::ios::binary);
        if (!in) {
            error = "复核读不回临时文件";
            stdfs::remove(temp, ec);
            return false;
        }
        std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        if (back.size() != data.size() ||
            (data.size() != 0 && std::memcmp(back.data(), data.data(), data.size()) != 0)) {
            error = "复核不一致（临时文件与待写内容不同）";
            stdfs::remove(temp, ec);
            return false;
        }
    }

    // 同盘 rename（1.x 是 `os.replace`）：要么旧文件还在，要么新文件完整。
    stdfs::rename(temp, target, ec);
    if (ec) {
        error = "替换目标文件失败：" + ec.message();
        stdfs::remove(temp, ec);
        return false;
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
        if (!write_file_atomic(target.path, data.data, options.atomic, error)) {
            result.failures.push_back({entry->path, error});
            continue;
        }
        ++result.files_written;
        result.bytes_written += data.data.size();
        result.written.push_back(entry->path);
    }

    return finish();
}

}  // namespace xrkit::engine::fs
