#include "xrkit/engine/text/extract_pack.h"

#include "xrkit/engine/text/extract.h"
#include "xrkit/engine/text/langpack.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace xrkit::engine::text {
namespace {

namespace fs = std::filesystem;

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

fs::path join(const fs::path& a, const std::string& b) { return a / fs::path(b); }

std::string base_name(const fs::path& p) { return p.filename().string(); }

std::string read_small_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 从 localization.ltx 读 language = xxx（与页面旧逻辑同一口径；不引入 Qt 正则依赖）
std::string read_language_ltx(const fs::path& cfg_root) {
    const std::string text = read_small_text(cfg_root / "localization.ltx");
    if (text.empty()) return {};
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t line_end = text.find('\n', pos);
        const std::string line =
            text.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (lower_ascii(line.substr(i, 8)) == "language") {
            i += 8;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i < line.size() && line[i] == '=') {
                ++i;
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
                const std::size_t start = i;
                while (i < line.size()) {
                    const unsigned char c = static_cast<unsigned char>(line[i]);
                    if (std::isalnum(c) == 0 && line[i] != '_' && line[i] != '-') break;
                    ++i;
                }
                if (i > start) return line.substr(start, i - start);
            }
        }
        if (line_end == std::string::npos) break;
        pos = line_end + 1;
    }
    return {};
}

// 在树里找所有 .../configs/text/<lang> 与 .../config/text/<lang>
std::vector<fs::path> find_text_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                              ec),
         end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string name = lower_ascii(base_name(it->path()));
        if (name != "configs" && name != "config") continue;
        const fs::path text = it->path() / "text";
        if (!is_dir(text)) continue;
        std::error_code lec;
        for (fs::directory_iterator lit(text, lec), lend; lit != lend && !lec;
             lit.increment(lec)) {
            if (lit->is_directory(lec)) dirs.push_back(lit->path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

fs::path find_cfg_root(const fs::path& source) {
    for (const fs::path& candidate :
         {join(join(source, "gamedata"), "configs"), join(join(source, "gamedata"), "config"),
          join(source, "configs"), join(source, "config")}) {
        if (is_dir(candidate)) return candidate;
    }
    return {};
}

struct Job {
    fs::path source;
    fs::path target;
    std::string label;
    std::string id_prefix;
    bool append = false;
};

}  // namespace

ExtractPackResult extract_language_pack(const ExtractPackOptions& options) {
    ExtractPackResult out;
    out.source = options.source_root;
    out.out = options.out_root;
    const fs::path source(options.source_root);
    const fs::path out_root(options.out_root);
    const auto say = [&options](const std::string& text) {
        if (options.message) options.message(text);
    };
    const auto stop = [&options]() { return options.should_stop && options.should_stop(); };
    const auto progress = [&options](int pct) {
        if (options.progress) options.progress(pct);
    };

    if (!is_dir(source)) {
        out.error = "源目录不存在：" + options.source_root;
        return out;
    }
    if (options.out_root.empty()) {
        out.error = "缺输出目录";
        return out;
    }
    fs::path cfg_root = find_cfg_root(source);
    if (cfg_root.empty()) {
        if ((base_name(source) == "configs" || base_name(source) == "config") &&
            is_dir(source / "text")) {
            cfg_root = source;
        }
    }
    if (cfg_root.empty()) {
        out.error = "提取只接受已解包的松散目录（需含 gamedata/configs|config 或 configs|config）";
        return out;
    }
    const std::string cfg_name = base_name(cfg_root);
    const bool has_gamedata_prefix =
        (cfg_root.parent_path() == (source / "gamedata")) &&
        (base_name(source) != "gamedata");
    const std::string rel_prefix = has_gamedata_prefix ? "gamedata/" : "";
    const fs::path game_root = has_gamedata_prefix ? (source / "gamedata") : source;

    // ① 可选：语言包收集（文本 XML / fonts / localization）
    if (options.copy_language_pack) {
        LangPackCollectOptions co;
        co.source_root = options.source_root;
        co.out_root = options.out_root;
        const LangPackCollectResult collected = collect_language_pack(co);
        out.copied = static_cast<int>(collected.copied);
        out.langs = collected.langs;
        if (!collected.langs.empty()) {
            std::string langs;
            for (const std::string& one : collected.langs) {
                if (!langs.empty()) langs += ",";
                langs += one;
            }
            say("语言包收敛：" + std::to_string(collected.copied) + " 个文件；语言=" + langs);
        } else {
            say("语言包收敛跳过：" + collected.error);
        }
    } else {
        say("跳过语言包复制：只抽硬编码文本（不复制 text/fonts/localization）");
    }
    progress(25);

    // ② 语言目录：复制语言包 ⇒ 用输出树；否则用源树推断语言，文本表仍落输出树。
    const std::vector<fs::path> source_text_dirs = find_text_dirs(source);
    const std::vector<fs::path> copied_text_dirs = find_text_dirs(out_root);
    const std::vector<fs::path>& candidate_dirs =
        options.copy_language_pack ? copied_text_dirs : source_text_dirs;
    const std::string chosen_lang =
        options.lang.empty() ? read_language_ltx(cfg_root) : options.lang;
    std::string lang = chosen_lang;
    if (lang.empty()) {
        std::vector<std::string> names;
        for (const fs::path& dir : candidate_dirs) names.push_back(base_name(dir));
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        if (!names.empty()) lang = names.front();
    }
    if (lang.empty()) {
        out.error = "无法确定输出语言：源里没有 configs/text/<语言>，也没有 localization.ltx";
        return out;
    }
    fs::path texts_out;
    if (options.copy_language_pack && !copied_text_dirs.empty()) {
        for (const fs::path& dir : copied_text_dirs) {
            if (base_name(dir) == lang) {
                texts_out = dir;
                break;
            }
        }
        if (texts_out.empty()) texts_out = copied_text_dirs.front();
        lang = base_name(texts_out);
    } else {
        texts_out = join(join(join(join(out_root, rel_prefix), cfg_name), "text"), lang);
    }
    std::error_code ec;
    fs::create_directories(texts_out, ec);
    out.texts_out = texts_out.string();
    out.lang = lang;
    std::vector<fs::path> output_text_dirs =
        (options.copy_language_pack && !copied_text_dirs.empty()) ? copied_text_dirs
                                                                  : std::vector<fs::path>{texts_out};

    // ③ 三个源分支（根 scripts / configs/scripts / configs/gameplay）
    std::vector<Job> jobs;
    bool have_scripts_table = false;
    const fs::path root_scripts = join(game_root, "scripts");
    if (is_dir(root_scripts)) {
        jobs.push_back({root_scripts, join(join(out_root, rel_prefix), "scripts"), "scripts", "",
                        false});
        have_scripts_table = true;
    }
    const fs::path cfg_scripts = join(cfg_root, "scripts");
    if (is_dir(cfg_scripts) && cfg_scripts != root_scripts) {
        jobs.push_back({cfg_scripts,
                        join(join(join(out_root, rel_prefix), cfg_name), "scripts"),
                        "configs/scripts", have_scripts_table ? "sgtat_cfg_sc_0" : "",
                        have_scripts_table});
        have_scripts_table = true;
    }
    const fs::path gameplay = join(cfg_root, "gameplay");
    if (is_dir(gameplay)) {
        jobs.push_back({gameplay, join(join(join(out_root, rel_prefix), cfg_name), "gameplay"),
                        "gameplay", "", false});
    }
    if (jobs.empty()) {
        out.ok = true;
        out.empty = true;
        say("目录里没有 scripts/ 或 gameplay/：" + cfg_root.string());
        return out;
    }

    // ④ 逐分支提取；文本表按 scripts 表追加合并
    int pct = 25;
    for (const Job& job : jobs) {
        if (stop()) {
            out.cancelled = true;
            out.error = "已取消：停在文件边界";
            return out;
        }
        fs::path target = job.target;
        if (!options.write_rewritten) {
            target = fs::temp_directory_path() / "xrkit_extract_discard" / job.label;
        }
        fs::create_directories(target, ec);
        ExtractOptions eo;
        eo.should_stop = options.should_stop;
        eo.id_prefix = job.id_prefix;
        eo.append_texts = job.append;
        eo.progress = [&](uint64_t done, uint64_t total) {
            if (total > 0) {
                progress(25 + static_cast<int>(70 * done / total));
            }
        };
        const ExtractStats st =
            extract_dir(job.source.string(), target.string(), texts_out.string(), eo);
        ++out.jobs;
        if (!st.ok) {
            out.error = "提取失败（" + job.label + "）：" + st.error;
            return out;
        }
        out.files += static_cast<int>(st.files);
        out.strings += static_cast<int>(st.strings);
        out.no_text += static_cast<int>(st.no_text_files);
        say(job.label + "：" + std::to_string(st.strings) + " 条，改写 " +
            std::to_string(st.files) + " 个，无文本跳过 " +
            std::to_string(st.no_text_files) + " 个");
        if (st.cancelled) {
            out.cancelled = true;
            out.error = "已取消：停在文件边界";
            return out;
        }
        pct = 25 + static_cast<int>(70 * out.jobs / jobs.size());
        progress(std::min(99, pct));
    }

    // ⑤ 文本表同步到其它语言目录（每语言一份；已存在不覆盖）
    if (out.files > 0) {
        for (const fs::path& dir : output_text_dirs) {
            if (dir == texts_out) continue;
            std::error_code copy_ec;
            fs::create_directories(dir, copy_ec);
            for (fs::directory_iterator it(texts_out, copy_ec), end; it != end && !copy_ec;
                 it.increment(copy_ec)) {
                if (!it->is_regular_file(copy_ec)) continue;
                const std::string name = base_name(it->path());
                if (name.find("_texts.xml") == std::string::npos) continue;
                const fs::path dst = dir / name;
                if (!fs::exists(dst, copy_ec)) {
                    fs::copy_file(it->path(), dst, copy_ec);
                }
            }
        }
    }
    progress(100);
    out.ok = true;
    out.empty = (out.files == 0 && out.strings == 0);
    return out;
}

}  // namespace xrkit::engine::text
