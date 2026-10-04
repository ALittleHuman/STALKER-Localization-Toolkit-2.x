#pragma once
// xrkit::engine::text —— 编码转换页的**批处理层**（2026-10-06 D2 批）。
//
// 这一层是 1.x `apps/convert_app.py` 里"页面之外"的那半：
//   · `get_files()`            → `collect_files()`   （后缀过滤 / 递归 / 排除 / 跳备份区）
//   · `get_backup_path()`+`backup_file()` → `backup_file()`
//   · `run_restore()`          → `restore_backups()`
//   · `run_clear_backup()`     → `clear_backups()`
//   · `export_stats()`         → `write_stats_csv()`
//   · `is_excluded()`          → `is_excluded()` / `glob_match()`
//   · `_run_convert_body()`    → `convert_batch()`
//
// 为什么放引擎（而不是页面里）：① 这全是纯文件 IO，没有一行需要 Qt；② 放这里才能被
// CLI 自检**逐条驱动**（页面里那份只能靠离屏 GUI 自检，代价高得多）；③ 用户口径
// 「GUI 的功能，CLI 肯定要全部拥有啊」—— 批处理口径只有一份实现，两个前端都调它。
//
// ★ 递归遍历**不在这里新造**：转调 `engine::fs::collect_tree()`（仓库里已有的那份，
//   自带 `WalkReport` 台账）—— 用户口径「同类东西只维护一套组件」，不许出现第 5 份
//   `recursive_directory_iterator`。
//
// 零静默：所有"跳过/排除/读不到"都计数 + 逐条留痕；`BatchOutcome::exit_code` 在
// "有失败 / 有遍历错误"时非 0（部分失败也是失败）。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xrkit::engine::text {

// ── 排除模式（逐条照 1.x `ConvertApp.is_excluded`）───────────────────────
//
// 1.x 的三条判据，一条不少（顺序也照抄）：
//   ① `fnmatch.fnmatch(basename(path), pattern)`
//   ② `fnmatch.fnmatch(rel_path, pattern)`
//   ③ `pattern.replace('*', '') in rel_path`
//
// ★ `fnmatch.fnmatch` 在 Windows 上走 `os.path.normcase`（**小写化**）⇒ 1.x 的 ①②
//   实际是**大小写不敏感**的；③ 是 Python 的 `in`（**大小写敏感**）。这里逐条复刻，
//   不"统一成一套"——统一了就会与 1.x 的判定分叉。
// ★ 分隔符：1.x 的 `rel_path` 来自 `os.path.relpath`（Windows 上是 `\`），我们内部
//   一律 `/`。所以 ①②③ 都**同时**喂两种写法（`/` 与 `\`）—— 用户照 Windows 习惯写
//   `sub\*` 或照工具内部写 `sub/*` 都命中，不会出现"写了却静默不排除"。
bool glob_match(const std::string& pattern, const std::string& text);
bool is_excluded(const std::string& rel_path, const std::string& file_name,
                 const std::string& patterns);

// ── 候选文件收集 ────────────────────────────────────────────────────────
struct CollectOptions {
    std::vector<std::string> extensions;  // 小写带点（".xml"）；空 ⇒ 不按后缀过滤
    bool recursive = true;
    std::string exclude_patterns;
    std::string backup_dirname = "backup";
    std::string backup_suffix = ".bak";
};

// 台账（零静默）：**扫了多少层 / 扫到多少文件 / 各自为什么被刷掉**，全在这里。
struct CollectReport {
    int depth = 0;              // 递归到的层数（root 的直接子项 = 1）
    int dirs = 0;               // 扫到的目录数
    int files_seen = 0;         // 扫到的常规文件总数（含被刷掉的）
    int selected = 0;           // 选中（将处理）
    int skipped_ext = 0;        // 后缀不符
    int skipped_excluded = 0;   // 命中排除模式
    int skipped_backup = 0;     // 在 backup 区里 / 本身是 .bak
    int skipped_depth = 0;      // 非递归时被"只扫顶层"挡住
    std::vector<std::string> excluded;  // 被排除的（相对路径），逐条留痕
    std::vector<std::string> errors;    // 遍历/属性读取失败，逐条"路径 | 原因"
};

struct CollectedFile {
    std::string rel;   // 相对源根的路径（'/' 分隔）
    std::string full;  // 本机绝对路径（UTF-8）
};

// `root` 必须是目录（是文件时由调用方走单文件分支）。顺序 = 相对路径字节序（确定性）。
std::vector<CollectedFile> collect_files(const std::string& root, const CollectOptions& opt,
                                         CollectReport* report);

// ── 备份三件：`<源根>\backup\<相对路径><.bak>` ──────────────────────────
struct BackupOutcome {
    bool ok = false;
    bool existed = false;  // 已有备份 ⇒ 跳过（1.x 的 `[备份跳过]`）
    uint64_t bytes = 0;
    std::string path;      // 备份落点
    std::string error;
};

// `original` 相对 `source_root` 的备份落点；不以 source_root 为前缀 ⇒ 退回文件名（1.x 同）。
std::string backup_path_for(const std::string& original, const std::string& source_root,
                           const std::string& backup_dirname, const std::string& backup_suffix,
                           std::string* error);

// 备份一个文件（父目录自动建；已存在 ⇒ ok=true + existed=true，不覆盖）。
BackupOutcome backup_file(const std::string& original, const std::string& source_root,
                          const std::string& backup_dirname = "backup",
                          const std::string& backup_suffix = ".bak");

// ── 恢复 / 清空 ─────────────────────────────────────────────────────────
struct BackupTaskOptions {
    std::string source_root;
    bool recursive = true;
    // 与页面的"扩展名"框同源：只认 `<ext><backup_suffix>`（1.x `bak_exts`）。
    std::vector<std::string> extensions;
    std::string backup_dirname = "backup";
    std::string backup_suffix = ".bak";
};

struct BackupItemResult {
    std::string bak;       // 备份文件（本机路径）
    std::string target;    // 恢复目标（清空时为空）
    bool ok = false;
    std::string error;     // 失败原因（逐条，绝不含糊）
};

struct BackupTaskResult {
    bool dir_missing = false;  // 没有 backup 目录（1.x 会明确提示，不是静默 0）
    int depth = 0;
    int total = 0;
    int ok = 0;
    int failed = 0;
    bool cancelled = false;
    std::string stop_reason;
    std::vector<std::string> errors;  // 列出备份时的遍历错误
    std::vector<BackupItemResult> items;
};

using BatchCancel = std::function<bool()>;
using BatchMessage = std::function<void(const std::string&)>;

// 钩子（页面把自己的"暂停门 / 取消位 / 日志回投 / 进度"接进来）：
//   · `should_stop` —— **文件边界**轮询；true ⇒ 立刻停（停在完整产物上）。
//   · `gate`        —— 每一步之前的"让路"（页面的暂停门 / 退出）；返回 false ⇒ 收工。
//   · `message`     —— 逐条留痕（零静默）。
//   · `progress`    —— (已处理, 总数)；总数未知时 total=0（页面自己决定怎么画）。
struct BatchHooks {
    BatchCancel should_stop;
    std::function<bool()> gate;
    BatchMessage message;
    std::function<void(int, int)> progress;
};

// 恢复：逐条 copy2 回原位；**原文件不存在 ⇒ 该条失败并给理由**（1.x 同）。
BackupTaskResult restore_backups(const BackupTaskOptions& opt, const BatchHooks& hooks);
// 清空：逐条删除并**报数**（删了哪些、失败哪些）。
BackupTaskResult clear_backups(const BackupTaskOptions& opt, const BatchHooks& hooks);

// ── 导出统计 CSV（1.x `export_stats`；utf-8-sig）────────────────────────
struct ConvertStats {
    std::string generated_at;   // "YYYY-MM-DD HH:MM:SS"
    std::string source;         // 源目录 / 单个文件
    bool single_file = false;
    std::string output_dir;     // 空 ⇒ 原地转换
    std::string source_enc;
    std::string target_enc;
    uint64_t candidates = 0;    // 候选文件数
    uint64_t processed = 0;     // 已处理（含跳过与失败）
    uint64_t success = 0;
    uint64_t skipped = 0;
    uint64_t failed = 0;
    std::string duration;       // "H:MM:SS"（1.x 是 `str(timedelta).split('.')[0]`）
};

// Python `str(timedelta)` 的口径：`H:MM:SS`，跨天是 `N day, H:MM:SS`。
std::string format_duration(double seconds);
// 逐行照 1.x `export_stats`（标题行 + 12 行"字段,值"）；**utf-8-sig**（写 BOM）。
bool write_stats_csv(const std::string& path, const ConvertStats& stats, std::string* error);

// ── 批处理主流程（页面与 CLI 自检共用同一条入口）────────────────────────
struct BatchOptions {
    std::string source;      // 单个文件或目录
    std::string output_dir;  // 空 ⇒ 原地（此时才可能备份）
    std::vector<std::string> extensions;
    bool recursive = true;
    bool backup = true;      // 仅原地转换时生效（1.x：输出到新目录时该勾选项禁用）
    std::string exclude_patterns;
    std::string source_encoding = "auto";
    std::string target_encoding = "utf-8";
    std::string backup_dirname = "backup";
    std::string backup_suffix = ".bak";
};

struct BatchOutcome {
    CollectReport collect;
    bool single_file = false;
    int candidates = 0;
    int processed = 0;
    int success = 0;
    int skipped = 0;
    int failed = 0;
    int backed_up = 0;
    int backup_existed = 0;
    int backup_failed = 0;
    bool cancelled = false;
    std::string stop_reason;  // 取消了 ⇒ 说清停在哪（零静默）
    std::string duration;
    // 0 = 全部成功；1 = 有失败/有遍历错误（**部分失败也是失败**）；2 = 输入不可用
    int exit_code = 0;
};

// `hooks.should_stop` 在**每个文件之前**轮询（文件边界）；`hooks.message` 逐条留痕。
BatchOutcome convert_batch(const BatchOptions& opt, const BatchHooks& hooks);

}  // namespace xrkit::engine::text
