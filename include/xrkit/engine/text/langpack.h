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

}  // namespace xrkit::engine::text
