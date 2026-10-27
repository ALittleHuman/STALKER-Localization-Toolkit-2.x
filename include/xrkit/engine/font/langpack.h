#pragma once
// xrkit::engine::font::langpack —— **语言包识别**（IX-Ray 生成器「指定」模式的底座）。
//
// 口径（用户 2026-10-26 拍板 + `12_规划_语言包识别与分部位字体.md` §三/§3.1）：
//   · **只读扫描**用户给的目录，不改任何文件；输入只有一个目录（用户自己解/自己装）；
//   · **识别结构，不识别模组**：判断依据是文件结构、`fonts.ltx` 的段与键、文本目录布局，
//     代码里不出现任何模组名，也不为某个模组写分支；
//   · 识别结论分两件、互不代替：
//       FontProfile（字体怎么生成）＝ SoC / ClS / CoP / IX-Ray / unknown；
//       PackLayout（包怎么摆）＝ 直接从包本身读（config vs configs、text/<lang>、localization.ltx）。
//   · 判定顺序（IX-Ray 优先，矢量优先）：
//       ① `configs/fonts.ltx` 的段里有 `name` 且 `fonts/<lang>/*.ttf` 非空 ⇒ **IX-Ray**
//          （即使同时有 `ui_font_*.dds` 也按 IX-Ray，dds 忽略）；
//       ② 否则有 `textures/ui/ui_font_*.dds` ⇒ 位图三档：`shader = hud\font` ⇒ CoP，
//          `shader = font` ⇒ 配置目录 `config` ⇒ SoC，`configs` ⇒ ClS；
//       ③ 两边都没有 ⇒ unknown（零静默：`evidence` 里写清缺什么）。
#pragma once

#include <string>
#include <vector>

namespace xrkit::engine::font {

enum class PackProfile { Unknown, Soc, ClearSky, Cop, IxRay };

const char* profile_name(PackProfile profile);   // "soc" / "cis" / "cop" / "ixr" / "unknown"

struct LangPackSlot {
    std::string section;   // 段名（如 ui_font_letterica16_russian）
    std::string base;      // 继承的基段（`[A]:B` 的 B；可空）
    std::string name;      // 矢量：ttf 名（无扩展名）；位图：空
    int size = 0;          // 字号（0 = 段里没写）
    std::string opentype;  // 矢量：true/false/空
    std::string shader;    // 位图：font / hud\font / 空
    bool has_name = false; // 段里有没有 `name`
};

struct LangPackModel {
    std::string root;         // 归一化后的 gamedata 根（找到 gamedata 就下沉一层）
    PackProfile profile = PackProfile::Unknown;
    std::string lang;         // localization.ltx `[string_table] language`；缺 ⇒ "rus"
    bool lang_from_ltx = false;
    std::string font_config;  // 找到的那份 fonts.ltx（configs/fonts.ltx 或 configs/ui/fonts.ltx）
    std::string config_dir;   // "configs" / "config"
    std::string text_dir;     // 相对 root 的 configs/text/<lang>（存在时）
    std::string ttf_dir;      // 相对 root 的 fonts/<lang>（存在时）
    int ttf_count = 0;
    int dds_count = 0;
    std::vector<LangPackSlot> font_slots;  // ★ 不叫 slots：Qt 里那是宏
    std::vector<std::string> evidence;  // 判定依据/缺什么（零静默）
};

// 扫描一个语言包目录（可以是游戏根、`gamedata`、或某个 mod 的整包目录）。
LangPackModel scan_lang_pack(const std::string& root);

}  // namespace xrkit::engine::font
