#include "xrkit/engine/text/langpack.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/overlay.h"   // 遍历只有一套：fs::collect_tree()

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
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

    const fs::path out_dir = core::path_from_utf8(options.out_root) / lang;
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
            copy_tree(rew, out_dir, options.overwrite, copied, out.skipped_existing, out.warnings);
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

}  // namespace xrkit::engine::text
