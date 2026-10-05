#include "xrkit/engine/fs/extract_tree.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/overlay.h"   // format_key_is_sqfs（`pack_ext_for` 要按 1.x 规则判 sqfs）
#include "xrkit/engine/fs/squashfs.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

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

// 「两条记录是不是指向**同一个产物**」的归一化键：`\`→`/`，Windows 下再折成小写
//（与上面的 `component_equal` 同一口径 —— 产物层判"同一份东西"必须大小写不敏感）。
std::string artifact_key(const std::string& path) {
    std::string key = path;
    for (char& c : key) {
        if (c == '\\') c = '/';
    }
#ifdef _WIN32
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
#endif
    return key;
}

// ★★ 2026-10-04（判红锁②）：**产物口径**的记账器 —— 同一落点只算一次。**实现只有这一份**：
//   GUI（`FsPage::run_unpack_jobs` 读 `files_written`）与 CLI（`--unpack-package` 的
//   `files=/dirs=/bytes=`）都读它算出来的数 ⇒ "GUI 的功能 CLI 全有"那条红线自动成立。
//   为什么必须记在本层：包内条目表**可以**把同一条记录登记多遍（真机 `maps\*.db` 就是），
//   而"写"这件事是本层做的 —— "处理了几条记录"与"盘上有几份产物"是两件事，计数不能混。
class ArtifactTally {
public:
    // true ⇒ 该落点**第一次**出现（计入 files/dirs/bytes）；false ⇒ 重复记录（产物没变多）。
    // `kind` 把文件与目录分开记账（`d:`/`f:`）—— 真出现"同一路径既有目录项又有文件项"时
    // 两条都要如实各算一次，不能互相顶掉。
    bool note(const char* kind, const std::string& target_path) {
        std::string key = kind;
        key += artifact_key(target_path);
        return seen_.insert(std::move(key)).second;
    }

private:
    std::set<std::string> seen_;
};

}  // namespace

size_t unique_entry_count(const std::vector<std::string>& entry_paths) {
    // 与 `ArtifactTally` **同一把尺子**（`artifact_key`）：包内路径表里"同一条路径写了多遍"
    // ⇒ 产物只有一份 ⇒ 报数只算一份。`--dump` **不**走这里（协议层要忠实报记录数）。
    std::set<std::string> seen;
    for (const std::string& p : entry_paths) seen.insert(artifact_key(p));
    return seen.size();
}

namespace {

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
    ArtifactTally tally;   // ★ 产物口径：同一落点只算一次（见 `ArtifactTally` 的说明）
    // ★★ 2026-10-05（性能）：整批**只打开一次镜像、只列一次目录树**。
    //   原先循环里每条目都调 `read_file()`，而它每条目都重开镜像 + 重新 list() 整棵目录树
    //   ⇒ 14861 个条目把同一份元数据遍历了 14861 遍（探针实测 100.3 s / 总 135.3 s，74%）。
    //   作用域只活在本函数内 ⇒ 函数外的 `read_file` 行为与今天逐字节一致。
    const ReadSessionScope read_session;

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
            if (tally.note("d:", target.path)) ++result.dirs_created;   // 产物口径（同落点只算一次）
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
        // 6.9 原子写：默认"边写边算哈希"自校验；`options.verify_readback` 才走读回逐字节复核。
        const core::WriteVerify verify = options.verify_readback ? core::WriteVerify::Readback
                                                                : core::WriteVerify::Streamed;
        if (!xrkit::core::write_file_atomic(target.path, data.data, options.atomic, error, verify)) {
            result.failures.push_back({entry->path, error});
            continue;
        }
        // ★ 写是**照写**（同落点后写覆盖前写 —— 保持既有落盘语义不变），但计数走产物口径：
        //   同一条落点又要写一遍时，盘上的东西没有变多 ⇒ 只记 `duplicate_records`。
        if (tally.note("f:", target.path)) {
            ++result.files_written;
            result.bytes_written += data.data.size();
        } else {
            ++result.duplicate_records;
        }
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
    ArtifactTally tally;   // ★ 产物口径：同一落点只算一次（真机 `maps\*.db` 的重复登记就靠它）

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
            if (tally.note("d:", safe.path)) ++result.dirs_created;   // 产物口径（同落点只算一次）
            continue;
        }
        const DbExtract data = extract_entry_seek(image_path, entry);
        if (!data.ok) {
            result.failures.push_back({entry.path, data.error});
            continue;
        }
        std::string write_error;
        // 与 SquashFS 分支同一条纪律（6.9）：默认流式哈希自校验，开关打开才读回。
        const core::WriteVerify verify = options.verify_readback ? core::WriteVerify::Readback
                                                                : core::WriteVerify::Streamed;
        if (!core::write_file_atomic(safe.path, data.data, options.atomic, write_error, verify)) {
            result.failures.push_back({entry.path, write_error});
            continue;
        }
        // 空文件是**合法条目**（写出 0 字节）—— 与"解不出来"分开（6.8）
        // ★ 写照写（后写覆盖前写，落盘语义不变）；计数走**最终产物**口径（判红锁②）。
        if (tally.note("f:", safe.path)) {
            ++result.files_written;
            result.bytes_written += data.data.size();
        } else {
            ++result.duplicate_records;
        }
        result.written.push_back(entry.path);
    }
    return finish();
}

// ── 解包落点：**GUI 与 CLI 共用的唯一一份实现**（用户 2026-10-03 口径 + 2026-10-04 架构红线）
//   用户红线原话：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。**」
//   ⇒ 这条规则原来只写在 `FsPage::plan_unpack()` 里 ⇒ CLI 复现不了 ⇒ 真机产物布局只能在
//     GUI/合成夹具上验。现在 shell 与 CLI 都调这里（自检锁：GUI 落点集合 == 这里算的）。
std::string package_folder_name(const std::string& package_path) {
    // 1.x `_package_folder` = `os.path.splitext(os.path.basename(p))[0]` —— **最后一个**扩展名。
    const std::string name = core::path_to_utf8(core::path_from_utf8(package_path).filename());
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name;   // 无扩展名 / 以点开头 ⇒ 原样
    return name.substr(0, dot);
}

bool paths_carry_shell(const std::vector<std::string>& entry_paths, const std::string& shell) {
    if (entry_paths.empty() || shell.empty()) return false;   // 空表 ⇒ 照常规叠一层（1.x 兜底）
    // ★★ 2026-10-04 **真机**抓到的坑（判红锁①）：量词**不是**"全部"，是"**过半**"。
    //   9 个 VESSEL 真机包每个**恰好含一条**根级 `readme.md`(8148 B)、其余条目全在 `壳/…` 下
    //   ⇒ 旧的"全部带壳才算自带壳"把它们判成"不带壳" ⇒ 解出 `ai/ai/alife/…`（违背用户口径
    //   「这个案例，就解出来一个 configs。」）。一个 8 KB 的说明文件就能掀翻整个判定。
    //   ★ 裸壳项（`p == shell`，即"它自己那一层"的目录条目）**算带壳**；
    //     `IWP\database\config\configs.db` 的第一条就是裸 `configs`（2026-10-04 的坑）。
    //   ★ **不要**改用"存在裸壳目录项"当判据 —— `IWP\database\resource\meshes.db1` /
    //     `textures.db1` 是**纯带壳但 bare_shell=0**（条目全 `壳/…`，没有裸壳项），那样会回归。
    size_t shelled = 0;
    for (const std::string& p : entry_paths) {
        if (p == shell) {            // 裸壳项：那条名字正好等于壳的目录条目
            ++shelled;
            continue;
        }
        if (p.size() <= shell.size() + 1) continue;            // 得比 "shell/" 更长才算壳下的项
        if (p.compare(0, shell.size(), shell) != 0) continue;  // 第一段不是包名
        if (p[shell.size()] != '/' && p[shell.size()] != '\\') continue;
        ++shelled;
    }
    return shelled * 2 > entry_paths.size();   // 过半即判"自带壳"
}

std::string unpack_target_dir(const std::string& out_root, const std::string& package_path,
                              const std::vector<std::string>& entry_paths) {
    const std::string shell = package_folder_name(package_path);
    if (shell.empty()) return out_root;                              // 算不出壳 ⇒ 直接把根当落点
    if (paths_carry_shell(entry_paths, shell)) return out_root;      // 自带壳 ⇒ 不再叠一层
    std::string root = out_root;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    if (root.empty()) return shell;
    // ★ 这里**只拼目录名**：绝不生成 `<root>/.` 那种写法（`safe_out_path` 会把它当越界拒掉，真机踩过）。
    return root + "/" + shell;
}

std::vector<UnpackTarget> plan_unpack_targets(
    const std::string& out_root,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& sources) {
    std::vector<UnpackTarget> out;
    out.reserve(sources.size());
    for (const auto& item : sources) {
        UnpackTarget t;
        t.image = item.first;
        t.label = core::path_to_utf8(core::path_from_utf8(item.first).filename());
        t.target = unpack_target_dir(out_root, item.first, item.second);
        out.push_back(std::move(t));
    }
    return out;
}

std::string resolve_output_root(const std::string& front_source_path, const std::string& current_dir) {
    // 1.x `_resolve_output`：空则按"第一个包所在目录"推断（界面侧还要把它**回填**到输入框）。
    if (front_source_path.empty()) return current_dir;
    const stdfs::path p = core::path_from_utf8(front_source_path);
    const stdfs::path parent = p.parent_path();
    if (parent.empty()) return current_dir;
    return core::path_to_utf8(parent);
}

std::string pack_ext_for(const std::string& format_key) {
    if (format_key_is_sqfs(format_key)) return ".sqfs";
    return "." + format_key + ".db";
}

PackTarget resolve_pack_target(const std::string& typed, const std::string& output_basename,
                               const std::string& format_key) {
    const std::string ext = pack_ext_for(format_key);
    const std::string base = output_basename.empty() ? std::string("packed") : output_basename;
    PackTarget out;
    if (typed.empty()) {
        out.path = base + ext;              // 只有文件名 ⇒ 前端弹框时用它当初始值
        out.needs_frontend_dialog = true;
        return out;
    }
    std::error_code ec;
    const stdfs::path p = core::path_from_utf8(typed);
    if (stdfs::is_directory(p, ec)) {
        out.path = core::path_to_utf8(p / core::path_from_utf8(base + ext));
        out.needs_frontend_dialog = output_basename.empty();   // 连基名都没有 ⇒ 还得让前端定
        return out;
    }
    if (p.extension().empty()) out.path = typed + ext;
    else out.path = typed;
    return out;
}

}  // namespace xrkit::engine::fs
