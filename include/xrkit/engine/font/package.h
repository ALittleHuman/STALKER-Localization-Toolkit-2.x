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
    bool full_cyrillic = true;
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
};

PackageResult build_package(const PackageOptions& opt);

}  // namespace xrkit::engine::font
