// xrkit::engine::xml —— 目录对比的实现（2026-10-08 P11b 从 `xml_page.cpp` 下沉）。
//
// 与页面版**逐条等价**的三处要点（下沉时不许走样）：
//   ① 逐个文件之前问 `should_stop`（取消只停在**文件边界**，rows 里都是完整结果）；
//   ② 读不出文本 ⇒ 照样出一行，标 `untrusted_a/b` + `warning`（绝不静默当一致）；
//   ③ 插件模式的 `compare` 优先，其次内置 `text`，最后内置 `stats`。

#include "xrkit/engine/xml/dirdiff.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/text/sniff.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <system_error>

namespace xrkit::engine::xml {
namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

bool is_xml_name(const std::string& name) {
    const std::string lower = lowercase(name);
    return lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".xml") == 0;
}

}  // namespace

std::vector<std::string> list_xml_files(const std::string& root,
                                        std::vector<std::string>* errors) {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path base = core::path_from_utf8(root);
    if (!std::filesystem::is_directory(base, ec)) {
        // ★★ P17-10（02 F-05）：**目录读不了也要说**（修前这里直接 `return out` ⇒
        //   调用方看到的是"这一侧一个 xml 都没有"，随后给出"全都只在另一侧"的假结论）。
        if (errors != nullptr && ec) {
            errors->push_back("目录不可用：" + root + "：" + ec.message() + "（错误码 " +
                              std::to_string(ec.value()) + "）");
        }
        return out;
    }
    std::filesystem::recursive_directory_iterator it(base, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            // ★★ P17-10（02 F-05）：修前这里 `break` —— 不计数、不上报，那一侧后续文件
            //   全缺 ⇒ 结果表可能给出**虚假的"一致/缺失"**。现在：记一条出错理由，
            //   并让调用方知道"这份列表不完整"（保留已列到的部分，列表本身仍可用来看进度）。
            if (errors != nullptr) {
                errors->push_back("目录遍历中断：" + root + "：" + ec.message() + "（错误码 " +
                                  std::to_string(ec.value()) + "）—— 该侧列表**不完整**，"
                                  "本次比较结论不可信");
            }
            break;
        }
        if (!it->is_regular_file(ec)) continue;
        const std::string name = core::path_to_utf8(it->path().filename());
        if (!is_xml_name(name)) continue;
        // 相对路径统一用 '/'（页面版走 QDir::relativeFilePath ⇒ 也是 '/'）
        std::error_code rel_ec;
        const std::filesystem::path rel = std::filesystem::relative(it->path(), base, rel_ec);
        if (rel_ec) continue;
        std::string text = core::path_to_utf8(rel);
        std::replace(text.begin(), text.end(), '\\', '/');
        out.push_back(std::move(text));
    }
    std::sort(out.begin(), out.end());   // 字节序升序（与页面的 std::set 同序）
    return out;
}

namespace {

// 覆盖树里的"根"：出现这些段 ⇒ 从这里开始是一条 gamedata 内部的路径
bool is_align_root(const std::string& segment) {
    static const char* kRoots[] = {"configs", "scripts", "textures", "sounds", "meshes",
                                   "anims", "levels", "prefetch", "spawns", "shaders",
                                   "localization", "gamedata"};
    for (const char* root : kRoots) {
        if (segment == root) return true;
    }
    return false;
}

// rel → (外层前缀, 从根开始的 key)；没有根 ⇒ ("", rel)
std::pair<std::string, std::string> split_at_root(const std::string& rel) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : rel) {
        if (c == '/') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (!is_align_root(parts[i])) continue;
        std::string outer;
        for (std::size_t j = 0; j < i; ++j) {
            if (!outer.empty()) outer += '/';
            outer += parts[j];
        }
        std::string key;
        for (std::size_t j = i; j < parts.size(); ++j) {
            if (!key.empty()) key += '/';
            key += parts[j];
        }
        return {outer, key};
    }
    return {"", rel};
}

}  // namespace

DirDiffReport compare_dirs(const DirDiffOptions& options) {
    DirDiffReport report;
    std::map<std::string, std::string> a;   // rel → 绝对路径
    std::map<std::string, std::string> b;
    const std::filesystem::path base_a = core::path_from_utf8(options.dir_a);
    const std::filesystem::path base_b = core::path_from_utf8(options.dir_b);
    // ★★ P17-10（02 F-05）：两侧的遍历错误都带进报告（带侧标，别让调用方猜是哪一侧）
    std::vector<std::string> err_a;
    std::vector<std::string> err_b;
    for (const std::string& rel : list_xml_files(options.dir_a, &err_a)) {
        a[rel] = core::path_to_utf8(base_a / core::path_from_utf8(rel));
    }
    for (const std::string& rel : list_xml_files(options.dir_b, &err_b)) {
        b[rel] = core::path_to_utf8(base_b / core::path_from_utf8(rel));
    }
    for (const std::string& e : err_a) report.errors.push_back("A 侧：" + e);
    for (const std::string& e : err_b) report.errors.push_back("B 侧：" + e);
    // ★ 工作单：每条 = (显示用 rel, A 侧绝对路径 或 空, B 侧绝对路径 或 空)。
    //   正常配对与"mod 覆盖树对齐"都归一到它 ⇒ 下面的比较逻辑只有一份。
    struct WorkItem {
        std::string rel;
        std::string path_a;
        std::string path_b;
    };
    std::vector<WorkItem> work;
    if (!options.align_mod_dirs) {
        std::set<std::string> all;
        for (const auto& [rel, path] : a) all.insert(rel);
        for (const auto& [rel, path] : b) all.insert(rel);
        for (const std::string& rel : all) {
            const auto ita = a.find(rel);
            const auto itb = b.find(rel);
            work.push_back({rel, ita == a.end() ? "" : ita->second,
                            itb == b.end() ? "" : itb->second});
        }
    } else {
        // A 侧按"根之后的 key"建索引（A 的外层前缀在本口径下被忽略）
        std::map<std::string, std::pair<std::string, std::string>> a_by_key;  // key → (rel, path)
        for (const auto& [rel, path] : a) {
            const auto [outer, key] = split_at_root(rel);
            (void)outer;
            a_by_key[key] = {rel, path};
        }
        std::set<std::string> matched;
        for (const auto& [rel_b, path_b] : b) {
            const auto [outer_b, key] = split_at_root(rel_b);
            const std::string display = outer_b.empty() ? key : outer_b + "/" + key;
            const auto it = a_by_key.find(key);
            if (it == a_by_key.end()) {
                work.push_back({display, "", path_b});
            } else {
                work.push_back({display, it->second.second, path_b});
                matched.insert(key);
            }
        }
        for (const auto& [key, rel_path] : a_by_key) {
            if (matched.count(key) == 0) work.push_back({rel_path.first, rel_path.second, ""});
        }
    }
    report.total = static_cast<int>(work.size());

    const bool text_mode = options.mode_key == "text";
    for (const WorkItem& item : work) {
        const std::string& rel = item.rel;
        // ① 每个文件之前轮询取消（停下时 rows 里都是完整结果）
        if (options.should_stop && options.should_stop()) {
            report.cancelled = true;
            break;
        }
        DirDiffRow row;
        // ★★ 2026-10-26（用户：「还有 ID 个数错误呢」）：**只在一侧的文件也要报那一侧的真实计数**。
        //   修前：`only_one_side()` 直接返回，行数/ID 数恒为 0 ⇒ 186 行全是 0/0，
        //   报告看起来"这些文件是空的"，与事实不符（1.x 也是 0，但那是它的短板，不是口径）。
        //   现在：仍然标 `only_one_side`（明细里那句"文件仅在一侧存在"不变、也不假装比较过），
        //   但把**存在那一侧**的行数与 ID 数如实读出来填进 lines_*/ids_*。
        if (item.path_a.empty()) {
            row.result = only_one_side(rel, "b");
            row.kind = "only_b";
            const text::DecodedBytes only_b_text =
                text::read_text_bytes(item.path_b, options.enc_b);
            if (only_b_text.ok) {
                const XmlSideStats stats =
                    parse_stats_text(only_b_text.text, only_b_text.used_encoding);
                if (stats.ok) {
                    row.result.lines_b = stats.lines;
                    row.result.ids_b = stats.id_counts.size();
                }
            }
            report.rows.push_back(std::move(row));
            continue;
        }
        if (item.path_b.empty()) {
            row.result = only_one_side(rel, "a");
            row.kind = "only_a";
            const text::DecodedBytes only_a_text =
                text::read_text_bytes(item.path_a, options.enc_a);
            if (only_a_text.ok) {
                const XmlSideStats stats =
                    parse_stats_text(only_a_text.text, only_a_text.used_encoding);
                if (stats.ok) {
                    row.result.lines_a = stats.lines;
                    row.result.ids_a = stats.id_counts.size();
                }
            }
            report.rows.push_back(std::move(row));
            continue;
        }
        const text::DecodedBytes text_a = text::read_text_bytes(item.path_a, options.enc_a);
        const text::DecodedBytes text_b = text::read_text_bytes(item.path_b, options.enc_b);
        if (!text_a.ok || !text_b.ok) {
            // ② 读不出来 ⇒ 如实记一条"不可信"
            row.result = only_one_side(rel, text_a.ok ? "b" : "a");
            row.result.warning = !text_a.ok ? text_a.error : text_b.error;
            row.result.untrusted_a = !text_a.ok;
            row.result.untrusted_b = !text_b.ok;
            row.kind = text_a.ok ? "only_b" : "only_a";
            report.rows.push_back(std::move(row));
            continue;
        }
        // ③ 插件回调优先，其次内置 text，最后内置 stats
        if (options.compare) {
            row.result = options.compare(rel, text_a.text, text_b.text, text_a.used_encoding,
                                         text_b.used_encoding);
            row.result.rel = rel;   // 回调没回 rel 也要有（表格第一列靠它）
            if (row.result.mode.empty() || row.result.mode == "stats") {
                row.result.mode = options.mode_key.empty() ? "plugin" : options.mode_key;
            }
            row.result.encoding_a = text_a.used_encoding;
            row.result.encoding_b = text_b.used_encoding;
        } else {
            row.result = text_mode
                             ? compare_text(rel, text_a.text, text_b.text, text_a.used_encoding,
                                            text_b.used_encoding)
                             : compare_stats(rel, text_a.text, text_b.text, text_a.used_encoding,
                                             text_b.used_encoding);
        }
        row.kind = row.result.consistent
                       ? "same"
                       : (row.result.only_one_side
                              ? (row.result.only_side == "a" ? "only_a" : "only_b")
                              : "diff");
        report.rows.push_back(std::move(row));
    }
    return report;
}

bool row_matches_filter(const XmlFilter& filter, const std::string& kind,
                        const XmlCompareResult& result) {
    if (filter.row_kinds.empty()) return true;
    if (std::find(filter.row_kinds.begin(), filter.row_kinds.end(), kind) ==
        filter.row_kinds.end()) {
        return false;
    }
    if (!filter.differ_types.empty()) {
        std::set<std::string> types;
        if (!result.only_a.empty() || !result.only_b.empty()) types.insert("only");
        if (!result.text_diff.empty()) types.insert("text");
        for (const std::string& want : filter.differ_types) {
            if (types.count(want) == 0) return false;
        }
    }
    return true;
}

}  // namespace xrkit::engine::xml
