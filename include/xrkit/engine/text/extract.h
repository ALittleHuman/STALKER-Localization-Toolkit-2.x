#pragma once
// xrkit::engine::text —— 文本提取（gameplay XML / .script / .ltx 三分支）。
//
// 逐字节复刻 1.x `apps/text_extract_app.py` 的 `_extract_gameplay` / `_extract_scripts`：
// 产物 = ① 改写后的源文件树（把原文换成 ID 引用，写进 目标目录/{gameplay|scripts}/）
//        ② 汇总的文本 XML（目标目录/{prefix}_{gameplay|scripts}_texts.xml，固定 utf-8）。
//
// 口径（1.x 的血账，逐条照抄）：
//   · 候选遍历**必须排序**（Python set 迭代随哈希随机化，不排序会让同一份源两次运行
//     得到不同 ID）；我们这里直接产出"排序去重"后的候选表。
//   · 正则用 PCRE2（`std::regex` 不支持 lookbehind）；`PCRE2_UTF|PCRE2_UCP` 对齐
//     Python `re` 的 Unicode `\s` 语义。
//   · 源目录名必须是 gameplay / scripts（只看文件夹自己的名字，不向下查找）。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xrkit::engine::text {

enum class ExtractKind { Gameplay, Scripts };

inline constexpr const char* kDefaultIdPrefix = "gs";
inline constexpr const char* kFallbackIdPrefix = "mod";

struct ExtractOptions {
    // 文本 XML 文件名用的用户前缀（1.x `GP_TEXTS_NAME.format(prefix=...)`）
    std::string prefix = kDefaultIdPrefix;
    // ID 前缀（1.x `sgtat_{prefix}_{abbr}_{ts}`）。留空则按 prefix + kind 生成（ts=0）。
    // ★ 对拍时必须显式给固定值：1.x 的 `run_extraction` 里 ts = int(time.time())。
    std::string id_prefix;
    // 先清目标目录里的 gameplay/scripts 与 {prefix}_*_texts.xml（1.x UI 里默认勾选）
    bool clean = false;
    // ★ 2026-10-06（用户红线：取消要**真停**）：可选的停止谓词。**只在文件边界轮询**
    //   ⇒ 停下来时已写出的产物都是**完整的单文件产物**，绝不写半个文件（零静默的反面：
    //   "停是停了，但产物半截" 比不停更糟）。默认空 ⇒ 行为与旧版逐字节相同。
    //   引擎**不依赖 Qt**：这里用 std::function（前端把"取消请求位"包成 lambda）。
    std::function<bool()> should_stop;
};

struct ExtractStats {
    bool ok = false;
    std::string error;
    ExtractKind kind = ExtractKind::Gameplay;
    uint64_t files = 0;
    uint64_t strings = 0;
    uint64_t chars = 0;
    std::vector<std::string> skipped;  // 「相对路径：原因」，对应 1.x 的 log_detail
    std::string texts_xml_name;        // 实际产出的文本 XML 文件名（空=这一类没提到文本）
    double elapsed_ms = 0.0;
    // ★ 2026-10-06：`should_stop()` 在文件边界返回 true ⇒ **已按请求停下**。
    //   此时 `files/strings/chars` 是**停下前已经落盘的那部分**的如实计数（不是"全部"）。
    //   `ok` 仍为 true（没出错）；调用方**必须先看这个位**再报结论（否则会把"取消"
    //   报成"完成"——那就是静默）。
    bool cancelled = false;
};

// 源目录名 → 类型（不是 gameplay/scripts 就失败，理由同 1.x：不要选它们的上层目录）
bool kind_from_dir_name(const std::string& source_dir, ExtractKind& out, std::string& err);

// ── 7.1 / 7.5 的守卫（放引擎，免得两条入口各判一次还判得不一样）────────
struct ExtractRequest {
    std::string source_dir;  // 必须是 gameplay / scripts **文件夹本身**
    std::string target_dir;  // 必填；改写后的源文件与文本 XML 都写这里
    std::string prefix = kDefaultIdPrefix;
};

// 通过返回**空串**；否则是给用户看的中文理由（与 1.x 的 ValueError 文案逐字一致）。
// 检查顺序照 1.x `run_extraction`：源目录存在 → target 必填 → 文件夹名合法 →
// **目标子目录是否撞上源目录**（撞上＝原地覆盖，原文不可恢复）。
std::string validate_request(const ExtractRequest& request);

// 跑一个分支：把 src 下的候选文件提取到 dst（dst 是 **该类型的目标子目录**，例如 out/gameplay），
// 文本 XML 写到 `xml_out_dir`（通常就是 out 的根）。
ExtractStats extract_dir(const std::string& source_dir, const std::string& target_dir,
                         const std::string& xml_out_dir,
                         const ExtractOptions& options = ExtractOptions{});

// 给内部/测试用：三分支的纯文本处理（不碰文件系统）
std::vector<std::string> config_xml_candidates(const std::string& text);
std::vector<std::string> script_candidates(const std::string& text);
std::vector<std::string> ltx_candidates(const std::string& text);
std::string normalize_xml_string(const std::string& text, bool need_fix_st, bool delete_header);
std::string generate_text_xml_body(const std::vector<std::pair<std::string, std::string>>& items);

}  // namespace xrkit::engine::text
