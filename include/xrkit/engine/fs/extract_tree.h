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
//   · 6.9 **原子写**：同目录 `.part` → 逐字节复核 → 同盘 rename；失败 unlink；
//   · 6.10 选中集**只有目录 / 空选择**时，1.x 会退化成"整镜像导出"并记为成功 ——
//     2.0 把"全部"与"空选择"分成两个显式状态，空选择直接拒绝并说明理由；
//   · 6.14 失败原因**随返回值走**（1.x 用模块级全局变量报错，并发任务互相覆盖）。

#include "xrkit/engine/fs/db.h"

#include <cstdint>
#include <optional>
#include <string>
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

// ── 整树导出 ─────────────────────────────────────────────────────────

enum class Selection {
    All,     // 整镜像（1.x 的 `files is None`）
    Listed,  // 只导出 only 里列出的项（1.x 的 `--cat` 逐项路径）
};

struct ExtractTreeOptions {
    Selection selection = Selection::All;
    std::vector<std::string> only;  // selection == Listed 时的镜像内路径
    bool atomic = true;             // 6.9：.part → 复核 → rename
};

struct ExtractTreeFailure {
    std::string path;
    std::string reason;
};

struct ExtractTreeResult {
    bool ok = false;       // 只有"整批根本没跑起来"才是 false（单项失败看 failures）
    std::string error;     // ok=false 的原因；**随返回值走**，没有全局变量（6.14）
    uint64_t files_written = 0;
    uint64_t dirs_created = 0;
    uint64_t bytes_written = 0;
    uint64_t skipped_dirs = 0;                 // Listed 模式下按 1.x 跳过的目录项
    std::vector<std::string> written;          // 实际写出的镜像内路径（对拍/日志）
    std::vector<ExtractTreeFailure> failures;  // 单项失败：只算这一项，不炸整批
    double elapsed_ms = 0.0;

    // 1.x `_sqfs_failure_note`：把"哪些项失败、为什么"压成一行（最多列 3 项）——
    // 只报"返回 0"用户无从判断。
    std::string failure_note() const;
};

// 把镜像里的条目导到 out_root。**空文件写出 0 字节**；目录项在 All 模式下建出来、
// 在 Listed 模式下按 1.x 跳过；`.part` 临时文件写完复核后才 rename。
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

}  // namespace xrkit::engine::fs
