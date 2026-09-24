#include "xrkit/engine/font/package.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/font/charset.h"
#include "xrkit/engine/font/render.h"
#include "xrkit/engine/font/xml_bytes.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace xrkit::engine::font {
namespace {

// 生成物（tools/gen_font_templates.py，门禁第 [13] 步 --check 锁）
#include "ltx_templates.inc"  // NOLINT

bool write_bytes(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream f(core::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

bool write_text(const std::string& path, const std::string& text) {
    return write_bytes(path, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(core::path_from_utf8(path), std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

const GameTemplate* find_game(const std::string& game) {
    for (const GameTemplate& t : kGameTemplates) {
        if (game == t.game) return &t;
    }
    return nullptr;
}

// 1.x `build_package`：`os.path.join(out_dir, "gamedata", ...)`。
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

void remove_tree(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(core::path_from_utf8(path), ec);
}

// Python 的 `str.replace`：全局替换（不是只替第一处）。
void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

}  // namespace

PackageResult build_package(const PackageOptions& opt) {
    PackageResult res;
    const GameTemplate* game = find_game(opt.game);
    if (game == nullptr) {
        res.error = "未知游戏版本：" + opt.game;
        return res;
    }
    CharsetResult cs;
    if (!extract_chars(opt.xml_dir, opt.full_cyrillic, cs)) {
        res.error = "xml 目录不存在：" + opt.xml_dir;
        return res;
    }
    if (cs.chars.empty()) {
        res.error = "xml 目录为空或没有 XML 文件";
        return res;
    }

    // ── 字体守卫（**在创建任何目录之前**）：字体画不出汉字/西里尔时，整套图集会是同一个
    //    `.notdef` 方框却照样报"完成" —— 这里直接拒绝，且不留半成品 gamedata。
    {
        const std::vector<uint32_t> miss = font_missing_chars(opt.font_path, cs.chars);
        if (!miss.empty()) {
            std::string list;
            for (size_t i = 0; i < miss.size() && i < 12; ++i) {
                list += "U+" + std::to_string(miss[i]) + " ";
            }
            res.error = "所选字体画不出这些字符（图集会全部变成同一个方框）：" + list +
                        "—— 请改用 msyh / simsun / simhei 等含中日韩字形的字体";
            return res;
        }
        const std::vector<uint32_t> req = required_chars();
        const std::vector<uint32_t> req_miss = font_unrenderable(opt.font_path, req);
        if (!req_miss.empty()) {
            res.warnings.push_back("所选字体缺 " + std::to_string(req_miss.size()) +
                                   " 个必需字形（西里尔/特殊字符，简繁替换也补不了）："
                                   "正文里出现这些字符时会显示空白");
        }
    }

    const std::string out_gd = join(opt.out_dir, "gamedata");
    const std::string tex_dir = join(join(out_gd, "textures"), "ui");
    const std::string cfg_root = join(out_gd, game->cfg_dir);
    const std::string text_dir = join(join(cfg_root, "text"), opt.lang);
    std::error_code ec;
    std::filesystem::create_directories(core::path_from_utf8(tex_dir), ec);
    std::filesystem::create_directories(core::path_from_utf8(text_dir), ec);
    if (ec) {
        res.error = "建目录失败：" + ec.message();
        return res;
    }

    // ── 字库（10 个字号；任一处失败就把 gamedata 整个删掉，绝不留半成品）
    for (const FontSize& slot : kFontSizes) {
        if (!opt.only_sizes.empty() &&
            std::find(opt.only_sizes.begin(), opt.only_sizes.end(), slot.size) ==
                opt.only_sizes.end()) {
            continue;
        }
        const int render_size = slot.size + opt.offset;
        FontProduct product;
        if (!render_font(opt.font_path, cs.chars, render_size, slot.x2 != 0, game->dds_fmt,
                         product)) {
            res.error = "字号 " + std::to_string(slot.size) + " 渲染失败：" + product.error;
            remove_tree(out_gd);
            return res;
        }
        const std::string base =
            join(tex_dir, "ui_font_" + std::to_string(slot.size) + opt.suffix);
        if (!write_bytes(base + ".dds", product.dds.data(), product.dds.size())) {
            res.error = "写不出 " + base + ".dds";
            remove_tree(out_gd);
            return res;
        }
        // 1.x：`open(path, "w", encoding="ascii")` ⇒ 文本模式把 \n 翻成 \r\n
        if (!write_text(base + ".ini", to_windows_newlines(product.ini))) {
            res.error = "写不出 " + base + ".ini";
            remove_tree(out_gd);
            return res;
        }
        ++res.fonts_written;
    }

    // ── XML：只扫**顶层**（1.x `sorted(os.listdir(xml_dir))`，与 extract_chars 的递归不同），
    //    一律转无 BOM UTF-8，并把第一条声明的 encoding 改成 utf-8。
    std::vector<std::string> xmls;
    for (std::filesystem::directory_iterator it(core::path_from_utf8(opt.xml_dir), ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string name = core::path_to_utf8(it->path().filename());
        if (name.size() < 4) continue;
        std::string tail = name.substr(name.size() - 4);
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == ".xml") xmls.push_back(name);
    }
    std::sort(xmls.begin(), xmls.end());
    for (const std::string& name : xmls) {
        const std::string src = join(opt.xml_dir, name);
        const std::string dst = join(text_dir, name);
        const std::vector<uint8_t> raw = read_bytes(src);
        std::string text;
        std::string enc;
        bool trusted = false;
        if (!decode_xml_bytes(raw, text, enc, trusted)) {
            // 1.x 在这一步有 `raw.decode("windows-1251", errors="replace")` 兜底，但那条路
            // 实际上不可达（decode_bytes 的 latin-1 保底永不失败）。这里**报错而不是静默
            // 塞原始字节** —— 原样写出去只会得到一个看着像 XML 的坏文件。
            res.error = "XML 解不出来（本实现认为 1.x 的兜底分支不可达）：" + name;
            remove_tree(out_gd);
            return res;
        }
        if (!trusted) res.warnings.push_back(name + ": " + enc);
        const std::string body = rewrite_xml_declaration_encoding(text);
        if (!write_text(dst, body)) {
            res.error = "写不出 " + dst;
            remove_tree(out_gd);
            return res;
        }
        ++res.xml_written;
    }

    // ── fonts.ltx + localization.ltx
    // ★ 1.x 用 `open(..., "w", encoding="utf-8")`（**文本模式**）⇒ \n 翻成 \r\n；
    //   而 XML 那批用的是 `newline=""`（不翻译）。两者不一样，别写反了。
    std::string loc = game->localization_ltx;
    replace_all(loc, "_#LANG#", opt.suffix);
    replace_all(loc, "#LANG#", opt.lang);
    if (!write_text(join(cfg_root, "fonts.ltx"), to_windows_newlines(game->fonts_ltx)) ||
        !write_text(join(cfg_root, "localization.ltx"), to_windows_newlines(loc))) {
        res.error = "写不出 fonts.ltx / localization.ltx";
        remove_tree(out_gd);
        return res;
    }

    res.ok = true;
    res.out_gamedata = out_gd;
    return res;
}

}  // namespace xrkit::engine::font
