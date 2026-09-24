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
    std::string action;
    std::vector<std::string> operands;

    for (const std::string& a : args) {
        if (a == "-v" || a == "--verbose") {
            verbose = true;
            continue;
        }
        if (a == "--pack-dirs") {
            pack_dirs = true;
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
