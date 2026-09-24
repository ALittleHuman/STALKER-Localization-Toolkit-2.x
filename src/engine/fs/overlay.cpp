#include "xrkit/engine/fs/overlay.h"

#include "xrkit/core/path.h"

#include <algorithm>
#include <filesystem>
#include <functional>

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
    std::sort(node.children.begin(), node.children.end(),
              [](const MergeNode& a, const MergeNode& b) { return a.name < b.name; });
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
    for (const auto& [path, is_dir] : input.entries) {
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
                fresh.source_index = 0;
                cur->children.push_back(fresh);
                next = &cur->children.back();
            } else if (last && !is_dir) {
                // 同名同路径的重复条目：**同一份源里后者为准**（与跨源规则一致）
                if (next->is_dir) {
                    next->is_dir = false;
                    next->children.clear();
                }
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
    // 顺序照 1.x 的下拉构造：六种 DB 格式 + SquashFS。**key 一律内部键**。
    static const std::vector<FsFormatItem> items = [] {
        std::vector<FsFormatItem> out;
        const DbFormat all[] = {DbFormat::Xdb, DbFormat::Ru2947, DbFormat::Ww2947,
                                DbFormat::F2945, DbFormat::F2215, DbFormat::F11xx};
        for (DbFormat f : all) {
            out.push_back({db_format_key(f), db_format_name(f), false});
        }
        out.push_back({"sqfs", "SquashFS", true});
        return out;
    }();
    return items;
}

std::optional<DbFormat> format_key_to_db(const std::string& key) {
    // **只认内部键**：显示名（含空格/括号/CJK）不许解析得出来 ——
    // 1.x 的事故正是把显示名当下拉取值。
    for (const FsFormatItem& item : fs_formats()) {
        if (!item.is_sqfs && item.key == key) return db_format_from_any(key);
    }
    return std::nullopt;
}

bool format_key_is_sqfs(const std::string& key) {
    for (const FsFormatItem& item : fs_formats()) {
        if (item.is_sqfs && item.key == key) return true;
    }
    return false;
}

std::vector<TreeItem> collect_tree(const std::string& root) {
    std::vector<TreeItem> out;
    std::error_code ec;
    const stdfs::path base = xrkit::core::path_from_utf8(root);
    if (!stdfs::is_directory(base, ec)) return out;
    for (stdfs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        const stdfs::path rel = stdfs::relative(it->path(), base, ec);
        if (ec) continue;
        TreeItem item;
        item.path = xrkit::core::path_to_utf8(rel);
        item.is_dir = it->is_directory(ec);
        if (!item.is_dir) item.source_path = xrkit::core::path_to_utf8(it->path());
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(),
              [](const TreeItem& a, const TreeItem& b) { return a.path < b.path; });
    return out;
}

}  // namespace xrkit::engine::fs
