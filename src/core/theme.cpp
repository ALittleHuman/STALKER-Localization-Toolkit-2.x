#include "xrkit/core/theme.h"

#include "xrkit/core/atomic_write.h"   // M-shell-2：user.ltx 走原子写
#include "xrkit/core/path.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace xrkit::core {
namespace {

// 生成物（tools/gen_theme_tables.py，门禁 [13] 的 `--check` 锁）
#include "theme_tables.inc"  // NOLINT

constexpr int kRoleCount = static_cast<int>(sizeof(kThemeRoles) / sizeof(kThemeRoles[0]));
constexpr int kFontCount = static_cast<int>(sizeof(kThemeFonts) / sizeof(kThemeFonts[0]));
constexpr int kBgRoleCount = static_cast<int>(sizeof(kBgRoles) / sizeof(kBgRoles[0]));

ThemeMode g_mode = ThemeMode::Dark;

// ★ 2.x 配色覆盖（2026-09-27 用户："配色也改一下"）—— **有意与 1.x 不同**：
//   配色属**表现层**，与行为口径无关；改这里**不动** 1.x 源、也**不手改**生成的
//   `theme_tables.inc`（手改会让 `gen_theme_tables.py --check` 变红）。
struct ColorOverride {
    const char* role;
    const char* dark;
    const char* light;
};
constexpr ColorOverride kOverrides[] = {
    {"bg", "#1e2124", "#f4f5f7"},
    {"surface", "#26292d", "#ffffff"},
    {"surface2", "#2b2f33", "#eef0f3"},
    {"border", "#3a3f45", "#d8dce1"},
    {"accent", "#4c9ed9", "#2b6cb0"},
    {"accent_hover", "#63b0e6", "#3a7cc0"},
    {"text", "#e8eaed", "#1f2328"},
    {"text_dim", "#a8b0b8", "#5a636b"},
    {"text_bright", "#ffffff", "#000000"},
    {"entry_bg", "#23262a", "#ffffff"},
    {"selected", "#31363b", "#dce7f5"},
    {"scroll_border", "#3a3f45", "#cdd3da"},
    {"sash", "#2f3338", "#e3e6ea"},
    {"sash_light", "#4c9ed9", "#2b6cb0"},
    {"red", "#d9534f", "#c0392b"},
    {"green", "#5cb85c", "#2f8f46"},
    {"yellow", "#e5c07b", "#9a7b1f"},
    {"orange", "#e08b4a", "#c46a1f"},
};

const char* override_color(ThemeMode mode, const char* role) {
    if (role == nullptr) return nullptr;
    for (const ColorOverride& o : kOverrides) {
        if (std::strcmp(o.role, role) == 0) {
            return (mode == ThemeMode::Light) ? o.light : o.dark;
        }
    }
    return nullptr;
}
double g_ui_scale = 1.0;

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

}  // namespace

// ── 令牌 ───────────────────────────────────────────────────────────────

int theme_role_count() { return kRoleCount; }

const char* theme_role(int index) {
    if (index < 0 || index >= kRoleCount) return nullptr;
    return kThemeRoles[index];
}

const char* theme_color(ThemeMode mode, const char* role) {
    if (role == nullptr) return nullptr;
        // ★ 先查 2.x 覆盖（kOverrides）；没覆盖才回到生成表
        if (const char* over = override_color(mode, role); over != nullptr) return over;
    for (int i = 0; i < kRoleCount; ++i) {
        if (std::strcmp(kThemeRoles[i], role) != 0) continue;
        return (mode == ThemeMode::Light) ? kThemeLight[i] : kThemeDark[i];
    }
    return nullptr;
}

ThemeMode theme_mode() { return g_mode; }
void set_theme_mode(ThemeMode mode) { g_mode = mode; }

const char* theme_mode_name(ThemeMode mode) {
    return (mode == ThemeMode::Light) ? "light" : "dark";
}

bool theme_mode_from_name(const std::string& name, ThemeMode& out) {
    const std::string lower = [&name] {
        std::string s = name;
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    if (lower == "dark") {
        out = ThemeMode::Dark;
        return true;
    }
    if (lower == "light") {
        out = ThemeMode::Light;
        return true;
    }
    return false;
}

const char* theme_color_current(const char* role) { return theme_color(g_mode, role); }

int theme_bg_role_count() { return kBgRoleCount; }
const char* theme_bg_role(int index) {
    if (index < 0 || index >= kBgRoleCount) return nullptr;
    return kBgRoles[index];
}

int theme_font_count() { return kFontCount; }
const ThemeFontSpec& theme_font(int index) {
    static const ThemeFontSpec kFallback{"font", "Segoe UI", 12, false};
    if (index < 0 || index >= kFontCount) return kFallback;
    // 生成表里的行结构名是 `ThemeFontRow`（避开与公开头同名的"不明确符号"）
    static thread_local ThemeFontSpec row;
    row = ThemeFontSpec{kThemeFonts[index].role, kThemeFonts[index].family,
                        kThemeFonts[index].size, kThemeFonts[index].bold != 0};
    return row;
}

// ── HiDPI ─────────────────────────────────────────────────────────────

void set_ui_scale(double value) { g_ui_scale = (value > 0.0) ? value : 1.0; }
double ui_scale() { return g_ui_scale; }

int px(int value) {
    // 1.x `px()`：`int(round(value * _UI_SCALE))`（四舍六入五成双，与 Python round 同口径）
    const double scaled = static_cast<double>(value) * g_ui_scale;
    return static_cast<int>(std::nearbyint(scaled));
}

// ── user.ltx ──────────────────────────────────────────────────────────

LtxData parse_ltx(const std::string& text) {
    LtxData data;
    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = text.find('\n', pos);
        const std::string raw =
            text.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? text.size() + 1 : end + 1;

        const std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (std::find(data.sections.begin(), data.sections.end(), section) ==
                data.sections.end()) {
                data.sections.push_back(section);
                data.items.emplace_back();
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (std::find(data.sections.begin(), data.sections.end(), section) ==
            data.sections.end()) {
            data.sections.push_back(section);
            data.items.emplace_back();
        }
        const size_t index =
            static_cast<size_t>(std::find(data.sections.begin(), data.sections.end(), section) -
                                data.sections.begin());
        auto& bucket = data.items[index];
        bool replaced = false;
        for (auto& kv : bucket) {
            if (kv.first == key) {
                kv.second = value;
                replaced = true;
                break;
            }
        }
        if (!replaced) bucket.emplace_back(key, value);
    }
    return data;
}

std::string render_ltx(const LtxData& data) {
    std::string out;
    bool first = true;
    for (size_t i = 0; i < data.sections.size(); ++i) {
        if (!first) out += "\n\n";
        first = false;
        out += "[" + data.sections[i] + "]\n";
        bool first_item = true;
        for (const auto& kv : data.items[i]) {
            if (!first_item) out += "\n";
            first_item = false;
            out += kv.first + " = " + kv.second;
        }
    }
    if (!first) out += "\n";
    return out;
}

std::string app_dir() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::filesystem::path exe(buffer);
        return path_to_utf8(exe.parent_path());
    }
#endif
    return ".";
}

std::string user_ltx_path() {
#if defined(_WIN32)
    const std::string dir = app_dir();
    if (dir != ".") return path_to_utf8(path_from_utf8(dir) / L"user.ltx");
#endif
    return "user.ltx";
}

LtxData load_user_config() {
    std::ifstream in(path_from_utf8(user_ltx_path()), std::ios::binary);
    if (!in) return LtxData{};
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return parse_ltx(text);
}

std::string load_user_value(const std::string& section, const std::string& key,
                            const std::string& fallback) {
    const LtxData data = load_user_config();
    for (size_t i = 0; i < data.sections.size(); ++i) {
        if (data.sections[i] != section) continue;
        for (const auto& kv : data.items[i]) {
            if (kv.first == key) return kv.second;
        }
    }
    return fallback;
}

bool save_user_values(const std::string& section,
                      const std::vector<std::pair<std::string, std::string>>& values) {
    LtxData data = load_user_config();
    size_t index = static_cast<size_t>(
        std::find(data.sections.begin(), data.sections.end(), section) - data.sections.begin());
    if (index >= data.sections.size()) {
        data.sections.push_back(section);
        data.items.emplace_back();
    }
    for (const auto& kv : values) {
        auto& bucket = data.items[index];
        bool replaced = false;
        for (auto& item : bucket) {
            if (item.first == kv.first) {
                item.second = kv.second;
                replaced = true;
                break;
            }
        }
        if (!replaced) bucket.push_back(kv);
    }
    // ★★ 2026-10-06（M-shell-2）：**裸 `ofstream(trunc)` → `core::write_file_atomic`**。
    //   修前写到一半失败/断电就留下半截 user.ltx，而它是"下次启动继承主题与界面大小"的
    //   唯一载体（半截文件 ⇒ 偏好静默丢失，而且下次启动读出来是垃圾值）。改走公共原子写：
    //   同目录 `.part` → 写后自校验 → 同盘 rename；任一环失败先 unlink 并返回 false
    //   （**零静默**：调用方拿 false 去报错，不留"看起来存在"的坏文件）。
    //   注：**丢注释/空行是既定口径**（与 1.x 一致，审计里也写明"别改"）——这里只换写法。
    const std::string text = to_windows_newlines(render_ltx(data));
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    std::string error;
    return write_file_atomic(user_ltx_path(), bytes, /*atomic=*/true, error,
                             WriteVerify::Streamed);
}

bool save_user_value(const std::string& section, const std::string& key,
                     const std::string& value) {
    return save_user_values(section, {{key, value}});
}

// ── 用户偏好：主题 / 界面大小（键名与读写形态见 core/theme.h 的说明）──

ThemeMode load_theme_mode(ThemeMode fallback) {
    const std::string raw = load_user_value(kPrefSection, kPrefModeKey, "");
    ThemeMode mode = fallback;
    if (theme_mode_from_name(raw, mode)) return mode;
    return fallback;   // 缺键/垃圾值 ⇒ 默认（口径：**不改默认值**）
}

bool save_theme_mode(ThemeMode mode) {
    return save_user_value(kPrefSection, kPrefModeKey, theme_mode_name(mode));
}

double load_ui_scale(double fallback) {
    const std::string raw = trim(load_user_value(kPrefSection, kPrefScaleKey, ""));
    if (raw.empty()) return fallback;
    // 只认十进制浮点（`strtod` 的 `end` 必须走到串尾 ⇒ "1.1abc" 不算）
    char* end = nullptr;
    const double value = std::strtod(raw.c_str(), &end);
    if (end == nullptr || *end != '\0') return fallback;
    if (!(value > 0.0)) return fallback;
    // ★★ 2026-10-06（审计处理批·2-4 / §5.8 / A2-9）：修前这里只挡了"非正数"，
    //   而 `strtod("1e400")` 返回 **+∞** 且 `*end == '\0'` ⇒ 一路通过 ⇒
    //   `set_ui_scale(∞)` ⇒ `px()` 里 `static_cast<int>(nearbyint(value * ∞))`
    //   是 **double→int 越界转换（UB）**；`shell/theme.cpp:568` 的 `(基准+2.0)*ui_scale()`
    //   也把字号算成 ∞。`theme.h:95` 的口径本来就写着"**越界**/垃圾值一律当作用户没选过
    //   ⇒ 回落屏幕自适应" —— 这是**漏写**，不是有意。
    //   上界 4.0：正常人不可能的倍率（DPI 档位封顶也在个位数），超过即按垃圾值处理。
    constexpr double kMaxUiScale = 4.0;
    if (!std::isfinite(value) || value > kMaxUiScale) return fallback;
    return value;
}

bool save_ui_scale(double scale) {
    if (!(scale > 0.0)) return false;
    // 十进制、最多 4 位小数、去掉尾随 0（`1.1` 而不是 `1.1000`）—— 只影响可读性
    std::string text = std::to_string(scale);
    const size_t dot = text.find('.');
    if (dot != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text.empty()) text = "1";
    return save_user_value(kPrefSection, kPrefScaleKey, text);
}

std::string to_windows_newlines(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 16 + 8);
    for (char c : text) {
        if (c == '\n') out.push_back('\r');
        out.push_back(c);
    }
    return out;
}

}  // namespace xrkit::core
