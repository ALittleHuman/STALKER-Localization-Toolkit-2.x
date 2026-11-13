#include "xrkit/engine/font/package.h"
#include "xrkit/engine/font/ixr.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/font/charset.h"
#include "xrkit/engine/font/render.h"
#include "xrkit/engine/font/xml_bytes.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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

// 出包先写 <out>/gamedata.xrkit_part；成功后才合并到 gamedata/。
// 失败只删临时目录，绝不 remove_tree(最终 gamedata)（审计 F-061）。
bool commit_staged_gamedata(const std::string& staged, const std::string& final_dir,
                            std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(core::path_from_utf8(staged), ec)) {
        if (error != nullptr) *error = "临时 gamedata 不存在";
        return false;
    }
    const fs::path dst = core::path_from_utf8(final_dir);
    if (!fs::exists(dst, ec)) {
        fs::create_directories(dst.parent_path(), ec);
        fs::rename(core::path_from_utf8(staged), dst, ec);
        if (ec) {
            if (error != nullptr) *error = "提交 gamedata 失败：" + ec.message();
            return false;
        }
        return true;
    }
    // 目标已存在：只覆盖本次生成的文件，不删用户原有内容。
    fs::create_directories(dst, ec);
    if (ec) {
        if (error != nullptr) *error = "建目标 gamedata 失败：" + ec.message();
        return false;
    }
    // R-1：覆盖前先把旧文件放进 rollback；中途失败按逆序恢复，避免半更新。
    const fs::path rollback = core::path_from_utf8(staged + ".rollback");
    fs::remove_all(rollback, ec);
    fs::create_directories(rollback, ec);
    std::vector<std::pair<fs::path, fs::path>> backups;  // target -> backup
    std::vector<fs::path> created;
    const auto roll_back = [&]() {
        for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
            std::error_code rec;
            fs::copy_file(it->second, it->first, fs::copy_options::overwrite_existing, rec);
        }
        for (const fs::path& path : created) {
            std::error_code rec;
            fs::remove(path, rec);
        }
    };
    for (fs::recursive_directory_iterator it(core::path_from_utf8(staged), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), core::path_from_utf8(staged), ec);
        if (ec) continue;
        const fs::path target = dst / rel;
        std::error_code exists_ec;
        const bool target_exists = fs::is_regular_file(target, exists_ec);
        if (exists_ec) {
            roll_back();
            if (error != nullptr) *error = "提交前检查 " + core::path_to_utf8(rel.string()) +
                                           " 失败：" + exists_ec.message() + "；已回滚";
            return false;
        }
        if (target_exists) {
            const fs::path backup = rollback / rel;
            fs::create_directories(backup.parent_path(), ec);
            fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                roll_back();
                if (error != nullptr) *error = "备份 " + core::path_to_utf8(rel.string()) +
                                               " 失败：" + ec.message() + "；已回滚";
                return false;
            }
            backups.emplace_back(target, backup);
        } else {
            created.push_back(target);
        }
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            roll_back();
            if (error != nullptr) *error = "覆盖 " + core::path_to_utf8(rel.string()) +
                                           " 失败：" + ec.message() + "；已回滚";
            return false;
        }
    }
    if (ec) {
        roll_back();
        if (error != nullptr) *error = "遍历临时 gamedata 失败：" + ec.message() + "；已回滚";
        return false;
    }
    fs::remove_all(rollback, ec);
    fs::remove_all(core::path_from_utf8(staged), ec);
    return true;
}

bool has_top_xml(const std::filesystem::path& dir) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".xml") return true;
    }
    return false;
}

struct TextRootLayout {
    bool ok = false;
    std::filesystem::path text_dir;
    std::filesystem::path package_root;
    std::string error;
};

// `--text-root` 既接受包根，也接受 gamedata 根、configs 根、text/<lang> 或文本目录本身。
// 旧实现把输入直接当“只含顶层 XML 的目录”，所以指向包根时 xml=0；这里先归一化。
TextRootLayout resolve_text_root(const std::string& value, const std::string& lang) {
    namespace fs = std::filesystem;
    TextRootLayout out;
    const fs::path root = core::path_from_utf8(value);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        out.error = "译文根不存在或不是目录：" + value;
        return out;
    }

    std::vector<fs::path> candidates;
    candidates.push_back(root);
    if (!lang.empty()) {
        candidates.push_back(root / "gamedata" / "configs" / "text" / lang);
        candidates.push_back(root / "configs" / "text" / lang);
        candidates.push_back(root / "text" / lang);
        if (root.filename() == core::path_from_utf8(lang)) candidates.push_back(root);
    }
    for (const fs::path& candidate : candidates) {
        if (!fs::is_directory(candidate, ec) || !has_top_xml(candidate)) continue;
        out.ok = true;
        out.text_dir = candidate;
        fs::path p = candidate;
        while (!p.empty() && p.filename() != "gamedata") p = p.parent_path();
        if (!p.empty()) out.package_root = p.parent_path();
        return out;
    }
    out.error = "译文根下找不到 XML：既不是文本目录，也没有 "
                "gamedata/configs/text/" + (lang.empty() ? std::string("<lang>") : lang) +
                "（输入：" + value + "）";
    return out;
}

int copy_tree_files(const std::filesystem::path& src, const std::filesystem::path& dst,
                    std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return 0;
    int copied = 0;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path rel = fs::relative(it->path(), src, ec);
        if (ec) continue;
        const fs::path target = dst / rel;
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error != nullptr) *error = "复制 " + core::path_to_utf8(it->path().string()) +
                                           " 失败：" + ec.message();
            return copied;
        }
        ++copied;
    }
    if (ec && error != nullptr) *error = "遍历 " + core::path_to_utf8(src.string()) +
                                         " 失败：" + ec.message();
    return copied;
}

// ★★ 2026-10-07（审计补充 A1-4）：本文件原来的本地 `read_bytes`（打不开就 `return {}`）
// 已**删除** —— 读 XML 只有 `read_file_bytes()` 一份实现（`xml_bytes.h`）。修前那条路
// 把"打不开"当成空文件，而 `decode_xml_bytes({})` 必然成功 ⇒ **写出空 XML 并记成功**。

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

std::vector<std::string> parse_segments(const std::string& ltx_text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < ltx_text.size()) {
        std::size_t eol = ltx_text.find('\n', pos);
        if (eol == std::string::npos) eol = ltx_text.size();
        std::string line = ltx_text.substr(pos, eol - pos);
        pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size() || line[i] != '[') continue;
        const std::size_t close = line.find(']', i);
        if (close == std::string::npos || close == i + 1) continue;
        out.push_back(line.substr(i + 1, close - i - 1));
    }
    return out;
}

int ref_segment_count(const std::string& pack_dir) {
    const std::filesystem::path p =
        core::path_from_utf8(pack_dir) / "gamedata" / "configs" / "fonts.ltx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return -1;
    const std::vector<uint8_t> raw = [&] {
        std::ifstream in(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    }();
    std::string text;
    std::string enc;
    bool trusted = false;
    if (!decode_xml_bytes(raw, text, enc, trusted)) {
        text.assign(raw.begin(), raw.end());   // 退一步：原样按字节解析（ltx 是 ASCII 结构）
    }
    return static_cast<int>(parse_segments(text).size());
}

int template_segment_count(const std::string& game) {
    const GameTemplate* t = find_game(game);
    if (t == nullptr) return -1;
    return static_cast<int>(parse_segments(t->fonts_ltx).size());
}

FontPackOutcome build_font_pack(const FontPackRequest& request) {
    FontPackOutcome out;
    out.vector_path = request.use_ref;
    // ① 字符集：**统一核心**（两条 writer 都用它）
    if (!request.text_root.empty()) {
        CharsetResult cs;
        if (extract_chars(request.text_root, /*full_cyrillic=*/true, cs)) {
            out.charset_count = cs.chars.size();
        }
    }
    // ② 段表：**统一解析器**（内置模板与参照包同一份）
    // ★ 矢量路径**不读 game**：段表来自参照包；`game` 只在位图路径（内置模板）才有意义。
    //   调用方（页面/CLI）在 use_ref=true 时应当传空 game —— 传了也会被忽略（见下）。
    out.segments = request.use_ref ? ref_segment_count(request.ref)
                                   : template_segment_count(request.game);
    out.segments_from_ref = request.use_ref;
    if (out.segments < 0) {
        out.error = request.use_ref
                        ? ("参照包缺 gamedata/configs/fonts.ltx（或不含任何段）：" + request.ref)
                        : ("未知游戏版本：" + request.game);
        return out;
    }
    // ③ 分流到 writer（只有这一处）
    if (request.use_ref) {
        if (request.patch_only) {
            out.error = "矢量字号补丁暂不支持（IX-Ray 的字号在 fonts.ltx 的 size，需实测后再发）";
            return out;
        }
        IxrBuildOptions opt;
        opt.pack = request.ref;
        opt.font = request.font;
        opt.out = request.out;
        opt.text_root = request.text_root;
        opt.langs = request.langs.empty() ? request.lang : request.langs;
        opt.merge_glyphs = request.merge_glyphs;
        opt.force = request.force;
        opt.missing_warn = request.missing_warn;
        out.vector_result = ixr_build(opt);
        out.ok = out.vector_result.ok;
        if (!out.ok) out.error = out.vector_result.error;
        // 缺字账目：从 `IXRGEN {...}` 报告里取。
        // ★ 报告里**每个字体**都有 `"missing"/"missing_cps"`（第一个常常是 0），
        //   顶层汇总的 `"missing"` 在最后 ⇒ 取**最后一个** `"missing":`；缺字清单则是
        //   所有 `missing_cps` 的并集（去重）。
        if (out.ok) {
            const std::string& tail = out.vector_result.stdout_tail;
            const std::size_t at = tail.rfind("\"missing\":");
            if (at != std::string::npos) {
                std::size_t i = at + 10;
                while (i < tail.size() && (tail[i] == ' ' || tail[i] == '\t')) ++i;
                int n = 0;
                while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9') {
                    n = n * 10 + (tail[i] - '0');
                    ++i;
                }
                out.missing = n;
            }
            std::size_t pos = 0;
            while ((pos = tail.find("\"missing_cps\":", pos)) != std::string::npos) {
                const std::size_t open = tail.find('[', pos);
                const std::size_t close = open == std::string::npos ? std::string::npos
                                                                   : tail.find(']', open);
                if (open == std::string::npos || close == std::string::npos) break;
                std::size_t i = open + 1;
                while (i < close) {
                    const std::size_t q1 = tail.find('"', i);
                    if (q1 == std::string::npos || q1 >= close) break;
                    const std::size_t q2 = tail.find('"', q1 + 1);
                    if (q2 == std::string::npos || q2 > close) break;
                    const std::string one = tail.substr(q1 + 1, q2 - q1 - 1);
                    if (one.rfind("U+", 0) == 0 &&
                        std::find(out.missing_cps.begin(), out.missing_cps.end(), one) ==
                            out.missing_cps.end() &&
                        out.missing_cps.size() < 64) {
                        out.missing_cps.push_back(one);
                    }
                    i = q2 + 1;
                }
                pos = close + 1;
            }
        }
        return out;
    }
    PackageOptions opt;
    opt.game = request.game;
    opt.lang = request.lang;
    opt.xml_dir = request.text_root;
    opt.font_path = request.font;
    opt.out_dir = request.out;
    opt.missing_warn = request.missing_warn;
    opt.require_full_cyrillic = request.require_full_cyrillic;
    opt.suffix = request.suffix;
    opt.offset = request.offset;
    opt.only_sizes = request.only_sizes;
    opt.patch_only = request.patch_only;
    out.bitmap = build_package(opt);
    out.ok = out.bitmap.ok;
    if (!out.ok) out.error = out.bitmap.error;
    return out;
}

FontSizePatchResult build_font_size_patch(const FontSizePatchRequest& request) {
    FontSizePatchResult res;
    if (request.ref.empty()) {
        res.error = "缺参照包（fonts.ltx 来源）";
        return res;
    }
    if (request.offset == 0) {
        res.error = "offset=0 就是标准包，不需要字号补丁";
        return res;
    }
    std::string src;
    for (const std::string& candidate :
         {join(join(request.ref, "gamedata"), "configs/fonts.ltx"),
          join(request.ref, "configs/fonts.ltx")}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(core::path_from_utf8(candidate), ec)) {
            src = candidate;
            break;
        }
    }
    if (src.empty()) {
        res.error = "参照包缺 gamedata/configs/fonts.ltx：" + request.ref;
        return res;
    }
    std::vector<uint8_t> bytes;
    std::string read_error;
    if (!read_file_bytes(src, bytes, read_error)) {
        res.error = "读不出 fonts.ltx：" + read_error;
        return res;
    }
    const std::string text(bytes.begin(), bytes.end());
    std::string out;
    out.reserve(text.size() + 64);
    int changed = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::size_t k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
        if (k < line.size() && line[k] != ';') {
            const std::size_t key_start = k;
            while (k < line.size() &&
                   std::isalpha(static_cast<unsigned char>(line[k])) != 0) {
                ++k;
            }
            std::string key = line.substr(key_start, k - key_start);
            for (char& c : key) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (key == "size") {
                while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                if (k < line.size() && line[k] == '=') {
                    ++k;
                    while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k;
                    const std::size_t num_start = k;
                    while (k < line.size() &&
                           std::isdigit(static_cast<unsigned char>(line[k])) != 0) {
                        ++k;
                    }
                    if (k > num_start) {
                        const int old_size = std::atoi(line.substr(num_start, k - num_start).c_str());
                        const int new_size = std::max(1, old_size + request.offset);
                        line.replace(num_start, k - num_start, std::to_string(new_size));
                        ++changed;
                    }
                }
            }
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    if (changed == 0) {
        res.error = "fonts.ltx 里没有可改的 `size = N` 行：" + src;
        return res;
    }
    const std::string dest = join(join(request.out, "gamedata"), "configs/fonts.ltx");
    std::error_code ec;
    std::filesystem::create_directories(
        core::path_from_utf8(join(join(request.out, "gamedata"), "configs")), ec);
    if (ec || !write_text(dest, out)) {
        res.error = "写不出字号补丁：" + dest;
        return res;
    }
    res.ok = true;
    res.out_file = dest;
    res.sizes_changed = changed;
    return res;
}

PackageResult build_package(const PackageOptions& opt) {
    namespace fs = std::filesystem;
    PackageResult res;
    const GameTemplate* game = find_game(opt.game);
    if (game == nullptr) {
        res.error = "未知游戏版本：" + opt.game;
        return res;
    }
    const TextRootLayout layout = resolve_text_root(opt.xml_dir, opt.lang);
    if (!layout.ok) {
        res.error = layout.error;
        return res;
    }
    const std::string xml_root = core::path_to_utf8(layout.text_dir.string());
    CharsetResult cs;
    // ★ full_cyrillic 内置：完整西里尔 + 必需字符一律纳入（不再读 opt.full_cyrillic）
    if (!extract_chars(xml_root, /*full_cyrillic=*/true, cs)) {
        res.error = cs.error.empty() ? ("xml 目录不存在：" + xml_root) : cs.error;
        return res;
    }
    // ★★ 2026-10-07（审计补充 A1-4）：**打不开的文件必须计入失败** —— 原来那种文件被当成
    //   "空文件"，于是它的字符全被静默丢掉、包照样出成功。
    if (cs.files_unreadable > 0) {
        res.error = "有 " + std::to_string(cs.files_unreadable) +
                    " 个 XML 读不出来（被占用/权限不足/路径过长），字符集会缺它们的内容 ⇒ "
                    "不出包（第一份：" +
                    (cs.guessed.empty() ? std::string("?") : cs.guessed.front()) + "）";
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
        if (!miss.empty() && !opt.missing_warn) {
            std::string list;
            for (size_t i = 0; i < miss.size() && i < 12; ++i) {
                // ★ U+XXXX 必须是十六进制（修前打印十进制，读起来像另一个码位）
                char buf[16] = {};
                std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(miss[i]));
                list += std::string(buf) + " ";
            }
            res.error = "所选字体画不出这些字符（图集会全部变成同一个方框）：" + list +
                        "—— 请改用 msyh / simsun / simhei 等含中日韩字形的字体";
            return res;
        }
        if (!miss.empty() && opt.missing_warn) {
            // --on-missing warn：语料缺字降级为警告（抓取/预览用），但仍然**说出来**
            std::string list;
            for (std::size_t i = 0; i < miss.size() && i < 32; ++i) {
                if (i) list += ",";
                char buf[16] = {};
                std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(miss[i]));
                list += buf;
            }
            if (miss.size() > 32) list += "…(+" + std::to_string(miss.size() - 32) + ")";
            res.warnings.push_back("MISSING used=" + std::to_string(miss.size()) +
                                   " 个语料用字在所选字体里画不出：" + list +
                                   "；已按 --on-missing warn 继续出包（这些字会显示方框）");
        }
        const std::vector<uint32_t> req = required_chars();
        const std::vector<uint32_t> req_miss = font_unrenderable(opt.font_path, req);
        if (!req_miss.empty()) {
            // ★ 2026-11-06：full_cyrillic 内置后，缺必需字形**升级为可见警告**（不回退成静默）：
            //   · 语料里真实出现的字缺了 ⇒ 上面的 `miss` 检查已经硬失败（不出包）；
            //   · 这里 `required_chars()` 是**基线全集**（ASCII/西里尔块/桥接/生僻补集）——
            //     字体不含西里尔时它必然非空，若一律硬失败，会误伤"中文语料 + 无西里尔字体"的既有产物。
            std::string list;
            for (std::size_t i = 0; i < req_miss.size() && i < 32; ++i) {
                if (i) list += ",";
                char buf[16] = {};
                std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(req_miss[i]));
                list += buf;
            }
            if (req_miss.size() > 32) list += "…(+" + std::to_string(req_miss.size() - 32) + ")";
            if (opt.require_full_cyrillic) {
                res.error = "所选字体缺 " + std::to_string(req_miss.size()) +
                            " 个基线必需字形（西里尔/特殊字符）：" + list +
                            "。已按 --require-full-cyrillic 要求全量覆盖 ⇒ 不出包；"
                            "请换含西里尔的字体（Noto Sans / GoNotoKurrent）或用 IX-Ray 矢量包补字形";
                return res;
            }
            // 基线缺失但当前语料没用到的，不再打 warn：这类提示会被误读成
            // “字体选错了”。真正用到的字缺失已在上面的 `miss` 分支硬失败。
            // 需要全量西里尔时用 --require-full-cyrillic（缺则不出包）。
        }
    }

    // ★★ 2026-10-30（交接区"扫描一次、后面全自动"）：产物里带了 `_xrkit_profile.json`
    //   ⇒ 语言以它为准（`--ori-tree` 扫出来的证据），不许再按默认值自己猜。
    //   查找顺序：<out>/_xrkit_profile.json → <out>/../_xrkit_profile.json（out 常是 ori/gamedata）。
    //   这里只读 `"lang":"…"`；文件坏了就 warn 并保留页面给的语言（零静默）。
    std::string lang_from_profile;   // 非空 ⇒ 覆盖页面语言（证据优先）
    for (const char* candidate : {"", ".."}) {
        const std::string dir = (std::string(candidate).empty())
                                    ? opt.out_dir
                                    : join(opt.out_dir, candidate);
        const std::string prof = join(dir, "_xrkit_profile.json");
        std::ifstream in(core::path_from_utf8(prof), std::ios::binary);
        if (!in) continue;
        std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t key = body.find("\"lang\":\"");
        if (key == std::string::npos) {
            res.warnings.push_back("profile 里没有 lang 字段，沿用页面语言：" + opt.lang);
            break;
        }
        const size_t begin = key + std::string("\"lang\":\"").size();
        const size_t endq = body.find('"', begin);
        if (endq == std::string::npos) break;
        const std::string from_profile = body.substr(begin, endq - begin);
        if (!from_profile.empty() && from_profile != opt.lang) {
            res.warnings.push_back("语言采用产物内 profile：" + from_profile +
                                   "（页面给的是 " + opt.lang + "）");
            lang_from_profile = from_profile;
        }
        break;
    }

    // `opt` 是 const：用局部变量承载"证据优先"的语言，后面所有用到语言的地方都用它
    const std::string lang = lang_from_profile.empty() ? opt.lang : lang_from_profile;
    res.lang = lang;

    const std::string final_gd = join(opt.out_dir, "gamedata");
    const std::string out_gd = final_gd + ".xrkit_part";
    remove_tree(out_gd);
    const std::string tex_dir = join(join(out_gd, "textures"), "ui");
    const std::string cfg_root = join(out_gd, game->cfg_dir);
    const std::string text_dir = join(join(cfg_root, "text"), lang);
    std::error_code ec;
    std::filesystem::create_directories(core::path_from_utf8(tex_dir), ec);
    if (!opt.patch_only) {
        std::filesystem::create_directories(core::path_from_utf8(text_dir), ec);
    }
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

    if (opt.patch_only) {
        // ★ 2026-11-10：字号补丁 = **只有字体纹理/INI**，绝不写文本、脚本、fonts.ltx。
        std::string commit_error;
        if (!commit_staged_gamedata(out_gd, final_gd, &commit_error)) {
            res.error = commit_error;
            remove_tree(out_gd);
            return res;
        }
        res.ok = true;
        res.out_gamedata = final_gd;
        return res;
    }

    // ── XML：只扫**顶层**（1.x `sorted(os.listdir(xml_dir))`，与 extract_chars 的递归不同），
    //    一律转无 BOM UTF-8，并把第一条声明的 encoding 改成 utf-8。
    std::vector<std::string> xmls;
    for (std::filesystem::directory_iterator it(core::path_from_utf8(xml_root), ec), end;
         it != end; it.increment(ec)) {
        // ★ A1-4 的附带面：枚举出错原来 `break`（**静默截断** XML 列表）⇒ 现在明确报错。
        if (ec) {
            res.error = "枚举 xml 目录失败：" + ec.message();
            remove_tree(out_gd);
            return res;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string name = core::path_to_utf8(it->path().filename());
        if (name.size() < 4) continue;
        std::string tail = name.substr(name.size() - 4);
        for (char& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == ".xml") xmls.push_back(name);
    }
    if (ec) {
        res.error = "枚举 xml 目录失败：" + ec.message();
        remove_tree(out_gd);
        return res;
    }
    std::sort(xmls.begin(), xmls.end());
    for (const std::string& name : xmls) {
        const std::string src = join(xml_root, name);
        const std::string dst = join(text_dir, name);
        std::vector<uint8_t> raw;
        std::string read_error;
        // ★★ 2026-10-07（审计补充 A1-4）：**打不开 ≠ 空文件** —— 原来打不开时 `decode_xml_bytes({})`
        //   必然成功 ⇒ 写出一个**空 XML** 并记成功。现在明确报错并计入失败。
        if (!read_file_bytes(src, raw, read_error)) {
            res.error = "读不出 " + src + "：" + read_error;
            remove_tree(out_gd);
            return res;
        }
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

    // ── 包根模式：把 scripts / configs/gameplay 一并带进包。
    //   只认“能从文本路径反推出 gamedata 父目录”的情况；纯文本目录不猜。
    if (!layout.package_root.empty()) {
        std::string copy_error;
        const fs::path package_gamedata = layout.package_root / "gamedata";
        res.scripts_copied = copy_tree_files(package_gamedata / "scripts",
                                             core::path_from_utf8(out_gd) / "scripts",
                                             &copy_error);
        res.gameplay_copied = copy_tree_files(package_gamedata / "configs" / "gameplay",
                                              core::path_from_utf8(out_gd) / "configs" /
                                                  "gameplay",
                                              &copy_error);
        if (!copy_error.empty()) {
            res.error = copy_error;
            remove_tree(out_gd);
            return res;
        }
    }

    // ── fonts.ltx + localization.ltx
    // ★ 1.x 用 `open(..., "w", encoding="utf-8")`（**文本模式**）⇒ \n 翻成 \r\n；
    //   而 XML 那批用的是 `newline=""`（不翻译）。两者不一样，别写反了。
    std::string loc = game->localization_ltx;
    replace_all(loc, "_#LANG#", opt.suffix);
    replace_all(loc, "#LANG#", lang);
    if (!write_text(join(cfg_root, "fonts.ltx"), to_windows_newlines(game->fonts_ltx)) ||
        !write_text(join(cfg_root, "localization.ltx"), to_windows_newlines(loc))) {
        res.error = "写不出 fonts.ltx / localization.ltx";
        remove_tree(out_gd);
        return res;
    }

    std::string commit_error;
    if (!commit_staged_gamedata(out_gd, final_gd, &commit_error)) {
        res.error = commit_error;
        remove_tree(out_gd);
        return res;
    }
    res.ok = true;
    res.out_gamedata = final_gd;
    return res;
}

}  // namespace xrkit::engine::font
