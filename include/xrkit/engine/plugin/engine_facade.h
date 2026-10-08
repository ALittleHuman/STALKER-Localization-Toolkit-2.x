#pragma once
// xrkit::plugin —— **引擎调用门面**（插件改装的"引擎面"；用户 2026-10-07 拍板走**方案 3**）。
//
// 为什么是这一套（用户问"哪个方案最终插件能力最强"，答完他选了 3）：
//   · 方案 1（`api.engine.*` 把引擎函数直绑）与方案 3 **运行时完全一样**（进程内直调、字节
//     与内置页一致），差别只在**治理**：方案 1 一旦开了口，"顺手多暴露一个内部函数"是免费的
//     ⇒ 必然长成影子 API、且没法版本化；
//   · 方案 3 把同一件事写成**声明式表**（本文件）+ **能力表判据**（多一个＝泄漏、少一个＝削能力）
//     ⇒ 加能力是**有意为之的一次提交**，宿主重构内部时插件一行不用改；
//   · 方案 2（`xrkit_cli.exe` 子进程）达不到同样能力：富结构过不去、编排逻辑要重写一份，
//     而且**支持不了"插件给引擎加扩展点"**（规格 §四.2 要的正是它）⇒ 只留作诊断/兜底。
//
// 三条口径：
//   ① 门面由**外壳**注入（`set_engine_facade`）：引擎层（Qt-free）只持指针并转发；
//   ② ★**表外无门**：Python 侧能看到的只有本文件里的方法（判据＝`dir(xrkit.Engine)` 与预期表对账）；
//   ③ **值类型是 Python 友好的**（dict / list / str / int），**不把 C++ 结构体、指针、
//      生命周期漏给插件** —— 那是方案 1 的病根。
//
// 本批（P7a）只开**样板页 fontpack 真正用到的那几件**（反向定义，不是猜）：
//   `font_check` / `font_pack` / `font_state_marks` / `font_size_slots`。
// 下一批随样板页迁移补齐（页面侧还要：偏好读写、系统字体目录、目录选择 —— 那些属**宿主服务**，
// 进 UI/宿主门面，不进这里）。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::plugin {

// ── 值类型（Python 侧对应 dict / list）──────────────────────────────────────
struct FontCheckResult {
    std::string state;                    // "ok" / "japanese" / "missing" / "unknown"
    std::string mark;                     // 引擎给的短标记（"✓ 中文" / "⚠ 日语字体…" 等）
    std::vector<std::string> missing;     // 画不出墨迹的字（UTF-8 单字，按样本顺序）
};

struct FontStateMarks {
    std::string ok;
    std::string japanese;
    std::string missing;
    std::string unknown;
};

struct FontPackOptions {
    std::string game = "SoC";             // SoC / ClS / CoP
    std::string lang = "chs";
    std::string xml_dir;
    std::string font_path;
    std::string out_dir;
    int offset = 0;
    std::string suffix;
    bool full_cyrillic = true;
    std::vector<int> only_sizes;          // 空 = 全部槽位
};

struct FontPackResult {
    bool ok = false;
    std::string error;
    std::string out_gamedata;
    int fonts_written = 0;
    int xml_written = 0;
    std::vector<std::string> warnings;
};


// ── ★★ P9a：convert 页（第二页迁移）用到的值类型 ─────────────────────────────
//   口径同上面几条：**门面自己的值类型**（Python 友好），实现在引擎层转成引擎结构；
//   门面**不写业务逻辑**（阈值/默认值/文案一律从引擎拿）。

struct TextEncodings {
    std::vector<std::string> manual;        // 源/目标共用那张表
    std::vector<std::string> target_only;   // 只在目标侧多出来的（ascii）
};

struct TextBatchOptions {
    std::string source;                     // 单个文件或目录
    std::string output_dir;                 // 空 ⇒ 原地（此时才可能备份）
    std::vector<std::string> extensions;
    bool recursive = true;
    bool backup = true;                     // 仅原地转换时生效
    std::string exclude_patterns;
    std::string source_encoding = "auto";
    std::string target_encoding = "utf-8";
    std::string backup_dirname = "backup";
    std::string backup_suffix = ".bak";
};

struct TextBackupOptions {
    std::string source_root;
    bool recursive = true;
    std::vector<std::string> extensions;    // 只认 `<ext><backup_suffix>`
};

// hooks：取消 / **暂停闸门** / 消息 / 进度 —— 门面只负责转交（引擎在**工作线程**里叫它们）
//   `gate`：返回 false ⇒ 引擎**停下**（页面用它实现「暂停/继续」：暂停时在里面等，恢复后返回 true）。
struct TextHooks {
    std::function<bool()> should_stop;
    std::function<bool()> gate;
    std::function<void(const std::string&)> message;
    std::function<void(int, int)> progress;
};

struct TextConvertOutcome {
    // 扫描台账（**零静默**：扫了多少 / 各自为什么被刷掉）
    int depth = 0;
    int dirs = 0;
    int files_seen = 0;
    int selected = 0;
    int skipped_ext = 0;
    int skipped_excluded = 0;
    int skipped_backup = 0;
    int skipped_depth = 0;
    std::vector<std::string> excluded;
    std::vector<std::string> collect_errors;
    // 逐条计数
    bool single_file = false;
    int candidates = 0;
    int processed = 0;
    int success = 0;
    int skipped = 0;
    int failed = 0;
    int backed_up = 0;
    int backup_existed = 0;
    int backup_failed = 0;
    int low_confidence = 0;
    int override_decl = 0;
    int used_mismatch = 0;
    bool cancelled = false;
    std::string stop_reason;
    std::string duration;
    int exit_code = 0;
};

struct TextBackupItem {
    std::string bak;
    std::string target;
    bool ok = false;
    std::string error;
};

struct TextBackupOutcome {
    bool dir_missing = false;
    int depth = 0;
    int total = 0;
    int ok = 0;
    int failed = 0;
    bool cancelled = false;
    std::string stop_reason;
    std::vector<std::string> errors;
    std::vector<TextBackupItem> items;
};

struct TextStats {
    std::string generated_at;
    std::string source;
    bool single_file = false;
    std::string output_dir;
    std::string source_enc;
    std::string target_enc;
    uint64_t candidates = 0;
    uint64_t processed = 0;
    uint64_t success = 0;
    uint64_t skipped = 0;
    uint64_t failed = 0;
    std::string duration;
};

struct EngineFacade {
    std::function<FontCheckResult(const std::string& font_path)> font_check;
    std::function<FontPackResult(const FontPackOptions& options)> font_pack;
    std::function<FontStateMarks()> font_state_marks;
    std::function<std::vector<int>()> font_size_slots;

    // ── ★★ P9a：convert 页那一组（全部**转发**引擎唯一实现）────────────────────
    std::function<TextEncodings()> text_encodings;
    std::function<TextConvertOutcome(const TextBatchOptions&, const TextHooks&)> text_convert_batch;
    std::function<TextBackupOutcome(const TextBackupOptions&, const TextHooks&)>
        text_restore_backups;
    std::function<TextBackupOutcome(const TextBackupOptions&, const TextHooks&)> text_clear_backups;
    // 导出统计 CSV：返回**空字符串 = 成功**；否则是失败原因（绝不静默）
    std::function<std::string(const std::string& path, const TextStats&)> text_write_stats_csv;
    // 秒数 → 1.x 那种 `H:MM:SS`（唯一一份在引擎 `batch.cpp`）
    std::function<std::string(double seconds)> text_format_duration;
};

// 外壳注入实现（`src/shell/engine_facade_impl.cpp`）。传 nullptr = 撤销。
void set_engine_facade(const EngineFacade* facade);
const EngineFacade* engine_facade();

// 不可用时 `reason` 里是**为什么**（零静默：插件作者能立刻看出为什么调用没生效）
bool engine_facade_available(std::string* reason = nullptr);
bool engine_facade_has(const std::string& name);
std::vector<std::string> engine_facade_method_names();

}  // namespace xrkit::plugin
