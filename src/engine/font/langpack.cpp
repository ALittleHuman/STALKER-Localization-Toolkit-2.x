#include "xrkit/engine/font/langpack.h"

#include "xrkit/core/path.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// 读整个 ltx（Cyrillic/UTF-8 都当字节读；键值只做 ASCII 比较）
std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}

struct RawSection {
    std::string name;
    std::string base;
    std::vector<std::pair<std::string, std::string>> kv;
};

// 只认 `[sec]` / `[sec]:base` / `k = v`；注释与未知行原样忽略（扫描不改文件）。
std::vector<RawSection> parse_ltx(const std::string& text) {
    std::vector<RawSection> out;
    std::stringstream ss(text);
    std::string line;
    RawSection* cur = nullptr;
    while (std::getline(ss, line)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            const auto close = s.find(']');
            if (close == std::string::npos) continue;
            RawSection sec;
            const std::string head = s.substr(1, close - 1);
            const auto colon = head.find(':');
            if (colon == std::string::npos) {
                sec.name = trim(head);
            } else {
                sec.name = trim(head.substr(0, colon));
                sec.base = trim(head.substr(colon + 1));
            }
            if (sec.name.empty()) continue;
            out.push_back(std::move(sec));
            cur = &out.back();
            continue;
        }
        if (cur != nullptr) {
            const auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            cur->kv.emplace_back(lower(trim(s.substr(0, eq))), trim(s.substr(eq + 1)));
        }
    }
    return out;
}

std::string kv_of(const RawSection& sec, const char* key) {
    for (const auto& [k, v] : sec.kv) {
        if (k == key) return v;
    }
    return {};
}

// 目录里有没有任意 `*.ttf`
int count_ext(const fs::path& dir, const std::string& ext) {
    if (!is_dir(dir)) return 0;
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_regular_file(it->path())) continue;
        if (lower(it->path().extension().string()) == ext) ++n;
    }
    return n;
}

// `textures/ui/ui_font_*.dds` 计数（相对 gamedata 根，与配置目录无关）
int count_ui_font_dds(const fs::path& gamedata) {
    const fs::path dir = gamedata / "textures" / "ui";
    if (!is_dir(dir)) return 0;
    int n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = lower(it->path().filename().string());
        if (name.rfind("ui_font_", 0) == 0 && lower(it->path().extension().string()) == ".dds") ++n;
    }
    return n;
}

}  // namespace

const char* profile_name(PackProfile profile) {
    switch (profile) {
        case PackProfile::Soc: return "soc";
        case PackProfile::ClearSky: return "cis";
        case PackProfile::Cop: return "cop";
        case PackProfile::IxRay: return "ixr";
        case PackProfile::Unknown: break;
    }
    return "unknown";
}

LangPackModel scan_lang_pack(const std::string& root) {
    LangPackModel model;
    fs::path base = core::path_from_utf8(root);
    if (!is_dir(base)) {
        model.evidence.push_back("目录不存在：" + root);
        return model;
    }
    // 归一化：给了游戏根就下沉到 gamedata；给了 gamedata 就原样
    if (!is_dir(base / "configs") && !is_dir(base / "config") && is_dir(base / "gamedata")) {
        base /= "gamedata";
    }
    model.root = core::path_to_utf8(base);

    // ① 配置目录（布局跟着包走）
    fs::path config_dir;
    for (const char* cd : {"configs", "config"}) {
        if (is_dir(base / cd)) {
            config_dir = base / cd;
            model.config_dir = cd;
            break;
        }
    }
    if (config_dir.empty()) {
        model.evidence.push_back("缺 configs/ 或 config/：这不是一个语言包");
        return model;
    }

    // ② 字体配置：矢量在 configs/fonts.ltx；位图在 configs/ui/fonts.ltx（两处都找）
    std::vector<RawSection> sections;
    for (const char* rel : {"fonts.ltx", "ui/fonts.ltx", "ui\\fonts.ltx"}) {
        const fs::path p = config_dir / rel;
        if (!is_file(p)) continue;
        const auto parsed = parse_ltx(read_text(p));
        if (parsed.empty()) continue;
        model.font_config = core::path_to_utf8(p);
        sections = parsed;
        if (std::string(rel) == "fonts.ltx") break;   // 优先矢量那份
    }
    if (sections.empty()) {
        model.evidence.push_back("包内没有字体配置（configs/fonts.ltx 与 configs/ui/fonts.ltx 都不在或为空）");
        return model;
    }

    // ③ lang：localization.ltx 的 [string_table] language
    model.lang = "rus";
    if (const fs::path loc = config_dir / "localization.ltx"; is_file(loc)) {
        for (const auto& sec : parse_ltx(read_text(loc))) {
            if (lower(sec.name) != "string_table") continue;
            const std::string v = kv_of(sec, "language");
            if (!v.empty()) {
                model.lang = v;
                model.lang_from_ltx = true;
            }
        }
    }

    // ④ 槽位表（原样收；继承/未知键都保留在 base 与 kv 里）
    for (const auto& sec : sections) {
        if (lower(sec.name) == "string_table") continue;
        LangPackSlot slot;
        slot.section = sec.name;
        slot.base = sec.base;
        slot.name = kv_of(sec, "name");
        slot.has_name = !slot.name.empty();
        const std::string size = kv_of(sec, "size");
        slot.size = size.empty() ? 0 : std::atoi(size.c_str());
        slot.opentype = kv_of(sec, "opentype");
        slot.shader = kv_of(sec, "shader");
        model.font_slots.push_back(std::move(slot));
    }

    // ⑤ 资源证据：矢量 fonts/<lang>/*.ttf vs 位图 textures/ui/ui_font_*.dds
    const fs::path ttf_dir = base / "fonts" / model.lang;
    model.ttf_count = count_ext(ttf_dir, ".ttf");
    if (model.ttf_count > 0) model.ttf_dir = core::path_to_utf8(ttf_dir);
    model.dds_count = count_ui_font_dds(base);

    // ⑥ 文本目录（布局跟着包走：configs/text/<lang>）
    const fs::path text_dir = config_dir / "text" / model.lang;
    if (is_dir(text_dir)) model.text_dir = core::path_to_utf8(text_dir);

    // ⑦ 判定（顺序写死：矢量优先；位图内部按 shader，再按配置目录）
    bool any_name = false;
    std::string shader_sample;
    for (const auto& slot : model.font_slots) {
        if (slot.has_name) any_name = true;
        if (shader_sample.empty() && !slot.shader.empty()) shader_sample = lower(slot.shader);
    }
    if (any_name && model.ttf_count > 0) {
        model.profile = PackProfile::IxRay;
        model.evidence.push_back("fonts.ltx 段里有 name=" + model.font_slots.front().name +
                                 "，且 fonts/" + model.lang + "/ 有 " +
                                 std::to_string(model.ttf_count) + " 个 .ttf ⇒ 矢量（IX-Ray）");
        if (model.dds_count > 0) {
            model.evidence.push_back("同时有 " + std::to_string(model.dds_count) +
                                     " 个 ui_font_*.dds ⇒ 按 IX-Ray 处理，dds 忽略（矢量优先）");
        }
    } else if (model.dds_count > 0) {
        if (shader_sample == "hud\\font" || shader_sample == "hud/font") {
            model.profile = PackProfile::Cop;
            model.evidence.push_back("没有 ttf；ui_font_*.dds=" + std::to_string(model.dds_count) +
                                     "，fonts.ltx 的 shader=hud\\font ⇒ CoP");
        } else if (model.config_dir == "config") {
            model.profile = PackProfile::Soc;
            model.evidence.push_back("没有 ttf；ui_font_*.dds=" + std::to_string(model.dds_count) +
                                     "，配置目录=config ⇒ SoC");
        } else {
            model.profile = PackProfile::ClearSky;
            model.evidence.push_back("没有 ttf；ui_font_*.dds=" + std::to_string(model.dds_count) +
                                     "，配置目录=configs 且 shader 不是 hud\\font ⇒ ClS");
        }
    } else {
        model.evidence.push_back("既没有 fonts/<lang>/*.ttf（0 个），也没有 textures/ui/ui_font_*.dds（0 个）"
                                 " ⇒ 认不出四档中的任何一档");
        // 仍然把能读到的槽位/文本交出去（供上层列出"缺什么"），但 profile=unknown
    }
    return model;
}

}  // namespace xrkit::engine::font
