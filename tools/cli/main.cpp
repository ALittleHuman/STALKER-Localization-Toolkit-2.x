// xrkit_cli —— 引擎命令行外壳。
//
// 为什么要有这个东西（口径）：
//   1.x 的判据是"进程内 import + 打桩"，抓不到"一启动就崩"，也让冻结态与大包场景
//   测不了。2.0 的引擎判据一律走**黑盒命令行**：黄金语料差分、性能基线、
//   真启动锁都从外部驱动它。
//
// 退出码是判据的一部分：
//   0 = 通过；1 = 有失败；2 = **显式跳过（未实现）**；3 = 用法错误。
//   ★ 2 绝不能与 0 混同 —— 门禁必须把 2 记入「本次未覆盖」。

#include "xrkit/core/check.h"
#include "xrkit/core/log.h"
#include "xrkit/core/path.h"
#include "xrkit/core/version.h"
#include "xrkit/engine/engine.h"
#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/lzhuf.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/font/charset.h"
#include "xrkit/engine/font/glyph.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/render.h"
#include "xrkit/engine/font/resample.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/extract.h"
#include "xrkit/engine/text/sniff.h"
#include "xrkit/engine/third_party_probe.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitFail = 1;
constexpr int kExitSkip = 2;
constexpr int kExitUsage = 3;

// ★ Windows 上 `char** argv` 是按 **ANSI 代码页**转出来的：西里尔/中文路径会被转坏
//   （而且 PowerShell 5.1 传原生参数也是按 ANSI 代码页，西里尔字符会直接变成 '?'）。
//   所以：入口用 wmain（宽字符命令行为准），内部一律 UTF-8，进出路径显式 UTF-8↔UTF-16。
std::filesystem::path path_from_utf8(const std::string& s) {
    return xrkit::core::path_from_utf8(s);
}

std::string path_to_utf8(const std::filesystem::path& p) {
    return xrkit::core::path_to_utf8(p);
}

std::vector<uint8_t> read_all(const std::string& path) {
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::vector<uint8_t> read_all_path(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// 按魔数判容器：hsqs（小端）/ sqsh（大端）= SquashFS，其余按 DB 处理
bool is_squashfs_head(const std::string& path) {
    std::ifstream in(path_from_utf8(path), std::ios::binary);
    if (!in) return false;
    char head[4] = {};
    in.read(head, 4);
    if (in.gcount() != 4) return false;
    return std::memcmp(head, "hsqs", 4) == 0 || std::memcmp(head, "sqsh", 4) == 0;
}

void print_usage() {
    std::cout
        << "xrkit_cli —— STALKER 汉化工具集 2.0 引擎命令行\n"
           "\n"
           "用法: xrkit_cli <动作> [选项]\n"
           "\n"
           "动作:\n"
           "  --selftest      跑内置自检（ASCII 收尾行: SELFTEST pass=N fail=M）\n"
           "  --bench         跑性能基线（未实现时输出 BENCH skip ... 并返回 2）\n"
           "  --themecheck    检查主题令牌（未实现时输出 THEMECHECK skip ... 并返回 2）\n"
           "  --dump <文件> [DB格式]   导出解析结果（SquashFS 条目表 或 DB 条目表 TSV）\n"
           "  --extract <源> <内部路径> <输出> [DB格式]   提取单个文件（进程内解压）\n"
           "  --pack-db <输出> <格式> <输入目录>          按目录打包 DB（逐字节对拍用）\n"
           "  --pack-sqfs <输出> <输入目录> [--pack-dirs]  自写 SquashFS 封包（xz 块，逐字节对拍用）\n"
           "  --decode-text <输入> <输出>                 六编码链解码（把正文写成 UTF-8；对拍用）\n"
           "  --sniff <输入> <输出> [--encoding <手动指定>]  判定链（六步）判定 + 解码（对拍用）\n"
           "  --extract-text <源目录> <输出目录> [--prefix p] [--id-prefix p] [--clean]\n"
           "                                              文本提取（gameplay|scripts 一个分支）\n"
           "  --glyph-dump <字体> <字号> <输出> [字符集文件]  导出 FreeType 栅格（与 Pillow 对拍）\n"
           "  --font-metrics <输出>                       导出字库几何/宽度表（与 1.x font_pack 对拍）\n"
           "  --font-render <字体> <字号> <x2> <fmt> <字符集文件> <输出前缀>\n"
           "                                              渲染一个字号的 DDS + ini（与 1.x 逐字节对拍）\n"
           "  --resample-dump <输入raw> <源宽> <源高> <目标宽> <目标高> <输出raw>\n"
           "                                              Pillow LANCZOS 重采样（与 Pillow 对拍）\n"
           "  --extract-chars <xml目录> [输出文件] [--no-full-cyrillic]\n"
           "                                              字符集提取（每行一个十六进制码位）\n"
           "  --version       打印版本\n"
           "  --help, -h      本帮助\n"
           "选项:\n"
           "  --verbose, -v   逐条打印自检结果\n"
           "\n"
           "退出码: 0=通过  1=有失败  2=显式跳过（未实现）  3=用法错误\n";
}

void register_core_checks(xrkit::core::CheckRegistry& reg) {
    using xrkit::core::CheckContext;
    using xrkit::core::check;

    reg.add("version: 版本串形态合法", [](CheckContext& ctx) {
        const std::string s = xrkit::core::version_string();
        check(ctx, xrkit::core::version_string_is_well_formed(s), "非法版本串: " + s);
        check(ctx, s.rfind("2.0.0", 0) == 0, "主版本不是 2.0.0: " + s);
    });

    reg.add("version: 构建号与版本可组合", [](CheckContext& ctx) {
        const std::string s = xrkit::core::version_with_build();
        check(ctx, s.find('+') != std::string::npos, "缺少构建号分隔符: " + s);
    });

    reg.add("log: 落盘可用、可关闭、内容真写出", [](CheckContext& ctx) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path p = fs::temp_directory_path(ec) / "xrkit_selftest_log.txt";
        fs::remove(p, ec);

        xrkit::core::set_log_file(p.string());
        check(ctx, xrkit::core::log_file_active(), "日志文件没挂上");
        xrkit::core::log(xrkit::core::Level::Info, "selftest-marker");
        xrkit::core::set_log_file("");
        check(ctx, !xrkit::core::log_file_active(), "日志文件没关掉");

        std::ifstream in(p, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        check(ctx, body.find("selftest-marker") != std::string::npos,
              "落盘内容里找不到刚写的那行");
        fs::remove(p, ec);
    });

    reg.add("check: 注册表按登记顺序执行", [](CheckContext& ctx) {
        static std::string trace;
        trace.clear();
        xrkit::core::CheckRegistry local;
        local.add("甲", [](CheckContext&) { trace += "a"; });
        local.add("乙", [](CheckContext&) { trace += "b"; });
        for (const auto& item : local.items()) item.second(ctx);
        check(ctx, trace == "ab", "执行顺序异常: " + trace);
    });

    reg.add("engine: M0 阶段如实上报未就绪（不许假绿）", [](CheckContext& ctx) {
        const bool is_ready = xrkit::engine::ready();
        check(ctx, !is_ready, "M0 阶段引擎应为未就绪；若确已就绪，请同步更新本断言");
        const std::string line = xrkit::engine::status_line();
        check(ctx, line.rfind("engine id=", 0) == 0, "status_line 形态不符: " + line);
    });
}

}  // namespace

// 依赖探针的计数：skip 单独统计，绝不混进 ok ——
// "没验证"与"验证通过"是两件事，混起来就是 1.x 反复吃亏的那种假绿。
struct DepCounts {
    int ok = 0;
    int skip = 0;
    int fail = 0;
};

namespace {

void register_dep_checks(xrkit::core::CheckRegistry& reg, DepCounts& counts) {
    using xrkit::core::CheckContext;
    using xrkit::core::check;
    using ProbeFn = xrkit::engine::ProbeResult (*)();

    struct Item {
        const char* name;
        ProbeFn fn;
    };
    static const Item items[] = {
        {"deps: zlib 往返压缩", &xrkit::engine::probe_zlib},
        {"deps: lz4 往返压缩", &xrkit::engine::probe_lz4},
        {"deps: zstd 往返压缩", &xrkit::engine::probe_zstd},
        {"deps: xz/liblzma 往返压缩", &xrkit::engine::probe_xz},
        {"deps: lzo 往返压缩", &xrkit::engine::probe_lzo},
        {"deps: PCRE2 lookbehind 匹配", &xrkit::engine::probe_pcre2},
        {"deps: FreeType 字形栅格", &xrkit::engine::probe_freetype},
    };

    for (const Item& item : items) {
        reg.add(item.name, [fn = item.fn, &counts](CheckContext& ctx) {
            const xrkit::engine::ProbeResult r = fn();
            if (r.skipped) {
                ++counts.skip;
            } else if (r.ok) {
                ++counts.ok;
            } else {
                ++counts.fail;
            }
            xrkit::core::log(r.ok ? xrkit::core::Level::Info : xrkit::core::Level::Warn, r.detail);
            check(ctx, r.ok || r.skipped, r.detail);
        });
    }
}

}  // namespace

int run_cli(const std::vector<std::string>& args) {
    bool verbose = false;
    bool pack_dirs = false;
    bool clean = false;
    std::string text_prefix;
    std::string text_id_prefix;
    std::string text_encoding = "auto";
    std::string action;
    std::vector<std::string> operands;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-v" || a == "--verbose") {
            verbose = true;
            continue;
        }
        if (a == "--pack-dirs") {
            pack_dirs = true;
            continue;
        }
        if (a == "--clean") {
            clean = true;
            continue;
        }
        if (a == "--prefix" && i + 1 < args.size()) {
            text_prefix = args[++i];
            continue;
        }
        if (a == "--id-prefix" && i + 1 < args.size()) {
            text_id_prefix = args[++i];
            continue;
        }
        if (a == "--encoding" && i + 1 < args.size()) {
            text_encoding = args[++i];
            continue;
        }
        if (a == "-h" || a == "--help") {
            print_usage();
            return kExitOk;
        }
        if (a == "--version") {
            std::cout << xrkit::core::version_string() << " build=" << xrkit::core::kBuildCount
                      << "\n";
            return kExitOk;
        }
        if (a.size() > 1 && a[0] == '-' && action.empty()) {
            action = a;
            continue;
        }
        operands.push_back(a);
    }

    if (action.empty()) {
        print_usage();
        return kExitUsage;
    }

    if (action == "--selftest") {
        xrkit::core::CheckRegistry reg;
        DepCounts counts;
        register_core_checks(reg);
        register_dep_checks(reg, counts);
        const int rc = xrkit::core::run_registry(reg, verbose);
        std::cout << "DEPCHECK ok=" << counts.ok << " skip=" << counts.skip
                  << " fail=" << counts.fail << "\n";
        return rc;
    }
    if (action == "--bench") {
        std::cout << "BENCH skip reason=engine-not-implemented\n";
        return kExitSkip;
    }
    if (action == "--themecheck") {
        std::cout << "THEMECHECK skip reason=shell-not-implemented\n";
        return kExitSkip;
    }
    if (action == "--dump") {
        if (operands.empty()) {
            std::cerr << "DUMP error reason=missing-path\n";
            return kExitUsage;
        }
        const std::string& target = operands.front();
        if (is_squashfs_head(target)) {
            const xrkit::engine::fs::ListResult res = xrkit::engine::fs::list_entries(target);
            if (!res.ok) {
                std::cout << "DUMP error reason=" << res.error << "\n";
                return kExitFail;
            }
            // TSV：<d|f> TAB <路径> TAB <大小>（路径与 rdsquashfs --describe 同格式）
            for (const xrkit::engine::fs::Entry& e : res.entries) {
                std::cout << (e.is_dir ? 'd' : 'f') << '\t' << e.path << '\t' << e.size << '\n';
            }
            std::cout << "DUMP ok kind=sqfs entries=" << res.entries.size()
                      << " ms=" << static_cast<long long>(res.elapsed_ms) << "\n";
            return kExitOk;
        }

        const std::vector<uint8_t> raw = read_all(target);
        if (raw.empty()) {
            std::cout << "DUMP error reason=读不到文件或文件为空: " << target << "\n";
            return kExitFail;
        }
        std::optional<xrkit::engine::fs::DbFormat> fmt;
        if (operands.size() >= 2) {
            fmt = xrkit::engine::fs::db_format_from_any(operands[1]);
            if (!fmt) {
                std::cout << "DUMP error reason=未知格式名: " << operands[1] << "\n";
                return kExitUsage;
            }
        }
        if (!fmt) fmt = xrkit::engine::fs::auto_detect(raw);
        if (!fmt) {
            std::cout << "DUMP error reason=无法唯一判定 DB 格式（歧义或损坏，请显式指定格式）\n";
            return kExitFail;
        }
        const xrkit::engine::fs::DbList res = xrkit::engine::fs::unpack_db(raw, *fmt);
        if (!res.ok) {
            std::cout << "DUMP error reason=" << res.error << "\n";
            return kExitFail;
        }
        // TSV：<d|f> TAB 路径 TAB size_real TAB size_comp TAB offset TAB use_lzhuf
        for (const xrkit::engine::fs::DbEntry& e : res.entries) {
            std::cout << (e.is_dir ? 'd' : 'f') << '\t' << e.path << '\t' << e.size_real << '\t'
                      << e.size_comp << '\t' << e.offset << '\t' << (e.use_lzhuf ? 1 : 0) << '\n';
        }
        std::cout << "DUMP ok kind=db format=" << xrkit::engine::fs::db_format_key(*fmt)
                  << " entries=" << res.entries.size()
                  << " ms=" << static_cast<long long>(res.elapsed_ms) << "\n";
        return kExitOk;
    }

    if (action == "--extract") {
        if (operands.size() < 3) {
            std::cerr << "EXTRACT error reason=用法：--extract <源> <内部路径> <输出文件> [DB格式]\n";
            return kExitUsage;
        }
        const std::string& source = operands[0];
        const std::string& rel = operands[1];
        std::vector<uint8_t> data;

        if (is_squashfs_head(source)) {
            const xrkit::engine::fs::FileData r = xrkit::engine::fs::read_file(source, rel);
            if (!r.ok) {
                std::cout << "EXTRACT error reason=" << r.error << "\n";
                return kExitFail;
            }
            data = r.data;
        } else {
            const std::vector<uint8_t> raw = read_all(source);
            std::optional<xrkit::engine::fs::DbFormat> fmt;
            if (operands.size() >= 4) fmt = xrkit::engine::fs::db_format_from_any(operands[3]);
            if (!fmt) fmt = xrkit::engine::fs::auto_detect(raw);
            if (!fmt) {
                std::cout << "EXTRACT error reason=无法唯一判定 DB 格式\n";
                return kExitFail;
            }
            const xrkit::engine::fs::DbList list = xrkit::engine::fs::unpack_db(raw, *fmt);
            if (!list.ok) {
                std::cout << "EXTRACT error reason=" << list.error << "\n";
                return kExitFail;
            }
            const xrkit::engine::fs::DbEntry* found = nullptr;
            for (const xrkit::engine::fs::DbEntry& e : list.entries) {
                if (e.path == rel) {
                    found = &e;
                    break;
                }
            }
            if (found == nullptr) {
                std::cout << "EXTRACT error reason=归档里没有该条目: " << rel << "\n";
                return kExitFail;
            }
            const xrkit::engine::fs::DbExtract r = xrkit::engine::fs::extract_entry(raw, *found);
            if (!r.ok) {
                std::cout << "EXTRACT error reason=" << r.error << "\n";
                return kExitFail;
            }
            data = r.data;
        }

        std::ofstream out(operands[2], std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "EXTRACT error reason=无法写输出文件：" << operands[2] << "\n";
            return kExitFail;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        out.close();
        std::cout << "EXTRACT ok path=" << rel << " bytes=" << data.size() << "\n";
        return kExitOk;
    }

    if (action == "--pack-db") {
        if (operands.size() < 3) {
            std::cerr << "PACKDB error reason=用法：--pack-db <输出文件> <格式> <输入目录>\n";
            return kExitUsage;
        }
        const auto fmt = xrkit::engine::fs::db_format_from_any(operands[1]);
        if (!fmt) {
            std::cout << "PACKDB error reason=未知格式名: " << operands[1] << "\n";
            return kExitUsage;
        }
        std::error_code ec;
        const std::filesystem::path root = path_from_utf8(operands[2]);
        struct Found {
            std::string rel;             // UTF-8，'/' 分隔
            std::filesystem::path full;  // 用宽字符路径读文件（否则非 ASCII 名读不到）
            bool is_dir = false;
        };
        std::vector<Found> found;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            const std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec);
            if (ec) continue;
            found.push_back({path_to_utf8(rel), it->path(), it->is_directory()});
        }
        std::sort(found.begin(), found.end(),
                  [](const Found& a, const Found& b) { return a.rel < b.rel; });
        std::vector<xrkit::engine::fs::DbPackItem> items;
        for (const Found& f : found) {
            xrkit::engine::fs::DbPackItem item;
            item.path = f.rel;
            item.is_dir = f.is_dir;
            if (!f.is_dir) item.data = read_all_path(f.full);
            items.push_back(std::move(item));
        }
        const std::vector<uint8_t> bytes = xrkit::engine::fs::pack_db(items, *fmt);
        std::ofstream out(path_from_utf8(operands[0]), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "PACKDB error reason=无法写输出文件\n";
            return kExitFail;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        std::cout << "PACKDB ok items=" << items.size() << " bytes=" << bytes.size() << "\n";
        return kExitOk;
    }

    // 自写 SquashFS 封包（M1-5）。判据是"与 1.x 的 sqfs_pack / tar2sqfs 逐字节一致"，
    // 所以这里只做**确定性枚举 + 调用引擎**，不做任何"顺手优化"。
    //   枚举规则（测试与它对齐）：递归目录（不跟目录符号链接）、只收普通文件，
    //   相对路径 UTF-8 '\\'→'/'，按 UTF-8 字节序升序。--pack-dirs 时目录也作为条目
    //   （用于验证"显式目录条目"这条路径；1.x 的 fs 页只喂文件，产物应与之一致）。
    if (action == "--pack-sqfs") {
        if (operands.size() < 2) {
            std::cerr << "PACK error reason=用法：--pack-sqfs <输出> <输入目录> [--pack-dirs]\n";
            return kExitUsage;
        }
        const std::string& out_path = operands[0];
        const std::filesystem::path root = path_from_utf8(operands[1]);
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            std::cout << "PACK error reason=输入目录不存在：" << operands[1] << "\n";
            return kExitFail;
        }

        struct Found {
            std::string rel;
            std::string full;
            bool is_dir = false;
        };
        std::vector<Found> found;
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            std::error_code ec2;
            const bool is_dir = it->is_directory(ec2);
            const bool is_file = it->is_regular_file(ec2);
            if (!is_dir && !is_file) continue;
            if (is_dir && !pack_dirs) continue;
            const std::filesystem::path rel = std::filesystem::relative(it->path(), root, ec2);
            if (ec2) continue;
            std::string rel_utf8 = path_to_utf8(rel);
            std::replace(rel_utf8.begin(), rel_utf8.end(), '\\', '/');
            if (rel_utf8.empty() || rel_utf8.rfind("..", 0) == 0) continue;
            found.push_back(Found{rel_utf8, path_to_utf8(it->path()), is_dir});
        }
        std::sort(found.begin(), found.end(),
                  [](const Found& a, const Found& b) { return a.rel < b.rel; });

        std::vector<xrkit::engine::fs::PackItem> items;
        items.reserve(found.size());
        for (const Found& f : found) {
            xrkit::engine::fs::PackItem item;
            item.path = f.rel;
            item.is_dir = f.is_dir;
            item.source_path = f.is_dir ? std::string() : f.full;
            items.push_back(std::move(item));
        }

        const xrkit::engine::fs::PackResult r =
            xrkit::engine::fs::pack_squashfs(out_path, items);
        if (!r.ok) {
            std::cout << "PACK error reason=" << r.error << "\n";
            return kExitFail;
        }
        std::cout << "PACK ok items=" << items.size() << " bytes_used=" << r.bytes_used
                  << " file_size=" << r.file_size << " inodes=" << r.inode_count
                  << " fragments=" << r.fragment_count << " data_blocks=" << r.data_blocks
                  << " frag_blocks=" << r.fragment_blocks << " dedup_blocks=" << r.dedup_blocks
                  << " dedup_frags=" << r.dedup_fragments
                  << " ms=" << static_cast<long long>(r.elapsed_ms) << "\n";
        return kExitOk;
    }

    // 字库可行性探针（M1-7 第①步）：把 FreeType 栅格结果按**定长容器**导出，
    // 供 tests/golden_font_raster.py 与 Pillow 的 getmask 逐字节对拍。
    // 只做"导出"这一件事：比较逻辑在 Python 侧（Pillow 在那边）。
    if (action == "--glyph-dump") {
        if (operands.size() < 3) {
            std::cerr << "GLYPH error reason=用法：--glyph-dump <字体文件> <字号> <输出文件> "
                         "[字符集文件]\n";
            return kExitUsage;
        }
        const std::string& font_path = operands[0];
        const int size = std::atoi(operands[1].c_str());
        const std::string& out_path = operands[2];
        std::vector<uint32_t> cps;
        if (operands.size() >= 4) {
            const std::vector<uint8_t> raw = read_all(operands[3]);
            const std::string text(raw.begin(), raw.end());
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string token = text.substr(pos, end - pos);
                pos = end + 1;
                while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
                    token.pop_back();
                }
                if (token.empty()) continue;
                cps.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
            }
        }
        if (!xrkit::engine::font::dump_glyphs(font_path, size, cps, out_path)) {
            std::cout << "GLYPH error reason=导出失败（字体打不开 / 字号非法 / 写不出文件）\n";
            return kExitFail;
        }
        std::cout << "GLYPH ok font=" << font_path << " size=" << size << " out=" << out_path
                  << "\n";
        return kExitOk;
    }

    // 字库**渲染管线**（M1-7 第④⑤步）：一个字号的 DDS + ini，与 1.x `render_font` 逐字节对拍。
    // 字符集文件 = 每行一个**十六进制码位**（顺序即槽位顺序）—— 两端都以它为准，
    // 不用"UTF-8 文本里有没有尾换行"这种含糊口径。
    if (action == "--font-render") {
        if (operands.size() < 6) {
            std::cerr << "FONTRENDER error reason=用法：--font-render <字体> <渲染字号> <x2:0|1> "
                         "<8888|A8> <字符集文件> <输出前缀>\n";
            return kExitUsage;
        }
        const std::string& font_path = operands[0];
        const int render_size = std::atoi(operands[1].c_str());
        const bool x2 = (operands[2] == "1");
        const std::string& fmt = operands[3];        const std::vector<uint8_t> raw = read_all(operands[4]);
        if (raw.empty()) {
            std::cout << "FONTRENDER error reason=字符集文件读不到或为空：" << operands[4] << "\n";
            return kExitFail;
        }
        std::vector<uint32_t> chars;
        {
            const std::string text(raw.begin(), raw.end());
            size_t pos = 0;
            while (pos < text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string token = text.substr(pos, end - pos);
                pos = end + 1;
                while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
                    token.pop_back();
                }
                if (token.empty()) continue;
                chars.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 16)));
            }
        }
        if (x2) {
            // x2 档已实现（Pillow 的 LANCZOS 见 font/resample.cpp，门禁 [20] 孤立锁）。
        }
        xrkit::engine::font::FontProduct product;
        if (!xrkit::engine::font::render_font(font_path, chars, render_size, x2, fmt, product)) {
            std::cout << "FONTRENDER error reason=" << product.error << "\n";
            return kExitFail;
        }
        const std::string dds_path = operands[5] + ".dds";
        const std::string ini_path = operands[5] + ".ini";
        {
            std::ofstream f(path_from_utf8(dds_path), std::ios::binary | std::ios::trunc);
            if (!f) {
                std::cout << "FONTRENDER error reason=写不出 " << dds_path << "\n";
                return kExitFail;
            }
            f.write(reinterpret_cast<const char*>(product.dds.data()),
                    static_cast<std::streamsize>(product.dds.size()));
            if (!f) {
                std::cout << "FONTRENDER error reason=写 DDS 失败\n";
                return kExitFail;
            }
        }
        const std::string ini_bytes =
            xrkit::engine::font::to_windows_newlines(product.ini);
        {
            // 1.x 用 `open(path, "w", encoding="ascii")` ⇒ 文本模式把 \n 翻成 \r\n。
            std::ofstream f(path_from_utf8(ini_path), std::ios::binary | std::ios::trunc);
            if (!f) {
                std::cout << "FONTRENDER error reason=写不出 " << ini_path << "\n";
                return kExitFail;
            }
            f.write(ini_bytes.data(), static_cast<std::streamsize>(ini_bytes.size()));
            if (!f) {
                std::cout << "FONTRENDER error reason=写 ini 失败\n";
                return kExitFail;
            }
        }
        std::cout << "FONTRENDER ok size=" << render_size << " x2=" << (x2 ? 1 : 0)
                  << " fmt=" << fmt << " chars=" << chars.size()
                  << " fitted=" << product.fitted_size
                  << " tex=" << product.tex_w << "x" << product.tex_h
                  << " max_y=" << product.max_y
                  << " clipped=" << product.clipped
                  << " substituted=" << product.substituted.size()
                  << " dds=" << product.dds.size() << " ini=" << ini_bytes.size() << "\n";
        return kExitOk;
    }

    // 字符集提取（M1-7 第⑦步）：递归扫 XML 目录 → 字符集（每行一个十六进制码位）。
    // 判据：与 1.x `extract_chars` 的 `"".join(sorted(chars))` 完全一致。
    if (action == "--extract-chars") {
        if (operands.empty()) {
            std::cerr << "CHARSET error reason=用法：--extract-chars <xml目录> [输出文件] "
                         "[--no-full-cyrillic]\n";
            return kExitUsage;
        }
        bool full_cyrillic = true;
        std::vector<std::string> args;
        for (const std::string& op : operands) {
            if (op == "--no-full-cyrillic") {
                full_cyrillic = false;
            } else {
                args.push_back(op);
            }
        }
        const std::string& xml_dir = args[0];
        xrkit::engine::font::CharsetResult result;
        if (!xrkit::engine::font::extract_chars(xml_dir, full_cyrillic, result)) {
            std::cout << "CHARSET error reason=目录不存在：" << xml_dir << "\n";
            return kExitFail;
        }
        const std::string body = xrkit::engine::font::charset_to_lines(result.chars);
        if (args.size() >= 2) {
            std::ofstream f(path_from_utf8(args[1]), std::ios::binary | std::ios::trunc);
            if (!f) {
                std::cout << "CHARSET error reason=写不出 " << args[1] << "\n";
                return kExitFail;
            }
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
            if (!f) {
                std::cout << "CHARSET error reason=写失败\n";
                return kExitFail;
            }
        } else {
            std::cout << body;
        }
        std::cout << "CHARSET ok dir=" << xml_dir << " chars=" << result.chars.size()
                  << " files=" << result.files_scanned
                  << " undecodable=" << result.files_undecodable
                  << " guessed=" << result.guessed.size()
                  << " full_cyrillic=" << (full_cyrillic ? 1 : 0) << "\n";
        return kExitOk;
    }

    // 字库几何/宽度探针（M1-7 第③步）：把槽位表、EAW 方块判定、推进宽表、简繁表与
    // 框宽裁决全流程按固定扫描顺序导出，供 tests/golden_font_metrics.py 按 1.x 的
    // `font_pack` 逐条重算对拍。比较逻辑全在 Python 侧（1.x 就是那边的实现）。
    if (action == "--font-metrics") {
        if (operands.empty()) {
            std::cerr << "FONTMETRICS error reason=用法：--font-metrics <输出文件>\n";
            return kExitUsage;
        }
        const std::string& out_path = operands[0];
        if (!xrkit::engine::font::dump_metrics(out_path)) {
            std::cout << "FONTMETRICS error reason=写不出文件\n";
            return kExitFail;
        }
        std::cout << "FONTMETRICS ok out=" << out_path << "\n";
        return kExitOk;
    }

    // 重采样探针（M1-7 第⑥步）：把**原始 8 位图**按 Pillow 的 LANCZOS 重采样后导出，
    // 供 tests/golden_font_resample.py 与 `PIL.Image.resize(..., LANCZOS)` 逐字节对拍。
    // 单独拆出来是为了让 x2 档那个"看着像实则全错"的高危点能被**孤立**验证。
    if (action == "--resample-dump") {
        if (operands.size() < 6) {
            std::cerr << "RESAMPLE error reason=用法：--resample-dump <输入raw> <源宽> <源高> "
                         "<目标宽> <目标高> <输出raw>\n";
            return kExitUsage;
        }
        const std::vector<uint8_t> src = read_all(operands[0]);
        const int sw = std::atoi(operands[1].c_str());
        const int sh = std::atoi(operands[2].c_str());
        const int dw = std::atoi(operands[3].c_str());
        const int dh = std::atoi(operands[4].c_str());
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 ||
            src.size() != static_cast<size_t>(sw) * static_cast<size_t>(sh)) {
            std::cout << "RESAMPLE error reason=输入大小与源宽高不符（" << src.size() << " vs "
                      << sw * sh << "）\n";
            return kExitFail;
        }
        std::vector<uint8_t> dst(static_cast<size_t>(dw) * static_cast<size_t>(dh), 0);
        xrkit::engine::font::resample_lanczos_8bpc(src.data(), sw, sh, dst.data(), dw, dh);
        std::ofstream f(path_from_utf8(operands[5]), std::ios::binary | std::ios::trunc);
        if (!f) {
            std::cout << "RESAMPLE error reason=写不出 " << operands[5] << "\n";
            return kExitFail;
        }
        f.write(reinterpret_cast<const char*>(dst.data()),
                static_cast<std::streamsize>(dst.size()));
        if (!f) {
            std::cout << "RESAMPLE error reason=写失败\n";
            return kExitFail;
        }
        std::cout << "RESAMPLE ok " << sw << "x" << sh << " -> " << dw << "x" << dh
                  << " out=" << operands[5] << "\n";
        return kExitOk;
    }

    // 编码**判定链**（B 链，M1-6 尾段）：六步 → (步号, 编码) + 解码结果。
    // 判据：与 1.x 的 (step, enc, used, forced, 正文) 一致；第 5 步只做已对齐的
    // ASCII 分支，其余家族在门禁的差异台账里如实列出。
    if (action == "--sniff") {
        if (operands.size() < 2) {
            std::cerr << "SNIFF error reason=用法：--sniff <输入文件> <输出文件> "
                         "[--encoding <手动指定>]\n";
            return kExitUsage;
        }
        const std::vector<uint8_t> raw = read_all(operands[0]);
        if (raw.empty() && !std::filesystem::exists(path_from_utf8(operands[0]))) {
            std::cout << "SNIFF error reason=读不到文件：" << operands[0] << "\n";
            return kExitFail;
        }
        const xrkit::engine::text::SniffResult sr =
            xrkit::engine::text::sniff_encoding(raw, text_encoding);
        const xrkit::engine::text::DecodedBytes db =
            xrkit::engine::text::decode_bytes(raw, text_encoding);
        if (!db.ok) {
            std::cout << "SNIFF error reason=" << db.error << "\n";
            return kExitFail;
        }
        std::ofstream out(path_from_utf8(operands[1]), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "SNIFF error reason=无法写输出文件：" << operands[1] << "\n";
            return kExitFail;
        }
        out.write(db.text.data(), static_cast<std::streamsize>(db.text.size()));
        out.close();
        std::cout << "SNIFF ok step=" << static_cast<int>(sr.step) << " enc=" << sr.encoding
                  << " used=" << db.used_encoding << " forced=" << (db.forced ? 1 : 0)
                  << " chars=" << db.chars << " bytes=" << db.text.size() << "\n";
        return kExitOk;
    }

    // 文本提取（M1-6）：gameplay / scripts 一个分支。类型**只看源目录自己的名字**，
    // 与 1.x 的 `_resolve_source_types` 同口径（不向下查找、不要选上层目录）。
    // 产物 = 改写后的源文件树 + 文本 XML；对拍判据是"整棵产物树逐字节一致"。
    if (action == "--extract-text") {
        if (operands.size() < 2) {
            std::cerr << "TEXT error reason=用法：--extract-text <源目录(gameplay|scripts)> <输出目录>"
                         " [--prefix p] [--id-prefix p] [--clean]\n";
            return kExitUsage;
        }
        const std::string& source = operands[0];
        const std::string& target = operands[1];
        xrkit::engine::text::ExtractKind kind{};
        std::string kind_err;
        if (!xrkit::engine::text::kind_from_dir_name(source, kind, kind_err)) {
            std::cout << "TEXT error reason=" << kind_err << "\n";
            return kExitFail;
        }
        const std::string type_dir =
            (kind == xrkit::engine::text::ExtractKind::Gameplay) ? "gameplay" : "scripts";
        const std::filesystem::path target_sub =
            std::filesystem::path(path_from_utf8(target)) / path_from_utf8(type_dir);
        xrkit::engine::text::ExtractOptions opt;
        if (!text_prefix.empty()) opt.prefix = text_prefix;
        opt.id_prefix = text_id_prefix;
        opt.clean = clean;
        const xrkit::engine::text::ExtractStats st =
            xrkit::engine::text::extract_dir(source, path_to_utf8(target_sub), target, opt);
        if (!st.ok) {
            std::cout << "TEXT error reason=" << st.error << "\n";
            return kExitFail;
        }
        for (const std::string& s : st.skipped) {
            std::cout << "TEXT skip " << s << "\n";
        }
        std::cout << "TEXT ok kind=" << type_dir << " files=" << st.files
                  << " strings=" << st.strings << " chars=" << st.chars
                  << " skipped=" << st.skipped.size()
                  << " xml=" << (st.texts_xml_name.empty() ? "-" : st.texts_xml_name)
                  << " ms=" << static_cast<long long>(st.elapsed_ms) << "\n";
        return kExitOk;
    }

    // 文本解码层（M1-6）：六编码链的**黑盒**判据入口。产物是解码后的 UTF-8 正文，
    // 编码名与码位数打在 ASCII 收尾行上，供 tests/golden_text_decode.py 逐文件对拍。
    if (action == "--decode-text") {
        if (operands.size() < 2) {
            std::cerr << "DECODE error reason=用法：--decode-text <输入文件> <输出文件>\n";
            return kExitUsage;
        }
        const xrkit::engine::text::DecodeResult r =
            xrkit::engine::text::read_text_file(operands[0]);
        if (!r.ok) {
            std::cout << "DECODE error reason=" << r.error << "\n";
            return kExitFail;
        }
        std::ofstream out(path_from_utf8(operands[1]), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "DECODE error reason=无法写输出文件：" << operands[1] << "\n";
            return kExitFail;
        }
        out.write(r.text.data(), static_cast<std::streamsize>(r.text.size()));
        out.close();
        std::cout << "DECODE ok enc=" << r.encoding << " chars=" << r.chars
                  << " bytes=" << r.text.size() << "\n";
        return kExitOk;
    }

    // 引擎级小工具：LZHUF 编/解码对拍用（判据要能直接打在 LZHUF 上，而不是隔着 DB 猜）
    if (action == "--lzhuf") {
        if (operands.size() < 3) {
            std::cerr << "LZHUF error reason=用法：--lzhuf <encode|decode> <输入> <输出>\n";
            return kExitUsage;
        }
        const std::vector<uint8_t> in = read_all(operands[1]);
        if (in.empty()) {
            std::cout << "LZHUF error reason=输入为空或读不到\n";
            return kExitFail;
        }
        std::vector<uint8_t> out;
        if (operands[0] == "encode") {
            out = xrkit::engine::fs::lzhuf_encode(in.data(), in.size());
        } else if (operands[0] == "decode") {
            std::string err;
            if (!xrkit::engine::fs::lzhuf_decode(in.data(), in.size(), out, err)) {
                std::cout << "LZHUF error reason=" << err << "\n";
                return kExitFail;
            }
        } else {
            std::cerr << "LZHUF error reason=未知子命令: " << operands[0] << "\n";
            return kExitUsage;
        }
        std::ofstream out_file(operands[2], std::ios::binary | std::ios::trunc);
        if (!out_file) {
            std::cout << "LZHUF error reason=无法写输出文件\n";
            return kExitFail;
        }
        out_file.write(reinterpret_cast<const char*>(out.data()),
                       static_cast<std::streamsize>(out.size()));
        out_file.close();
        std::cout << "LZHUF ok mode=" << operands[0] << " in=" << in.size()
                  << " out=" << out.size() << "\n";
        return kExitOk;
    }

    std::cerr << "未知动作: " << action << "\n";
    print_usage();
    return kExitUsage;
}

#ifdef _WIN32
namespace {

std::string wide_to_utf8(const wchar_t* w) {
    if (w == nullptr) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    out.resize(static_cast<size_t>(need - 1));  // 去掉结尾的 NUL
    return out;
}

}  // namespace

// ★ 用 wmain 而不是 main：`char** argv` 会按 ANSI 代码页转换，西里尔/中文路径全被转坏。
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) args.push_back(wide_to_utf8(argv[i]));
    return run_cli(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run_cli(args);
}
#endif
