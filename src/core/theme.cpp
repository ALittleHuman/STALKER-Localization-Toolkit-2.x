#include "xrkit/core/theme.h"

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

std::string user_ltx_path() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::filesystem::path exe(buffer);
        return path_to_utf8(exe.parent_path() / L"user.ltx");
    }
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
    std::ofstream out(path_from_utf8(user_ltx_path()), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string text = to_windows_newlines(render_ltx(data));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool save_user_value(const std::string& section, const std::string& key,
                     const std::string& value) {
    return save_user_values(section, {{key, value}});
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
