#include "xrkit/engine/plugin/engine_facade.h"

#include <mutex>

namespace xrkit::plugin {

// 引擎自带的默认表（实现在 `engine_facade_impl.cpp`；只依赖引擎、不依赖外壳）
const EngineFacade* builtin_engine_facade();

namespace {

std::mutex& facade_mutex() {
    static std::mutex instance;
    return instance;
}

const EngineFacade*& facade_storage() {
    static const EngineFacade* instance = nullptr;
    return instance;
}

// 名字与 `EngineFacade` 的字段一一对应（判据按它对账）
const char* const kMethodNames[] = {
    "font_check",
    "font_pack",
    "font_state_marks",
    "font_size_slots",
    // ★★ P9a：convert 页那一组（两张编码表 + 批量转换 + 备份两件 + 统计 CSV + 时长格式化）
    "text_encodings", "text_convert_batch", "text_restore_backups", "text_clear_backups",
    "text_write_stats_csv", "text_format_duration",
    // ★★ P10a：text 页那一组（第三页迁移）
    "text_default_id_prefix", "text_validate_extract", "text_extract_kind",
    "text_extract_dir",
    // ★★ P11a：xml 页那一组（第四页迁移）
    "xml_modes", "xml_find_mode", "text_read_bytes", "xml_compare_stats",
    "xml_compare_text", "xml_only_one_side", "xml_inline_detail", "xml_full_detail",
    "xml_full_text_detail", "xml_render_csv", "xml_write_csv",
    // ★★ P11b：目录对比 + 筛选
    "xml_compare_dirs", "xml_row_matches",
    // ★★ P12a：video 页那一组（第五页迁移）
    "video_find_ffmpeg", "video_major_version", "video_version_rejected",
    "video_plan_transcode", "video_check_output_conflict", "video_parse_ffprobe",
    "video_parse_encoders", "video_progress_from_line",
    // ★★ P14-0a：fs 面第一批（第六页迁移）
    "fs_formats", "fs_format_to_db", "fs_format_is_sqfs", "fs_format_is_auto",
    "fs_is_package_name", "fs_pack_ext_for", "fs_package_folder_name",
    "fs_resolve_output_root", "fs_unpack_target_dir", "fs_resolve_pack_target",
    "fs_safe_out_path", "fs_plan_unpack_targets", "fs_preview_layout",
    // ★★ P14-0b：fs 面第二批（合并树走句柄）
    "fs_loose_entries", "fs_sort_sources_by_basename",
    "fs_tree_build", "fs_tree_free", "fs_tree_sources", "fs_tree_flatten",
    "fs_tree_toggle_all", "fs_tree_toggle_dir", "fs_tree_toggle_source",
    "fs_tree_source_state", "fs_tree_checked_paths", "fs_tree_apply_checked_paths",
    "fs_tree_conflict_count", "fs_tree_checked_file_count", "fs_tree_find",
    // ★★ P14-0c：fs 面第三批（读条目/闸门/收集/解包）
    "fs_gate_path", "fs_decrypt_output_dir", "fs_package_entries",
    "fs_collect_pack_files", "fs_extract_package",
};

bool has_impl(const EngineFacade& facade, const std::string& name) {
    if (name == "font_check") return static_cast<bool>(facade.font_check);
    if (name == "font_pack") return static_cast<bool>(facade.font_pack);
    if (name == "font_state_marks") return static_cast<bool>(facade.font_state_marks);
    if (name == "font_size_slots") return static_cast<bool>(facade.font_size_slots);
    if (name == "text_encodings") return static_cast<bool>(facade.text_encodings);
    if (name == "text_convert_batch") return static_cast<bool>(facade.text_convert_batch);
    if (name == "text_restore_backups") return static_cast<bool>(facade.text_restore_backups);
    if (name == "text_clear_backups") return static_cast<bool>(facade.text_clear_backups);
    if (name == "text_write_stats_csv") return static_cast<bool>(facade.text_write_stats_csv);
    if (name == "text_format_duration") return static_cast<bool>(facade.text_format_duration);
    if (name == "text_default_id_prefix") return static_cast<bool>(facade.text_default_id_prefix);
    if (name == "text_validate_extract") return static_cast<bool>(facade.text_validate_extract);
    if (name == "text_extract_kind") return static_cast<bool>(facade.text_extract_kind);
    if (name == "text_extract_dir") return static_cast<bool>(facade.text_extract_dir);
    if (name == "xml_modes") return static_cast<bool>(facade.xml_modes);
    if (name == "xml_find_mode") return static_cast<bool>(facade.xml_find_mode);
    if (name == "text_read_bytes") return static_cast<bool>(facade.text_read_bytes);
    if (name == "xml_compare_stats") return static_cast<bool>(facade.xml_compare_stats);
    if (name == "xml_compare_text") return static_cast<bool>(facade.xml_compare_text);
    if (name == "xml_only_one_side") return static_cast<bool>(facade.xml_only_one_side);
    if (name == "xml_inline_detail") return static_cast<bool>(facade.xml_inline_detail);
    if (name == "xml_full_detail") return static_cast<bool>(facade.xml_full_detail);
    if (name == "xml_full_text_detail") return static_cast<bool>(facade.xml_full_text_detail);
    if (name == "xml_render_csv") return static_cast<bool>(facade.xml_render_csv);
    if (name == "xml_write_csv") return static_cast<bool>(facade.xml_write_csv);
    if (name == "xml_compare_dirs") return static_cast<bool>(facade.xml_compare_dirs);
    if (name == "xml_row_matches") return static_cast<bool>(facade.xml_row_matches);
    if (name == "video_find_ffmpeg") return static_cast<bool>(facade.video_find_ffmpeg);
    if (name == "video_major_version") return static_cast<bool>(facade.video_major_version);
    if (name == "video_version_rejected") return static_cast<bool>(facade.video_version_rejected);
    if (name == "video_plan_transcode") return static_cast<bool>(facade.video_plan_transcode);
    if (name == "video_check_output_conflict") return static_cast<bool>(facade.video_check_output_conflict);
    if (name == "video_parse_ffprobe") return static_cast<bool>(facade.video_parse_ffprobe);
    if (name == "video_parse_encoders") return static_cast<bool>(facade.video_parse_encoders);
    if (name == "video_progress_from_line") return static_cast<bool>(facade.video_progress_from_line);
    if (name == "fs_formats") return static_cast<bool>(facade.fs_formats);
    if (name == "fs_format_to_db") return static_cast<bool>(facade.fs_format_to_db);
    if (name == "fs_format_is_sqfs") return static_cast<bool>(facade.fs_format_is_sqfs);
    if (name == "fs_format_is_auto") return static_cast<bool>(facade.fs_format_is_auto);
    if (name == "fs_is_package_name") return static_cast<bool>(facade.fs_is_package_name);
    if (name == "fs_pack_ext_for") return static_cast<bool>(facade.fs_pack_ext_for);
    if (name == "fs_package_folder_name") return static_cast<bool>(facade.fs_package_folder_name);
    if (name == "fs_resolve_output_root") return static_cast<bool>(facade.fs_resolve_output_root);
    if (name == "fs_unpack_target_dir") return static_cast<bool>(facade.fs_unpack_target_dir);
    if (name == "fs_resolve_pack_target") return static_cast<bool>(facade.fs_resolve_pack_target);
    if (name == "fs_safe_out_path") return static_cast<bool>(facade.fs_safe_out_path);
    if (name == "fs_plan_unpack_targets") return static_cast<bool>(facade.fs_plan_unpack_targets);
    if (name == "fs_preview_layout") return static_cast<bool>(facade.fs_preview_layout);
    if (name == "fs_loose_entries") return static_cast<bool>(facade.fs_loose_entries);
    if (name == "fs_sort_sources_by_basename") return static_cast<bool>(facade.fs_sort_sources_by_basename);
    if (name == "fs_tree_build") return static_cast<bool>(facade.fs_tree_build);
    if (name == "fs_tree_free") return static_cast<bool>(facade.fs_tree_free);
    if (name == "fs_tree_sources") return static_cast<bool>(facade.fs_tree_sources);
    if (name == "fs_tree_flatten") return static_cast<bool>(facade.fs_tree_flatten);
    if (name == "fs_tree_toggle_all") return static_cast<bool>(facade.fs_tree_toggle_all);
    if (name == "fs_tree_toggle_dir") return static_cast<bool>(facade.fs_tree_toggle_dir);
    if (name == "fs_tree_toggle_source") return static_cast<bool>(facade.fs_tree_toggle_source);
    if (name == "fs_tree_source_state") return static_cast<bool>(facade.fs_tree_source_state);
    if (name == "fs_tree_checked_paths") return static_cast<bool>(facade.fs_tree_checked_paths);
    if (name == "fs_tree_apply_checked_paths") return static_cast<bool>(facade.fs_tree_apply_checked_paths);
    if (name == "fs_tree_conflict_count") return static_cast<bool>(facade.fs_tree_conflict_count);
    if (name == "fs_tree_checked_file_count") return static_cast<bool>(facade.fs_tree_checked_file_count);
    if (name == "fs_tree_find") return static_cast<bool>(facade.fs_tree_find);
    if (name == "fs_gate_path") return static_cast<bool>(facade.fs_gate_path);
    if (name == "fs_decrypt_output_dir") return static_cast<bool>(facade.fs_decrypt_output_dir);
    if (name == "fs_package_entries") return static_cast<bool>(facade.fs_package_entries);
    if (name == "fs_collect_pack_files") return static_cast<bool>(facade.fs_collect_pack_files);
    if (name == "fs_extract_package") return static_cast<bool>(facade.fs_extract_package);
    return false;
}

// ★★ P7a：**没注入就用引擎自带的默认表** —— 字体那几件只依赖引擎、不依赖外壳，而 CLI
//   （`--py-plugins`）也是插件宿主 ⇒ 默认表让两个宿主都能用（实测：实现在外壳里时，CLI 里的
//   插件调 `font_size_slots()` 拿到的是 None）。外壳仍可 `set_engine_facade()` 覆盖/扩展
//   —— 表是宿主的东西，只是**有默认值**。
const EngineFacade* facade_or_default() {
    return facade_storage() != nullptr ? facade_storage() : builtin_engine_facade();
}

}  // namespace

void set_engine_facade(const EngineFacade* facade) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    facade_storage() = facade;
}

const EngineFacade* engine_facade() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    return facade_or_default();
}

bool engine_facade_available(std::string* reason) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    if (facade == nullptr) {
        if (reason != nullptr) {
            *reason = "宿主没注入引擎调用门面，且引擎没带默认表 —— 插件的注册面照常可用，"
                      "引擎调用一律返回空并在日志里记账";
        }
        return false;
    }
    for (const char* name : kMethodNames) {
        if (!has_impl(*facade, name)) {
            if (reason != nullptr) {
                *reason = std::string("引擎调用门面缺方法「") + name + "」（部分实现 ⇒ 该方法不可用）";
            }
            return false;
        }
    }
    if (reason != nullptr) reason->clear();
    return true;
}

bool engine_facade_has(const std::string& name) {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    return facade != nullptr && has_impl(*facade, name);
}

std::vector<std::string> engine_facade_method_names() {
    std::lock_guard<std::mutex> lock(facade_mutex());
    const EngineFacade* facade = facade_or_default();
    std::vector<std::string> out;
    if (facade == nullptr) return out;
    for (const char* name : kMethodNames) {
        if (has_impl(*facade, name)) out.emplace_back(name);
    }
    return out;
}

}  // namespace xrkit::plugin
