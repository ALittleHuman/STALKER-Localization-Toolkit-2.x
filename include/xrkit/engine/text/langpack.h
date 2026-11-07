#pragma once
// xrkit::engine::text —— **标准语言包**装配（XML 校对第三模式「提取文本」的落点）。
//
// 用户 2026-10-26 的口径（逐条）：
//   · 触发：比较的一侧是**空的目录**（含"只有空子目录"）⇒ 模式出现第三种「提取文本」；
//   · 产物两种，可各自勾选、至少一个：
//       ① 文本形式的 XML（**空表也要写**成 `<string_table></string_table>`，不是删掉）；
//       ② 当前提取产物，但**按标准语言包布局**摆：`configs/text/<lang>/*.xml` +
//          被改写的 `scripts/`、`configs/gameplay/`；
//   · 每个语言目录各出一份（`<out>/<lang>/…`）；
//   · 同名文件已存在 ⇒ **默认拒绝并报告**（`--overwrite` 才覆盖）。
//
// 本模块只做"摆盘 + 归一 + 拒绝/记账"，不重写提取逻辑（那是 `text::extract`）。

#include <string>
#include <vector>

namespace xrkit::engine::text {

struct LangPackBuildOptions {
    std::string texts_dir;    // 源文本目录（…/configs/text/<lang> 或包含多语言子目录的父目录）
    std::string rewritten;    // 提取产物目录（含 scripts/ 与 gameplay/ 或 configs/gameplay/）；可空
    std::string out_root;     // 输出根：<out_root>/<lang>/…
    std::string lang;         // 语言目录名；空 = 从 texts_dir 的父目录名推断
    bool include_texts = true;      // ① 文本 XML（含空表）
    bool include_rewritten = true;  // ② 改写后的 scripts/configs
    bool overwrite = false;         // 同名文件：true=覆盖，false=拒绝并记账
    // ★ 2026-11-05（CoP 107 交接 P1-3）：`--pack-layout` —— 直接产出**标准语言包布局**：
    //   <out_root>/gamedata/configs/text/<lang>/*.xml、<out_root>/gamedata/scripts/…
    //   修前落盘是 <out_root>/<lang>/configs/text/<lang>/…（多一层），与"复制进游戏就生效"不符。
    bool pack_layout = false;
};

struct LangPackBuildResult {
    bool ok = false;
    std::string error;
    std::string out_dir;             // 实际输出目录（<out_root>/<lang>）
    std::string lang;                // 实际用到的语言目录名（可能由推断得来）
    int xml_written = 0;
    int rewritten_copied = 0;
    int empty_tables = 0;            // 其中"空 string_table 被显式写出"的个数
    int skipped_existing = 0;        // 因已存在而拒绝的文件数
    std::vector<std::string> warnings;
};

LangPackBuildResult build_language_pack(const LangPackBuildOptions& options);

// ★★ 2026-10-27：**从"非空一侧"直接装配**（XML 校对第三模式的完整链）——
//   先按标准提取把 scripts/gameplay 抽成"改写后的源 + 文本 XML"，再按标准语言包摆盘。
//   两步都在引擎层：页面/CLI 只传目录，不各写一套。
struct LangPackFromGameOptions {
    std::string source_dir;   // 非空一侧（含 scripts/ 与/或 gameplay/，可含 configs/text/<lang>）
    std::string out_root;
    std::string lang = "rus";
    bool include_texts = true;
    bool include_rewritten = true;
    bool overwrite = false;
    bool pack_layout = false;   // 见 LangPackBuildOptions::pack_layout
};

struct LangPackFromGameResult {
    bool ok = false;
    std::string error;
    uint64_t extracted_files = 0;
    uint64_t extracted_strings = 0;
    LangPackBuildResult pack;
    std::vector<std::string> warnings;
};

LangPackFromGameResult build_language_pack_from_game(const LangPackFromGameOptions& options);

// ★★ 2026-11-07（用户口径：提取只管松散文件；语言包是**收敛**，不是解包/抽字面量）：
//   从一棵**已解包的松散树**里把"语言包该有的东西"按原相对路径收敛出来：
//     gamedata/configs/text/<lang>/**
//     gamedata/fonts/**（含 eng/rus/editors）
//     gamedata/configs/fonts.ltx、gamedata/configs/localization.ltx
//     gamedata/textures/map/**、gamedata/textures/ui_new_game_map_*
//     ixr_addons/<任意>/configs/text/<lang>/**、ixr_addons/<任意>/fonts/**
//   其余（scripts/gameplay/anims/sounds/meshes/其它语言）**一律不拷**。
//   逐字节复制、不改写、不覆盖已有（除非 overwrite）。
struct LangPackCollectOptions {
    std::string source_root;   // 已解包的松散树根
    std::string out_root;      // 输出根（与源重叠会被拒绝）
    // ★ 2026-11-07（用户："需要通用，不要特异性内容"）：空 = **自动收所有语言**；
    //   非空 = 只收这些语言（语言名是在树里**扫出来的**，不是我们定的）。
    std::vector<std::string> langs;
    bool overwrite = false;
    // ★ 2026-11-08（用户："全是实打实扫出来的"）：保留清单不再硬编码
    //   `gamedata/` / `ixr_addons/` / `textures/map` —— 只按**结构规则**判断：
    //     ① 任意前缀下的 `configs/text/<lang>/**`（语言名扫出来的）
    //     ② 任意前缀下的 `configs/fonts.ltx`、`configs/localization.ltx`
    //     ③ 与 ①/② 同根的 `fonts/**`
    //   其它（scripts/gameplay/anims/textures/meshes/sounds…）一律不收；
    //   `--include-map` 之类的**模组特异开关已删除**。
};

struct LangPackCollectResult {
    bool ok = false;
    std::string error;
    std::string out_root;
    uint64_t copied = 0;
    uint64_t skipped_existing = 0;
    // ★ 2026-11-08（用户："为什么还是只有 rus"）：把**扫到的语言**如实报出来 ——
    //   源里只有一种语言时，输出就只有一种；这不是漏收。
    std::vector<std::string> langs;
    std::vector<std::string> warnings;
};

LangPackCollectResult collect_language_pack(const LangPackCollectOptions& options);

}  // namespace xrkit::engine::text
