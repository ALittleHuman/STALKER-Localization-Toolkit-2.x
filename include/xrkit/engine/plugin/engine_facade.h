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

#include "xrkit/engine/patch/patch.h"

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


// ── ★★ P10a：text 页（第三页迁移）── 文本提取/改写 ─────────────────────────
//   `kind` 一律 **字符串**（"gameplay" / "scripts"）：数字枚举一变序就静默错。

struct TextExtractRequest {
    std::string source_dir;   // 必须是 gameplay / scripts **文件夹本身**
    std::string target_dir;   // 必填
    std::string prefix;       // 文本 XML 文件名前缀
};

struct TextExtractKindResult {
    bool ok = false;
    std::string kind;         // "gameplay" / "scripts"
    std::string error;
};

struct TextExtractOptions {
    std::string prefix;
    std::string id_prefix;
    bool clean = false;
    // 取消谓词（引擎在**文件边界**轮询；返回 true ⇒ 停下，已写出的产物都是完整的）
    std::function<bool()> should_stop;
};

struct TextExtractStats {
    bool ok = false;
    std::string error;
    std::string kind;
    uint64_t files = 0;
    uint64_t strings = 0;
    uint64_t chars = 0;
    std::vector<std::string> skipped;        // 「相对路径：原因」
    std::vector<std::string> repl_skipped;   // 因保护而没替换的命中（零静默）
    uint64_t write_failures = 0;
    std::string texts_xml_name;
    double elapsed_ms = 0.0;
    bool cancelled = false;
};


// ── ★★ P11a：xml 页（第四页迁移）── XML 对比 ────────────────────────────────
//   明细与 CSV 都**由引擎渲染**（插件只负责摆到界面上）—— 免得两处口径分叉。

struct XmlColumnSpec {
    std::string key;       // "rel"/"status"/"detail" 是保留键
    std::string heading;
    int width = 0;
};

struct XmlFilterSpec {
    std::string label;
    std::vector<std::string> row_kinds;
    std::vector<std::string> differ_types;
};

struct XmlMode {
    std::string name;      // 显示名（下拉取值）
    std::string key;       // **内部键**（"stats" / "text"）—— 不许拿显示名当取值
    int order = 0;
    std::vector<XmlColumnSpec> columns;
    std::vector<XmlFilterSpec> filters;
    std::string plugin;
};

struct XmlModeLookup {
    bool found = false;
    XmlMode mode;
};

struct XmlCompareResult {
    std::string rel;
    std::string mode;
    bool consistent = false;
    bool only_one_side = false;
    std::string only_side;
    uint64_t lines_a = 0;
    uint64_t lines_b = 0;
    uint64_t ids_a = 0;
    uint64_t ids_b = 0;
    std::vector<std::string> only_a;
    std::vector<std::string> only_b;
    std::vector<std::vector<std::string>> count_diff;   // (id, A 次数, B 次数)
    std::vector<std::vector<std::string>> text_diff;    // (id, A 文本, B 文本)
    bool untrusted_a = false;
    bool untrusted_b = false;
    std::string warning;
    std::string encoding_a;
    std::string encoding_b;
    std::string error;
};

struct XmlTextRead {
    bool ok = false;
    std::string text;
    std::string used_encoding;
    std::string error;
    bool forced = false;
    bool used_mismatch = false;
};

struct XmlCsvReport {
    std::string mode_name;
    std::string folder_a;
    std::string folder_b;
    std::string name_a;
    std::string name_b;
    std::string enc_a = "auto";
    std::string enc_b = "auto";
    std::string filter_label;
    std::string generated_at;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    uint64_t consistent = 0;
    uint64_t inconsistent = 0;
    uint64_t only_a = 0;
    uint64_t only_b = 0;
    std::string encodings_used;
};

// ── ★★ P11b：xml 目录对比（结果表那一整块业务；实现只在引擎）────────────────
struct XmlDirDiffRow {
    XmlCompareResult result;
    std::string kind;      // "same" / "diff" / "only_a" / "only_b"
};

struct XmlDirDiffOptions {
    std::string dir_a;
    std::string dir_b;
    std::string enc_a = "auto";
    std::string enc_b = "auto";
    std::string mode_key = "stats";
    std::function<bool()> should_stop;   // 每个文件之前轮询
};

struct XmlDirDiffReport {
    std::vector<XmlDirDiffRow> rows;
    int total = 0;
    bool cancelled = false;
    // ★★ P17-10（02 F-05）：目录遍历出错理由（引擎 `DirDiffReport::errors` 透上来）——
    //   非空即"本次比较结论不可信"，插件页与 headless 都要把它报出去。
    std::vector<std::string> errors;
};


// ── ★★ P12a：video 页（第五页迁移）── 转码 ──────────────────────────────────
//   面板要显示的"视频参数"**全部**来自 `VideoTranscodePlan`（缩放/帧率/码率/声道…），
//   插件不许自己把阈值再判一遍 —— 那是"面板说 OK、argv 里却插了滤镜"那类分叉的根源。

struct VideoStreamInfo {
    std::string codec;
    int width = 0;
    int height = 0;
    double fps = 0.0;
    long long bitrate = 0;
};

struct AudioStreamInfo {
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    long long bitrate = 0;
};

struct VideoMediaInfo {
    bool parsed = false;
    bool has_video = false;
    bool has_audio = false;
    VideoStreamInfo video;
    AudioStreamInfo audio;
    double duration = 0.0;
    double size_mb = 0.0;
};

struct VideoPaths {
    bool found = false;
    bool has_probe = false;
    std::string ffmpeg;
    std::string ffprobe;
    bool ffmpeg_from_config = false;
    bool ffprobe_from_config = false;
};

struct VideoRequest {
    std::string ffmpeg = "ffmpeg";
    std::string input;
    std::string output;
    bool has_reference = false;
    std::string reference_path;   // 空 = 不判"输出撞参考"
    VideoMediaInfo reference;
    VideoMediaInfo source;
    std::vector<std::string> available_encoders;
};

struct VideoPlan {
    std::vector<std::string> argv;
    std::string video_encoder;
    bool encoder_warning = false;
    bool audio_kept_on_probe_failure = false;
    double duration = 0.0;
    bool scale_applied = false;
    bool fps_applied = false;
    long long video_bitrate = 0;
    long long audio_bitrate = 0;
    int audio_sample_rate = 0;
    int audio_channels = 0;
    bool audio_disabled = false;
};

struct VideoOutputCheck {
    std::string conflict;   // "none" / "same_as_source" / "same_as_reference" / "exists"
    std::string reason;     // 给用户看的那句话（**引擎给的原文**）
};

struct VideoProgress {
    bool matched = false;
    double percent = 0.0;
};


// ── ★★ P14-0a：fs 页（第六页迁移）**第一批** —— 格式表 / 格式谓词 / 路径解析 / 落点预览 ──
//   全是**纯函数**（不写盘、不起进程）：判据好写，风险低；解包/封包那些"重活"放下一批。

struct FsFormatInfo {
    std::string key;     // 下拉**取值**（内部键：auto / xdb / sqfs / …）
    std::string label;   // 用户可见名
    bool is_sqfs = false;
    bool is_auto = false;   // "auto" = 不给格式（自洽判定；歧义时显式拒绝）
};

struct FsDbLookup {
    bool is_db = false;      // 这个键是不是 DB 格式
    std::string db_key;      // 是 DB 时：DbFormat 的键（"xdb" / "2947ww" …）
    std::string db_name;     // 是 DB 时：用户可见名
};

struct FsEntry {
    std::string path;
    bool is_dir = false;
    uint64_t size = 0;
};

struct FsOutPath {
    bool ok = false;
    std::string reason;   // ok=false 时非空（中文，界面/日志直接用）
    std::string path;     // ok=true 时：落盘用的本机路径（UTF-8）
};

struct FsPackTarget {
    std::string path;
    bool needs_frontend_dialog = false;   // 需要前端弹「保存到」
};

struct FsUnpackTarget {
    std::string image;    // 源包路径
    std::string label;    // 源包文件名（显示/日志用）
    std::string target;   // 落点目录
};


// ── ★★ P14-0b：fs 面**第二批** —— 条目与合并树 ────────────────────────────────
//   `MergeTree` 是**有状态的 C++ 对象**（勾选态/冲突数活在它里面），而门面只跨"值"
//   ⇒ 仿照控件句柄：门面里存一张**句柄表**，插件拿 `int` 句柄操作，用完 `fs_tree_free`。

struct FsTreeSource {
    std::string source;          // 用户看到的包名/路径
    std::vector<FsEntry> entries;
};

struct FsTreeRow {               // `MergeTree::flatten()` 的一行
    std::string name;
    std::string path;            // 累计到本节点的镜像内路径（'/' 分隔）
    bool is_dir = false;
    bool checked = false;
    uint64_t size = 0;
    int source_index = -1;       // "引擎实际会读到的那一份"来自第几个源（-1 = 没来源）
};

struct FsSourceState {
    int files = 0;
    int checked_files = 0;
};

struct FsTreeLookup {
    bool found = false;
    bool is_dir = false;
    bool checked = false;
    uint64_t size = 0;
    int source_index = -1;
};


// ── ★★ P14-0c：fs 面**第三批** —— 读条目（含解密闸门）/ 收集 / 解包 ──────────────
//   ★ 口径：**闸门已焊在引擎读入口**（`db.cpp`/`overlay.cpp`/`squashfs.cpp` 都调
//     `plugin::gate_active_read`）⇒ 门面**不需要**传注册表，`active_registry()` 自己认。

struct FsGateResult {
    bool ok = false;
    std::string path;    // ok=true 时：**该读的那个路径**（可能是解密产物）
    std::string error;   // ok=false 时的原因（界面直接显示）
};

struct FsPackFile {
    std::string rel;          // 包内路径（'/' 分隔，相对 base）
    std::string source_path;  // 本机文件
};

struct FsPackCollect {
    std::vector<FsPackFile> items;
    int skipped_outside_base = 0;
    int skipped_package_files = 0;
};

struct FsExtractFailure {
    std::string path;
    std::string reason;
};

struct FsExtractResult {
    bool ok = false;
    std::string error;
    uint64_t files_written = 0;
    uint64_t dirs_created = 0;
    uint64_t bytes_written = 0;
    uint64_t duplicate_records = 0;
    uint64_t skipped_dirs = 0;
    std::vector<std::string> overwritten;   // **覆盖必须可见**（逐条）
    std::vector<std::string> written;
    std::vector<FsExtractFailure> failures;
    std::string failure_note;
    double elapsed_ms = 0.0;
};


// ── ★★ P14-0d：fs 面**第四批** —— 封包 ────────────────────────────────────────
//   两条：SquashFS（引擎自己落盘）与 X-Ray DB（引擎给它字节，**宿主原子写盘**）。
//   ★ DB 那条要带**名字改写台账**：cp1251 表示不了的字符会被逐码点顶替成 '?'
//     —— 用户口径"不需要支持中文名，但**必须看得见**改了哪些"（零静默红线）。

struct FsPackItem {
    std::string path;         // 镜像内相对路径（'/' 分隔，无前导斜杠）
    bool is_dir = false;
    std::string source_path;  // is_dir=false 时：本机文件（内容从它读）
};

struct FsPackResult {
    bool ok = false;
    std::string error;
    uint64_t bytes_used = 0;
    uint64_t file_size = 0;
    uint64_t inode_count = 0;
    uint64_t fragment_count = 0;
    uint64_t data_blocks = 0;
    uint64_t fragment_blocks = 0;
    uint64_t dedup_blocks = 0;
    uint64_t dedup_fragments = 0;
    double elapsed_ms = 0.0;
};

struct FsNameRewrite {
    std::string source;    // UTF-8 原名
    std::string written;   // 实际写进包里的名字
};

struct FsDbPackResult {
    bool ok = false;
    std::string error;
    uint64_t file_size = 0;
    int item_count = 0;
    std::vector<FsNameRewrite> rewrites;   // ★ 零静默：改了哪些、改成了什么
};


// ── ★★ P16-1a：**统一产物模型**的地基（规格第 50 行：字库+ltx 改写+xml 译文+二进制补丁
//   +**安装/还原+清单/哈希+台账**；第 16 行③★**可回退**："任何产物可还原，二进制补丁尤甚"）
struct PkgEntry {
    std::string rel;          // 目标根之下的相对路径（'/' 分隔）
    std::string source_path;  // 要装进去的本机文件（UTF-8）
    std::string sha256;       // 由**插件**算（核心层没有 sha256；门禁也用 hashlib，同一把尺子）
};

struct PkgInstallResult {
    bool ok = false;
    std::string error;
    std::string backup_dir;               // 备份落在哪（还原要用；空 = 没备份）
    std::vector<std::string> installed;   // 逐条台账
    std::vector<std::string> overwritten; // ★ 覆盖必须可见
    std::vector<std::string> failures;    // 逐条失败（"rel：原因"）
};

struct PkgRestoreResult {
    bool ok = false;
    std::string error;
    int restored = 0;                     // 从备份还原回来的
    int removed = 0;                      // 原本不存在 ⇒ 删掉的（装了新文件要能退回去）
    std::vector<std::string> missing;     // 备份里找不到的（逐条）
    std::vector<std::string> failures;    // 逐条失败（"rel：原因"）
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

    // ── ★★ P10a：text 页那一组 ──────────────────────────────────────────────
    std::function<std::string()> text_default_id_prefix;
    std::function<std::string(const TextExtractRequest&)> text_validate_extract;
    std::function<TextExtractKindResult(const std::string&)> text_extract_kind;
    // `target_dir` = **该类型的目标子目录的完整路径**（例如 <out>/gameplay），
    // `xml_out_dir` = 文本 XML 的落点（通常就是 <out> 根）—— 与引擎同名同义，
    // ★ 传相对路径会按**当前工作目录**解析（曾把仓库自己的 scripts\ 清了，血账）。
    std::function<TextExtractStats(const std::string& source_dir,
                                   const std::string& target_dir,
                                   const std::string& xml_out_dir,
                                   const TextExtractOptions&)> text_extract_dir;

    // ── ★★ P11a：xml 页那一组（第四页迁移）──────────────────────────────────
    std::function<std::vector<XmlMode>()> xml_modes;
    std::function<XmlModeLookup(const std::string&)> xml_find_mode;
    std::function<XmlTextRead(const std::string& path, const std::string& encoding)>
        text_read_bytes;
    std::function<XmlCompareResult(const std::string& rel, const std::string& text_a,
                                   const std::string& text_b, const std::string& enc_a,
                                   const std::string& enc_b)> xml_compare_stats;
    std::function<XmlCompareResult(const std::string& rel, const std::string& text_a,
                                   const std::string& text_b, const std::string& enc_a,
                                   const std::string& enc_b)> xml_compare_text;
    std::function<XmlCompareResult(const std::string& rel, const std::string& side)>
        xml_only_one_side;
    std::function<std::string(const XmlCompareResult&, const std::string& name_a,
                              const std::string& name_b)> xml_inline_detail;
    std::function<std::string(const XmlCompareResult&, const std::string& name_a,
                              const std::string& name_b)> xml_full_detail;
    std::function<std::string(const XmlCompareResult&, const std::string& name_a,
                              const std::string& name_b)> xml_full_text_detail;
    std::function<std::string(const XmlCsvReport&)> xml_render_csv;
    std::function<std::string(const std::string& path, const XmlCsvReport&)> xml_write_csv;
    // ★★ P11b：目录对比 + 筛选命中（口径与页面同一份引擎实现）
    std::function<XmlDirDiffReport(const XmlDirDiffOptions&)> xml_compare_dirs;
    std::function<bool(const XmlFilterSpec&, const std::string&, const XmlCompareResult&)>
        xml_row_matches;

    // ── ★★ P12a：video 页那一组（第五页迁移）─────────────────────────────────
    std::function<VideoPaths(const std::string& app_dir, const std::string& ffmpeg_path,
                             const std::string& ffprobe_path)> video_find_ffmpeg;
    std::function<int(const std::string& version_output)> video_major_version;
    std::function<bool(const std::string& version_output)> video_version_rejected;
    std::function<VideoPlan(const VideoRequest&)> video_plan_transcode;
    std::function<VideoOutputCheck(const VideoRequest&)> video_check_output_conflict;
    std::function<VideoMediaInfo(const std::string& json)> video_parse_ffprobe;
    std::function<std::vector<std::string>(const std::string& output)> video_parse_encoders;
    std::function<VideoProgress(const std::string& line, double duration_sec)>
        video_progress_from_line;

    // ── ★★ P14-0a：fs 面第一批（第六页迁移）────────────────────────────────
    std::function<std::vector<FsFormatInfo>()> fs_formats;
    std::function<FsDbLookup(const std::string& key)> fs_format_to_db;
    std::function<bool(const std::string& key)> fs_format_is_sqfs;
    std::function<bool(const std::string& key)> fs_format_is_auto;
    std::function<bool(const std::string& path)> fs_is_package_name;
    std::function<std::string(const std::string& key)> fs_pack_ext_for;
    std::function<std::string(const std::string& package_path)> fs_package_folder_name;
    std::function<std::string(const std::string& front_source_path, const std::string& current_dir)>
        fs_resolve_output_root;
    std::function<std::string(const std::string& out_root, const std::string& package_path,
                              const std::vector<std::string>& entry_paths)>
        fs_unpack_target_dir;
    std::function<FsPackTarget(const std::string& typed, const std::string& output_basename,
                               const std::string& format_key)>
        fs_resolve_pack_target;
    std::function<FsOutPath(const std::string& out_dir, const std::string& rel)> fs_safe_out_path;
    std::function<std::vector<FsUnpackTarget>(
        const std::string& out_root,
        const std::vector<std::pair<std::string, std::vector<std::string>>>& sources)>
        fs_plan_unpack_targets;
    std::function<std::vector<FsEntry>(const std::string& package_path,
                                       const std::vector<FsEntry>& entries)>
        fs_preview_layout;

    // ── ★★ P14-0b：fs 面第二批（合并树；句柄式）────────────────────────────
    std::function<std::vector<FsEntry>(const std::string& root, std::string& error)>
        fs_loose_entries;
    std::function<std::vector<size_t>(const std::vector<std::string>& paths)>
        fs_sort_sources_by_basename;
    std::function<int(const std::vector<FsTreeSource>& sources)> fs_tree_build;
    std::function<void(int handle)> fs_tree_free;
    std::function<std::vector<std::string>(int handle)> fs_tree_sources;
    std::function<std::vector<FsTreeRow>(int handle)> fs_tree_flatten;
    std::function<void(int handle, bool checked)> fs_tree_toggle_all;
    std::function<void(int handle, const std::string& path, bool checked)> fs_tree_toggle_dir;
    std::function<int(int handle, int source_index, bool checked)> fs_tree_toggle_source;
    std::function<FsSourceState(int handle, int source_index)> fs_tree_source_state;
    std::function<std::vector<std::string>(int handle)> fs_tree_checked_paths;
    std::function<int(int handle, const std::vector<std::string>& paths)>
        fs_tree_apply_checked_paths;
    std::function<int(int handle)> fs_tree_conflict_count;
    std::function<int(int handle)> fs_tree_checked_file_count;
    std::function<FsTreeLookup(int handle, const std::string& path)> fs_tree_find;

    // ── ★★ P14-0c：fs 面第三批（读条目/闸门/收集/解包）──────────────────────
    std::function<FsGateResult(const std::string& path)> fs_gate_path;
    std::function<std::string()> fs_decrypt_output_dir;
    std::function<std::vector<FsEntry>(const std::string& path, const std::string& db_fallback_key,
                                       std::string& error)>
        fs_package_entries;
    std::function<FsPackCollect(const std::string& base, const std::vector<std::string>& roots)>
        fs_collect_pack_files;
    std::function<FsExtractResult(const std::string& image_path, const std::string& out_root,
                                  const std::string& selection, const std::vector<std::string>& only,
                                  const std::string& db_fallback_key)>
        fs_extract_package;

    // ── ★★ P14-0d：fs 面第四批（封包）──────────────────────────────────────
    std::function<FsPackResult(const std::string& out_path, const std::vector<FsPackItem>& items)>
        fs_pack_squashfs;
    std::function<FsDbPackResult(const std::string& out_path, const std::vector<FsPackItem>& items,
                                 const std::string& format_key)>
        fs_pack_db;

    // ── ★★ P16-1a：统一产物模型（清单 / 安装 / 还原）──────────────────────
    std::function<bool(const std::string& path, const std::vector<PkgEntry>& entries,
                       std::string& error)>
        pkg_write_manifest;
    std::function<PkgInstallResult(const std::string& target_root,
                                   const std::vector<PkgEntry>& entries,
                                   const std::string& backup_root)>
        pkg_install;
    std::function<PkgRestoreResult(const std::string& target_root,
                                   const std::string& backup_root,
                                   const std::vector<std::string>& rels)>
        pkg_restore;

    // ── ★★ P16-1c：**二进制补丁**（判定半 / 写盘半）────────────────────────────
    //   口径（用户 2026-10-09 过目的设计笔记 §二，方案 (b)）：
    //     · 引擎＝**通用机器**（PE 布局守卫 / 逐项判态 / 指纹状态机 / 守卫链 / 原子写）；
    //     · **站点表由调用方作为数据传入**（`engine::patch::PatchSite`，二进制字段是 hex 文本）
    //       —— 表是"单一构建的特化数据"，按本仓红线不焊进内核；加一处不用重编引擎。
    //   ★ 判定纪律：结论只来自逐项字节内容，**绝不看"差了多少字节"**
    //     （`reference_diff_bytes` 只是报告参考值）。
    std::function<engine::patch::PatchIdentifyResult(const std::string& path,
                                                     const std::vector<engine::patch::PatchSite>&
                                                         sites)>
        patch_identify;
    std::function<engine::patch::PatchApplyResult(const std::string& src, const std::string& out,
                                                  const std::vector<engine::patch::PatchSite>& sites,
                                                  bool force)>
        patch_apply;
};

// 外壳注入实现（`src/shell/engine_facade_impl.cpp`）。传 nullptr = 撤销。
void set_engine_facade(const EngineFacade* facade);
const EngineFacade* engine_facade();

// 不可用时 `reason` 里是**为什么**（零静默：插件作者能立刻看出为什么调用没生效）
bool engine_facade_available(std::string* reason = nullptr);
bool engine_facade_has(const std::string& name);
std::vector<std::string> engine_facade_method_names();

}  // namespace xrkit::plugin
