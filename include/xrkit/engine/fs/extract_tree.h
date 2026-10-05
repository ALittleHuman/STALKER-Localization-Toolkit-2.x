#pragma once
// xrkit::engine::fs —— 整树导出（提取到磁盘）与路径越界判据。
//
// 对照表 ⑥ 的 6.7/6.8/6.9/6.10/6.14 落在这一层。为什么它必须待在引擎里而不是页面里：
// 归档条目名是**不可信输入**（本工具的主要入口就是"打开从网上拿到的 mod 包"），
// 而落盘就是**任意文件写**；这条判据必须和"写文件"这件事绑在同一个函数里，
// 页面只负责传参和显示。
//
// 逐条照抄 1.x `file_system/stalker_fs.py`（照代码，不照文档）：
//   · 6.7 `safe_out_path` 五条拒因（空/非串、`/` 或 UNC 开头、`rel[1]==':'`、
//     分段含 `..`、`commonpath` 兜底复核）；
//   · 6.8 空文件是**合法条目**（写出 0 字节），与"解不出来"分开；
//   · 6.9 **原子写**：同目录 `.part` → 写后自校验 → 同盘 rename；失败 unlink；
//     ★ 2026-10-05（用户裁决 ③）：自校验默认＝**边写边算哈希 + 实际字节数**（零额外 I/O），
//     `verify_readback=true` 才回到"读回逐字节复核"；
//   · 6.10 选中集**只有目录 / 空选择**时，1.x 会退化成"整镜像导出"并记为成功 ——
//     2.0 把"全部"与"空选择"分成两个显式状态，空选择直接拒绝并说明理由；
//   · 6.14 失败原因**随返回值走**（1.x 用模块级全局变量报错，并发任务互相覆盖）。

#include "xrkit/engine/fs/db.h"
// `EntryInfo`（右栏预览的条目表要和合并树**同一个类型** —— 免得两处各写一份同形结构）。
// 方向是单向的（`overlay.*` 不 include 本文件），不会绕回来。
#include "xrkit/engine/fs/overlay.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xrkit::engine::fs {

// ── 6.7 路径越界判据 ─────────────────────────────────────────────────
//
// 1.x 返回 `None`（"不安全"），理由只写在注释里。2.0 把理由**带出来**：
// 页面要显示"哪一条被拒、为什么"，黄金语料要能对拍"拒绝理由"本身。
struct OutPathResult {
    bool ok = false;
    std::string reason;  // ok=false 时非空（中文，给日志/界面直接用）
    std::string path;    // ok=true 时：落盘用的本机路径（UTF-8）
};

// 把归档内相对路径安全地拼到 out_dir 下。**逐条照抄 1.x 的判定顺序**：
//   ① 空 / 不是字符串 → 拒
//   ② `\` → `/` 归一后以 `/` 开头（绝对路径、UNC）→ 拒
//   ③ 第 2 个字符是 `:`（盘符）→ 拒
//   ④ 拆段时**丢掉空段与 `.`**，剩下的段里出现 `..` → 拒（含 `a/../../../x`）
//   ⑤ 兜底复核：规范化后逐组件比对必须仍在 out_dir 之下（Windows 大小写不敏感；
//      跨盘/盘符相对路径在这一步被拦）
OutPathResult safe_out_path(const std::string& out_dir, const std::string& rel);

// ── 前端共用的**纯路径逻辑**（★ 架构红线：GUI 的功能 CLI 必须全有 ⇒ 这类逻辑只许一份）──
//
// 用户 2026-10-04 原话：「GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。」
// 下面这几条原先只写在 FS 页里（`resolve_output_dir` / `pack_ext_for` / `resolve_pack_target`），
// **CLI 复现不了** ⇒ 命令行与界面会算出不同的根与落点。现在实现只有这一份，页面只剩"调 + 回填"。

// 「解包到」留空时的缺省根：**第一个源所在目录**（1.x `_resolve_output`）；没有源 ⇒ current_dir。
std::string resolve_output_root(const std::string& front_source_path, const std::string& current_dir);

// 封包产物的扩展名：sqfs ⇒ `.sqfs`；其余 ⇒ `.<内部键>.db`（1.x `_pack_ext`）。
std::string pack_ext_for(const std::string& format_key);

// 封包落点：`typed` ＝调用方给的输出（可为空 / 可为目录 / 可为无扩展名路径）。
// `needs_frontend_dialog=true` ⇒ **需要前端弹"保存到"**（CLI 无界面 ⇒ 直接用默认 `path`）。
struct PackTarget {
    std::string path;  // 推断出的目标（UTF-8；可能只是文件名 ⇒ 由前端定目录）
    bool needs_frontend_dialog = false;
};
PackTarget resolve_pack_target(const std::string& typed, const std::string& output_basename,
                               const std::string& format_key);

// ── 整树导出 ─────────────────────────────────────────────────────────

enum class Selection {
    All,     // 整镜像（1.x 的 `files is None`）
    Listed,  // 只导出 only 里列出的项（1.x 的 `--cat` 逐项路径）
};

struct ExtractTreeOptions {
    Selection selection = Selection::All;
    std::vector<std::string> only;  // selection == Listed 时的镜像内路径
    bool atomic = true;             // 6.9：.part → （自校验）→ rename
    // ★★ 2026-10-05（用户裁决 ③）：写后校验档位。
    //   false（默认）＝**边写边算哈希 + 实际写出字节数**，与内存那份数据比对（零额外 I/O）；
    //   true         ＝**读回逐字节复核**（旧的 6.9 行为，绝对确定的场合用）。
    // 为什么默认换成前者：读回复核是"按文件个数付钱"——真机 `gamedata.sq_base_configs`
    // （14,861 文件 / 13.5 MB）上实测 ≈ 17.9 s（31.1 s → 13.25 s 的差），而它换来的只是
    // "确认写出的等于想写的字节"；同一条判据用流式哈希在内存里就能得到。
    // `atomic=false` 时本项无意义（没有临时文件，也就没有可校验的中间态）。
    // CLI 入口：`--unpack-tree … --verify-readback`；**GUI 不做这个开关**（用户没要）。
    bool verify_readback = false;
};

struct ExtractTreeFailure {
    std::string path;
    std::string reason;
};

struct ExtractTreeResult {
    bool ok = false;       // 只有"整批根本没跑起来"才是 false（单项失败看 failures）
    std::string error;     // ok=false 的原因；**随返回值走**，没有全局变量（6.14）
    // ★★ 2026-10-04（判红锁②）：三个计数一律走**最终产物**口径 —— 同一个落点被多条记录占据时
    //   只算**一次**（后写覆盖前写 ⇒ 产物没有变多）。用户口径：右栏＝**解包产物的预览**，
    //   所以"报出来的数"必须等于"盘上真有的东西"，不是包内记录条数。
    //   现场：`IWP\database\maps\fake_start.db` 包内 26 条文件记录 / 实际落盘 13 个文件、
    //   4,451,688 B / 实际 2,225,844 B（改前报数正好是真实值的 2 倍 −1）。
    uint64_t files_written = 0;
    uint64_t dirs_created = 0;
    uint64_t bytes_written = 0;
    // 同落点被多条记录占据的次数（重复记录 ⇒ **没有**多出产物；报数/排查用）。
    // `files_written + duplicate_records` ≈ 包内文件记录数。
    uint64_t duplicate_records = 0;
    uint64_t skipped_dirs = 0;                 // Listed 模式下按 1.x 跳过的目录项
    // ★★ 2026-10-05（**落点覆盖必须可见**）：本次写盘**覆盖掉**的已存在文件（逐条）。
    //
    // 起因（用户报的真实事故）：解包到**默认落点**（＝来源所在目录）时，一次把 3.9 万个文件
    // 盖在了模组目录上，而工具**一个字都没说**。用户口径不变（**不改默认落点** —— 默认落点就是
    // 来源所在目录），但要满足**零静默红线**："任何改写都必须可见：逐条原因 + 计数"。
    //   · `overwritten` ＝ 逐条路径（GUI/CLI 拿它去打"哪一条被盖了"）；
    //   · `overwritten.size()` ＝ 计数（CLI 的 `overwritten=` 那一格）。
    // ★ 判据落点：`overwritten_size()` —— 写回"不检测"（永远 0）即红（见 smoke / CLI 自检）。
    // ★ 本项**不弹模态确认框**：那会改交互口径（用户没要），只做"看得见"。
    std::vector<std::string> overwritten;
    // 覆盖计数（`overwritten.size()` 的显式入口 —— 调用方不必自己 `.size()` 出下溢风险）。
    size_t overwritten_size() const { return overwritten.size(); }
    // 本次**成功处理**的镜像内路径（含同落点的重复记录；统计口径看上面的 counts）。
    std::vector<std::string> written;          // 对拍/日志
    std::vector<ExtractTreeFailure> failures;  // 单项失败：只算这一项，不炸整批
    double elapsed_ms = 0.0;

    // 1.x `_sqfs_failure_note`：把"哪些项失败、为什么"压成一行（最多列 3 项）——
    // 只报"返回 0"用户无从判断。
    std::string failure_note() const;
};

// 把镜像里的条目导到 out_root。**空文件写出 0 字节**；目录项在 All 模式下建出来、
// 在 Listed 模式下按 1.x 跳过；`.part` 临时文件写完自校验后才 rename。
// ⚠ 只认 **SquashFS**（走 `list_entries`）。要"什么包都能解"请用下面的 `extract_package`。
ExtractTreeResult extract_tree(const std::string& image_path, const std::string& out_root,
                               const ExtractTreeOptions& options = ExtractTreeOptions{});

// ★ 2026-09-28（对齐 1.x `_unpack_checked`，"FS 方便程度"的一半）：**统一的"从包导出"入口** ——
// 按包类型分派：magic 命中 SquashFS ⇒ 走上面的 `extract_tree`；否则按 X-Ray DB 六格式
// （整文件读 + `auto_detect`，歧义时用 `db_fallback`）逐条 `extract_entry` 写出。
// 为什么合成一条：1.x 是两条路各写一份（SquashFS 调外部 exe、DB 走 raw bytes），
// 成功计数/越界判据/原子写/失败台账都在两份里各有一套 —— 合成后**同一套** 6.7 越界判据、
// 6.9 原子写、6.14 失败理由随返回值走。**这也是"解包只有一条路"那条静态锁的新指向。**
ExtractTreeResult extract_package(const std::string& image_path, const std::string& out_root,
                                  const ExtractTreeOptions& options = ExtractTreeOptions{},
                                  std::optional<DbFormat> db_fallback = std::nullopt);

// ── 解包落点（用户 2026-10-03 口径；★GUI 与 CLI **必须共用这一份**）────────────────────
//
// 用户口径原话：「如果就拖入 `E:\...\IWP\database\config` 的 `configs`，那么**解包出来是一个
// `configs` 文件夹**（和 `gamedata.db*`/`sq*` 解出 `gamedata` 一样），**不包含 `database` 和
// `config`**。」追问"包内路径本身就带一层 `configs/` 会不会变成 `configs/configs`"后定案：
// **「这个案例，就解出来一个 configs。」**
// ⇒ 规则：① 产物顶层＝**包名去扩展名**（1.x `_package_folder`）；② **磁盘上的父目录层级不进产物**；
//    ③ **包内路径已经自带这一层同名壳时不再叠一层**（否则真机会解出 `configs/configs/…`）。
//
// 为什么这条规则必须在**引擎**里：2026-10-04 用户拍了架构红线——
// **「GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。」**
// 此前它只写在 `FsPage::plan_unpack()` 里 ⇒ CLI 复现不了 ⇒ 真机产物布局只能在 GUI/合成夹具上验。
// 现在 shell 与 CLI 都调这里（自检锁：GUI 算出的落点集合 == 这里算的）。

// 1.x `_package_folder`：basename 去掉**最后一个**扩展名。
//   gamedata.db0 → gamedata；gamedata.sq_base → gamedata；a.b.db0 → a.b；patch_00.xdb → patch_00
std::string package_folder_name(const std::string& package_path);

// 包内路径是否**已经自带**这一层同名壳：**过半**条目的第一段等于 shell ⇒ true。
//   ★ 2026-10-04（判红锁①）：量词由"**全部**条目都带壳"改成"**过半**带壳"。真机 9 个 VESSEL 包
//     每个**恰好含一条**根级 `readme.md`(8148 B)、其余条目全在 `壳/…` 下 ⇒ "全部"量词把它们判成
//     "不带壳" ⇒ 解出 `ai/ai/alife/…`（违背用户口径「这个案例，就解出来一个 configs。」）。
//     `IWP\database\config\configs.db`（27/27 带壳，多一条 `a.txt` 就翻转）就是同族不稳的现场。
//   · 裸壳项（`p == shell`，那条"自己那一层"的目录条目）计为带壳；
//   · **不用**"存在裸壳目录项"当判据 —— `IWP\database\resource\meshes.db1` / `textures.db1` 是
//     纯带壳但 `bare_shell=0`，那样会回归。
//（空条目表 ⇒ false，与"不知道 ⇒ 照常规叠一层"一致；1.x 的空表兜底语义不变。）
bool paths_carry_shell(const std::vector<std::string>& entry_paths, const std::string& shell);

// 条目路径表**去重后**的条数（同一路径只算一条；`\` 与大小写按 Windows 口径归一）。
// ★ 为什么需要：`IWP\database\maps\*.db` 的 HEADER 表把**除根记录外的每条记录登记了两遍**
//   （同路径、同 offset、同 size ⇒ 同一份数据，已用不依赖工具的独立解码证实"重复在包内"）。
//   ⇒ 包内记录数是产物数的 2 倍 −1。**分工**：协议层（`--dump`）报**包内记录数**（忠实、不去重，
//   它有独立用途）；产物层（`--unpack-package` 的 `plan` 行 / UI 计数）报**产物口径**。
size_t unique_entry_count(const std::vector<std::string>& entry_paths);

// 落点目录＝`out_root`（自带壳时）或 `out_root/shell`。
// ★**绝不返回 `<root>/.`** —— 那种写法会被 `safe_out_path` 当越界拒掉（真机踩过）。
std::string unpack_target_dir(const std::string& out_root, const std::string& package_path,
                              const std::vector<std::string>& entry_paths);

// 一次"解包计划"里的一项：源包 → 落点。CLI 打它、shell 按它排 job、自检拿它对拍。
struct UnpackTarget {
    std::string image;   // 源包路径（UTF-8）
    std::string label;   // 源包文件名（日志/显示用）
    std::string target;  // 落点目录（已按上面的规则算好）
};

// 多源 ⇒ 落点清单（顺序与入参一致）。`sources` 每项＝(包路径, 该包的条目路径表)。
std::vector<UnpackTarget> plan_unpack_targets(
    const std::string& out_root,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& sources);

// ── ★★ 2026-10-05：**解包产物预览**（右栏）的条目表 —— 与上面那套落点规则**同一份真值** ──
//
// 用户口径（2026-10-03，原话）：「**这是一个解包的工具，而右边罗列的是包的内容，换句话说
// 就是解包产物的预览。**」⇒ 右栏每一条显示的路径，必须**恰好等于** `--unpack-package` /
// GUI「解包」真正会把那个文件写到的**落点相对路径**。
//
// 为什么必须在引擎里（架构红线）：用户 2026-10-04 原话「**GUI 的功能，CLI 肯定要全部拥有啊。
// 哪有 GUI 的细节功能比 CLI 还多的。**」—— 落点规则（`unpack_target_dir`）已经在引擎里，
// 但"预览怎么拼出那条路径"如果写在外壳里，就又变成"同一条规则两个实现"（CLI 复现不了）。
//
// 规则（与 `extract_package` 的落盘顺序逐字对应：`safe_out_path(target, entry.path)`）：
//   ① `unpack_target_dir(...)` 相对 `out_root` 那一段（＝包名壳；**不带壳时为空**）
//      就是**前缀**；② 前缀非空 ⇒ 条目路径前面拼 `前缀 + '/'`。
//   ★ 自带壳的包（`ai.db` 的条目已经是 `ai/…`）⇒ 前缀为空 ⇒ **条目路径原样**（不叠第二层）。
//   ★ **目录项也补**：否则右栏的目录树缺中间层（`xr/a/b.txt` 的 `xr`、`xr/a` 建不出来）。
//   ★ 签名里**没有输出根**（有意的）：`unpack_target_dir` 的返回相对根那一段只可能是
//     "**空**" 或 "**壳**"（`resolve_output_root` 已经把根归一过了），所以预览与"解包到哪"
//     无关 —— 也就不需要为了预览去回填/推算用户的"解包到"输入框（不留副作用）。
//
// 返回表的顺序：先是补出来的那批目录（祖先在前），再是 `entries` 原序。
std::vector<EntryInfo> preview_entries_in_layout(const std::string& package_path,
                                                 const std::vector<EntryInfo>& entries);

}  // namespace xrkit::engine::fs
