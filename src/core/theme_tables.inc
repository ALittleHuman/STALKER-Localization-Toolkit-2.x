// 本文件由 tools/gen_theme_tables.py 从 1.x toolkit_theme.py **按 AST** 生成，请勿手改。
// 数据源 sha256 60810e54b8a65ec10b2c6f73fa5f3c309af6185870a8228247e4f82f1211136b
//
// 1.x 的口径：**只有一份真值来源**（就是它的 THEMES/_FONTS），Qt 侧按 AST 读这份表、
// 不抄一份（抄一份就是漂移来源）。这里同样是生成出来的，别手改。

// 颜色角色（顺序 = 1.x 表序，18 个）
static const char* const kThemeRoles[18] = {
    "bg", "surface", "surface2", "border", "accent", "accent_hover",
    "text", "text_dim", "text_bright", "green", "red", "yellow",
    "orange", "entry_bg", "selected", "scroll_border", "sash", "sash_light",
};

// dark 模式
static const char* const kThemeDark[18] = {
    "#1b1d23", "#22252c", "#2b2f38", "#363b46", "#4c8dff", "#6ba0ff",
    "#d6d9e0", "#8b919e", "#f0f2f6", "#5cc08d", "#e5686d", "#d9b45b",
    "#d98d5b", "#161920", "#2b4a7d", "#2f333d", "#2f333d", "#4c8dff",
};

// light 模式
static const char* const kThemeLight[18] = {
    "#f6f7f9", "#ffffff", "#eef0f4", "#d3d7de", "#2f6fe4", "#4a86f0",
    "#2c3038", "#6b7280", "#14171c", "#1f8a5f", "#cf4444", "#9a7b12",
    "#b5651d", "#ffffff", "#d6e4ff", "#cfd4dc", "#d3d7de", "#2f6fe4",
};

// 会把控件底色一起改掉的角色（1.x `_BG_ROLES`）
static const char* const kBgRoles[5] = {"bg", "surface", "surface2", "entry_bg", "selected"};

// 字体角色（1.x `_FONTS`：(family, size[, "bold"])）
// ★ 结构体名字要避开公开头里的 `core::ThemeFontSpec`（同名会"不明确符号"）
struct ThemeFontRow {
    const char* role;
    const char* family;
    int size;
    int bold;
};

static const ThemeFontRow kThemeFonts[6] = {
    {"font", "Segoe UI", 12, 0},
    {"font_sm", "Segoe UI", 11, 0},
    {"font_mono", "Consolas", 11, 0},
    {"font_title", "Segoe UI", 14, 1},
    {"font_header", "Segoe UI", 12, 1},
    {"font_stats", "Segoe UI", 20, 1},
};
