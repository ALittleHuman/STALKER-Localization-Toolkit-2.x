#pragma once
// xrkit::engine::font —— 出包（M1-7 第⑦步）：1.x `build_package` 的等价实现。
//
// 产物树（与 1.x 逐字节一致）：
//   <out>/gamedata/textures/ui/ui_font_{11,13,15,16,17,18,20,21,23,25}<suffix>.dds / .ini
//   <out>/gamedata/{config|configs}/text/<lang>/*.xml          （一律无 BOM UTF-8 + 声明改 utf-8）
//   <out>/gamedata/{config|configs}/fonts.ltx
//   <out>/gamedata/{config|configs}/localization.ltx           （`_#LANG#`→suffix、`#LANG#`→lang）
//
// 逐句照抄的几处关键语义（错了就不是逐字节一致）：
//   · 字号表 `SIZES` = 11/13/15（S=1）+ 16/17/18/20/21/23/25（x2）；`render_size = size + offset`，
//     但**文件名与 ltx 仍用槽位名**；
//   · XML 只扫**顶层**（`sorted(os.listdir(xml_dir))`，不递归 —— 与 `extract_chars` 的递归不同！）；
//   · XML 以 `encoding="utf-8", newline=""` 写：无 BOM、`\n` 不翻译；
//   · 任一字号失败 → **把已写出的 gamedata 整个删掉**（否则用户看到半成品以为成功）。

#include <string>

#include "xrkit/engine/font/ixr.h"
#include <vector>

namespace xrkit::engine::font {

struct PackageOptions {
    std::string game = "SoC";  // SoC / ClS / CoP
    std::string lang = "chs";
    std::string xml_dir;
    std::string font_path;
    std::string out_dir;
    int offset = 0;
    std::string suffix;
    // ★ 2026-11-06（用户拍板）：full_cyrillic **内置，不再是选项**。
    //   字段保留只为兼容既有调用方；引擎一律按 true 处理（完整西里尔 + 必需字符必测）。
    bool full_cyrillic = true;   // 已忽略（恒 true）
    // ★ 2026-11-06（HA/DSH）：**基线全集**缺失默认只警告；显式要求"全量覆盖"时才硬失败。
    //   语料里真实出现的字缺失始终硬失败（那是"确定会坏"），与此开关无关。
    bool require_full_cyrillic = false;
    // ★ 2026-11-06（最小统一）：语料缺字的策略。fail（默认）= 不出包；warn = 警告并继续。
    //   基线缺字**始终只警告**（两线口径，见 f08a1d2）。
    bool missing_warn = false;
    // 只做这些槽位（空 = 全部 10 档）。门禁分片跑时用得上。
    std::vector<int> only_sizes;
};

struct PackageResult {
    bool ok = false;
    std::string error;
    std::string out_gamedata;
    int fonts_written = 0;
    int xml_written = 0;
    std::vector<std::string> warnings;  // 编码靠猜的 XML（"文件: 编码"）
    std::string lang;  // 实际采用的语言（profile 可能覆盖页面给的值）
};

PackageResult build_package(const PackageOptions& opt);

// ★ 2026-11-06（最小统一·段表归一）：从 fonts.ltx 文本解析**段名序列**。
//   · 内置模板与参照包 `configs/fonts.ltx` 走**同一个**解析器 ⇒ 两者产出同一结构；
//   · 调用方据此打印 `SEGMENTS source=builtin|ref n=N`，并可在 writer 之外做校验。
std::vector<std::string> parse_segments(const std::string& ltx_text);
// 内置模板（SoC/ClS/CoP）的段数；未知游戏返回 -1。
int template_segment_count(const std::string& game);
// 参照包（`<pack>/gamedata/configs/fonts.ltx`）的段数 —— **与内置模板同一个解析器**；
// 读不到返回 -1。addon 覆盖层的段不在这里（那是加载顺序的职责）。
int ref_segment_count(const std::string& pack_dir);

// ★★ 2026-11-06（HA 打回点 ①）：**唯一统一入口** —— GUI 与 CLI 都只许调它。
//   规则：`use_ref` 决定 writer（true=矢量/扫描参照；false=位图/内置模板），
//   **游戏名只用来决定默认值**（页面把 IX-Ray 映射成 use_ref=true + ref=快照），
//   不决定调用路径。writer 实现仍是两个，但"选哪条"只有这里一处。
struct FontPackRequest {
    bool use_ref = false;          // true ⇒ 扫描 ref 的 fonts.ltx（矢量）；false ⇒ 内置模板（位图）
    std::string ref;               // 参照包目录（use_ref 时必填）
    std::string text_root;         // 译文根（矢量可选；位图必填）
    std::string font;
    std::string out;
    std::string game = "CoP";      // 位图模板档
    std::string lang = "rus";      // 语言（`--langs` 与它同义；单个也接受）
    std::string langs;             // 矢量：`rus,eng`；空 = 用包内 primary
    bool missing_warn = false;     // 语料缺字降级为警告
    bool require_full_cyrillic = false;
    std::string suffix;
    int offset = 0;
    std::vector<int> only_sizes;
};

struct FontPackOutcome {
    bool ok = false;
    bool vector_path = false;
    std::string error;
    int segments = 0;              // 段数（来源见 segments_from_ref）
    bool segments_from_ref = false;
    std::size_t charset_count = 0; // 字符数（来自统一字符集核心）
    PackageResult bitmap;          // 位图路径结果
    IxrBuildResult vector_result;  // 矢量路径结果
};

FontPackOutcome build_font_pack(const FontPackRequest& request);

}  // namespace xrkit::engine::font
