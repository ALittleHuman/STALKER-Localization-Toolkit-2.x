#include "xrkit/core/case_fold.h"
#include "xrkit/engine/fs/extract_tree.h"
#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/fs/link_check.h"
#include "name_check.h"

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
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace xrkit::engine::fs {
namespace {

namespace stdfs = std::filesystem;

constexpr const char* kRejectEmpty = "条目路径为空（或不是字符串）";
constexpr const char* kRejectAbsolute = "绝对路径 / UNC，拒绝写出";
constexpr const char* kRejectDrive = "带盘符，拒绝写出";
constexpr const char* kRejectDotDot = "分段含 ..（试图逃出输出目录）";
constexpr const char* kRejectEmptySegment = "分段剥掉尾随空格/点后为空（Windows 建不出这种名字）";
constexpr const char* kRejectColon = "分段含冒号（Windows 数据流 ADS），拒绝写出";
constexpr const char* kRejectFallback = "复核后仍不在输出目录下（含跨盘）";

// ★★ 2026-10-06（F-1 路径逃逸；判红锁＝`tests/check_fs_hardening.py`）：
//   Windows 落盘时**剥掉组件末尾的空格与点**（实测：`a. ` 落成 `a`、`.. ` 落成 `..`；
//   `mkdir "out\a. "` 建出来的就是 `out\a`）。于是"`..` 只做**精确字符串匹配**"这条判定
//   在 `".. "` / `".. ."` 面前是**假绿**：判定说"仍在输出目录下"，而系统会把它当 `..` 解释。
//   ★ 同一批实测的**边界**（写在这里免得把结论放大）：剥离**只作用于最后一个组件** ——
//   中间段 `".. "` 会被原样当目录名（`out\.. \x` 在 CreateFileW 层报"找不到路径"），
//   所以"真的写到输出目录之外"这一步在本机**复现不出来**。但这不改变"判定错了"这件事：
//   判定必须按**系统真正会用的名字**比，而不是按字节相等比（见报告【没做到/风险】）。
//   归一规则（**只归一名字，不猜落点**）：每段先剥掉末尾的空格/点，然后
//     · 剥完空掉、且原串里**有点**（`..`、`.. `、`.. .`、`...`）⇒ 拒，**理由仍是 `分段含 ..`**
//       —— 这一族就是"用点的变体绕过 `..`"，Windows 对"只剩点"的组件按上一级目录解释
//       （实测 `.. ` 落成父目录），所以归到同一条拒因，别让它变成"另一种拒因"而蒙过判据；
//     · 剥完空掉、原串里没有点（纯空格）⇒ 拒，理由单列 —— 这种段 Windows 建不出来
//       （实测 `out\...\y` 报"找不到路径"），也不该被悄悄当成 `.` 或空组件（那等于把落点
//       挪到别处）；
//     · 其余 ⇒ **用剥完的名字**（`a. ` → `a`，`x..` → `x`）——这样返回的落点就是盘上
//       真会出现的那条路径，`ArtifactTally` 的"同一落点只算一次"与"报出来的等于盘上真发生的"
//       才对得上（修前 `a. ` 与 `a` 会被算成两个落点，而盘上只有一份）。
std::string strip_trailing_dots_spaces(const std::string& seg) {
    size_t end = seg.size();
    while (end > 0 && (seg[end - 1] == ' ' || seg[end - 1] == '.')) --end;
    return seg.substr(0, end);
}

// 段里有没有点（用来区分"只剩点的变体"与"纯空格"两种空段，见上）
bool has_dot(const std::string& seg) { return seg.find('.') != std::string::npos; }

bool ascii_iequal(char a, char b) {
    const auto lower = [](char c) { return xrkit::core::ascii_lower_char(c); };
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
        c = xrkit::core::ascii_lower_char(c);
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

// ── ★★ P17-103（审计 `01 F-06`）：落盘前的**真实文件系统**复核（反对软链接跟随）──────────
// `safe_out_path` 的 `is_within` 是**词法**判断 —— 它拦得住名字里的逃逸（`..`／盘符／冒号／ADS），
// 但拦不住**盘上已经存在的软链接/重解析点**：把 `<输出根>/<某段>` 预先做成指向别处的
// junction/symlink，词法复核照样通过，而写入会**跟着链接落到根外**（静默覆盖无关文件）。
//
// 口径（与 `safe_out_path` 的"兜底复核"是同一件事的两半：**一个看名字、一个看盘**）：
// 从**输出根**到**落点的父目录**逐段复核，任何一段是软链接/重解析点、或不是目录，
// 就拒写这一条并把理由交回去（调用方记进 `failures` —— **零静默**，不许"跳过当成功"）。
//
// ★ 只用 `lexically_relative` ＋ 单段 `symlink_status()`：**不**做 `weakly_canonical()` 那种
//   逐段真实化解析（那是 O(深度²) 的 syscall，且对**不存在**的段行为有坑）。
// ★ 落点**自己**已经存在且是软链接时也拒 —— 两种处置都不完美（跟着写＝写到根外；
//   原子重命名＝**替换**那个链接、变成另一种静默），所以选"明确拒绝"。
std::string refuse_if_link_traversed(const std::string& out_root, const std::string& target_utf8) {
    std::error_code ec;
    const stdfs::path root =
        stdfs::absolute(xrkit::core::path_from_utf8(out_root), ec).lexically_normal();
    const stdfs::path target =
        stdfs::absolute(xrkit::core::path_from_utf8(target_utf8), ec).lexically_normal();
    const stdfs::path rel = target.parent_path().lexically_relative(root);
    if (rel.empty() && target.parent_path() != root) {
        return "落盘前复核：落点父目录不在输出根下（词法复核没拦住，这里再拦一次）";
    }
    // ★ **输出根本身**也要查：用户完全可能把输出目录做成一个 junction/symlink
    //   （本判据的 ② 就是这一格 —— 我第一版漏了这一步，探针当场把文件写到根外）。
    {
        std::error_code root_ec;
        const stdfs::file_status root_status = stdfs::symlink_status(root, root_ec);
        // ★★ S-06（第三方只读审计 2026-10-18）：**问不出来就拒绝**（fail-closed）。
        //   修前这里只看 `is_link_or_junction()`，而它在 stat 失败时返回 false ⇒ 守卫在
        //   非法字符/超长路径/权限异常这些**最需要它**的输入上静默放行。
        const LinkProbe root_probe = probe_link(root);
        if (root_probe == LinkProbe::StatFailed) {
            return "拒绝落盘：**问不出**输出根的属性（stat 失败，且不是「不存在」）：" +
                   xrkit::core::path_to_utf8(root) + " —— 守卫不许 fail-open";
        }
        if (root_probe == LinkProbe::IsLink) {
            return "拒绝落盘：**输出根**本身是软链接/重解析点（" +
                   xrkit::core::path_to_utf8(root) + "）—— 会写到根外，本工具不跟";
        }
    }
    // 逐段拼（**不用** `operator/`：Windows 上 rhs 带根名会替换整条路径 —— 与 `safe_out_path` 同理）
    stdfs::path cur = root;
    for (const stdfs::path& part : rel) {
        const std::string name = part.generic_string();
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            return "落盘前复核：落点父目录相对输出根发生了上跳（`..`）";
        }
        cur /= part;
        std::error_code seg_ec;
        const stdfs::file_status status = stdfs::symlink_status(cur, seg_ec);
        // ★★ S-06：同一条口径 —— 段上问不出来也拒（不存在的那一段仍放行，见下面那条注释）。
        const LinkProbe seg_probe = probe_link(cur);
        if (seg_probe == LinkProbe::StatFailed) {
            return "拒绝落盘：**问不出**输出路径上某一段的属性（stat 失败）：" +
                   xrkit::core::path_to_utf8(cur) + " —— 守门人自己看不见，就不许放行";
        }
        if (seg_probe == LinkProbe::IsLink) {
            return "拒绝落盘：输出目录里有**软链接/重解析点**（" +
                   xrkit::core::path_to_utf8(cur) + "）—— 会写到根外，本工具不跟";
        }
        // ★ 只在"这段确实存在"时才要求它是目录：**不存在**的段是"等会儿要建的普通目录"，
        //   不能因为没有它就把正常落盘判成拒绝（那是把守卫变成误伤）。
        if (!seg_ec && stdfs::exists(status) && !stdfs::is_directory(status)) {
            return "拒绝落盘：输出路径上有一段不是目录（" + xrkit::core::path_to_utf8(cur) + "）";
        }
    }
    std::error_code leaf_ec;
    const stdfs::file_status leaf = stdfs::symlink_status(target, leaf_ec);
    // ★★ S-06：落点本身问不出来也拒（同一条 fail-closed 口径）。
    const LinkProbe leaf_probe = probe_link(target);
    if (leaf_probe == LinkProbe::StatFailed) {
        return "拒绝落盘：**问不出**落点的属性（stat 失败）：" + xrkit::core::path_to_utf8(target) +
               " —— 守卫不许 fail-open";
    }
    if (leaf_probe == LinkProbe::IsLink) {
        return "拒绝落盘：落点本身是**软链接/重解析点**（" +
               xrkit::core::path_to_utf8(target) + "）—— 本工具不跟";
    }
    return {};
}

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
    //    ★ F-1 起：每段先按 Windows 的有效名归一（剥尾随空格/点）**再判**，`..`/`.` 的
    //      比较对象是归一后的名字（详见 `strip_trailing_dots_spaces` 上方的说明）。
    //      `a/../b` 这类仍然拒（两段里第二段就是 `..`）—— 口径与 1.x 一致：**不做**
    //      "先归约路径再看是否越界"，只要分段里出现 `..` 就拒。
    std::vector<std::string> parts;
    for (const std::string& raw_seg : split_segments(normalized, '/')) {
        if (raw_seg.empty() || raw_seg == ".") continue;   // 1.x 口径：空段与 `.` 丢
        const std::string seg = strip_trailing_dots_spaces(raw_seg);
        if (seg.empty()) {
            // 只剩点（`..`/`.. `/`.. .`/`...`）＝同一族绕过，理由保持 `分段含 ..`；
            // 纯空格才走"建不出这种名字"那条（见 `strip_trailing_dots_spaces` 上方）。
            result.reason = has_dot(raw_seg) ? kRejectDotDot : kRejectEmptySegment;
            return result;
        }
        if (seg == "..") {
            result.reason = kRejectDotDot;
            return result;
        }
        // ★★ 2026-10-24（hotfix-1 / R10 + R20）：**每一段**都查名字合法性 ——
        //   控制字符（含 NUL）+ Windows 保留设备名。修前只有 `write_file_atomic` 查**叶子**的
        //   保留名，**目录段**（`CON/`）没人查；NUL 则三层都不查（会被 Win32 截断到别处）。
        //   与 parser 共用同一把尺（`fs/name_check.h`）。
        if (const std::string why = name_check::segment_reject_reason(seg); !why.empty()) {
            result.reason = why;
            return result;
        }
        if (seg == ".") continue;   // 剥完正好是 `.`（`".. "` 之类已在上面被 `..` 拦掉）
        // 段里带**根名/根目录**的（Windows 上是 `D:` 这种盘符相对路径）：
        // 1.x 靠 `os.path.join` 的语义变化 + `commonpath` 的 ValueError 拦下它，
        // 这里显式拦 —— 判据是同一个（"拼完不再位于输出目录下"），只是不再依赖
        // "join 悄悄换根"这个副作用。
        const stdfs::path seg_path = xrkit::core::path_from_utf8(seg);
        if (seg_path.has_root_name() || seg_path.has_root_directory()) {
            result.reason = kRejectFallback;
            return result;
        }
        // ★ F-1 同批：段内冒号＝NTFS **数据流（ADS）**（`ok.txt:evil` 会把内容写进
        //  `ok.txt` 的隐藏流里，盘面上"看不见这份产物"，而判定说它落在输出目录下）。
        //  Windows 文件名本来就不许有冒号 ⇒ 显式拒，不留给系统去当数据流解释。
        if (seg.find(':') != std::string::npos) {
            result.reason = kRejectColon;
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

// ── `only` 口径：**语义只许一份**（审计 §1 第 4 条 / S1-7 / 1-4）─────────────
//
// 病根：同一条规则写了两份、两边不同源 ——
//   · 页面（`fs_page.cpp:1493-1496`）把 `tree_.flatten()` 的 `node->path` 塞进 `only`，
//     而那棵树是 `preview_entries_in_layout()` 的产物 ⇒ 那是**落点相对路径**（带包名壳）；
//   · 引擎比的是 `listed.entries` 的**镜像内路径** ⇒ 不自带壳的包**一条都命中不了**。
// 落地纪律（审计"最终收口"第 4 条）：凡是"规则/数据/判据"都要问一句"第二份在哪"。
// 这里把口径**收到引擎一份**：`only` 既接受落点路径（带壳，页面/CLI 实发的那种），
// 也接受镜像内路径（1.x `--cat` 与既有 CLI 调用点的写法）—— 两种写法指的**同一条目**，
// 谁都不会"静默 0 产物"。壳的算法与 `preview_entries_in_layout()` **同源**（同一个
// `unpack_target_dir` 减哨兵根），不许再抄第二份。
std::string pattern_shell_prefix(const std::string& image_path,
                                 const std::vector<std::string>& raw_paths) {
    const std::string kSentinelRoot = "\x01pattern-root\x02";
    const std::string target = unpack_target_dir(kSentinelRoot, image_path, raw_paths);
    if (target.size() <= kSentinelRoot.size() ||
        target.compare(0, kSentinelRoot.size(), kSentinelRoot) != 0) {
        return std::string();
    }
    std::string prefix = target.substr(kSentinelRoot.size());
    while (!prefix.empty() && (prefix.front() == '/' || prefix.front() == '\\')) {
        prefix.erase(prefix.begin());
    }
    return prefix;
}

// `want` 与某条目的**镜像内路径**是否指同一条：先按落点路径（壳 + 路径）比，再按镜像内路径比。
bool only_matches(const std::string& want, const std::string& shell, const std::string& entry_path) {
    if (want == entry_path) return true;   // 镜像内路径（1.x --cat 的写法）
    if (shell.empty()) return false;
    return want == shell + "/" + entry_path;
}

std::string ExtractTreeResult::failure_note() const {
    // ★★ C-11（第三方只读审计 2026-10-18）：两条语句挤在一行里（`return` 后面还跟着一条
    //   声明）—— 读起来像"提前返回"把后半句也带走了。拆开。
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
    std::string shell_prefix;
    if (options.selection == Selection::All) {
        todo.reserve(listed.entries.size());
        for (const Entry& e : listed.entries) todo.push_back(&e);
    } else {
        // `only` 的两套写法都收（见 `only_matches` 的说明）：壳算法与右栏预览同源。
        std::vector<std::string> raw_paths;
        raw_paths.reserve(listed.entries.size());
        for (const Entry& e : listed.entries) raw_paths.push_back(e.path);
        shell_prefix = pattern_shell_prefix(image_path, raw_paths);
        for (const std::string& want : options.only) {
            const Entry* found = nullptr;
            for (const Entry& e : listed.entries) {
                if (only_matches(want, shell_prefix, e.path)) {
                    found = &e;
                    break;
                }
            }
            if (found == nullptr) {
                result.failures.push_back({want, "归档里没有该条目（按落点路径与镜像内路径都比过）"});
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
            // ★★ P17-103（审计 `01 F-06`）：**建目录也要过同一道守卫**。
            //   判据（② 格的"外部目录一个字节都不许多"）就是被这一步抓出来的：
            //   只守写文件那一支时，链接上的**目录**照样被建到根外去了。
            if (const std::string link_reason = refuse_if_link_traversed(out_root, target.path);
                !link_reason.empty()) {
                result.failures.push_back({entry->path, link_reason});
                continue;
            }
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
        // ★★ P17-103（审计 `01 F-06`）：**落盘前复核"真实文件系统"** —— `safe_out_path` 的
        //   `is_within` 是**词法**判断（`lexically_normal` ＋ 逐段比），它拦得住 `..`／盘符／
        //   冒号那些**名字里**的逃逸，但拦不住**盘上已经存在的软链接/重解析点**：
        //   把 `<输出根>/<某段>` 预先做成指向别处的 junction/symlink，词法复核照样通过，
        //   而 `write_file_atomic` 会**跟着链接写到根外**（静默覆盖无关文件）。
        //   口径（与 `safe_out_path` 那句"兜底复核"是同一件事的两半：一个看名字、一个看盘）：
        //   写到某个落点之前，从**输出根**到**落点的父目录**逐段查 —— 任何一段是软链接/重解析点
        //   就**拒写这一条**并把理由记进 `failures`（**零静默**：不许"跳过就当成功"）。
        if (const std::string link_reason = refuse_if_link_traversed(out_root, target.path);
            !link_reason.empty()) {
            result.failures.push_back({entry->path, link_reason});
            continue;
        }
        // 6.9 原子写：默认"边写边算哈希"自校验；`options.verify_readback` 才走读回逐字节复核。
        const core::WriteVerify verify = options.verify_readback ? core::WriteVerify::Readback
                                                                : core::WriteVerify::Streamed;
        // ★★ 2026-10-05（**落点覆盖必须可见**）：写之前先看落点上有没有东西。
        //   为什么记在**写成功之后**（而不是发现存在就记）：本次写失败时落点没被改写，
        //   记进 `overwritten` 就是**谎报**（"报出来的必须等于盘上真发生的"）。
        std::error_code exists_ec;
        const bool had_file = stdfs::exists(core::path_from_utf8(target.path), exists_ec);
        if (!xrkit::core::write_file_atomic(target.path, data.data, options.atomic, error, verify)) {
            result.failures.push_back({entry->path, error});
            continue;
        }
        if (had_file) result.overwritten.push_back(entry->path);
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
    // ★★ S-05 操作级（用户裁决）：**一次引擎操作返回时**就把解密临时件删掉
    //   （不是每个文件读完就删 —— Windows 句柄；也不是退出才删 —— 会躺半天）。
    const xrkit::plugin::DecryptSessionScope decrypt_session_scope;
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
    // ★★ T-02（第三方只读审计 2026-10-18）：**条目偏移属于 `listed.read_path` 那份文件**。
    //   有插件解密器命中时它是**解密产物**；逐条提取必须读那一份（修前读的是原始密文 ⇒
    //   列条目成功、逐条报「偏移越界」/读出垃圾字节）。
    //   ★ 这样做**整批只解密一次**（`extract_entry_seek` 里的闸门认出"已在产物目录里"）。
    const std::string& db_source = listed.read_path.empty() ? image_path : listed.read_path;
    result.ok = true;   // 从这里往后：单项失败算单项（与 SquashFS 分支同口径）
    ArtifactTally tally;   // ★ 产物口径：同一落点只算一次（真机 `maps\*.db` 的重复登记就靠它）

    const bool want_all = options.selection == Selection::All;
    // ★★ 2026-10-06（审计处理批·1-4 / S1-7 的**另一半**）：修前 Listed 模式在 DB 分支是
    //   `if (!wanted || entry.is_dir) continue;` —— **静默**：不计 failures、不计 written。
    //   于是"解包选中"给出的路径若与镜像内路径对不上（页面喂的是**带包名壳的落点路径**、
    //   引擎比的是**镜像内路径**），每条都 `continue` ⇒ `files_written=0`、`failures` 空、
    //   `ok=true`，界面显示"解包：0 个文件"、CLI 退 0 —— 用户看不到任何理由。
    //   SquashFS 分支早就是"先按清单解析、对不上的逐条记 failures"（本文件 `:290-303`），
    //   这里把 DB 分支**收成同一条口径**（同一条规则只许一份）：
    //     · 清单里对不上的路径 ⇒ 逐条记 `failures`（可见：`failure_note()` / CLI 台账）；
    //     · 清单里选的**目录项** ⇒ 照 1.x 跳过，但进 `skipped_dirs` 计数（不是静默）。
    std::vector<const DbEntry*> todo;
    std::string shell_prefix;
    if (want_all) {
        todo.reserve(listed.entries.size());
        for (const DbEntry& e : listed.entries) todo.push_back(&e);
    } else {
        // ★ 与 SquashFS 分支**逐字同口径**（`only_matches` 的说明）：`only` 既收落点路径
        //   （带包名壳，页面/CLI 实发形态）也收镜像内路径，对不上的逐条记 failures。
        std::vector<std::string> raw_paths;
        raw_paths.reserve(listed.entries.size());
        for (const DbEntry& e : listed.entries) raw_paths.push_back(e.path);
        shell_prefix = pattern_shell_prefix(image_path, raw_paths);
        for (const std::string& want : options.only) {
            const DbEntry* found = nullptr;
            for (const DbEntry& e : listed.entries) {
                if (only_matches(want, shell_prefix, e.path)) {
                    found = &e;
                    break;
                }
            }
            if (found == nullptr) {
                result.failures.push_back({want, "归档里没有该条目（按落点路径与镜像内路径都比过）"});
                continue;
            }
            if (found->is_dir) {
                ++result.skipped_dirs;   // 选中目录项：照 1.x 跳过（目录由文件的父目录顺带建出）
                continue;
            }
            todo.push_back(found);
        }
    }

    for (const DbEntry* entry_ptr : todo) {
        const DbEntry& entry = *entry_ptr;
        const OutPathResult safe = safe_out_path(out_root, entry.path);
        if (!safe.ok) {
            result.failures.push_back({entry.path, safe.reason});
            continue;
        }
        if (entry.is_dir) {
            std::error_code ec;
            // ★★ P17-103：与 SquashFS 分支**逐条同口径**（建目录也要过那道守卫）
            if (const std::string link_reason = refuse_if_link_traversed(out_root, safe.path);
                !link_reason.empty()) {
                result.failures.push_back({entry.path, link_reason});
                continue;
            }
            stdfs::create_directories(core::path_from_utf8(safe.path), ec);
            if (ec) {
                result.failures.push_back({entry.path, "建目录失败：" + ec.message()});
                continue;
            }
            if (tally.note("d:", safe.path)) ++result.dirs_created;   // 产物口径（同落点只算一次）
            continue;
        }
        const DbExtract data = extract_entry_seek(db_source, entry);
        if (!data.ok) {
            result.failures.push_back({entry.path, data.error});
            continue;
        }
        std::string write_error;
        // ★★ P17-103（审计 `01 F-06`）：**这一条分支也要有**（DbEntry 批量解包那条路）。
        //   ★ 探针第一次跑"输出根是 junction"时**判定仍绿** —— 就是因为我只加在了上面
        //     SquashFS 那条分支，而 `--unpack-package` 走的是**这一条**。
        //     ⇒ 两个分支必须逐条同口径（本仓已有多次"漏一个分支等于锁形同虚设"的教训）。
        if (const std::string link_reason = refuse_if_link_traversed(out_root, safe.path);
            !link_reason.empty()) {
            result.failures.push_back({entry.path, link_reason});
            continue;
        }
        // 与 SquashFS 分支同一条纪律（6.9）：默认流式哈希自校验，开关打开才读回。
        const core::WriteVerify verify = options.verify_readback ? core::WriteVerify::Readback
                                                                : core::WriteVerify::Streamed;
        // ★★ 2026-10-05（**落点覆盖必须可见**）：与 SquashFS 分支**逐条同口径**（两个分支
        //   都不许漏 —— DB 六格式是本工具的主要入口，漏了它等于这条锁形同虚设）。
        std::error_code exists_ec;
        const bool had_file = stdfs::exists(core::path_from_utf8(safe.path), exists_ec);
        if (!core::write_file_atomic(safe.path, data.data, options.atomic, write_error, verify)) {
            result.failures.push_back({entry.path, write_error});
            continue;
        }
        if (had_file) result.overwritten.push_back(entry.path);
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

// ── ★★ 2026-10-05：**解包产物预览**（右栏）＝落点规则的**同一份真值**（见头文件说明）──
std::vector<EntryInfo> preview_entries_in_layout(const std::string& package_path,
                                                 const std::vector<EntryInfo>& entries) {
    std::vector<std::string> raw;
    raw.reserve(entries.size());
    for (const EntryInfo& e : entries) raw.push_back(e.path);

    // 这一段就是**包名壳**（自带壳的包 ⇒ 空串）。算法在 `pattern_shell_prefix()` ——
    // ★ 2026-10-06（1-4）：原来这里**自己抄了一遍**壳的算法，而"解包选中"的匹配又要用壳
    //   ⇒ 同一个事实两份实现正是本批要收的口径病根。现在两处都调那一个函数。
    const std::string prefix = pattern_shell_prefix(package_path, raw);
    if (prefix.empty()) return entries;   // 自带壳 / 算不出壳 ⇒ 条目路径原样（**不叠第二层**）

    std::vector<EntryInfo> out;
    out.reserve(entries.size() + 8);
    // ★ 目录项也要补（否则右栏建不出中间层）。用集合去重，避免与包内已有的同名目录项重复。
    std::set<std::string> made_dirs;
    auto add_dir = [&](const std::string& path) {
        if (path.empty()) return;
        if (!made_dirs.insert(path).second) return;
        out.emplace_back(path, /*d=*/true, /*s=*/0);
    };
    // 壳自己那一层
    add_dir(prefix);
    const std::string lead = prefix + "/";
    for (const EntryInfo& e : entries) {
        if (e.path.empty()) continue;
        // 逐级补祖先目录（条目自己带 `a/b/c` 时中间那几层也要在）
        std::string acc = prefix;
        std::string seg;
        for (size_t i = 0; i <= e.path.size(); ++i) {
            const char c = i < e.path.size() ? e.path[i] : '/';
            if (c == '/' || c == '\\') {
                if (!seg.empty()) {
                    acc += "/" + seg;
                    add_dir(acc);
                    seg.clear();
                }
            } else {
                seg += c;
            }
        }
        out.emplace_back(lead + e.path, e.is_dir, e.size);
    }
    return out;
}

// ★ 2026-10-06（1-4）：把"勾选项 ↔ 包里条目"的比对**唯一一份**暴露出去 ——
//   `tools/cli` 的 `--unpack-package` 预过滤此前自己抄了一份"只比镜像内路径"的写法，
//   于是 CLI 与 GUI 对同一个勾选集合给出两种结论（CLI 把带壳路径过滤成空 ⇒ 静默 0 产物）。
//   现在 CLI 也调这里（同一条规则只许一份）。
bool selection_matches(const std::string& package_path,
                       const std::vector<std::string>& entry_paths,
                       const std::string& want) {
    const std::string shell = pattern_shell_prefix(package_path, entry_paths);
    for (const std::string& p : entry_paths) {
        if (only_matches(want, shell, p)) return true;
    }
    return false;
}

std::string resolve_output_root(const std::string& front_source_path, const std::string& current_dir) {    // 1.x `_resolve_output`：空则按"第一个包所在目录"推断（界面侧还要把它**回填**到输入框）。
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
