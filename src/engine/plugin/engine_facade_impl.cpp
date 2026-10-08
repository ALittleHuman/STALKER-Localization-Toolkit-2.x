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
#include "xrkit/engine/text/sniff.h"
#include "xrkit/engine/xml/compare.h"
#include "xrkit/engine/xml/dirdiff.h"
#include "xrkit/engine/video/ffmpeg.h"
#include "xrkit/engine/video/transcode.h"

#include <array>
#include <string>
#include <vector>

namespace xrkit::plugin {
namespace {

namespace efont = xrkit::engine::font;
namespace etext = xrkit::engine::text;

namespace exml = xrkit::engine::xml;
namespace evideo = xrkit::engine::video;

XmlCompareResult to_facade(const exml::XmlCompareResult& native);
exml::XmlCompareResult from_facade(const XmlCompareResult& facade);
exml::XmlCsvReport to_native(const XmlCsvReport& facade);

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


XmlCompareResult to_facade(const exml::XmlCompareResult& native) {
    XmlCompareResult out;
    out.rel = native.rel;
    out.mode = native.mode;
    out.consistent = native.consistent;
    out.only_one_side = native.only_one_side;
    out.only_side = native.only_side;
    out.lines_a = native.lines_a;
    out.lines_b = native.lines_b;
    out.ids_a = native.ids_a;
    out.ids_b = native.ids_b;
    out.only_a = native.only_a;
    out.only_b = native.only_b;
    for (const auto& row : native.count_diff) {
        out.count_diff.push_back({row[0], row[1], row[2]});
    }
    for (const auto& row : native.text_diff) {
        out.text_diff.push_back({row[0], row[1], row[2]});
    }
    out.untrusted_a = native.untrusted_a;
    out.untrusted_b = native.untrusted_b;
    out.warning = native.warning;
    out.encoding_a = native.encoding_a;
    out.encoding_b = native.encoding_b;
    out.error = native.error;
    // ★ 故意**不转** `id_text_a/id_text_b`：那是"明细里要看原文"的内部表，
    //   而明细文本由引擎的 inline/full_detail 渲染 ⇒ 插件不需要它
    //   （传过去只会诱使插件自己拼一份明细，两处口径就会分叉）。
    return out;
}

exml::XmlCompareResult from_facade(const XmlCompareResult& facade) {
    exml::XmlCompareResult native;
    native.rel = facade.rel;
    native.mode = facade.mode;
    native.consistent = facade.consistent;
    native.only_one_side = facade.only_one_side;
    native.only_side = facade.only_side;
    native.lines_a = facade.lines_a;
    native.lines_b = facade.lines_b;
    native.ids_a = facade.ids_a;
    native.ids_b = facade.ids_b;
    native.only_a = facade.only_a;
    native.only_b = facade.only_b;
    for (const auto& row : facade.count_diff) {
        native.count_diff.push_back({row.size() > 0 ? row[0] : std::string(),
                                     row.size() > 1 ? row[1] : std::string(),
                                     row.size() > 2 ? row[2] : std::string()});
    }
    for (const auto& row : facade.text_diff) {
        native.text_diff.push_back({row.size() > 0 ? row[0] : std::string(),
                                    row.size() > 1 ? row[1] : std::string(),
                                    row.size() > 2 ? row[2] : std::string()});
    }
    native.untrusted_a = facade.untrusted_a;
    native.untrusted_b = facade.untrusted_b;
    native.warning = facade.warning;
    native.encoding_a = facade.encoding_a;
    native.encoding_b = facade.encoding_b;
    native.error = facade.error;
    return native;
}

exml::XmlCsvReport to_native(const XmlCsvReport& facade) {
    exml::XmlCsvReport native;
    native.mode_name = facade.mode_name;
    native.folder_a = facade.folder_a;
    native.folder_b = facade.folder_b;
    native.name_a = facade.name_a;
    native.name_b = facade.name_b;
    native.enc_a = facade.enc_a;
    native.enc_b = facade.enc_b;
    native.filter_label = facade.filter_label;
    native.generated_at = facade.generated_at;
    native.columns = facade.columns;
    native.rows = facade.rows;
    native.consistent = facade.consistent;
    native.inconsistent = facade.inconsistent;
    native.only_a = facade.only_a;
    native.only_b = facade.only_b;
    native.encodings_used = facade.encodings_used;
    return native;
}


VideoMediaInfo video_media_to_facade(const evideo::MediaInfo& native) {
    VideoMediaInfo out;
    out.parsed = native.parsed;
    out.has_video = native.has_video;
    out.has_audio = native.has_audio;
    out.video.codec = native.video.codec;
    out.video.width = native.video.width;
    out.video.height = native.video.height;
    out.video.fps = native.video.fps;
    out.video.bitrate = native.video.bitrate;
    out.audio.codec = native.audio.codec;
    out.audio.sample_rate = native.audio.sample_rate;
    out.audio.channels = native.audio.channels;
    out.audio.bitrate = native.audio.bitrate;
    out.duration = native.duration;
    out.size_mb = native.size_mb;
    return out;
}

evideo::MediaInfo video_media_from_facade(const VideoMediaInfo& facade) {
    evideo::MediaInfo native;
    native.parsed = facade.parsed;
    native.has_video = facade.has_video;
    native.has_audio = facade.has_audio;
    native.video.codec = facade.video.codec;
    native.video.width = facade.video.width;
    native.video.height = facade.video.height;
    native.video.fps = facade.video.fps;
    native.video.bitrate = facade.video.bitrate;
    native.audio.codec = facade.audio.codec;
    native.audio.sample_rate = facade.audio.sample_rate;
    native.audio.channels = facade.audio.channels;
    native.audio.bitrate = facade.audio.bitrate;
    native.duration = facade.duration;
    native.size_mb = facade.size_mb;
    return native;
}

evideo::TranscodeRequest video_request_from_facade(const VideoRequest& facade) {
    evideo::TranscodeRequest native;
    native.ffmpeg = facade.ffmpeg;
    native.input = facade.input;
    native.output = facade.output;
    native.has_reference = facade.has_reference;
    native.reference_path = facade.reference_path;
    native.reference = video_media_from_facade(facade.reference);
    native.source = video_media_from_facade(facade.source);
    native.available_encoders = facade.available_encoders;
    return native;
}

const char* video_conflict_code(evideo::OutputConflict conflict) {
    switch (conflict) {
        case evideo::OutputConflict::SameAsSource:
            return "same_as_source";
        case evideo::OutputConflict::SameAsReference:
            return "same_as_reference";
        case evideo::OutputConflict::Exists:
            return "exists";
        case evideo::OutputConflict::None:
            break;
    }
    return "none";
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
    /* ★★ P11a：xml 页那一组（模式表 / 两套对比 / 引擎渲染的明细 / CSV） */
    /* xml_modes：内置模式表（下拉与内部键的唯一来源） */
    []() {
        std::vector<XmlMode> out;
        for (const exml::XmlModeSpec& spec : exml::builtin_modes()) {
            XmlMode mode;
            mode.name = spec.name;
            mode.key = spec.key;
            mode.order = spec.order;
            mode.plugin = spec.plugin;
            for (const exml::XmlColumn& column : spec.columns) {
                mode.columns.push_back({column.key, column.heading, column.width});
            }
            for (const exml::XmlFilter& filter : spec.filters) {
                mode.filters.push_back({filter.label, filter.row_kinds, filter.differ_types});
            }
            out.push_back(std::move(mode));
        }
        return out;
    },
    /* xml_find_mode：按**内部键或显示名**找（找不到 ⇒ found=false，调用方自己退默认） */
    [](const std::string& key_or_name) {
        XmlModeLookup out;
        const exml::XmlModeSpec* spec = exml::find_mode(key_or_name);
        if (spec == nullptr) return out;
        out.found = true;
        out.mode.name = spec->name;
        out.mode.key = spec->key;
        out.mode.order = spec->order;
        out.mode.plugin = spec->plugin;
        for (const exml::XmlColumn& column : spec->columns) {
            out.mode.columns.push_back({column.key, column.heading, column.width});
        }
        for (const exml::XmlFilter& filter : spec->filters) {
            out.mode.filters.push_back({filter.label, filter.row_kinds, filter.differ_types});
        }
        return out;
    },
    /* text_read_bytes：按编码读文本（六步判定链；`auto` = 不手动指定） */
    [](const std::string& path, const std::string& encoding) {
        const etext::DecodedBytes decoded = etext::read_text_bytes(path, encoding);
        XmlTextRead out;
        out.ok = decoded.ok;
        out.text = decoded.text;
        out.used_encoding = decoded.used_encoding;
        out.error = decoded.error;
        out.forced = decoded.forced;
        out.used_mismatch = decoded.used_mismatch;
        return out;
    },
    /* xml_compare_stats：按"行数 / ID 统计"比（引擎唯一实现） */
    [](const std::string& rel, const std::string& text_a, const std::string& text_b,
       const std::string& enc_a, const std::string& enc_b) {
        return to_facade(exml::compare_stats(rel, text_a, text_b, enc_a, enc_b));
    },
    /* xml_compare_text：按"ID 文本"比（引擎唯一实现） */
    [](const std::string& rel, const std::string& text_a, const std::string& text_b,
       const std::string& enc_a, const std::string& enc_b) {
        return to_facade(exml::compare_text(rel, text_a, text_b, enc_a, enc_b));
    },
    /* xml_only_one_side：只有一侧有该文件 */
    [](const std::string& rel, const std::string& side) {
        return to_facade(exml::only_one_side(rel, side));
    },
    /* xml_inline_detail：表格里那一格（引擎渲染） */
    [](const XmlCompareResult& result, const std::string& name_a, const std::string& name_b) {
        return exml::inline_detail(from_facade(result), name_a, name_b);
    },
    /* xml_full_detail：状态栏/对话里的完整明细（引擎渲染） */
    [](const XmlCompareResult& result, const std::string& name_a, const std::string& name_b) {
        return exml::full_detail(from_facade(result), name_a, name_b);
    },
    /* xml_full_text_detail：文本模式的完整明细（引擎渲染） */
    [](const XmlCompareResult& result, const std::string& name_a, const std::string& name_b) {
        return exml::full_text_detail(from_facade(result), name_a, name_b);
    },
    /* xml_render_csv：CSV 文本（BOM + \r\n，引擎唯一实现） */
    [](const XmlCsvReport& report) { return exml::render_csv(to_native(report)); },
    /* xml_write_csv：渲染 + 原子写盘（空串 = 成功；否则失败原因） */
    [](const std::string& path, const XmlCsvReport& report) {
        std::string error;
        if (exml::write_csv_file(path, to_native(report), error)) return std::string();
        return error;
    },
    /* xml_compare_dirs：目录对比（遍历/配对/逐文件比/给 kind 全在引擎那一份实现里） */
    [](const XmlDirDiffOptions& options) {
        exml::DirDiffOptions native;
        native.dir_a = options.dir_a;
        native.dir_b = options.dir_b;
        native.enc_a = options.enc_a;
        native.enc_b = options.enc_b;
        native.mode_key = options.mode_key;
        native.should_stop = options.should_stop;
        const exml::DirDiffReport report = exml::compare_dirs(native);
        XmlDirDiffReport out;
        out.total = report.total;
        out.cancelled = report.cancelled;
        for (const exml::DirDiffRow& row : report.rows) {
            out.rows.push_back({to_facade(row.result), row.kind});
        }
        return out;
    },
    /* xml_row_matches：筛选命中（`row_kinds` 空=全过；`differ_types` 要同时命中） */
    [](const XmlFilterSpec& filter, const std::string& kind, const XmlCompareResult& result) {
        exml::XmlFilter native;
        native.label = filter.label;
        native.row_kinds = filter.row_kinds;
        native.differ_types = filter.differ_types;
        return exml::row_matches_filter(native, kind, from_facade(result));
    },
    /* video_find_ffmpeg：只认两处（user.ltx 配置 + 程序目录），**不搜 PATH** */
    [](const std::string& app_dir, const std::string& ffmpeg_path,
       const std::string& ffprobe_path) {
        evideo::FfmpegConfig config;
        config.path = ffmpeg_path;
        config.probe = ffprobe_path;
        const evideo::FfmpegPaths native = evideo::find_ffmpeg(app_dir, config);
        VideoPaths out;
        out.found = native.found;
        out.has_probe = native.has_probe;
        out.ffmpeg = native.ffmpeg;
        out.ffprobe = native.ffprobe;
        out.ffmpeg_from_config = native.ffmpeg_from_config;
        out.ffprobe_from_config = native.ffprobe_from_config;
        return out;
    },
    /* video_major_version：`ffmpeg -version` 输出里的主版本号 */
    [](const std::string& version_output) { return evideo::major_version(version_output); },
    /* video_version_rejected：太老/太新都要拒绝（口径在引擎） */
    [](const std::string& version_output) { return evideo::version_rejected(version_output); },
    /* video_plan_transcode：构建 argv **与面板要显示的每个值** */
    [](const VideoRequest& request) {
        const evideo::TranscodePlan native =
            evideo::plan_transcode(video_request_from_facade(request));
        VideoPlan out;
        out.argv = native.argv;
        out.video_encoder = native.video_encoder;
        out.encoder_warning = native.encoder_warning;
        out.audio_kept_on_probe_failure = native.audio_kept_on_probe_failure;
        out.duration = native.duration;
        out.scale_applied = native.scale_applied;
        out.fps_applied = native.fps_applied;
        out.video_bitrate = native.video_bitrate;
        out.audio_bitrate = native.audio_bitrate;
        out.audio_sample_rate = native.audio_sample_rate;
        out.audio_channels = native.audio_channels;
        out.audio_disabled = native.audio_disabled;
        return out;
    },
    /* video_check_output_conflict：判"输出撞谁"并**同一次给出理由**（页面本来就是这样成对用的） */
    [](const VideoRequest& request) {
        const evideo::TranscodeRequest native = video_request_from_facade(request);
        const evideo::OutputConflict conflict = evideo::check_output_conflict(native);
        VideoOutputCheck out;
        out.conflict = video_conflict_code(conflict);
        out.reason = evideo::output_conflict_reason(conflict, native);
        return out;
    },
    /* video_parse_ffprobe：ffprobe JSON → 媒体信息 */
    [](const std::string& json) {
        return video_media_to_facade(evideo::parse_ffprobe_json(json));
    },
    /* video_parse_encoders：`ffmpeg -encoders` 输出 → 编码器集合 */
    [](const std::string& output) { return evideo::parse_encoder_list(output); },
    /* video_progress_from_line：进度行 → 百分比（上限 99.5，口径在引擎） */
    [](const std::string& line, double duration_sec) {
        const evideo::ProgressSample sample = evideo::progress_from_line(line, duration_sec);
        VideoProgress out;
        out.matched = sample.matched;
        out.percent = sample.percent;
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
