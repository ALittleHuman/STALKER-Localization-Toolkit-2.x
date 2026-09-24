#pragma once
// xrkit::core —— 主题令牌与 `user.ltx`（用户设置）。
//
// **数据层不碰 Qt**（1.x `toolkit_theme.py` 的约束，2026-09-12 为 Qt 试点加的）：颜色/字体/
// `px()`/`palette()` 与具体 UI 技术无关，是暗/亮两套的**唯一真值来源**；Qt 外壳只负责把
// 这些令牌映射成 `QPalette`/`QSS`，不自己抄一份表。
//
// 令牌表由 `tools/gen_theme_tables.py` 从 1.x `toolkit_theme.py` **按 AST** 生成到
// `src/core/theme_tables.inc`（门禁 `--check` 锁住），所以改主题只能改 1.x 那份真值来源。

#include <string>
#include <utility>
#include <vector>

namespace xrkit::core {

// ── 主题令牌 ───────────────────────────────────────────────────────────

enum class ThemeMode { Dark, Light };

// 颜色角色名（顺序 = 1.x 表序）；越界返回 nullptr。
int theme_role_count();
const char* theme_role(int index);

// 某模式下某角色的颜色（"#RRGGBB"）；角色名不认识返回 nullptr。
const char* theme_color(ThemeMode mode, const char* role);

// 当前模式（1.x `CURRENT_MODE`）。切模式只改这一个值 —— 外壳那边要"原地更新"而不是重新
// 绑定（1.x 明文写着：任何重新绑定都会让切主题静默失效）。
ThemeMode theme_mode();
void set_theme_mode(ThemeMode mode);
const char* theme_mode_name(ThemeMode mode);
bool theme_mode_from_name(const std::string& name, ThemeMode& out);

// 当前模式下某角色的颜色。
const char* theme_color_current(const char* role);

// 会把控件底色一起改掉的角色（1.x `_BG_ROLES`）—— 主题映射时用得上。
int theme_bg_role_count();
const char* theme_bg_role(int index);

// 字体角色（1.x `_FONTS`）。
struct ThemeFontSpec {
    const char* role;
    const char* family;
    int size;
    bool bold;
};
int theme_font_count();
const ThemeFontSpec& theme_font(int index);

// ── HiDPI ─────────────────────────────────────────────────────────────
// 设计稿里的硬编码像素都按 **96 DPI（100% 缩放）** 定的；凡按 96 DPI 定的像素值都要过 `px()`，
// 否则 150% 屏上正文行高变大、写死的行高不变 → 文字被裁。
void set_ui_scale(double value);
double ui_scale();
int px(int value);

// ── user.ltx（所有工具共用的极简 INI）────────────────────────────────
// 口径（1.x）：`[section]` + `key = value`；**读→改一项→整体写回、保留其它段**，
// 不许整文件重写。无法识别的行（注释/垃圾）直接跳过，绝不因为一行坏数据丢掉整个文件。
struct LtxData {
    std::vector<std::string> sections;                                   // 段顺序 = 文件顺序
    std::vector<std::vector<std::pair<std::string, std::string>>> items;  // 每段的键值对（有序）
};

LtxData parse_ltx(const std::string& text);
std::string render_ltx(const LtxData& data);

// `user.ltx` 的路径：**exe 所在目录**（1.x `app_dir()/user.ltx`；自动生成，不入库）。
std::string user_ltx_path();
// 读整个文件；缺失/损坏返回空表（不抛）。
LtxData load_user_config();
std::string load_user_value(const std::string& section, const std::string& key,
                            const std::string& fallback = "");
// 一次写入某一段的若干项，**保留其它段**。写盘用文本模式语义（每个 \n → \r\n）。
bool save_user_values(const std::string& section,
                      const std::vector<std::pair<std::string, std::string>>& values);
bool save_user_value(const std::string& section, const std::string& key,
                     const std::string& value);

// Python 文本模式（`open(..., "w")`）在 Windows 的换行翻译：**每一个** `\n` → `\r\n`。
std::string to_windows_newlines(const std::string& text);

}  // namespace xrkit::core
