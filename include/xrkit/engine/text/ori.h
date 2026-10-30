#pragma once
// xrkit::engine::text::ori —— **ori 树**构建（交接区《文本提取工具最终规划 2026-10-29》）。
//
// 口径（用户已拍板）：
//   拖入一个模组根 / `gamedata` / `ixr_addons` / `configs` / 单文件，**空侧作为 `ori`**；
//   工具自动识别「容器」，把整棵树复制进 `ori/<容器相对路径>/...`，再在容器内提取硬编码、
//   改写、把文本 XML 落进 `<容器>/configs/text/<lang>`。
//
// 与旧链（`build_language_pack*`）的区别：
//   · 输出根是 `ori`（模组根），**不再** `out/<lang>`；
//   · `gameplay` 落 `configs/gameplay`，`scripts` 落 `scripts`（相对容器根）；
//   · 语言取自容器自己的 `configs/localization.ltx`，**不写死 rus**；
//   · 源只读，`ori` 非空默认拒绝（`clean=true` 才允许清空）。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::engine::text {

struct OriOptions {
    std::string source_dir;   // 模组根 / gamedata / ixr_addons / configs / 单文件
    std::string out_dir;      // ori
    std::string lang;         // 空 = 从 localization.ltx 推断
    std::string prefix = "gs";
    bool clean = false;       // 允许清空已有 ori（默认拒绝，防误删）
};

struct OriContainer {
    std::string rel;                 // 相对 source 根（"" = 源本身就是容器）
    std::string lang;                // 该容器的语言
    std::string lang_source;         // 语言证据：localization.ltx / text_dir / arg / fallback
    std::string kind;                // ixr / bitmap / mixed / none
    bool has_text = false;
    bool has_fonts = false;
    bool has_scripts = false;
    bool has_gameplay = false;
    uint64_t extracted = 0;          // 抽出的正文条数
    uint64_t rewritten = 0;          // 改写文件数
    uint64_t texts = 0;              // 写出的文本 XML 个数
};

struct OriResult {
    bool ok = false;
    std::string error;
    std::string out_dir;
    std::string lang;
    std::string lang_source;
    std::vector<OriContainer> containers;
    std::vector<std::string> warnings;   // 逐条：哪条分支没提取/为什么（零静默）
    uint64_t files_copied = 0;
    uint64_t rewritten = 0;
    uint64_t texts = 0;
    uint64_t extracted = 0;
};

// ★★ 2026-10-30（交接区最终规划 ④）：**LangPackProfile 独立结构** ——
//   三条后端（文本 / IXR / 位图）只消费它，生成阶段禁止再自己 detect 语言/猜源根。
//   `OriResult` 是"这次生成干了什么"；`OriProfile` 是"证据是什么"，两者分开：
//   前者可丢，后者要落盘（`ori/_xrkit_profile.json`）并传给下游。
struct OriProfile {
    std::string lang;
    std::string lang_source;
    std::string out_dir;
    std::vector<OriContainer> containers;
};

// 从一次生成结果里抽出 Profile（失败时返回空 lang 的 Profile）
OriProfile ori_profile_of(const OriResult& result);

// 序列化（CLI、落盘、下游共用同一份）
std::string ori_profile_json(const OriProfile& profile);
// 兼容旧调用：等价于 ori_profile_json(ori_profile_of(result))
std::string ori_profile_json(const OriResult& result);

OriResult build_ori_tree(const OriOptions& options);

// 生成用到的 Profile（**扫描证据的唯一出口**）。CLI 与页面都读这一份，不许各自再拼一遍。
// 形状稳定：{"lang","lang_source","out","containers":[{rel,lang,lang_source,kind,
//            has_text,has_fonts,has_scripts,has_gameplay,extracted,rewritten,texts}]}
std::string ori_profile_json(const OriResult& result);

}  // namespace xrkit::engine::text
