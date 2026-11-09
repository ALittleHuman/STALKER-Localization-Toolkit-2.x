#include "xrkit/engine/text/ori.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/overlay.h"
#include "xrkit/engine/text/extract.h"
#include "xrkit/engine/text/layout.h"

#include <algorithm>
#include <utility>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>

namespace xrkit::engine::text {
namespace {

namespace fs = std::filesystem;
namespace efs = xrkit::engine::fs;

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 容器标记：目录里直接出现这些子目录之一 ⇒ 它是容器
bool has_container_markers(const fs::path& dir) {
    // `text` 也算容器标记：`gamedata/configs` 这种"只含 text/ui 的中间层"以前会被拒
    // （复审计 P1：纯文本/配置子树输入）。真正的纯文本目录（直接放 *.xml）走后面的兜底。
    static const char* kMarkers[] = {"configs", "config", "scripts", "gameplay", "fonts", "textures",
                                     "text"};
    std::error_code ec;
    for (const char* marker : kMarkers) {
        if (fs::is_directory(dir / marker, ec)) return true;
    }
    return false;
}

// 从 localization.ltx 读 `[string_table] language`
std::string lang_from_ltx(const fs::path& ltx) {
    if (!is_file(ltx)) return std::string();
    std::ifstream in(ltx, std::ios::binary);
    if (!in) return std::string();
    std::string line;
    bool first_line = true;
    bool in_section = false;
    std::string found;
    while (std::getline(in, line)) {
        std::string s = line;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (first_line) {
            // PowerShell/编辑器存出来的 UTF-8 常带 BOM；不去掉第一行 `[string_table]` 就认不出
            if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
                static_cast<unsigned char>(s[1]) == 0xBB &&
                static_cast<unsigned char>(s[2]) == 0xBF) {
                s = s.substr(3);
            }
            first_line = false;
        }
        const size_t hash = s.find(';');
        if (hash != std::string::npos) s = s.substr(0, hash);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        if (s.empty()) continue;
        if (s.front() == '[') {
            in_section = lower(s.substr(1, s.find(']') == std::string::npos ? std::string::npos
                                                                          : s.find(']') - 1)) ==
                         "string_table";
            continue;
        }
        if (!in_section) continue;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(s.substr(0, eq));
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key != "language") continue;
        std::string value = s.substr(eq + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
        found = value;
    }
    return found;
}

// 容器里的语言：localization.ltx → configs/text/<x> 的目录名 → options.lang → rus
// 返回 (语言, 证据来源)。证据来源要如实报：这就是"扫描结果进生成"的最小合同
// ——生成阶段不再自己 detect_langs，语言只可能来自这四种。
std::pair<std::string, std::string> container_lang(const fs::path& container,
                                                   const std::string& fallback) {
    // ★ 2026-11-09：配置目录名按 `configs`/`config` 探测（SoC 用 config），不再写死 configs。
    const std::string cfg = config_dir_name(container);
    if (!cfg.empty()) {
        if (const std::string lang = lang_from_ltx(container / cfg / "localization.ltx");
            !lang.empty()) {
            return {lang, "localization.ltx"};
        }
        std::error_code ec;
        const fs::path texts = container / cfg / "text";
        if (is_dir(texts)) {
            // 规则：localization.ltx 优先；其次先找 rus（原版语言），没有再排序取第一个（确定性）。
            std::vector<std::string> names;
            for (const auto& entry : fs::directory_iterator(texts, ec)) {
                if (entry.is_directory(ec)) {
                    names.push_back(core::path_to_utf8(entry.path().filename()));
                }
            }
            std::sort(names.begin(), names.end());
            for (const std::string& n : names) {
                if (n == "rus") return {n, "text_dir"};
            }
            if (!names.empty()) return {names.front(), "text_dir"};
        }
    }
    if (!fallback.empty()) return {fallback, "arg"};
    return {"rus", "fallback"};
}

// 整树复制（源只读；目标必须不存在或由调用方保证可写）
uint64_t copy_tree_into(const fs::path& src, const fs::path& dst,
                        std::vector<std::string>* warnings) {
    efs::WalkReport report;
    const std::vector<efs::TreeItem> items =
        efs::collect_tree(core::path_to_utf8(src), &report);
    for (const std::string& err : report.errors) {
        if (warnings != nullptr) warnings->push_back("遍历：" + err);
    }
    uint64_t copied = 0;
    std::error_code ec;
    for (const efs::TreeItem& item : items) {
        const fs::path target = dst / core::path_from_utf8(item.path);
        if (item.is_dir) {
            fs::create_directories(target, ec);
            continue;
        }
        const fs::path rel = core::path_from_utf8(item.path);
        fs::create_directories(target.parent_path(), ec);
        ec.clear();
        fs::copy_file(rel.is_absolute() ? rel : src / rel, target,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (warnings != nullptr) {
                warnings->push_back("复制失败：" + item.path + "（" + ec.message() + "）");
            }
            continue;
        }
        ++copied;
    }
    return copied;
}

// 单分支提取：把分支先摊到一个以 gameplay/ 或 scripts/ 结尾的临时目录，
// 再交给 extract_dir（它按目录名判 kind）—— 这样 configs/ui 这类分支也能复用同一份内核。
ExtractStats extract_branch(const fs::path& branch, ExtractKind kind, const fs::path& target,
                            const fs::path& texts_out, const OriOptions& options,
                            std::vector<std::string>* warnings,
                            const std::string& prefix_suffix = "") {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec) / "xrkit_ori_branch" /
                         (kind == ExtractKind::Gameplay ? "gameplay" : "scripts");
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    copy_tree_into(branch, tmp, warnings);
    ExtractOptions eopt;
    eopt.prefix = options.prefix + prefix_suffix;
    ExtractStats st = extract_dir(core::path_to_utf8(tmp), core::path_to_utf8(target),
                                  core::path_to_utf8(texts_out), eopt);
    for (const std::string& skip : st.skipped) {
        if (warnings != nullptr) warnings->push_back("提取跳过：" + skip);
    }
    fs::remove_all(tmp, ec);
    return st;
}

}  // namespace

OriProfile ori_profile_of(const OriResult& result) {
    OriProfile profile;
    profile.lang = result.lang;
    profile.lang_source = result.lang_source;
    profile.out_dir = result.out_dir;
    profile.containers = result.containers;
    return profile;
}

// ★ 2026-10-31（BETA.46 复核 P1）：直接拖 `.../configs/text/rus` 时 `rus` 不是已知目录，
//   前缀会退化成 `rus`；这里沿祖先链把已知目录一层层拼回来（`gamedata/configs/text/rus`）。
// 本层或下一层是否有 *.xml（`configs/text` 这种"只有语言子目录"的形态要下探一层）
bool has_xml_within(const fs::path& dir, int depth) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".xml") return true;
        if (depth > 0 && it->is_directory(ec) && has_xml_within(it->path(), depth - 1)) return true;
    }
    return false;
}

std::string prefix_from_known_ancestors(const fs::path& dir) {
    static const char* kKnown[] = {"gamedata", "configs", "text",    "fonts",    "textures",
                                   "scripts",  "gameplay", "ui",     "ixr_addons"};
    const auto known = [](const std::string& name) {
        for (const char* k : kKnown) {
            if (name == k) return true;
        }
        return false;
    };
    std::vector<std::string> chain{core::path_to_utf8(dir.filename())};
    for (fs::path parent = dir.parent_path();
         !parent.empty() && known(core::path_to_utf8(parent.filename()));
         parent = parent.parent_path()) {
        chain.insert(chain.begin(), core::path_to_utf8(parent.filename()));
        if (parent == parent.parent_path()) break;
    }
    std::string joined;
    for (const std::string& part : chain) {
        if (!joined.empty()) joined += "/";
        joined += part;
    }
    return joined;
}

namespace {
// ★ 2026-10-31（复审计 P1）：手拼 JSON 必须转义 —— 路径/语言里出现引号或控制字符时
//   `_xrkit_profile.json` 会坏，下游解析直接失败。
std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}
}  // namespace

std::string ori_profile_json(const OriProfile& profile) {
    std::string out = "{\"lang\":\"" + json_escape(profile.lang) + "\",\"lang_source\":\"" +
                      profile.lang_source + "\",\"out\":\"" + profile.out_dir +
                      "\",\"containers\":[";
    bool first = true;
    for (const OriContainer& c : profile.containers) {
        if (!first) out += ",";
        first = false;
        out += "{\"rel\":\"" + json_escape(c.rel.empty() ? std::string(".") : c.rel) +
               "\",\"lang\":\"" + json_escape(c.lang) + "\",\"lang_source\":\"" + c.lang_source +
               "\",\"kind\":\"" + json_escape(c.kind) + "\"" +
               ",\"has_text\":" + (c.has_text ? "true" : "false") +
               ",\"has_fonts\":" + (c.has_fonts ? "true" : "false") +
               ",\"has_scripts\":" + (c.has_scripts ? "true" : "false") +
               ",\"has_gameplay\":" + (c.has_gameplay ? "true" : "false") +
               ",\"extracted\":" + std::to_string(c.extracted) +
               ",\"rewritten\":" + std::to_string(c.rewritten) +
               ",\"texts\":" + std::to_string(c.texts) + "}";
    }
    return out + "]}";
}

std::string ori_profile_json(const OriResult& result) {
    return ori_profile_json(ori_profile_of(result));
}

OriResult build_ori_tree(const OriOptions& options) {
    OriResult out;
    const fs::path src = core::path_from_utf8(options.source_dir);
    const fs::path out_root = core::path_from_utf8(options.out_dir);
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        out.error = "源不存在：" + options.source_dir;
        return out;
    }

    // 源是单文件 ⇒ 向上找**最近的容器**，按那棵容器整树生成（规划 4.5 / 6）
    fs::path effective_src = src;
    std::string single_file_rel;
    if (is_file(src)) {
        fs::path parent = src.parent_path();
        fs::path container = parent;
        bool found = false;
        while (!parent.empty()) {
            if (has_container_markers(parent)) {
                container = parent;
                found = true;
                break;
            }
            if (parent == parent.parent_path()) break;
            parent = parent.parent_path();
        }
        if (!found) container = src.parent_path();   // 找不到容器 ⇒ 文件所在目录当最小容器
        std::error_code rel_ec;
        fs::path rel = fs::relative(src, container, rel_ec);
        single_file_rel = rel_ec ? src.filename().string() : core::path_to_utf8(rel);
        out.warnings.push_back("单文件输入：按最近容器 " + core::path_to_utf8(container) +
                               " 生成整树（该文件相对路径 " + single_file_rel + "）");
        effective_src = container;
    }

    // ── 输入/输出边界（与 IXR 生成器同一口径：绝不删输入）─────────────────
    const fs::path src_abs = fs::weakly_canonical(effective_src, ec);
    const fs::path out_abs = fs::weakly_canonical(out_root, ec);
    // ★★ 2026-10-30（交接区复审计 O-P0-2）：`weakly_canonical().string()` 在 Windows
    //   是反斜杠，原来拼 '/' 做前缀比较永远匹配不上子目录关系。改用成分级 relative。
    const bool overlap = (src_abs == out_abs) ||
                         (fs::relative(out_abs, src_abs, ec).string().rfind("..", 0) != 0 &&
                          !fs::relative(out_abs, src_abs, ec).empty()) ||
                         (fs::relative(src_abs, out_abs, ec).string().rfind("..", 0) != 0 &&
                          !fs::relative(src_abs, out_abs, ec).empty());
    if (overlap) {
        out.error = "拒绝：ori 与源目录重叠（" + options.out_dir + "）";
        return out;
    }
    if (fs::exists(out_abs, ec)) {
        // 目录遍历只走 `fs::collect_tree`（D2 锁：同类东西只有一套）
        efs::WalkReport out_report;
        const std::vector<efs::TreeItem> existing_items =
            efs::collect_tree(core::path_to_utf8(out_abs), &out_report);
        uint64_t existing = 0;
        for (const efs::TreeItem& item : existing_items) {
            if (!item.is_dir) ++existing;
        }
        if (existing > 0) {
            if (!options.clean) {
                out.error = "拒绝：ori 已有 " + std::to_string(existing) +
                            " 个文件（确认清空请给 clean/--clean）";
                return out;
            }
            fs::remove_all(out_abs, ec);
        }
    }
    fs::create_directories(out_abs, ec);

    // ── 容器发现：带标记的目录，且不取"已被更外层容器覆盖"的目录 ──────────
    efs::WalkReport report;
    const std::vector<efs::TreeItem> items =
        efs::collect_tree(core::path_to_utf8(effective_src), &report);
    for (const std::string& err : report.errors) out.warnings.push_back("遍历：" + err);

    std::vector<std::string> candidates;
    if (has_container_markers(effective_src)) candidates.push_back("");
    for (const efs::TreeItem& item : items) {
        if (!item.is_dir) continue;
        const fs::path dir = effective_src / core::path_from_utf8(item.path);
        if (has_container_markers(dir)) candidates.push_back(item.path);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const std::string& a, const std::string& b) {
                  const auto depth = [](const std::string& s) {
                      return std::count(s.begin(), s.end(), '/');
                  };
                  if (depth(a) != depth(b)) return depth(a) < depth(b);
                  return a < b;
              });
    // ★★ 2026-10-30（复审计 O-P0-1）：选中"根"的名字不能丢 —— 拖 `gamedata` 必须出
    //   `ori/gamedata/...`，拖 `ixr_addons` 必须出 `ori/ixr_addons/<addon>/...`。
    const std::string src_name = core::path_to_utf8(effective_src.filename());
    std::string out_prefix;
    if (has_container_markers(effective_src)) {
        out_prefix = src_name;
        const std::string parent_name =
            core::path_to_utf8(effective_src.parent_path().filename());
        if ((src_name == "configs" || src_name == "config") && parent_name == "gamedata") {
            out_prefix = "gamedata/" + src_name;
        }
    } else if (src_name == "ixr_addons" || src_name == "gamedata") {
        out_prefix = src_name;
    }

    std::vector<std::string> containers;
    for (const std::string& cand : candidates) {
        bool covered = false;
        for (const std::string& kept : containers) {
            if (kept.empty()) { covered = true; break; }
            if (cand == kept || cand.rfind(kept + "/", 0) == 0) { covered = true; break; }
        }
        if (!covered) containers.push_back(cand);
    }
    if (containers.empty()) {
        // ★ 复审计 P1：直接拖「纯文本目录」（`configs/text/<lang>` 这种）也被当容器处理。
        bool has_xml_directly = false;
        std::error_code xml_ec;
        for (fs::directory_iterator it(effective_src, xml_ec), end; !xml_ec && it != end;
             it.increment(xml_ec)) {
            if (it->is_regular_file(xml_ec) && lower(it->path().extension().string()) == ".xml") {
                has_xml_directly = true;
                break;
            }
        }
        if (!has_xml_directly && !has_xml_within(effective_src, 1)) {
            out.error = "源里找不到容器（没有 configs|config/scripts/gameplay/fonts/textures，"
                        "也不是纯文本目录）：" + options.source_dir;
            return out;
        }
        containers.push_back("");
        if (out_prefix.empty()) out_prefix = prefix_from_known_ancestors(effective_src);
    }

    uint64_t extract_failed = 0;

    // ── 每个容器：整树复制 → 提取 → 文本落盘 → 多语言复制 ──────────────────
    for (const std::string& rel : containers) {
        const fs::path container = rel.empty() ? effective_src
                                              : effective_src / core::path_from_utf8(rel);
        const std::string dst_rel =
            out_prefix.empty() ? rel
                               : (rel.empty() ? out_prefix : out_prefix + "/" + rel);
        const fs::path dst = dst_rel.empty() ? out_abs : out_abs / core::path_from_utf8(dst_rel);
        OriContainer info;
        info.rel = rel;
        // ★ 2026-11-09：本容器的配置目录名（configs 或 config）。
        const std::string cfg = config_dir_name(container);
        const std::string cfg_name = cfg.empty() ? "configs" : cfg;
        const auto lang_pair = container_lang(container, options.lang);
        info.lang = lang_pair.first;
        info.lang_source = lang_pair.second;
        // ★ 2026-11-09：无证据时不再静默按 rus —— 明确告知用户可显式指定语言。
        if (info.lang_source == "fallback") {
            out.warnings.push_back("容器 " + (rel.empty() ? std::string(".") : rel) +
                                   "：无法确定语言（无 localization.ltx / text 目录），"
                                   "暂按 " + info.lang + "；可用 --lang 指定");
        }
        info.has_text = is_dir(container / cfg_name / "text");
        info.has_fonts = is_dir(container / "fonts") || is_file(container / cfg_name / "fonts.ltx");
        info.has_scripts = is_dir(container / "scripts") ||
                           is_dir(container / cfg_name / "scripts");
        info.has_gameplay = is_dir(container / "gameplay") ||
                            is_dir(container / cfg_name / "gameplay");
        // ★ 复审计 P1：不是"有 fonts.ltx 就是 ixr" —— 必须有真 ttf；两者都有 ⇒ mixed
        bool has_ttf = false;
        {
            const fs::path fonts_dir = container / "fonts";
            if (is_dir(fonts_dir)) {
                // 遍历只走 collect_tree（D2 锁：同类遍历只有一套）
                std::vector<std::string> ttf_names;
                efs::WalkReport ttf_report;
                for (const efs::TreeItem& item :
                     efs::collect_tree(core::path_to_utf8(fonts_dir), &ttf_report)) {
                    if (item.is_dir) continue;
                    const std::string ext = lower(fs::path(item.path).extension().string());
                    if (ext == ".ttf" || ext == ".otf") {
                        has_ttf = true;
                        break;
                    }
                }
            }
        }
        const bool has_bitmap = is_dir(container / "textures" / "ui");
        info.kind = (has_ttf && has_bitmap) ? "mixed"
                    : has_ttf               ? "ixr"
                    : has_bitmap            ? "bitmap"
                                            : "none";

        fs::create_directories(dst, ec);
        if (!options.texts_only) {
            out.files_copied += copy_tree_into(container, dst, &out.warnings);
        } else {
            // ★ 提取模式：**不复制整树**（用户口径：提取就是提取文本，不是解包）
            out.warnings.push_back("提取模式：不做整树复制（只产文本与被改写的脚本/配置）");
        }

        // ★ 复审计 P1：容器本身就是 `configs`（或 `text`）时，不能再往里套一层
        //   `configs/text` —— 否则出现 `ori/gamedata/configs/configs/text/rus` 这种幻影目录。
        const std::string container_name = core::path_to_utf8(container.filename());
        // ★ 2026-10-31：四态落点 —— 语言目录 / text 目录 / configs 目录 / 其它容器
        const std::string container_parent =
            core::path_to_utf8(container.parent_path().filename());
        fs::path texts_out;
        if (container_parent == "text") {
            texts_out = dst;
        } else if (container_name == "text") {
            texts_out = dst / core::path_from_utf8(info.lang);
        } else if ((container_name == "configs" || container_name == "config")) {
            texts_out = dst / "text" / core::path_from_utf8(info.lang);
        } else {
            texts_out = dst / cfg_name / "text" / core::path_from_utf8(info.lang);
        }
        fs::create_directories(texts_out, ec);

        // scripts：改写后回写到容器内 `scripts/`（相对容器根）
        if (is_dir(container / "scripts")) {
            const ExtractStats st = extract_branch(container / "scripts", ExtractKind::Scripts,
                                                   dst / "scripts", texts_out, options,
                                                   &out.warnings, "sc");
            if (!st.ok) {
                if (!st.error.empty()) out.warnings.push_back("scripts：" + st.error);
                ++extract_failed;
            }
            info.extracted += st.strings;
            info.rewritten += st.files;
            info.texts += st.texts_xml_name.empty() ? 0 : 1;
            out.extracted += st.strings;
            out.rewritten += st.files;
        }
        // gameplay（容器根下的 gameplay 或 configs/gameplay）：统一落 `configs/gameplay`
        // ★ 复审计：两分支原来共用 `gsgp_gameplay_texts.xml`，第二个直接覆盖第一个。
        //   按"是否 configs 下"给不同后缀（根 gameplay = gpg，configs/gameplay = gpgc）。
        int gameplay_branch_index = 0;
        for (const fs::path& branch : {container / "gameplay", container / cfg_name / "gameplay"}) {
            if (!is_dir(branch)) continue;
            const std::string gameplay_suffix =
                gameplay_branch_index++ == 0 ? "gpg" : "gpgc";
            const ExtractStats st = extract_branch(branch, ExtractKind::Gameplay,
                                                   dst / cfg_name / "gameplay", texts_out,
                                                   options, &out.warnings, gameplay_suffix);
            if (!st.ok) {
                if (!st.error.empty()) out.warnings.push_back("gameplay：" + st.error);
                ++extract_failed;
            }
            info.extracted += st.strings;
            info.rewritten += st.files;
            info.texts += st.texts_xml_name.empty() ? 0 : 1;
            out.extracted += st.strings;
            out.rewritten += st.files;
        }
        // 容器根的 `gameplay/` 不是游戏读取路径（读的是 `configs/gameplay/`）：
        // 提取完就把复制过来的那份删掉，免得同一份文本在 ori 里出现两次。
        if (is_dir(dst / "gameplay")) {
            std::error_code del_ec;
            fs::remove_all(dst / "gameplay", del_ec);
        }
        // configs/ui（XML）、configs/scripts（script/ltx）：同样提取，改写后按原相对路径回写
        struct ExtraBranch { std::string rel; ExtractKind kind; };
        for (const ExtraBranch& extra : {ExtraBranch{cfg_name + "/ui", ExtractKind::Gameplay},
                                         ExtraBranch{cfg_name + "/scripts", ExtractKind::Scripts}}) {
            const fs::path branch = container / core::path_from_utf8(extra.rel);
            if (!is_dir(branch)) continue;
            const std::string suffix = extra.rel == cfg_name + "/ui" ? "ui" : "cs";
            const ExtractStats st = extract_branch(branch, extra.kind,
                                                   dst / core::path_from_utf8(extra.rel),
                                                   texts_out, options, &out.warnings, suffix);
            if (!st.ok && !st.error.empty()) {
                out.warnings.push_back(std::string(extra.rel) + "：" + st.error);
            ++extract_failed;
            }
            info.extracted += st.strings;
            info.rewritten += st.files;
            info.texts += st.texts_xml_name.empty() ? 0 : 1;
            out.extracted += st.strings;
            out.rewritten += st.files;
        }

        // 多语言：容器已有的其它语言目录也放一份（规划 4.4）
        const fs::path text_root = container / cfg_name / "text";
        if (!options.texts_only && is_dir(text_root)) {
            for (const auto& entry : fs::directory_iterator(text_root, ec)) {
                if (!entry.is_directory(ec)) continue;
                const std::string other = core::path_to_utf8(entry.path().filename());
                if (other == info.lang) continue;
                const fs::path other_texts = dst / cfg_name / "text" / core::path_from_utf8(other);
                fs::create_directories(other_texts, ec);
                for (const auto& xml : fs::directory_iterator(texts_out, ec)) {
                    if (!xml.is_regular_file(ec)) continue;
                    // ★ 2026-11-07：只补"我们产出的字面量表"，**且目标不存在才补**。
                    //   修前：把 texts_out 里所有文件（含原版 st_*.xml）覆盖式抄到其它语言目录
                    //   ⇒ 实测把 rus 的俄文用 eng 覆盖（92 个文件），是本次灾难的直接原因。
                    const std::string fname = core::path_to_utf8(xml.path().filename());
                    if (fname.find("_texts.xml") == std::string::npos) continue;
                    const fs::path target_file = other_texts / xml.path().filename();
                    if (fs::exists(target_file, ec)) continue;
                    fs::copy_file(xml.path(), target_file, fs::copy_options::none, ec);
                }
            }
        }
        out.texts += info.texts;
        out.containers.push_back(info);
    }

    if (extract_failed > 0) {
        // 复审计：分支提取失败还报 ok=true 等于骗人 —— 失败原因已经在 warnings 里逐条写
        out.out_dir = core::path_to_utf8(out_abs);
        out.error = "有 " + std::to_string(extract_failed) + " 个分支提取失败（见 warnings）";
        return out;
    }
    out.out_dir = core::path_to_utf8(out_abs);
    out.lang = out.containers.front().lang;
    out.lang_source = out.containers.front().lang_source;
    out.ok = true;
    // ★★ 2026-10-30（交接区"扫描一次、后面全自动"）：Profile 落在**产物里**，
    //   位图 / IXR / 页面直接读 `ori/_xrkit_profile.json`，不必再扫一遍源目录。
    //   失败写不出来只 warn（不影响产物，但零静默）。
    try {
        std::ofstream profile_file(out_abs / "_xrkit_profile.json", std::ios::binary);
        if (profile_file) {
            profile_file << ori_profile_json(out) << "\n";
        } else {
            out.warnings.push_back("profile 落盘失败：ori/_xrkit_profile.json 写不出去");
        }
    } catch (...) {
        out.warnings.push_back("profile 落盘异常：ori/_xrkit_profile.json");
    }
    return out;
}

}  // namespace xrkit::engine::text
