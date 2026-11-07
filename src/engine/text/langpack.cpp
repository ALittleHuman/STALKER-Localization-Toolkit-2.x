#include "xrkit/engine/text/langpack.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/overlay.h"
#include "xrkit/engine/text/extract.h"   // 遍历只有一套：fs::collect_tree()

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace xrkit::engine::text {
namespace {

namespace fs = std::filesystem;

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool write_all(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// 空表归一：把 `<string_table/>`、`<string_table />`、`<string_table></string_table>`
// 统一成 `<string_table></string_table>`（用户要求"空表也要写"）。
// 只在**整篇没有任何 <string 条目**时才改写，避免碰有内容的大文件。
bool normalize_empty_table(std::string& xml) {
    const bool has_entries = xml.find("<string ") != std::string::npos ||
                             xml.find("<string\t") != std::string::npos ||
                             xml.find("<string\n") != std::string::npos;
    if (has_entries) return false;
    size_t pos = xml.find("<string_table/>");
    if (pos != std::string::npos) {
        xml.replace(pos, std::string("<string_table/>").size(), "<string_table></string_table>");
        return true;
    }
    pos = xml.find("<string_table />");
    if (pos != std::string::npos) {
        xml.replace(pos, std::string("<string_table />").size(), "<string_table></string_table>");
        return true;
    }
    // 已经是 `<string_table></string_table>` 但中间只有空白 ⇒ 也归一（去掉空白）
    pos = xml.find("<string_table>");
    if (pos != std::string::npos) {
        const size_t open_end = pos + std::string("<string_table>").size();
        const size_t close = xml.find("</string_table>", open_end);
        if (close != std::string::npos) {
            const std::string inner = xml.substr(open_end, close - open_end);
            const bool blank = std::all_of(inner.begin(), inner.end(),
                                           [](unsigned char c) { return std::isspace(c) != 0; });
            if (blank) {
                xml.replace(pos, close + std::string("</string_table>").size() - pos,
                            "<string_table></string_table>");
                return true;
            }
        }
    }
    return false;
}

// 递归拷目录（只拷文件；保留相对结构）
// ★ 遍历**转调** `fs::collect_tree()`（D2：递归遍历件全仓只有一套）——
//   自己再写一遍 `recursive_directory_iterator` 会被 `walk-one` 判红（实测过）。
void copy_tree(const fs::path& src, const fs::path& dst, bool overwrite, int& copied, int& skipped,
               std::vector<std::string>& warnings) {
    const std::string root = core::path_to_utf8(src);
    xrkit::engine::fs::WalkReport report;
    const std::vector<xrkit::engine::fs::TreeItem> items =
        xrkit::engine::fs::collect_tree(root, &report);
    for (const std::string& err : report.errors) {
        warnings.push_back("遍历记账：" + err);
    }
    for (const auto& item : items) {
        if (item.is_dir) continue;
        const fs::path target = dst / core::path_from_utf8(item.path);
        std::error_code ec;
        if (is_file(target) && !overwrite) {
            ++skipped;
            warnings.push_back("已存在，拒绝覆盖：" + core::path_to_utf8(target));
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const fs::path source = item.source_path.empty()
                                    ? (src / core::path_from_utf8(item.path))
                                    : core::path_from_utf8(item.source_path);
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            warnings.push_back("拷不出来：" + core::path_to_utf8(target));
            continue;
        }
        ++copied;
    }
}

}  // namespace

namespace {

// ★★ 2026-10-28（用户）：「有出现一段文本没有换行过的，超过上限需要弹警告并且写日志」。
//   口径：**不含换行**且 > 1024 码位（有换行的放宽到 2048）；日志形态
//   `长文本：<文件>|<id>|<码位数>|<前 60 字符>` —— 用户拿这条去原文里定位修改。
constexpr size_t kLongTextLimit = 1024;   // 用户 2026-10-28：>512 不算超限

void scan_long_texts(const std::string& xml, const std::string& file_label,
                     std::vector<std::string>* warnings) {
    // 只认 <string id="...">…<text>…</text>：id 取最近的 id=，文本取 <text> 内层
    size_t pos = 0;
    std::string current_id;
    while (true) {
        const size_t id_pos = xml.find("id=\"", pos);
        const size_t text_pos = xml.find("<text>", pos);
        if (text_pos == std::string::npos) break;
        if (id_pos != std::string::npos && id_pos < text_pos) {
            const size_t end = xml.find('"', id_pos + 4);
            if (end != std::string::npos) current_id = xml.substr(id_pos + 4, end - id_pos - 4);
        }
        const size_t close = xml.find("</text>", text_pos);
        if (close == std::string::npos) break;
        const std::string body = xml.substr(text_pos + 6, close - text_pos - 6);
        size_t cp = 0;
        bool ascii = true;
        for (unsigned char ch : body) {
            if (ch < 0x80) { ++cp; } else if ((ch & 0xC0) != 0x80) { ++cp; ascii = false; }
        }
        (void)ascii;
        const bool has_nl = body.find('\n') != std::string::npos;
        const size_t limit = has_nl ? kLongTextLimit * 2 : kLongTextLimit;
        if (cp > limit) {
            std::string head = body.substr(0, 180);
            for (char& ch : head) if (ch == '\n') ch = ' ';
            warnings->push_back("长文本：" + file_label + "|" + current_id + "|" +
                                std::to_string(cp) + "|" + head);
        }
        pos = close + 7;
    }
}

}  // namespace

LangPackBuildResult build_language_pack(const LangPackBuildOptions& options) {
    LangPackBuildResult out;
    const fs::path texts = core::path_from_utf8(options.texts_dir);
    if (!is_dir(texts)) {
        out.error = "源文本目录不存在：" + options.texts_dir;
        return out;
    }
    // 语言目录判定（三种形状都认）：
    //   ① 直接给 `…/rus/`（里面就是 .xml）⇒ lang = 目录名；
    //   ② 给 `…/text/`（多语言父目录）+ `--lang rus` ⇒ 下沉 `rus/`；
    //   ③ 给 `…/text/` 且没给 --lang ⇒ 里面只有**一个**子目录时下沉（其余情况必须给 --lang）。
    fs::path texts_root = texts;
    std::string lang = options.lang;
    int xml_here = 0;
    std::vector<fs::path> subdirs;
    {
        std::error_code ec;
        for (fs::directory_iterator it(texts, ec), end; !ec && it != end; it.increment(ec)) {
            if (is_file(it->path()) && lower(it->path().extension().string()) == ".xml") ++xml_here;
            if (is_dir(it->path())) subdirs.push_back(it->path());
        }
    }
    if (xml_here == 0 && !subdirs.empty()) {
        fs::path chosen = subdirs.front();
        if (!lang.empty()) {
            for (const auto& d : subdirs) {
                if (core::path_to_utf8(d.filename()) == lang) chosen = d;
            }
        } else if (subdirs.size() == 1) {
            lang = core::path_to_utf8(subdirs.front().filename());
        }
        texts_root = chosen;
    }
    if (lang.empty()) lang = core::path_to_utf8(texts_root.filename());
    if (lang.empty()) {
        out.error = "推不出语言目录名（给 --lang）";
        return out;
    }
    if (!options.include_texts && !options.include_rewritten) {
        out.error = "两个产物（文本 XML / 改写后的脚本与配置）至少勾一个";
        return out;
    }

    // ★ 2026-11-05（P1-3 `--pack-layout`）：标准布局 = <out_root>/gamedata/…；
    //   默认仍是 <out_root>/<lang>/…（保持既有调用方行为）。
    fs::path out_dir;
    if (options.pack_layout) {
        const fs::path root = core::path_from_utf8(options.out_root);
        // ★ 边界（HA）：输出根本身已经是 `…/gamedata` ⇒ 不许再拼一层（避免 gamedata/gamedata）
        const std::string leaf = core::path_to_utf8(root.filename());
        out_dir = (leaf == "gamedata") ? root : (root / "gamedata");
    } else {
        out_dir = core::path_from_utf8(options.out_root) / lang;
    }
    out.out_dir = core::path_to_utf8(out_dir);
    out.lang = lang;
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        out.error = "建不了输出目录：" + out.out_dir;
        return out;
    }

    if (options.include_texts) {
        const fs::path dst_texts = out_dir / "configs" / "text" / lang;
        for (fs::directory_iterator it(texts_root, ec), end; !ec && it != end; it.increment(ec)) {
            if (!is_file(it->path())) continue;
            if (lower(it->path().extension().string()) != ".xml") continue;
            const fs::path dst = dst_texts / it->path().filename();
            if (is_file(dst) && !options.overwrite) {
                ++out.skipped_existing;
                out.warnings.push_back("已存在，拒绝覆盖：" + core::path_to_utf8(dst));
                continue;
            }
            std::string xml = read_all(it->path());
            scan_long_texts(xml, core::path_to_utf8(it->path().filename()), &out.warnings);
            if (normalize_empty_table(xml)) ++out.empty_tables;
            if (!write_all(dst, xml)) {
                out.warnings.push_back("写不出去：" + core::path_to_utf8(dst));
                continue;
            }
            ++out.xml_written;
        }
    }

    if (options.include_rewritten && !options.rewritten.empty()) {
        const fs::path rew = core::path_from_utf8(options.rewritten);
        if (!is_dir(rew)) {
            out.warnings.push_back("提取产物目录不存在（跳过后半）：" + options.rewritten);
        } else {
            int copied = 0;
            // ★★ 2026-10-29（交接区审计 T-P0-2）：STALKER 读的是 `gamedata/configs/gameplay/`，
            //   `gameplay/` 平铺在根上游戏根本不认。这里按分支落点：
            //     rew/scripts/**  → <out>/scripts/**
            //     rew/gameplay/** → <out>/configs/gameplay/**
            for (const char* branch : {"scripts", "gameplay"}) {
                const fs::path src_branch = rew / branch;
                if (!is_dir(src_branch)) continue;
                const fs::path dst_branch = (std::string(branch) == "gameplay")
                                                ? out_dir / "configs" / "gameplay"
                                                : out_dir / "scripts";
                copy_tree(src_branch, dst_branch, options.overwrite, copied,
                          out.skipped_existing, out.warnings);
            }
            out.rewritten_copied = copied;
        }
    }

    if (out.xml_written == 0 && out.rewritten_copied == 0 && out.skipped_existing == 0) {
        out.error = "一个文件都没写（源目录里没有 .xml？）";
        return out;
    }
    out.ok = true;
    return out;
}




// ── ★★ 2026-10-27：从"非空一侧"直接装配（提取 + 摆盘两步都在引擎层）─────────
LangPackFromGameResult build_language_pack_from_game(const LangPackFromGameOptions& options) {
    LangPackFromGameResult out;
    const fs::path src = core::path_from_utf8(options.source_dir);
    if (!is_dir(src)) {
        out.error = "源目录不存在：" + options.source_dir;
        return out;
    }
    // 临时工作区（提取产物 + 文本 XML 都先落这里，最后再摆盘）
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) tmp = fs::path(".");
    const fs::path work = tmp / ("xrkit_lp_" + std::to_string(static_cast<unsigned long long>(
                                                  std::hash<std::string>{}(options.source_dir))) +
                                 "_" + std::to_string(std::time(nullptr)));
    const fs::path rew = work / "rewritten";
    const fs::path texts = work / "texts";
    fs::create_directories(rew, ec);
    fs::create_directories(texts, ec);

    // ① 提取：scripts / gameplay 两个分支，谁在就抽谁
    ExtractOptions eopt;
    eopt.prefix = "gs";
    for (const auto& pair : {std::make_pair("scripts", ExtractKind::Scripts),
                             std::make_pair("gameplay", ExtractKind::Gameplay)}) {
        const fs::path branch = src / pair.first;
        if (!is_dir(branch)) continue;
        // 目标按**分支名**落地：extract_dir 把改写后的源写进 target 的**根**，
        // 标准语言包要的是 `scripts/...` / `gameplay/...` ⇒ 这里给 target = rew/<分支>。
        const fs::path branch_out = rew / pair.first;
        fs::create_directories(branch_out, ec);
        const ExtractStats stats = extract_dir(core::path_to_utf8(branch),
                                               core::path_to_utf8(branch_out),
                                               core::path_to_utf8(texts), eopt);
        if (!stats.ok) {
            out.error = std::string("提取失败（") + pair.first + "）：" + stats.error;
            fs::remove_all(work, ec);
            return out;
        }
        out.extracted_files += stats.files;
        out.extracted_strings += stats.strings;
        for (const std::string& skip : stats.skipped) out.warnings.push_back("提取跳过：" + skip);
    }

    // ② 源里若自带文本目录，也一起收（语言包可能已经是"文本 + 字体"形态）
    fs::path lang_texts = src / "configs" / "text" / options.lang;
    if (!is_dir(lang_texts)) lang_texts = src / "text" / options.lang;
    if (is_dir(lang_texts)) {
        for (fs::directory_iterator it(lang_texts, ec), end; !ec && it != end; it.increment(ec)) {
            if (!is_file(it->path())) continue;
            if (lower(it->path().extension().string()) != ".xml") continue;
            fs::copy_file(it->path(), texts / it->path().filename(),
                          fs::copy_options::overwrite_existing, ec);
        }
    }

    // ③ 摆盘（复用上一刀那唯一一份实现）
    LangPackBuildOptions bopt;
    bopt.texts_dir = core::path_to_utf8(texts);
    bopt.rewritten = (options.include_rewritten && is_dir(rew)) ? core::path_to_utf8(rew) : std::string();
    bopt.out_root = options.out_root;
    bopt.lang = options.lang;
    bopt.include_texts = options.include_texts;
    bopt.include_rewritten = options.include_rewritten;
    bopt.overwrite = options.overwrite;
    out.pack = build_language_pack(bopt);
    for (const std::string& warn : out.pack.warnings) out.warnings.push_back(warn);
    fs::remove_all(work, ec);
    if (!out.pack.ok) {
        out.error = out.pack.error;
        return out;
    }
    out.ok = true;
    return out;
}

}  // namespace xrkit::engine::text


namespace xrkit::engine::text {

namespace {

bool collect_keep(const std::string& rel, const std::vector<std::string>& langs, bool include_map) {
    const auto lang_ok = [&langs](const std::string& name) {
        return langs.empty() ||
               std::find(langs.begin(), langs.end(), name) != langs.end();
    };
    // gamedata/configs/text/<lang>/...
    const std::string text = "gamedata/configs/text/";
    if (rel.rfind(text, 0) == 0) {
        const std::size_t slash = rel.find('/', text.size());
        if (slash != std::string::npos && lang_ok(rel.substr(text.size(), slash - text.size()))) {
            return true;
        }
    }
    if (rel.rfind("gamedata/fonts/", 0) == 0) return true;
    if (rel == "gamedata/configs/fonts.ltx" || rel == "gamedata/configs/localization.ltx") {
        return true;
    }
    if (include_map) {
        if (rel.rfind("gamedata/textures/map/", 0) == 0) return true;
        if (rel.rfind("gamedata/textures/ui_new_game_map", 0) == 0) return true;
    }
    // ixr_addons/<name>/configs/text/<lang>/... 与字体
    const std::string addon = "ixr_addons/";
    if (rel.rfind(addon, 0) == 0) {
        const std::size_t slash = rel.find('/', addon.size());
        if (slash == std::string::npos) return false;
        const std::string rest = rel.substr(slash + 1);
        const std::string atext = "configs/text/";
        if (rest.rfind(atext, 0) == 0) {
            const std::size_t s2 = rest.find('/', atext.size());
            if (s2 != std::string::npos && lang_ok(rest.substr(atext.size(), s2 - atext.size()))) {
                return true;
            }
        }
        if (rest.rfind("fonts/", 0) == 0) return true;
        if (rest == "configs/fonts.ltx" || rest == "configs/localization.ltx") return true;
    }
    return false;
}

}  // namespace

LangPackCollectResult collect_language_pack(const LangPackCollectOptions& options) {
    LangPackCollectResult out;
    std::error_code ec;
    const fs::path src = fs::weakly_canonical(core::path_from_utf8(options.source_root), ec);
    if (!is_dir(src)) {
        out.error = "源目录不存在（提取只管松散文件；容器请自行解包）：" + options.source_root;
        return out;
    }
    const fs::path dst = fs::weakly_canonical(core::path_from_utf8(options.out_root), ec);
    const auto inside = [](const fs::path& a, const fs::path& b) {
        std::error_code e;
        const fs::path rel = fs::relative(a, b, e);
        return !e && rel.string().rfind("..", 0) != 0 && !rel.empty();
    };
    if (dst == src || inside(src, dst) || inside(dst, src)) {
        out.error = "拒绝：语言包输出目录与源目录重叠（源只读）";
        return out;
    }

    xrkit::engine::fs::WalkReport report;
    const std::vector<xrkit::engine::fs::TreeItem> items = xrkit::engine::fs::collect_tree(core::path_to_utf8(src), &report);
    for (const std::string& err : report.errors) out.warnings.push_back("遍历：" + err);
    fs::create_directories(dst, ec);
    for (const xrkit::engine::fs::TreeItem& item : items) {
        if (item.is_dir) continue;
        if (!collect_keep(item.path, options.langs, options.include_map)) continue;
        const fs::path target = dst / core::path_from_utf8(item.path);
        if (fs::exists(target, ec) && !options.overwrite) {
            ++out.skipped_existing;
            out.warnings.push_back("已存在，跳过：" + item.path);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(src / core::path_from_utf8(item.path), target,
                      options.overwrite ? fs::copy_options::overwrite_existing
                                        : fs::copy_options::none, ec);
        if (ec) {
            out.warnings.push_back("复制失败：" + item.path + "（" + ec.message() + "）");
            continue;
        }
        ++out.copied;
    }
    out.out_root = core::path_to_utf8(dst);
    out.ok = out.copied > 0;
    if (!out.ok) out.error = "没有可收敛的语言包内容（检查 <lang> 与目录布局）";
    return out;
}

}  // namespace xrkit::engine::text
