#include "xrkit/engine/fs/overlay.h"

#include "probe_decls.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/squashfs.h"
// ★★ P17-110（复审计 `01 F-07`）：链接/junction 的判断**与解包那条路共用同一份**
#include "xrkit/engine/fs/link_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace xrkit::engine::fs {
namespace {

namespace stdfs = std::filesystem;

std::string basename_lower(const std::string& path) {
    std::string name = xrkit::core::path_from_utf8(path).filename().string();
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return name;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

MergeNode* child_by_name(MergeNode& parent, const std::string& name) {
    for (MergeNode& child : parent.children) {
        if (child.name == name) return &child;
    }
    return nullptr;
}

void sort_children(MergeNode& node) {
    // ★ 用户 2026-09-30：「包内文件的排序有问题。正确的是**文件夹>文件**，然后各自按名称排。」
    //   原来只按 name 排 ⇒ 目录与文件混在一起。
    std::sort(node.children.begin(), node.children.end(),
              [](const MergeNode& a, const MergeNode& b) {
                  if (a.is_dir != b.is_dir) return a.is_dir;   // 文件夹在前
                  return a.name < b.name;                       // 各自按名称
              });
    for (MergeNode& child : node.children) sort_children(child);
}

void collect_checked(const MergeNode& node, int& count) {
    if (!node.is_dir) {
        if (node.checked) ++count;
        return;
    }
    for (const MergeNode& child : node.children) collect_checked(child, count);
}

void apply_toggle(MergeNode& node, bool checked) {
    node.checked = checked;
    for (MergeNode& child : node.children) apply_toggle(child, checked);
}

void flatten_into(const MergeNode& node, std::vector<const MergeNode*>& out) {
    for (const MergeNode& child : node.children) {
        out.push_back(&child);
        if (child.is_dir) flatten_into(child, out);
    }
}

}  // namespace

MergeTree build_tree(const MergeInput& input) {
    MergeTree tree;
    tree.root.is_dir = true;  // 虚拟根：它不是条目，只是挂载点（计数/遍历要靠这个标志）
    tree.sources.push_back(input.source);
    for (const EntryInfo& entry : input.entries) {
        const std::string& path = entry.path;
        const bool is_dir = entry.is_dir;
        // 6.7 的同一份"不可信输入"意识：这里只做结构，越界路径由导出那一层拒。
        const std::vector<std::string> parts = split_path(path);
        if (parts.empty()) continue;
        MergeNode* cur = &tree.root;
        std::string acc;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (!acc.empty()) acc += '/';
            acc += parts[i];
            const bool last = (i + 1 == parts.size());
            MergeNode* next = child_by_name(*cur, parts[i]);
            if (next == nullptr) {
                MergeNode fresh;
                fresh.name = parts[i];
                fresh.path = acc;
                fresh.is_dir = !last || is_dir;
                // ★ 大小随条目进节点（**新建节点这一支也必须写** —— 我第一次只写在"已存在"那一支，
                //   界面上就是一列 0；锁当场把它抓出来了）
                fresh.size = fresh.is_dir ? 0 : entry.size;
                fresh.source_index = 0;
                cur->children.push_back(fresh);
                next = &cur->children.back();
            } else if (last && !is_dir) {
                // 同名同路径的重复条目：**同一份源里后者为准**（与跨源规则一致）
                if (next->is_dir) {
                    next->is_dir = false;
                    next->children.clear();
                }
                next->size = entry.size;   // ★ 大小随条目进节点（界面右列要显示它）
                next->source_index = 0;
            } else if (!last && !next->is_dir) {
                // 中间段先前被当成文件（另一份源里它是目录）：类型变化以后者为准
                next->is_dir = true;
                next->children.clear();
                next->source_index = 0;
            }
            cur = next;
        }
    }
    sort_children(tree.root);
    return tree;
}

void MergeTree::merge(const MergeTree& other, int other_source_index) {
    if (other.sources.size() == 1) {
        sources.push_back(other.sources.front());
    } else {
        sources.insert(sources.end(), other.sources.begin(), other.sources.end());
    }

    // 逐层合并。**递归只在"两边都是目录"时发生**（照 1.x `_Node.merge`）：
    // 类型不同就直接以后者为准（该子树整体换掉）。
    const std::function<void(MergeNode&, const MergeNode&)> merge_children =
        [&](MergeNode& dst, const MergeNode& src) {
            for (const MergeNode& src_child : src.children) {
                MergeNode* cur = child_by_name(dst, src_child.name);
                if (cur != nullptr && cur->is_dir && src_child.is_dir) {
                    merge_children(*cur, src_child);
                    continue;
                }
                MergeNode copy = src_child;
                if (cur != nullptr) {
                    // 6.1：勾选态跟随**原来的节点**（用户勾过的不能因为重建树而丢）
                    copy.checked = cur->checked;
                    ++conflicts_;
                }
                copy.source_index = other_source_index;
                // 整棵子树都属于新来源
                const std::function<void(MergeNode&)> retag = [&](MergeNode& n) {
                    n.source_index = other_source_index;
                    for (MergeNode& c : n.children) retag(c);
                };
                retag(copy);
                if (cur != nullptr) {
                    *cur = copy;  // 后者为准
                } else {
                    dst.children.push_back(copy);
                }
            }
            sort_children(dst);
        };
    merge_children(root, other.root);
}

MergeNode* MergeTree::find(const std::string& path) {
    const std::vector<std::string> parts = split_path(path);
    MergeNode* cur = &root;
    for (const std::string& part : parts) {
        cur = child_by_name(*cur, part);
        if (cur == nullptr) return nullptr;
    }
    return cur;
}

const MergeNode* MergeTree::find(const std::string& path) const {
    return const_cast<MergeTree*>(this)->find(path);
}

void MergeTree::toggle_all(bool checked) {
    for (MergeNode& child : root.children) apply_toggle(child, checked);
}

void MergeTree::toggle_dir(const std::string& path, bool checked) {
    MergeNode* node = find(path);
    if (node == nullptr) return;
    apply_toggle(*node, checked);
    // 目录自身的 checked 不参与计数（计数只看文件），但留给界面显示三态
}

int MergeTree::checked_file_count() const {
    int count = 0;
    collect_checked(root, count);
    return count;
}

// ★★ 2026-10-06（审计 3-1）：勾选态的**快照/回灌**（页面重建整棵树时不许把它丢掉）。
// 详见 overlay.h 里的说明：勾选态只活在树里，而 `rebuild_tree()` 是"丢掉整棵 + 重放"。
std::vector<std::string> MergeTree::checked_paths() const {
    std::vector<std::string> out;
    for (const MergeNode* node : flatten()) {
        if (node->is_dir) continue;         // 计数只看文件（与 `checked_file_count` 同口径）
        if (node->checked) out.push_back(node->path);
    }
    return out;
}

int MergeTree::apply_checked_paths(const std::vector<std::string>& paths) {
    int applied = 0;
    for (const std::string& path : paths) {
        MergeNode* node = find(path);
        if (node == nullptr) continue;      // 这一份在新树里没了（源被移除）⇒ 自然落空
        node->checked = true;
        ++applied;
    }
    return applied;
}

int MergeTree::toggle_source(int source_index, bool checked) {
    // 照 1.x `_db_toggle`：勾选一个包 = 它的**文件**全部打勾（目录跟着显示三态）
    int files = 0;
    int checked_files = 0;
    for (const MergeNode* node : flatten()) {
        if (node->source_index != source_index) continue;
        if (node->is_dir) continue;
        ++files;
    }
    // flatten() 给的是 const 指针 ⇒ 用路径回查可变节点（与 toggle_dir 同一手法）
    for (const MergeNode* node : flatten()) {
        if (node->source_index != source_index) continue;
        MergeNode* mutable_node = find(node->path);
        if (mutable_node == nullptr) continue;
        mutable_node->checked = checked;
        if (!mutable_node->is_dir && checked) ++checked_files;
    }
    (void)checked_files;
    return files;
}

void MergeTree::source_check_state(int source_index, int* files, int* checked_files) const {
    int total = 0;
    int on = 0;
    for (const MergeNode* node : flatten()) {
        if (node->source_index != source_index || node->is_dir) continue;
        ++total;
        if (node->checked) ++on;
    }
    if (files != nullptr) *files = total;
    if (checked_files != nullptr) *checked_files = on;
}

std::vector<const MergeNode*> MergeTree::flatten() const {
    std::vector<const MergeNode*> out;
    flatten_into(root, out);
    return out;
}

std::vector<size_t> sort_sources_by_basename(const std::vector<std::string>& paths) {
    std::vector<size_t> order(paths.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&paths](size_t a, size_t b) {
        return basename_lower(paths[a]) < basename_lower(paths[b]);
    });
    return order;
}

void FsSelection::select_all(const std::vector<std::string>& visible) {
    for (const std::string& key : visible) selected.insert(key);
}

void FsSelection::toggle(const std::string& key, bool on) {
    if (on) {
        selected.insert(key);
    } else {
        selected.erase(key);
    }
}

const std::vector<FsFormatItem>& fs_formats() {
    // 顺序照 1.x 的下拉构造：**auto 在第一位**（`fs_app.py:154` 的默认值就是 "auto"）
    // + 六种 DB 格式 + SquashFS。**key 一律内部键**。
    static const std::vector<FsFormatItem> items = [] {
        std::vector<FsFormatItem> out;
        // 1.x `fs_app.py:193`：`self.fmt_keys = ["auto"] + [ALL_FMTS[k]["name"] ...]`
        //   ⇒ auto 永远是第一项；它的"说明文字"由界面那一行标签给（1.x `fmt_desc`）。
        out.push_back({"auto", "auto", false, true});
        const DbFormat all[] = {DbFormat::Xdb, DbFormat::Ru2947, DbFormat::Ww2947,
                                DbFormat::F2945, DbFormat::F2215, DbFormat::F11xx};
        for (DbFormat f : all) {
            out.push_back({db_format_key(f), db_format_name(f), false, false});
        }
        out.push_back({"sqfs", "SquashFS", true, false});
        return out;
    }();
    return items;
}

std::optional<DbFormat> format_key_to_db(const std::string& key) {
    // **只认内部键**：显示名（含空格/括号/CJK）不许解析得出来 ——
    // 1.x 的事故正是把显示名当下拉取值。
    for (const FsFormatItem& item : fs_formats()) {
        if (item.is_auto || item.is_sqfs) continue;   // auto＝不给格式；sqfs 不是 DB 格式
        if (item.key == key) return db_format_from_any(key);
    }
    return std::nullopt;
}

bool format_key_is_sqfs(const std::string& key) {
    for (const FsFormatItem& item : fs_formats()) {
        if (item.is_sqfs && item.key == key) return true;
    }
    return false;
}

bool format_key_is_auto(const std::string& key) {
    for (const FsFormatItem& item : fs_formats()) {
        if (item.is_auto && item.key == key) return true;
    }
    return false;
}

namespace {
// 层数：相对路径里的 '/' 个数 + 1（root 的直接子项 = 1 层）。
int depth_of_rel(const std::string& rel) {
    int depth = 1;
    for (char c : rel) {
        if (c == '/') ++depth;
    }
    return depth;
}
}  // namespace

std::vector<TreeItem> collect_tree(const std::string& root, WalkReport* report) {
    std::vector<TreeItem> out;
    if (report != nullptr) {
        report->dirs = 0;
        report->files = 0;
        report->max_depth = 0;
        report->errors.clear();
    }
    std::error_code ec;
    const stdfs::path base = xrkit::core::path_from_utf8(root);
    if (!stdfs::is_directory(base, ec)) {
        // ★ 零静默：原先这里**静默返回空**（"目录不存在"与"目录是空的"在返回值上
        //   一模一样）—— 编码转换页据此会把"路径填错了"报成"没找到可转换的文件"。
        if (report != nullptr) {
            report->errors.push_back(root + " | 不是可读目录" +
                                     (ec ? "（" + ec.message() + "）" : "（路径不存在或不是目录）"));
        }
        return out;
    }
    for (stdfs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            // 原来是 `break`（后面还有多少项、为什么停，一个字都不说）。
            if (report != nullptr) {
                report->errors.push_back(core::path_to_utf8(it->path()) +
                                         " | 遍历中断：" + ec.message());
            }
            break;
        }
        const stdfs::path rel = stdfs::relative(it->path(), base, ec);
        if (ec) {
            if (report != nullptr) {
                report->errors.push_back(core::path_to_utf8(it->path()) +
                                         " | 取相对路径失败：" + ec.message());
            }
            ec.clear();
            continue;
        }
        std::error_code dir_ec;
        const bool is_dir = it->is_directory(dir_ec);
        TreeItem item;
        item.path = core::path_to_utf8(rel);
        item.is_dir = is_dir;
        if (!item.is_dir) item.source_path = core::path_to_utf8(it->path());
        if (report != nullptr) {
            if (item.is_dir) {
                ++report->dirs;
            } else {
                ++report->files;
            }
            report->max_depth = std::max(report->max_depth, depth_of_rel(item.path));
        }
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(),
              [](const TreeItem& a, const TreeItem& b) { return a.path < b.path; });
    return out;
}

bool is_package_file_name(const std::string& path) {
    // 照 1.x：`n = basename(p).lower(); d = n.rfind("."); return d >= 0 and n[d:d+3] == ".db"`
    // ★★ 2026-10-04 补**第三个家族 `.xdb*`**（用户问「`.xdb`，我们的工具能识别吗？」⇒ 实测**不能**，真缺陷）：
    //   旧写法只取"点后**两位**"必须等于 `db`/`sq` ⇒ `patch_00.xdb` 的 `xd` 判 false
    //   ⇒ **拖入 / 扫目录（`ScanDir` 与"封包排除包文件"共用这一份）会把 `.xdb` 静默忽略**。
    //   依据（实测）：`E:\Administrator01\Download\sop_standalone.7z` 里有 `patches/patch_00.xdb`(11.6 MB)、
    //   `data/core.db`、`data/resources/resources.db00`、`data/levels/levels.db00`，
    //   `xrkit_cli --dump` 对它们**全部判为 `format=xdb`** ⇒ `.xdb` 就是我们内部的
    //   `DbFormat::Xdb`，是 `.db*` / `.sq*` 之外**唯一已知的包家族**。
    //   实现：改为"点后**以** `db` / `sq` / `xdb` **开头**" —— 与旧规则对前两个家族**完全等价**
    //   （旧的就是取前两位比较），不误认 `x.xdoc`（前两位是 `xd` 但不是 `xdb`）。
    const std::string name = basename_lower(path);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return false;
    const std::string tail = name.substr(dot + 1);
    return tail.rfind("db", 0) == 0 || tail.rfind("sq", 0) == 0 || tail.rfind("xdb", 0) == 0;
}

PackCollectResult collect_pack_files(const std::string& base,
                                     const std::vector<std::string>& roots) {
    PackCollectResult out;
    std::set<std::string> have;   // 去重（1.x 的 `have` 集合）
    std::error_code ec;
    const stdfs::path base_path = base.empty() ? stdfs::path() : core::path_from_utf8(base);
    for (const std::string& root : roots) {
        const stdfs::path root_path = core::path_from_utf8(root);
        std::vector<stdfs::path> files;
        if (stdfs::is_directory(root_path, ec)) {
            for (stdfs::recursive_directory_iterator it(root_path, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec)) files.push_back(it->path());
            }
        } else if (stdfs::is_regular_file(root_path, ec)) {
            files.push_back(root_path);
        }
        std::sort(files.begin(), files.end());
        for (const stdfs::path& file : files) {
            if (is_package_file_name(core::path_to_utf8(file))) {
                ++out.skipped_package_files;   // 1.x `_is_pack_file`：包文件不参与封包
                continue;
            }
            std::string rel;
            if (base_path.empty()) {
                rel = core::path_to_utf8(file.filename());
            } else {
                const stdfs::path relative = file.lexically_relative(base_path);
                const std::string text = core::path_to_utf8(relative);
                if (relative.empty() || text.rfind("..", 0) == 0) {
                    ++out.skipped_outside_base;   // 不在 base 之下：跳过 + 报数（不静默丢）
                    continue;
                }
                rel = text;
            }
            if (rel.empty() || !have.insert(rel).second) continue;
            out.items.push_back({rel, core::path_to_utf8(file)});
        }
    }
    std::sort(out.items.begin(), out.items.end(),
              [](const PackFileItem& a, const PackFileItem& b) { return a.rel < b.rel; });
    return out;
}

std::vector<EntryInfo> loose_entries(const std::string& root, std::string& error) {
    error.clear();
    std::vector<EntryInfo> out;
    std::error_code ec;
    const std::filesystem::path base = core::path_from_utf8(root);
    if (!std::filesystem::is_directory(base, ec)) {
        error = "散装层根目录不存在：" + root;
        return out;
    }
    // ★★ P17-110（复审计 `01 F-07`）：**不许跟着软链接/junction 走 ＋ 必须有上限**。
    //   修前的事实：这里用 `std::filesystem::is_directory(full, ec)` —— 它**跟随**链接
    //   （Windows 上 junction 也算目录）⇒ 两件事同时成立：
    //     ① **走出根**：`<root>/out -> D:\别处` 会被当成普通子目录递归进去，条目表里出现
    //        根本不属于这个根的东西（GUI 的树/大小列会照它显示）；
    //     ② **自指成环 = 不终止**：`<root>/self -> <root>` 会被反复递归 —— 修前**没有任何
    //        深度/条数上限** ⇒ 内存吃满、界面挂死（`loose_entries` 是在 UI 线程被调的）。
    //   口径：链接**只作为"条目"报出去**（大小按链接本身给 0），**不递归进去**；
    //   同时加**深度上限**与**条数上限**，到顶就**如实停下来并给理由**（零静默：
    //   `error` 里写清"到上限了、表是截断的"，调用方就不会把它当完整清单）。
    constexpr int kMaxDepth = 32;        // 深度上限（正常游戏目录远小于此；到顶必是环或病态）
    constexpr size_t kMaxEntries = 200000;   // 条数上限（防"目录级炸弹"把内存吃光）
    bool truncated = false;
    int max_depth_hit = 0;
    // 递归收集：目录也算条目（与包侧同形）；每层按字节序排，保证渲染确定性
    std::vector<std::filesystem::path> dirs{base};
    std::vector<std::string> rels{""};
    std::vector<int> depths{1};
    while (!dirs.empty()) {
        const std::filesystem::path dir = dirs.back();
        const std::string rel = rels.back();
        const int depth = depths.back();
        dirs.pop_back();
        rels.pop_back();
        depths.pop_back();
        std::vector<std::string> names;
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            names.push_back(core::path_to_utf8(it->path().filename()));
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            if (out.size() >= kMaxEntries) {
                truncated = true;
                break;
            }
            const std::filesystem::path full = dir / core::path_from_utf8(name);
            const std::string child = rel.empty() ? name : rel + "/" + name;
            // ★ 链接：报成条目，但**绝不递归**（见上：走出根 / 自指成环）
            const bool linked = is_link_or_junction(full);
            const bool is_dir = !linked && std::filesystem::is_directory(full, ec);
            uint64_t size = 0;
            if (!is_dir && !linked) {
                const auto bytes = std::filesystem::file_size(full, ec);
                size = ec ? 0 : static_cast<uint64_t>(bytes);
            }
            out.emplace_back(child, is_dir, size);   // ★ 大小也带出去（1.x 的树有大小列）
            if (linked) continue;                    // ← 关键：不进链接
            if (is_dir) {
                if (depth >= kMaxDepth) {            // 到深度上限：停在这层，如实记账
                    truncated = true;
                    max_depth_hit = std::max(max_depth_hit, depth);
                    continue;
                }
                dirs.push_back(full);
                rels.push_back(child);
                depths.push_back(depth + 1);
            }
        }
    }
    std::sort(out.begin(), out.end());
    // ★★ P17-110：到上限就**如实说**（否则调用方会把截断的表当完整清单 —— 那是另一种静默）
    if (truncated) {
        error = "条目表已截断：";
        if (out.size() >= kMaxEntries) {
            error += "条数到上限（" + std::to_string(kMaxEntries) + " 条）";
        } else {
            error += "目录深度到上限（" + std::to_string(kMaxDepth) + " 层，最大到 " +
                     std::to_string(max_depth_hit) + " 层）";
        }
        error += "；链接/联接只作为条目列出、不递归进去。";
    }
    return out;
}

std::vector<EntryInfo> package_entries(const std::string& path,
                                       std::optional<DbFormat> db_fallback,
                                       std::string& error,
                                       const plugin::Registry* registry,
                                       DbReadInfo* read_info) {
    error.clear();
    std::vector<EntryInfo> out;
    // ★★ P17-85（01 F-09 收尾）：出参先**清零**（调用方可以复用一个变量；"没填"＝没发生整包读）。
    if (read_info != nullptr) *read_info = DbReadInfo{};

    // ★ 2026-10-04 解密闸门（⑫12.1 消费端）：CLI 与 GUI **共用这一枚**（见 overlay.h 的说明）。
    //   没有活动注册表 ⇒ 下面这段一个字节都不改行为（`source` 就是 `path`）。
    const plugin::DecryptGateResult gate = plugin::gate_active_read(path, registry);
    if (!gate.ok) {
        error = gate.error;   // 与 GUI 同一条文案（外面各自决定怎么显示/退出）
        return out;
    }
    const std::string& source = gate.used_plugin ? gate.path : path;

    // ① SquashFS：**先只读 96 字节超块**判 magic（8.3 GB 的镜像不能整包进内存）
    SuperBlock super;
    std::string super_error;
    if (read_superblock(source, super, super_error)) {
        // ★ 把**同一份** registry 传下去：内层若再去问全局钩子，就会把上面这次判定推翻
        //   （显式传入者优先这条判据就是这么被自检抓出来的）。
        const ListResult listed = list_entries(source, registry);
        if (!listed.ok) {
            error = listed.error.empty() ? std::string("读不出 SquashFS 条目表") : listed.error;
            return out;
        }
        out.reserve(listed.entries.size());
        for (const Entry& entry : listed.entries) {
            out.emplace_back(entry.path, entry.is_dir, entry.size);   // SquashFS 的 Entry.size
        }
        return out;
    }

    // ② X-Ray DB 六格式：条目表在**文件尾部** ⇒ seek 式只读"块头 + 尾部那一块"。
    //    ★ 2026-09-30 用户第四轮 ⑦（"现在还是慢得不行"）：旧实现把整文件读进内存，
    //      实测 1900 MB 的包光读盘就要 8,063 ms（耗时只与文件大小成正比、与条目数无关）。
    //      `list_db_seek` 的判定/错误文案与旧路径**逐值一致**（对不上就退回整文件读）。
    const DbListing listed = list_db_seek(source, db_fallback, registry);
    // ★★ P17-85（01 F-09 收尾）：**诊断就在这里被丢掉过**（修前只搬 `entries`）——
    //   现在按"给了出参才填"的纪律抄出去：`header_bytes`（读了多少）＋ `used_seek`
    //   （走没走 seek）＋ `whole_file_reason`（为什么不得不整包读）三件一起，才叫可观测。
    if (read_info != nullptr) {
        read_info->header_bytes = listed.header_bytes;
        read_info->used_seek = listed.used_seek;
        read_info->whole_file_reason = listed.whole_file_reason;
        read_info->ambiguous = listed.ambiguous;
    }
    if (!listed.ok) {
        error = listed.error;
        return out;
    }
    out.reserve(listed.entries.size());
    for (const DbEntry& entry : listed.entries) {
        out.emplace_back(entry.path, entry.is_dir, entry.size_real);   // DB 的 size_real（未压缩长度）
    }
    return out;
}

}  // namespace xrkit::engine::fs

// ── ★★ 2026-10-06（审计第 4 组）：模块**在编**探针 ─────────────────────────────
// 为什么放在本模块自己的翻译单元里：`probe_fs()` 能链上来，就说明**这一块真的在二进制里**
// （把本模块摘掉 ⇒ 链接失败，而不是 `status_line()` 里少报一个数字 —— 见 `probe_decls.h`）。
// 返回的是本文件内的常量（`extern "C"` 那个短名不给别的模块用），不复用别的模块的符号。
extern "C" const char* xrkit_engine_probe_fs_tag() { return "xrkit-engine-module-fs"; }

namespace xrkit::engine {
const char* probe_fs() { return xrkit_engine_probe_fs_tag(); }
}  // namespace xrkit::engine

