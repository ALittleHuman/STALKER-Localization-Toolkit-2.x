// xrkit::plugin —— **引擎调用门面的默认表**（插件改装 P7a，用户拍板的方案 3）。
//
// ★ 这份实现住在**引擎**层（不是外壳）：它只调用 `xrkit::engine::*`，**不依赖 Qt、也不依赖
//   外壳的任何服务**；而 CLI（`xrkit_cli.exe --py-plugins`）同样是插件宿主 —— 实现在外壳里时
//   实测 CLI 里的插件调 `font_size_slots()` 拿到的是 None，而"让 CLI 也装上门面"就得让 CLI
//   链接外壳 ⇒ 把 Qt 拖进 CLI（J5 那条"引擎不许 include Qt"的意义就没了）。
// ⇒ 默认表放引擎、外壳可 `set_engine_facade()` 覆盖/扩展。
//
// 本文件只做**转发**（门面签名 → 引擎函数），**不在这一层加业务逻辑**：阈值/默认值/文案一律
// 从引擎拿，别在这里抄第二份 —— 本仓已经因为"旁边再写一份差不多的"被门禁拦下过两次。
// 枚举一律**转成字符串**（"ok"/"japanese"/"missing"/"unknown"）：数字枚举一变序就静默错。

#include "xrkit/engine/plugin/engine_facade.h"

#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"
#include "xrkit/engine/text/batch.h"
#include "xrkit/engine/text/encodings.h"
#include "xrkit/engine/text/extract.h"

#include <string>
#include <vector>

namespace xrkit::plugin {
namespace {

namespace efont = xrkit::engine::font;
namespace etext = xrkit::engine::text;

const char* state_name(efont::CjkFontState state) {
    switch (state) {
        case efont::CjkFontState::Ok:
            return "ok";
        case efont::CjkFontState::Japanese:
            return "japanese";
        case efont::CjkFontState::Missing:
            return "missing";
        case efont::CjkFontState::Unknown:
            break;
    }
    return "unknown";
}

const EngineFacade kFontFacade = {
    /* font_check */
    [](const std::string& font_path) {
        FontCheckResult out;
        const efont::CjkFontCheck check = efont::classify_cjk_font(font_path);
        out.state = state_name(check.state);
        const char* mark = efont::cjk_state_mark(check.state);
        out.mark = mark == nullptr ? std::string() : std::string(mark);
        out.missing = check.missing;
        return out;
    },
    /* font_pack：与内置页**同一个**引擎实现（逐字节一致的前提） */
    [](const FontPackOptions& options) {
        efont::PackageOptions native;
        native.game = options.game;
        native.lang = options.lang;
        native.xml_dir = options.xml_dir;
        native.font_path = options.font_path;
        native.out_dir = options.out_dir;
        native.offset = options.offset;
        native.suffix = options.suffix;
        native.full_cyrillic = options.full_cyrillic;
        native.only_sizes = options.only_sizes;
        const efont::PackageResult result = efont::build_package(native);
        FontPackResult out;
        out.ok = result.ok;
        out.error = result.error;
        out.out_gamedata = result.out_gamedata;
        out.fonts_written = result.fonts_written;
        out.xml_written = result.xml_written;
        out.warnings = result.warnings;
        return out;
    },
    /* font_state_marks：四态文案**从引擎拿**（插件不许自己抄一份中文） */
    []() {
        FontStateMarks out;
        out.ok = efont::cjk_state_mark(efont::CjkFontState::Ok);
        out.japanese = efont::cjk_state_mark(efont::CjkFontState::Japanese);
        out.missing = efont::cjk_state_mark(efont::CjkFontState::Missing);
        out.unknown = efont::cjk_state_mark(efont::CjkFontState::Unknown);
        return out;
    },
    /* font_size_slots：字号槽位表（文件名与 ltx 用的是槽位名） */
    []() {
        std::vector<int> sizes;
        const int count = efont::slot_count();
        if (count > 0) sizes.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) sizes.push_back(efont::slot_size(i));
        return sizes;
    },

    /* ★★ P9a：convert 页那一组（源/目标编码表 + 批量转换 + 备份三件 + 统计 CSV） */
    /* text_encodings：两张表的**唯一一份**在 `engine/text/encodings.cpp` */
    []() {
        TextEncodings out;
        out.manual = etext::manual_encodings();
        out.target_only = etext::target_only_encodings();
        return out;
    },
    /* text_convert_batch：与内置页**同一个**引擎实现 */
    [](const TextBatchOptions& options, const TextHooks& hooks) {
        etext::BatchOptions native;
        native.source = options.source;
        native.output_dir = options.output_dir;
        native.extensions = options.extensions;
        native.recursive = options.recursive;
        native.backup = options.backup;
        native.exclude_patterns = options.exclude_patterns;
        native.source_encoding = options.source_encoding;
        native.target_encoding = options.target_encoding;
        native.backup_dirname = options.backup_dirname;
        native.backup_suffix = options.backup_suffix;
        etext::BatchHooks native_hooks;
        native_hooks.should_stop = hooks.should_stop;
        native_hooks.gate = hooks.gate;   // ★ P9b：暂停闸门（返回 false ⇒ 引擎停下）
        native_hooks.message = hooks.message;
        native_hooks.progress = hooks.progress;
        const etext::BatchOutcome result = etext::convert_batch(native, native_hooks);
        TextConvertOutcome out;
        out.depth = result.collect.depth;
        out.dirs = result.collect.dirs;
        out.files_seen = result.collect.files_seen;
        out.selected = result.collect.selected;
        out.skipped_ext = result.collect.skipped_ext;
        out.skipped_excluded = result.collect.skipped_excluded;
        out.skipped_backup = result.collect.skipped_backup;
        out.skipped_depth = result.collect.skipped_depth;
        out.excluded = result.collect.excluded;
        out.collect_errors = result.collect.errors;
        out.single_file = result.single_file;
        out.candidates = result.candidates;
        out.processed = result.processed;
        out.success = result.success;
        out.skipped = result.skipped;
        out.failed = result.failed;
        out.backed_up = result.backed_up;
        out.backup_existed = result.backup_existed;
        out.backup_failed = result.backup_failed;
        out.low_confidence = result.low_confidence;
        out.override_decl = result.override_decl;
        out.used_mismatch = result.used_mismatch;
        out.cancelled = result.cancelled;
        out.stop_reason = result.stop_reason;
        out.duration = result.duration;
        out.exit_code = result.exit_code;
        return out;
    },
    /* text_restore_backups */
    [](const TextBackupOptions& options, const TextHooks& hooks) {
        etext::BackupTaskOptions native;
        native.source_root = options.source_root;
        native.recursive = options.recursive;
        native.extensions = options.extensions;
        etext::BatchHooks native_hooks;
        native_hooks.should_stop = hooks.should_stop;
        native_hooks.gate = hooks.gate;   // ★ P9b：暂停闸门（返回 false ⇒ 引擎停下）
        native_hooks.message = hooks.message;
        native_hooks.progress = hooks.progress;
        const etext::BackupTaskResult result = etext::restore_backups(native, native_hooks);
        TextBackupOutcome out;
        out.dir_missing = result.dir_missing;
        out.depth = result.depth;
        out.total = result.total;
        out.ok = result.ok;
        out.failed = result.failed;
        out.cancelled = result.cancelled;
        out.stop_reason = result.stop_reason;
        out.errors = result.errors;
        for (const etext::BackupItemResult& item : result.items) {
            out.items.push_back({item.bak, item.target, item.ok, item.error});
        }
        return out;
    },
    /* text_clear_backups */
    [](const TextBackupOptions& options, const TextHooks& hooks) {
        etext::BackupTaskOptions native;
        native.source_root = options.source_root;
        native.recursive = options.recursive;
        native.extensions = options.extensions;
        etext::BatchHooks native_hooks;
        native_hooks.should_stop = hooks.should_stop;
        native_hooks.gate = hooks.gate;   // ★ P9b：暂停闸门（返回 false ⇒ 引擎停下）
        native_hooks.message = hooks.message;
        native_hooks.progress = hooks.progress;
        const etext::BackupTaskResult result = etext::clear_backups(native, native_hooks);
        TextBackupOutcome out;
        out.dir_missing = result.dir_missing;
        out.depth = result.depth;
        out.total = result.total;
        out.ok = result.ok;
        out.failed = result.failed;
        out.cancelled = result.cancelled;
        out.stop_reason = result.stop_reason;
        out.errors = result.errors;
        for (const etext::BackupItemResult& item : result.items) {
            out.items.push_back({item.bak, item.target, item.ok, item.error});
        }
        return out;
    },
    /* text_write_stats_csv：空字符串 = 成功；否则是失败原因（引擎给的原文） */
    [](const std::string& path, const TextStats& stats) {
        etext::ConvertStats native;
        native.generated_at = stats.generated_at;
        native.source = stats.source;
        native.single_file = stats.single_file;
        native.output_dir = stats.output_dir;
        native.source_enc = stats.source_enc;
        native.target_enc = stats.target_enc;
        native.candidates = stats.candidates;
        native.processed = stats.processed;
        native.success = stats.success;
        native.skipped = stats.skipped;
        native.failed = stats.failed;
        native.duration = stats.duration;
        std::string error;
        if (etext::write_stats_csv(path, native, &error)) return std::string();
        return error;
    },
    /* text_format_duration：唯一一份在引擎 `batch.cpp`（别在这儿抄一份格式化） */
    [](double seconds) { return etext::format_duration(seconds); },
    /* text_default_id_prefix：默认 ID 前缀（唯一一份在引擎 `extract.cpp`） */
    []() { return std::string(etext::kDefaultIdPrefix); },
    /* text_validate_extract：给用户看的那句话（判据只在引擎那一份） */
    [](const TextExtractRequest& request) {
        etext::ExtractRequest native;
        native.source_dir = request.source_dir;
        native.target_dir = request.target_dir;
        native.prefix = request.prefix;
        return etext::validate_request(native);
    },
    /* text_extract_kind：源目录必须是 gameplay / scripts 本身 */
    [](const std::string& dir) {
        TextExtractKindResult out;
        etext::ExtractKind kind = etext::ExtractKind::Gameplay;
        std::string error;
        if (!etext::kind_from_dir_name(dir, kind, error)) {
            out.ok = false;
            out.error = error;
            return out;
        }
        out.ok = true;
        out.kind = kind == etext::ExtractKind::Gameplay ? "gameplay" : "scripts";
        return out;
    },
    /* text_extract_dir：真提取（含文件边界的取消谓词） */
    [](const std::string& source_dir, const std::string& target_dir,
       const std::string& xml_out_dir, const TextExtractOptions& options) {
        etext::ExtractOptions native;
        native.prefix = options.prefix;
        native.id_prefix = options.id_prefix;
        native.clean = options.clean;
        native.should_stop = options.should_stop;
        const etext::ExtractStats stats =
            etext::extract_dir(source_dir, target_dir, xml_out_dir, native);
        TextExtractStats out;
        out.ok = stats.ok;
        out.error = stats.error;
        out.kind = stats.kind == etext::ExtractKind::Gameplay ? "gameplay" : "scripts";
        out.files = stats.files;
        out.strings = stats.strings;
        out.chars = stats.chars;
        out.skipped = stats.skipped;
        out.repl_skipped = stats.repl_skipped;
        out.write_failures = stats.write_failures;
        out.texts_xml_name = stats.texts_xml_name;
        out.elapsed_ms = stats.elapsed_ms;
        out.cancelled = stats.cancelled;
        return out;
    },
};

}  // namespace

// 引擎自带的默认表（惰性建出来；外壳可以 set_engine_facade() 覆盖它）
const EngineFacade* builtin_engine_facade() {
    static const EngineFacade instance = kFontFacade;
    return &instance;
}

}  // namespace xrkit::plugin
