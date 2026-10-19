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
std::string user_ltx_path();        // ★ H-02：现在是 `xrkit_user.ltx`（不再是游戏同名的 user.ltx）
std::string legacy_user_ltx_path();  // 旧名字（只用于有条件迁移）

// ★★ H-02（第三方只读审计 2026-10-18）：**有条件迁移**旧 `user.ltx` 的设置。
//   硬条件：新文件不存在 ＋ 旧文件存在 ＋ 旧文件里能解析出**我们写过的键**
//   （`[theme] mode|scale` 或 `[ffmpeg] path|probe`）—— 否则**一个字节都不碰**。
//   返回：迁移的段数 / 0＝没迁移 / -1＝迁移失败（已留痕）。★ 旧文件不删、不改、不再写。
int migrate_legacy_user_config();
// exe 所在目录（1.x `app_dir()`）。发行版把 ffmpeg.exe/ffprobe.exe 放在这里，源码版
// `download_ffmpeg.py` 也下到项目根 —— 两者都等于这个目录（⑧8.1"只认两处"之一）。
std::string app_dir();
// 读整个文件；缺失/损坏返回空表（不抛）。
LtxData load_user_config();
std::string load_user_value(const std::string& section, const std::string& key,
                            const std::string& fallback = "");
// 一次写入某一段的若干项，**保留其它段**。写盘用文本模式语义（每个 \n → \r\n）。
bool save_user_values(const std::string& section,
                      const std::vector<std::pair<std::string, std::string>>& values);
bool save_user_value(const std::string& section, const std::string& key,
                     const std::string& value);

// ── 用户偏好：主题模式 / 界面大小档位（2026-10-06 用户）─────────────────
// 用户原话：「主题和字体大小**目前并没有保留继承的功能**（即下次开启又是默认的方案）」
//   （说的是 2.x 视图菜单里的「暗色/亮色主题」与「界面大小」两项）。
//
// ★ 落点与形态**照抄**已在用的那份惯例（`[font] path` 的字体选择器路径）：
//   `user.ltx` 的 `[theme]` 段 + `read → 改一项 → 整体写回、保留其它段`。
//   · `mode`  = "dark" / "light"（1.x `toolkit_theme.load_user_theme/save_user_theme` 的**同一个**键）
//   · `scale` = 十进制倍率（如 `1.1`），十进制点号，与 1.x INI 里写浮点的写法一致
// ★ 取值范围与默认值**不变**：`scale` 的合法集合由外壳的 `ui_size_choices()` 决定
//   （`core` 不依赖 Qt/外壳 ⇒ 这里只做"能解析成 >0 的十进制数"这一层过滤，
//   越界/垃圾值一律当作"用户没选过"⇒ 回落屏幕自适应，**不发明新的默认值**）。
// ★ 读失败/键不存在都返回 fallback（不抛）；写失败返回 false —— 调用方**必须**把它说出来。
// ★ 每次只写**改的那一项**（`save_user_value` 单键），不整块覆盖别的键。
inline constexpr const char* kPrefSection = "theme";        // 段名与 1.x 的 [theme] 相同
inline constexpr const char* kPrefModeKey = "mode";         // 1.x `save_user_theme` 用的键
inline constexpr const char* kPrefScaleKey = "scale";       // 界面大小档位（2.x 新增的项）

// 读回上次的主题：缺失/垃圾值 ⇒ `fallback`（默认暗色），**不抛**。
ThemeMode load_theme_mode(ThemeMode fallback = ThemeMode::Dark);
// 写主题。返回 false = 写盘失败（调用方要可见地报出来）。
bool save_theme_mode(ThemeMode mode);

// 读回上次的界面大小倍率：缺失/无法解析/非正数 ⇒ `fallback`（0.0 = 没有偏好）。
double load_ui_scale(double fallback = 0.0);
// 写界面大小倍率（十进制、最多 4 位小数，去掉尾随 0）。false = 写盘失败。
bool save_ui_scale(double scale);

// Python 文本模式（`open(..., "w")`）在 Windows 的换行翻译：**每一个** `\n` → `\r\n`。
std::string to_windows_newlines(const std::string& text);

}  // namespace xrkit::core
