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
#include "xrkit/engine/plugin/registry.h"

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

// ★ 一个源的条目（**带大小** —— 用户 2026-09-28 对比 1.x 的树：人家右侧有大小列）：
// 原来这里只有 `(path, is_dir)`，所以 2.x 的树**根本没有大小可显示**。
// 两参构造留着是有意的：既有调用点写的是 `{{"a.txt", false}}`，不用改一行。
struct EntryInfo {
    std::string path;
    bool is_dir = false;
    uint64_t size = 0;
    EntryInfo() = default;
    EntryInfo(std::string p, bool d, uint64_t s = 0)
        : path(std::move(p)), is_dir(d), size(s) {}
    // 排序口径**照旧**（原来是 `pair<path, is_dir>` 的字典序）：先 path、再 is_dir；
    // size 只作最后的决胜（不影响既有顺序，`loose_entries` 的 `std::sort` 依赖它）
    bool operator<(const EntryInfo& other) const {
        if (path != other.path) return path < other.path;
        if (is_dir != other.is_dir) return is_dir < other.is_dir;
        return size < other.size;
    }
};

// 一个源的条目清单（SquashFS 的 `Entry` / DB 的 `DbEntry` / 散装目录都能喂进来）
struct MergeInput {
    std::string source;  // 用户看到的包名/路径（1.x 里就是包文件名）
    std::vector<EntryInfo> entries;
};

// 散装层（用户 2026-09-26 决策"最后并入层"；落地口径由我定，2026-09-27）：
// 把一个**真实目录**（解包后的 gamedata）变成与"包"同形的条目表，交给**同一个 MergeTree**
// —— 于是"同名条目以后加载的为准"这条语义对散装层与包**完全一致**，不需要第二套合并逻辑。
//   · 路径一律用 '/' 分隔、相对 root、**不含**前导 './'；
//   · 目录也作为条目出现（与包侧的 is_dir 同形），空目录照样算；
//   · 排序：每层按名字的字节序（与包侧 `children` 的排序口径一致，保证渲染确定性）。
std::vector<EntryInfo> loose_entries(const std::string& root, std::string& error);

// ★ 1.x `_is_db` / `_is_sq` 的**照抄**（看 basename 的最后一个扩展名头两个字母）：
//   `x.db` / `x.db0` / `x.db_base` ⇒ 真；`x.sq` / `x.sqfs` / `x.sq_textures` ⇒ 真。
// 用途：扫描目录时认包、加入封包列表时**排除包文件**（1.x `_is_pack_file`）。
bool is_package_file_name(const std::string& path);

// ── 封包列表（1.x `files_data` 的"相对路径"那一半）──────────────────
// 1.x `_add_to_pack` 的判据逐条照抄：
//   · 递归收集（目录进去找、文件就它自己）；
//   · **排除包文件**（`is_package_file_name` ⇔ 1.x `_is_pack_file`）——把 .db/.sq 打进新包
//     等于把旧包套娃，1.x 明确不做；
//   · 相对 `base` 归一（base 为空 ⇒ 只用文件名）；**不在 base 之下的跳过并计数**；
//   · 按 rel 去重（先到先得）、按 rel 字节序排序（产物确定性）。
struct PackFileItem {
    std::string rel;          // 打包进包的内部路径（'/' 分隔，相对 base）
    std::string source_path;  // 本机文件（UTF-8）
};
struct PackCollectResult {
    std::vector<PackFileItem> items;
    int skipped_outside_base = 0;
    int skipped_package_files = 0;
};
PackCollectResult collect_pack_files(const std::string& base,
                                     const std::vector<std::string>& roots);

// 从一个**包文件**列出条目表（与 `loose_entries` 同形 —— 好让"包"与"散装目录"走同一条
// `add_source` 路径，不出现第二套合并/加载逻辑）。
//
// 判定顺序（照 1.x `apps/fs_app.py` 的 `_load_one`）：
//   ① **先读 96 字节超块**判 SquashFS（magic `hsqs`/`sqsh`）：命中就走元数据列表，
//      **绝不整包进内存** —— 真机上有 8.3 GB 的 `gamedata.sq_textures`；
//   ② 否则按 X-Ray DB：整文件读进来 + `auto_detect`（DB 的条目表在尾部，绕不开整文件读）；
//      自洽格式**唯一**才采用；歧义/认不出时用 `db_fallback`（界面下拉里选的那个）；
//   ③ 都不成立 ⇒ `error` 里写清原因（界面直接显示，**不静默当空包**）。
// `db_fallback` 传 nullopt = "只认自动判定"。
std::vector<EntryInfo> package_entries(const std::string& path,
                                       std::optional<DbFormat> db_fallback,
                                       std::string& error,
                                       const plugin::Registry* registry = nullptr);
// ★ 2026-10-04（用户口径：「GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比
//   CLI 还多的。」）：**读入口过解密闸门**。闸门原来只在外壳的 `FsPage::gate_source` 里
//   ⇒ CLI 直接调引擎，加密包被当普通包读（前端分叉）。现在这里先过
//   `plugin::gate_active_read`：
//     · 没有活动注册表 / 没有解密器命中 ⇒ **行为与今天逐字节一致**（路径、文案都不动）；
//     · 命中且解开 ⇒ 读的是解密产物（闸门写在 `plugin::decrypt_output_dir()` 下）；
//     · 命中却解不开 ⇒ `error` 里是与 GUI **同一条**文案，零条目返回（绝不回落成普通包）。
//   `registry` 显式传入者优先；为空（默认）⇒ 用 `plugin::active_registry()`。
//   散装层 `loose_entries` **不过闸门**：它吃的是目录，没有"这个包需要解密"的语义；
//   目录里的包各自走这里时已经过闸门了。

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
    // ★ 2026-09-28（对齐 1.x `_db_toggle`）：**勾选一个包 = 该包贡献的条目全部打勾**。
    // 合并树里"这一份是谁的"由 `source_index` 说了算（后者为准），所以按它筛即可。
    // 返回被改动的**文件**条目数（目录不计，与 `checked_file_count` 同口径）。
    int toggle_source(int source_index, bool checked);
    // 某个源在合并树里还看得见的文件数（界面显示"整包勾选态"用它判三态）
    void source_check_state(int source_index, int* files, int* checked_files) const;

    // 打平成一列（前序遍历，目录在前）—— 界面用它喂虚拟列表
    std::vector<const MergeNode*> flatten() const;

    int conflict_count() const { return conflicts_; }
    int checked_file_count() const;

    // ★★ 2026-10-06（审计 3-1）：勾选态的**快照 / 回灌** —— 页面「加一个包」走的是
    //   `rebuild_tree()`（整棵树丢掉重放，见 `fs_page.cpp` 的同名函数），而勾选态
    //   **只活在树里**（`EntryInfo`/`Source` 都没有这个字段）⇒ 修前"源集合一变，用户
    //   勾过的全没了"（违背 6.1 的"勾选态跟随原节点"：那条只在 `merge()` 的**增量**合并
    //   里成立，而页面每次都从空树重放）。
    //   口径：**按路径快照、按路径回灌**（路径就是"原节点"的身份）—— 回灌时按
    //   `find(path)` 逐个置位；新树里没有的路径（那一份源被移除/被别的源顶掉且换了名）
    //   自然落空。`apply_checked_paths` 返回真正置上的条数（判据/自检可对账）。
    std::vector<std::string> checked_paths() const;
    int apply_checked_paths(const std::vector<std::string>& paths);

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
    std::string key;    // **下拉的取值**（内部键，如 "auto" / "xdb" / "sqfs"）
    std::string label;  // 用户可见名（如 "XDB (CS/CoP)" / "SquashFS"）
    bool is_sqfs = false;
    // ★ 2026-10-06（1.x `fs_app.py:154` 的默认值就是 `tk.StringVar(value="auto")`）：
    //   **"auto" = 不给格式** —— 走自洽判定（歧义时**显式拒绝**，那是既有正确行为，不改）。
    //   它既不是 DB 格式也不是 SquashFS ⇒ `format_key_to_db` / `format_key_is_sqfs`
    //   都**不认**它；要判它只走 `format_key_is_auto`（一个谓词，别在两处各判一次）。
    bool is_auto = false;
};

const std::vector<FsFormatItem>& fs_formats();

// 内部键 → DB 格式；"sqfs" 不是 DB 格式（返回 nullopt，用 is_sqfs 判）。
// **显示名不该能解析**：1.x 的事故就是把显示名当下拉取值 → `ALL_FMTS.get` 取不到
// → SquashFS 被静默按 xdb 打包。自检拿显示名喂进来看它必须解析不出来。
std::optional<DbFormat> format_key_to_db(const std::string& key);
bool format_key_is_sqfs(const std::string& key);
// "auto" 这一项：**不给格式**（等价于格式参数缺省）。两条读侧入口据此把"用户选的
// auto"与"没给格式"**收敛成同一条路**，而不是各自写一遍 `if (key == "auto")`。
bool format_key_is_auto(const std::string& key);

// ── 6.12 目录条目 ────────────────────────────────────────────────────

struct TreeItem {
    std::string path;         // 相对 root，'/' 分隔
    bool is_dir = false;
    std::string source_path;  // 本机路径（目录为空）
};

// 遍历台账（**零静默**口径，2026-10-06 D2 批）。
//
// 为什么要它：原来 `collect_tree` 遇到 `ec` 就 `break` / `continue`，**扫不到的东西
// 一个字都不报**。编码转换页那条"递归子目录"要如实说出"走了几层、扫到多少文件、
// 哪些没读到"，靠原来的返回值说不出来。所以台账跟着**同一份**遍历实现一起出来
// （而不是让调用方自己再写一遍遍历 —— 那就是第 5 份 `recursive_directory_iterator`）。
struct WalkReport {
    int dirs = 0;                     // 扫到的目录数（不含 root 自己）
    int files = 0;                    // 扫到的常规文件数
    int max_depth = 0;                // 层数：root 的直接子项 = 1
    std::vector<std::string> errors;  // 逐条："路径 | 原因"（读不到就记账，绝不静默跳过）
};

// 递归收集 root 下的条目，**目录如实带 is_dir**（1.x 在 App 侧写死 False）。
// 顺序 = 相对路径的 UTF-8 字节序（确定性，便于对拍）。
// `report` 非空 ⇒ 顺便把台账填好（可空，老调用点行为不变）。
std::vector<TreeItem> collect_tree(const std::string& root, WalkReport* report = nullptr);

}  // namespace xrkit::engine::fs
