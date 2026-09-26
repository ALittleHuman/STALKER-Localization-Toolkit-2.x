#pragma once
// xrkit::engine::fs —— 文件系统页的**纯模型**（对照表 ⑥ 的交互面）。
//
// 为什么模型放引擎而不是页面里：这些规则（合并胜负、加载序、勾选态、下拉取值、
// 目录条目）都是**可判定的行为**，页面只该负责把模型画出来。放在无 Qt 的一层，
// CLI 自检就能直接断言它们 —— 不用起 Qt、也不用点界面。
//
// 逐条照 1.x `apps/fs_app.py` 的 `_Node.merge` / `_load_async` / 下拉构造（照代码）：
//   · 6.1 合并 = **后者为准**；同名项的 `source` 随之切换成"引擎实际会读到的那一份"；
//     **勾选态跟随原节点**（用户勾过的不能因为重建树而丢）；两个都是目录就**继续递归**；
//     类型不同（文件↔目录）**不递归**，直接以后者为准。
//   · 6.2 加载顺序 = 按 `basename().lower()` 升序，字典序靠后的包赢。
//   · 6.3 `selected` 与 `checked` 是**两套独立状态**：DB 栏「全选」只改前者，
//     包内「全选/全不选」只改后者。
//   · 6.12 目录条目必须**如实**带 `is_dir`（1.x 的封包路径把 `is_dir` 写死 False，
//     于是引擎的 DIRTYPE 分支在 App 路径下永不触发）。
//   · 6.13 格式下拉存的是**内部键**（1.x 存显示名 ⇒ `ALL_FMTS.get(fmt)` 必然取不到）。

#include "xrkit/engine/fs/db.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xrkit::engine::fs {

// ── 6.1/6.2 合并树 ───────────────────────────────────────────────────

struct MergeNode {
    std::string name;  // 单段名字（树按段组织，与 1.x 的 `children: {name: node}` 同构）
    std::string path;  // 累计到本节点的镜像内路径（'/' 分隔）
    bool is_dir = false;
    bool checked = false;
    uint64_t size = 0;
    int source_index = -1;  // "引擎实际会读到的那一份"来自第几个源（-1 = 没来源）
    std::vector<MergeNode> children;  // 目录才有；按 name 字节序
};

// 一个源的条目清单（SquashFS 的 `Entry` / DB 的 `DbEntry` 都能喂进来）
struct MergeInput {
    std::string source;  // 用户看到的包名/路径（1.x 里就是包文件名）
    std::vector<std::pair<std::string, bool>> entries;  // (path, is_dir)
};

// 散装层（用户 2026-09-26 决策"最后并入层"；落地口径由我定，2026-09-27）：
// 把一个**真实目录**（解包后的 gamedata）变成与"包"同形的条目表，交给**同一个 MergeTree**
// —— 于是"同名条目以后加载的为准"这条语义对散装层与包**完全一致**，不需要第二套合并逻辑。
//   · 路径一律用 '/' 分隔、相对 root、**不含**前导 './'；
//   · 目录也作为条目出现（与包侧的 is_dir 同形），空目录照样算；
//   · 排序：每层按名字的字节序（与包侧 `children` 的排序口径一致，保证渲染确定性）。
std::vector<std::pair<std::string, bool>> loose_entries(const std::string& root,
                                                        std::string& error);

struct MergeTree {
    MergeNode root;                      // 虚拟根（name/path 为空）
    std::vector<std::string> sources;    // **合并顺序**（= 加载顺序，后者赢）

    // 6.1：把 `other` 树并进来（后者为准 + 勾选态跟随原节点 + 目录递归）
    void merge(const MergeTree& other, int other_source_index);

    // 递归找节点（找不到返回 nullptr）
    MergeNode* find(const std::string& path);
    const MergeNode* find(const std::string& path) const;

    // 6.3：包内「全选/全不选」—— 只改 `checked`（**不碰** selected）
    void toggle_all(bool checked);
    void toggle_dir(const std::string& path, bool checked);

    // 打平成一列（前序遍历，目录在前）—— 界面用它喂虚拟列表
    std::vector<const MergeNode*> flatten() const;

    int conflict_count() const { return conflicts_; }
    int checked_file_count() const;

private:
    int conflicts_ = 0;
};

// 从条目清单建树（单源，不做合并）
MergeTree build_tree(const MergeInput& input);

// 6.2：加载顺序 = 按 `basename().lower()` 升序；同名的按原顺序（稳定排序）。
// 返回的是**排序后的下标**，便于调用方按顺序取源。
std::vector<size_t> sort_sources_by_basename(const std::vector<std::string>& paths);

// ── 6.3 两套状态 ─────────────────────────────────────────────────────

// DB 栏的选中态。它和树里的 `checked` **没有关系**：1.x 里
// 「全选/加载选中/取消加载/移除」走 `_sel`，「解包」走 `checked`，
// 混成一套就会出现"点全选把包内文件也勾上了"。
struct FsSelection {
    std::set<std::string> selected;

    void select_all(const std::vector<std::string>& visible);
    void clear() { selected.clear(); }
    void toggle(const std::string& key, bool on);
    bool contains(const std::string& key) const { return selected.count(key) != 0; }
    size_t size() const { return selected.size(); }
};

// ── 6.13 格式下拉 ────────────────────────────────────────────────────

struct FsFormatItem {
    std::string key;    // **下拉的取值**（内部键，如 "xdb" / "sqfs"）
    std::string label;  // 用户可见名（如 "XDB (CS/CoP)" / "SquashFS"）
    bool is_sqfs = false;
};

const std::vector<FsFormatItem>& fs_formats();

// 内部键 → DB 格式；"sqfs" 不是 DB 格式（返回 nullopt，用 is_sqfs 判）。
// **显示名不该能解析**：1.x 的事故就是把显示名当下拉取值 → `ALL_FMTS.get` 取不到
// → SquashFS 被静默按 xdb 打包。自检拿显示名喂进来看它必须解析不出来。
std::optional<DbFormat> format_key_to_db(const std::string& key);
bool format_key_is_sqfs(const std::string& key);

// ── 6.12 目录条目 ────────────────────────────────────────────────────

struct TreeItem {
    std::string path;         // 相对 root，'/' 分隔
    bool is_dir = false;
    std::string source_path;  // 本机路径（目录为空）
};

// 递归收集 root 下的条目，**目录如实带 is_dir**（1.x 在 App 侧写死 False）。
// 顺序 = 相对路径的 UTF-8 字节序（确定性，便于对拍）。
std::vector<TreeItem> collect_tree(const std::string& root);

}  // namespace xrkit::engine::fs
