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

}  // namespace xrkit::engine::font
