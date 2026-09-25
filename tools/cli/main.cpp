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
#include "xrkit/core/theme.h"
#include "xrkit/core/version.h"
#include "xrkit/engine/engine.h"
#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/extract_tree.h"
#include "xrkit/engine/fs/lzhuf.h"
#include "xrkit/engine/fs/overlay.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/font/charset.h"
#include "xrkit/engine/font/glyph.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"
#include "xrkit/engine/font/render.h"
#include "xrkit/engine/font/resample.h"
#include "xrkit/engine/text/convert.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/extract.h"
#include "xrkit/engine/text/sniff.h"
#include "xrkit/engine/third_party_probe.h"
#include "xrkit/engine/video/ffmpeg.h"
#include "xrkit/engine/xml/compare.h"

#include <algorithm>
#include <array>
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
           "  --themecheck    检查主题令牌（两套模式 × 颜色/字体/px/user.ltx 语义；ASCII 收尾行）\n"
           "  --dump <文件> [DB格式]   导出解析结果（SquashFS 条目表 或 DB 条目表 TSV）\n"
           "  --extract <源> <内部路径> <输出> [DB格式]   提取单个文件（进程内解压）\n"
           "  --unpack-tree <镜像> <输出根> [--only <清单文件>]\n"
           "                                              整树导出（安全路径 + 原子写；空清单＝拒绝）\n"
           "  --pack-db <输出> <格式> <输入目录>          按目录打包 DB（逐字节对拍用）\n"
           "  --pack-sqfs <输出> <输入目录> [--pack-dirs]  自写 SquashFS 封包（xz 块，逐字节对拍用）\n"
           "  --decode-text <输入> <输出>                 六编码链解码（把正文写成 UTF-8；对拍用）\n"
           "  --codec-lossy <decode|encode> <编码> <输入> <输出>\n"
           "                                              有损编解码（Python errors=replace；对拍用）\n"
           "  --decl-fix <输入> <目标编码> <输出>          XML 声明规范化（1.x _fix_xml_decl；对拍用）\n"
           "  --sniff <输入> <输出> [--encoding <手动指定>]  判定链（六步）判定 + 解码（对拍用）\n"
           "  --extract-text <源目录> <输出目录> [--prefix p] [--id-prefix p] [--clean]\n"
           "                                              文本提取（gameplay|scripts 一个分支）\n"
           "  --text-plan <源目录> <输出目录>              文本提取的请求守卫（7.1 文件夹名 / 7.5 覆盖源目录）\n"
           "  --glyph-dump <字体> <字号> <输出> [字符集文件]  导出 FreeType 栅格（与 Pillow 对拍）\n"
           "  --font-metrics <输出>                       导出字库几何/宽度表（与 1.x font_pack 对拍）\n"
           "  --font-render <字体> <字号> <x2> <fmt> <字符集文件> <输出前缀>\n"
           "                                              渲染一个字号的 DDS + ini（与 1.x 逐字节对拍）\n"
           "  --resample-dump <输入raw> <源宽> <源高> <目标宽> <目标高> <输出raw>\n"
           "                                              Pillow LANCZOS 重采样（与 Pillow 对拍）\n"
           "  --extract-chars <xml目录> [输出文件] [--no-full-cyrillic]\n"
           "                                              字符集提取（每行一个十六进制码位）\n"
           "  --build-font-pack <SoC|ClS|CoP> <语言> <xml目录> <字体> <输出目录>\n"
           "                    [--suffix s] [--offset n] [--no-full-cyrillic]\n"
           "                                              出整棵 gamedata 汉化包（与 1.x 对拍）\n"
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

    // ⑥6.1/6.2 合并语义：**后者为准**，且 `source` 要跟着切到"引擎实际会读到的那一份"。
    // 这条以前是 first-wins（错的）：`sqzy_patch` 里的同名文件永远输给 `sq_base`。
    reg.add("fs: 合并以后者为准，勾选态跟随原节点（⑥6.1）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        // base 包：a.txt / dir/keep.txt / dir/only_base.txt
        // patch 包：a.txt（同名，应赢）/ dir/keep.txt（同名，应赢）/ dir2/ 目录（类型变化）
        const efs::MergeTree base = efs::build_tree({"sq_base",
                                                     {{"a.txt", false},
                                                      {"dir/keep.txt", false},
                                                      {"dir/only_base.txt", false},
                                                      {"swap", false}}});
        const efs::MergeTree patch = efs::build_tree({"sqzy_patch",
                                                      {{"a.txt", false},
                                                       {"dir/keep.txt", false},
                                                       {"swap/x.txt", false}}});

        efs::MergeTree merged = base;
        // 用户先勾过 base 里的 a.txt 与 dir/keep.txt
        merged.toggle_all(false);
        merged.toggle_dir("a.txt", true);
        merged.toggle_dir("dir/keep.txt", true);
        merged.merge(patch, 1);

        const efs::MergeNode* a = merged.find("a.txt");
        check(ctx, a != nullptr && a->source_index == 1, "同名文件没有以后并入的那份为准");
        // 6.1 的"勾选态跟随原节点"：并进来之后，用户之前勾的还要在
        check(ctx, a != nullptr && a->checked, "合并后丢了原来的勾选态");
        const efs::MergeNode* keep = merged.find("dir/keep.txt");
        check(ctx, keep != nullptr && keep->source_index == 1 && keep->checked,
              "目录下同名文件的胜负/勾选态不对");
        const efs::MergeNode* only_base = merged.find("dir/only_base.txt");
        check(ctx, only_base != nullptr && only_base->source_index == 0,
              "只有 base 有的文件被弄丢了");
        // 类型变化（base 里 swap 是文件，patch 里 swap 是目录）⇒ 以后者为准且其子树可用
        const efs::MergeNode* swap = merged.find("swap");
        check(ctx, swap != nullptr && swap->is_dir, "文件→目录的类型变化没以后者为准");
        check(ctx, merged.find("swap/x.txt") != nullptr, "类型变化后新子树没接上");
        // 同名替换计数 = 2 个同名文件 + 1 个类型变化（base 的 swap 是文件、patch 的是目录）。
        // **类型变化也算一次替换** —— 这正是"后者为准"的含义，不是特例。
        check(ctx, merged.conflict_count() == 3,
              "同名冲突计数不对（应为 3，实测 " + std::to_string(merged.conflict_count()) + "）");
        check(ctx, merged.checked_file_count() == 2,
              "勾选文件计数不对（应为 2，实测 " + std::to_string(merged.checked_file_count()) +
                  "）");
    });

    // ⑥6.2：加载顺序 = 按 basename 小写升序（字典序靠后的包赢）。
    reg.add("fs: 加载序按 basename 小写升序（⑥6.2）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        const std::vector<std::string> paths = {"D:/mods/sq_base", "D:/mods/sqzy_patch",
                                                "D:/mods/SQ_textures"};
        const std::vector<size_t> order = efs::sort_sources_by_basename(paths);
        const std::vector<size_t> want = {0, 2, 1};  // sq_base < sq_textures < sqzy_patch
        check(ctx, order == want, "加载顺序不对（大小写敏感或没排序）");
        // 稳定排序：同名时保持原顺序
        const std::vector<std::string> same = {"a/x", "b/x", "c/x"};
        check(ctx, efs::sort_sources_by_basename(same) == std::vector<size_t>({0, 1, 2}),
              "同名 basename 时没保持稳定顺序");
    });

    // ⑥6.3：`selected`（DB 栏）与 `checked`（包内文件）是**两套独立状态**。
    // 1.x 的判据：DB 栏「全选」只改 `_sel`；包内「全选/全不选」只改 `node.checked`。
    reg.add("fs: 选中态与勾选态互不影响（⑥6.3）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        efs::MergeTree tree = efs::build_tree({"sq_base",
                                               {{"a.txt", false}, {"b.txt", false}}});
        efs::FsSelection sel;
        sel.select_all({"sq_base", "sqzy_patch"});
        check(ctx, sel.size() == 2, "DB 栏全选没生效");
        // 「全选」不许把包内文件也勾上
        check(ctx, tree.checked_file_count() == 0, "DB 栏全选把包内文件也勾上了（两套状态混了）");
        tree.toggle_all(true);
        check(ctx, tree.checked_file_count() == 2, "包内全选没生效");
        check(ctx, sel.size() == 2, "包内全选动了 DB 栏选中态（两套状态混了）");
        sel.clear();
        check(ctx, tree.checked_file_count() == 2, "清空 DB 栏选中态把包内勾选也清了");
    });

    // ⑥6.13：下拉存的是**内部键**。1.x 存显示名 ⇒ `ALL_FMTS.get(fmt)` 必然取不到，
    // SquashFS 被静默按 xdb 打包（文件名还叫 .SquashFS.db）。
    reg.add("fs: 格式下拉用内部键、显示名解析不出来（⑥6.13）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        const std::vector<efs::FsFormatItem>& items = efs::fs_formats();
        check(ctx, items.size() == 7, "格式表条数不对（6 种 DB + SquashFS）");
        int sqfs_count = 0;
        for (const efs::FsFormatItem& item : items) {
            if (item.is_sqfs) ++sqfs_count;
            // 内部键必须能解析（sqfs 除外）
            if (item.is_sqfs) {
                check(ctx, efs::format_key_is_sqfs(item.key), "SquashFS 键没被认出来");
                check(ctx, !efs::format_key_to_db(item.key).has_value(),
                      "SquashFS 键被当成 DB 格式了");
            } else {
                check(ctx, efs::format_key_to_db(item.key).has_value(),
                      "内部键解析不出格式: " + item.key);
            }
            // **显示名一律不许被当成取值解析**（这条就是 1.x 那个事故的判据）
            check(ctx, !efs::format_key_to_db(item.label).has_value(),
                  "显示名被当成下拉取值解析了: " + item.label);
        }
        check(ctx, sqfs_count == 1, "SquashFS 在格式表里不是唯一一条");
        // 历史取值 "sqfs" 继续被识别（老配置/插件条目不能因此失效）
        check(ctx, efs::format_key_is_sqfs("sqfs"), "历史取值 sqfs 不被识别");
    });

    // ⑥6.12：目录条目必须**如实**带 is_dir ⇒ 引擎的 DIRTYPE 分支真的会被走到。
    // 1.x 在 App 侧把 is_dir 写死 False，那条分支在 App 路径下永不触发。
    reg.add("fs: 目录条目真走 DIRTYPE（⑥6.12）", [](CheckContext& ctx) {
        namespace fsys = std::filesystem;
        namespace efs = xrkit::engine::fs;
        std::error_code ec;
        const fsys::path dir = fsys::temp_directory_path(ec) / "xrkit_selftest_dirtree";
        fsys::remove_all(dir, ec);
        fsys::create_directories(dir / "sub" / "deep", ec);
        {
            std::ofstream(dir / "top.txt") << "top";
            std::ofstream(dir / "sub" / "mid.txt") << "mid";
            std::ofstream(dir / "sub" / "deep" / "leaf.txt") << "leaf";
        }
        const std::vector<efs::TreeItem> items = efs::collect_tree(dir.string());
        int dirs = 0;
        int files = 0;
        for (const efs::TreeItem& item : items) {
            if (item.is_dir) {
                ++dirs;
            } else {
                ++files;
                check(ctx, !item.source_path.empty() && item.source_path != item.path,
                      "文件条目没带本机路径: " + item.path);
            }
        }
        // sub / sub/deep = 2 个目录；3 个文件
        check(ctx, dirs == 2, "目录条目数不对（实测 " + std::to_string(dirs) + "）");
        check(ctx, files == 3, "文件条目数不对（实测 " + std::to_string(files) + "）");

        // 端到端：把**带 is_dir** 的条目交给 pack_db → 再解回来必须看得见目录条目。
        // 这就是"DIRTYPE 分支真的可达"的判据（不是看代码里有没有分支）。
        std::vector<efs::DbPackItem> pack;
        for (const efs::TreeItem& item : items) {
            efs::DbPackItem p;
            p.path = item.path;
            p.is_dir = item.is_dir;
            if (!item.is_dir) {
                std::ifstream in(xrkit::core::path_from_utf8(item.source_path), std::ios::binary);
                p.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            pack.push_back(std::move(p));
        }
        for (efs::DbFormat fmt : {efs::DbFormat::Xdb, efs::DbFormat::Ru2947}) {
            const std::vector<uint8_t> bytes = efs::pack_db(pack, fmt);
            check(ctx, !bytes.empty(), std::string("pack_db 没产出: ") + efs::db_format_key(fmt));
            const efs::DbList back = efs::unpack_db(bytes, fmt);
            check(ctx, back.ok, std::string("回读失败: ") + back.error);
            int back_dirs = 0;
            for (const efs::DbEntry& e : back.entries) {
                if (e.is_dir) ++back_dirs;
            }
            check(ctx, back_dirs == dirs,
                  std::string("回读出来的目录条目数不对（") + efs::db_format_key(fmt) + "，实测 " +
                      std::to_string(back_dirs) + "）");
        }
        fsys::remove_all(dir, ec);
    });
    // **任意文件写**。1.x 只把理由写在注释里，2.0 把"拒因"带出来 —— 所以这里连
    // **理由**一起断言：改判定顺序、把某一条删掉，都能当场变红。
    // ⑨ 判定链的**顺序与边界**（9.2/9.4/9.5/9.6/9.7）。整条链的产物对拍在门禁 [16]
    // （153 个真样本）；这里用合成字节把"顺序"本身钉住 —— 顺序错了，产物对拍**未必**红
    // （样本里可能恰好没触发那条边界），而这几条全是"只差一个字符就误判"的地方。
    reg.add("text: 判定链顺序与边界（⑨9.2/9.4/9.5/9.6）", [](CheckContext& ctx) {
        namespace txt = xrkit::engine::text;

        // 9.2 第 2 步（多字节检查）必须在第 3 步（手动指定）**之前**：
        // 真多字节 UTF-8 用单字节编码解必乱码 —— 用户的手动指定在"证据不足"时才生效。
        const std::vector<uint8_t> zh = {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87};  // "中文"
        const txt::SniffResult mb = txt::sniff_encoding(zh, "windows-1251");
        check(ctx, mb.step == txt::SniffStep::MultibyteUtf8,
              "真多字节被手动指定抢走了（9.2：多字节必须优先）");

        // 9.2 的反面：`is_multibyte_utf8` 必须要求 max(raw) >= 0x80 ——
        // 纯 ASCII 不算（否则英文 XML 的编码声明永远改不掉）。
        const std::vector<uint8_t> ascii = {'<', 'a', '/', '>'};
        const txt::SniffResult ascii_override = txt::sniff_encoding(ascii, "windows-1251");
        check(ctx, ascii_override.step == txt::SniffStep::Override,
              "纯 ASCII 被当成多字节了（9.2：必须含 ≥0x80）");

        // 9.4 gb18030 必须排在 windows-1251 **之前**：GBK 的"中文"两个字节
        // （D6 D0 CE C4）在 cp1251 下也能"解成功"（都是合法单字节），顺序错了就整篇乱码。
        const std::vector<uint8_t> gbk_zh = {0xD6, 0xD0, 0xCE, 0xC4};
        const txt::DecodeResult chained = txt::decode_text_chain(gbk_zh);
        check(ctx, chained.ok && chained.encoding == std::string(txt::kGb18030),
              "六编码链把 GBK 字节判成了 " + chained.encoding + "（9.4：gb18030 要在 cp1251 前）");

        // 9.3 单字节家族必须**程序化判定**（"能容错解成 256 个字符"），不许维护名单：
        // 1.x 曾漏掉 cp865 → 整篇乱码。判据落在"末位 latin-1 兜底永远成功且码位齐全"。
        std::vector<uint8_t> all_bytes(256);
        for (int i = 0; i < 256; ++i) all_bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        const txt::DecodedBytes fallback = txt::decode_bytes(all_bytes);
        check(ctx, fallback.ok && fallback.forced && fallback.chars == 256,
              "任意字节没有落到 latin-1 兜底并解出 256 个码位（9.3）");

        // 9.5 XML 声明只看 `raw[:500]`，且**必须被字节证实**
        //（变量别叫 `far`：MSVC 的历史头文件里 `far` 是个空宏，会把 `far.insert(...)`
        //  吃成 `.insert(...)` —— 报一堆莫名其妙的语法错。）
        const std::string decl = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>";
        std::vector<uint8_t> beyond_window(600, 'a');
        beyond_window.insert(beyond_window.end(), decl.begin(), decl.end());
        const txt::SniffResult beyond = txt::sniff_encoding(beyond_window);
        check(ctx, beyond.step != txt::SniffStep::XmlDeclaration,
              "500 字节以外的声明被采信了（9.5：搜索窗口 raw[:500]）");

        // 9.7 的核心判据：**"有声明"不等于"声明赢了"**，标签必须跟着判定走。
        // 这正是 1.x 注释里写下的那类文件：字节是真 UTF-8，声明却写 windows-1251 ——
        // 判定链第 2 步（多字节）胜出，而 1.x 的复算只看"有没有声明"，于是标成
        // `声明(utf-8)`，读起来像"声明说 utf-8"，与事实相反。
        std::vector<uint8_t> mismatched(decl.begin(), decl.end());  // 声明写 windows-1251
        mismatched.insert(mismatched.end(), zh.begin(), zh.end());  // 字节却是 UTF-8
        const txt::SniffResult winner = txt::sniff_encoding(mismatched);
        check(ctx, winner.step == txt::SniffStep::MultibyteUtf8,
              "含声明的真 UTF-8 没被第 2 步判走（9.2/9.7）");
        const std::string winner_label = txt::sniff_result_label(winner);
        check(ctx, winner_label == "多字节(utf-8)",
              "标签没跟着判定走（9.7）：得到 " + winner_label);
        check(ctx, winner_label.find("声明") == std::string::npos,
              "有声明就被标成 `声明(...)` —— 这正是 1.x 的自相矛盾：" + winner_label);

        // 反过来：声明**撒谎到字节不成立**（声明 utf-8、字节是 cp1251）时，
        // 第 4 步必须让位（9.5 的"须被字节证实"）。
        const std::string utf8_decl = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
        std::vector<uint8_t> liar(utf8_decl.begin(), utf8_decl.end());
        liar.insert(liar.end(), {0xEF, 0xE2, 0xE0, 0xEE});  // cp1251，不是合法 UTF-8
        const txt::SniffResult lied = txt::sniff_encoding(liar);
        check(ctx, lied.step != txt::SniffStep::XmlDeclaration,
              "声明撒谎（字节不成立）却被标成 声明 —— 9.5 的「须被字节证实」没生效");
        const std::string label = txt::sniff_result_label(lied);
        check(ctx, label.rfind(txt::sniff_step_label(lied.step), 0) == 0,
              "标签与判定步号不一致（9.7）：" + label);

        // 9.6 宽 BOM **先长后短**：FF FE 00 00 是 UTF-32LE，不能被当成 UTF-16LE
        const std::vector<uint8_t> utf32le = {0xFF, 0xFE, 0x00, 0x00, 'a', 0, 0, 0};
        check(ctx, txt::bom_encoding(utf32le) == std::string(txt::kUtf32),
              "宽 BOM 匹配顺序不对（9.6：先长后短）");
        check(ctx, txt::bom_encoding({0xFF, 0xFE, 'a', 0}) == std::string(txt::kUtf16),
              "UTF-16LE BOM 没认出来");
    });

    // ⑦7.1/7.5 文本提取的请求守卫。判据要点：① 只看**文件夹自己的名字**（大小写不敏感、
    // 无别名、绝不往下找）；② 空输出目录必拒，且文案里写明"源目录只读"；③ 真正被拒的
    // 覆盖情形是**输出目录 = 源目录的父目录**（目标子目录 == 源目录）—— 对照表把它简化
    // 成"输出目录 == 源目录"，照代码判会发现不是那么回事（target == source 时产物落在
    // source/gameplay 子目录里，不覆盖源文件，1.x 并不拒绝）。
    reg.add("text: 提取请求守卫（⑦7.1/7.5）", [](CheckContext& ctx) {
        namespace txt = xrkit::engine::text;
        namespace fsys = std::filesystem;
        std::error_code ec;
        const fsys::path root = fsys::temp_directory_path(ec) / "xrkit_selftest_textreq";
        fsys::remove_all(root, ec);
        fsys::create_directories(root / "gameplay" / "config", ec);
        fsys::create_directories(root / "Scripts", ec);   // 大小写不敏感的证据
        // 名字既不是 gameplay 也不是 scripts 的目录（"上层目录"的典型：
        // 1.x 只看**选中目录自己的名字**，所以哪怕它下面真有 gameplay/ 也不认）
        fsys::create_directories(root / "config", ec);
        fsys::create_directories(root / "out", ec);

        const std::string gameplay = (root / "gameplay").string();
        const std::string scripts_upper = (root / "Scripts").string();
        const std::string not_a_type = (root / "config").string();

        const auto guard = [](const std::string& src, const std::string& dst) {
            txt::ExtractRequest request;
            request.source_dir = src;
            request.target_dir = dst;
            return txt::validate_request(request);
        };

        // 7.1 大小写不敏感 + 只有这两个名字
        check(ctx, guard(gameplay, (root / "out").string()).empty(),
              "gameplay 目录被判非法");
        check(ctx, guard(scripts_upper, (root / "out").string()).empty(),
              "大小写不敏感失效（Scripts 被判非法）");
        const std::string bad_name = guard(not_a_type, (root / "out").string());
        check(ctx, bad_name.find("不要选它们的上层目录") != std::string::npos,
              "选了上层目录却没给出 1.x 那句理由：" + bad_name);
        check(ctx, !guard((root / "nope").string(), (root / "out").string()).empty(),
              "不存在的源目录没被拒");

        // 7.5 输出目录必填（文案里必须写明"源目录只读"这条口径）
        const std::string empty_target = guard(gameplay, "");
        check(ctx, empty_target.find("请指定输出目录") != std::string::npos,
              "空输出目录没被拒：" + empty_target);
        check(ctx, empty_target.find("不会被覆盖") != std::string::npos,
              "空输出目录的文案丢了『源目录不会被覆盖』：" + empty_target);

        // 7.5 覆盖判据：输出目录 = 源目录的**父目录** ⇒ 目标子目录就是源目录 ⇒ 拒
        const std::string parent = guard(gameplay, root.string());
        check(ctx, parent.find("会覆盖源目录中的原文件（且不可恢复）") != std::string::npos,
              "输出目录=源目录父目录没被拒：" + parent);
        // 大小写不同也算撞（Windows normcase 语义）：把父目录名改成大写再试
        std::string upper_root = root.parent_path().string() + "\\XRKIT_SELFTEST_TEXTREQ";
        check(ctx, !guard(gameplay, upper_root).empty(),
              "大小写不同的同一路径没被判为撞车（path_equals 语义）");
        // 反面：输出目录 = 源目录**本身** 时产物落在 source/gameplay 子目录，不覆盖源文件
        //（1.x 的 `_plan_output_dirs` 并不拒绝）—— 判据照代码，不照文档的简化措辞
        check(ctx, guard(gameplay, gameplay).empty(),
              "输出目录=源目录本身被误拒了（1.x 并不拒这一例）");
        fsys::remove_all(root, ec);
    });

    // ⑩10.1/10.3/10.6 XML 统计模式的判据面。10.6 的行数口径是 `len(text.splitlines())`，
    // 最容易错的就是"结尾换行算不算多一行"——1.x 曾对结尾换行额外 +1，于是纯格式差异
    // 被当成不一致。这里把 Python 的边界集合逐条钉住（含 \v \f \x1c-\x1e \x85 \u2028 \u2029）。
    reg.add("xml: 行数口径与模式声明（⑩10.1/10.6）", [](CheckContext& ctx) {
        namespace exml = xrkit::engine::xml;
        // 10.6：结尾是边界不额外多一行
        check(ctx, exml::split_lines("") == 0, "空串应为 0 行");
        check(ctx, exml::split_lines("a") == 1, "无边界应为 1 行");
        check(ctx, exml::split_lines("a\n") == 1, "结尾 \\n 不该多一行（1.x 的老毛病）");
        check(ctx, exml::split_lines("a\nb") == 2, "a\\nb 应为 2 行");
        check(ctx, exml::split_lines("a\nb\n") == 2, "结尾 \\n 不该多一行");
        check(ctx, exml::split_lines("a\n\n") == 2, "两个连续换行 = 2 行（空行算一行）");
        check(ctx, exml::split_lines("a\r\nb\r\n") == 2, "CRLF 结尾不该多一行");
        check(ctx, exml::split_lines("a\rb") == 2, "单独的 \\r 也是边界");
        // Python 的完整边界集合
        const std::vector<std::string> exotic = {"a\x0b" "b", "a\x0c" "b", "a\x1c" "b",
                                                 "a\x1d" "b", "a\x1e" "b", "a\xc2\x85" "b",
                                                 "a\xe2\x80\xa8" "b", "a\xe2\x80\xa9" "b"};
        for (const std::string& text : exotic) {
            check(ctx, exml::split_lines(text) == 2,
                  "Python 的边界字符没被认出来（应为 2 行）");
        }

        // 10.1：内置模式表 = 两条，order 10/20，哨兵值唯一
        const std::vector<exml::XmlModeSpec>& modes = exml::builtin_modes();
        check(ctx, modes.size() == 2, "内置模式应为 2 条");
        check(ctx, modes.size() == 2 && modes[0].order == 10 && modes[1].order == 20,
              "内置模式的 order 应为 10 / 20");
        check(ctx, modes.size() == 2 && modes[0].key == "stats" && modes[1].key == "text",
              "内置模式的内部键应为 stats / text");
        for (const exml::XmlModeSpec& mode : modes) {
            check(ctx, mode.plugin == exml::kBuiltinPlugin,
                  "内置模式的哨兵值不统一（⑫12.8）");
            bool has_detail = false;
            for (const exml::XmlColumn& column : mode.columns) {
                if (column.key == "detail") has_detail = true;
            }
            check(ctx, has_detail, "模式声明里必须有 detail 保留列（" + mode.name + "）");
        }
        // 显示名也能查到模式（历史引用），但**取值**用内部键
        check(ctx, exml::find_mode("stats") != nullptr && exml::find_mode("行数/ID 统计") != nullptr,
              "按内部键或显示名都该能查到模式");
        check(ctx, exml::find_mode("没这个模式") == nullptr, "未知模式名应为 nullptr");
    });

    // ⑩10.3：`detail` 列的单行摘要**只按计数拼** —— 完整明细 builder 的调用次数必须为 0。
    // 1.x 实测：1 万行时填充从 89ms 涨到 327ms，因为每行都调了一次 detail_builder。
    reg.add("xml: 摘要不许偷调完整明细（⑩10.3）", [](CheckContext& ctx) {
        namespace exml = xrkit::engine::xml;
        exml::XmlCompareResult result;
        result.rel = "config/gameplay/x.xml";
        result.lines_a = 100;
        result.lines_b = 101;
        result.ids_a = 50;
        result.ids_b = 50;
        for (int i = 0; i < 1000; ++i) result.only_a.push_back("id_a_" + std::to_string(i));
        for (int i = 0; i < 500; ++i) result.only_b.push_back("id_b_" + std::to_string(i));
        for (int i = 0; i < 200; ++i) {
            result.count_diff.push_back(
                std::array<std::string, 3>{"id_c_" + std::to_string(i), "1", "2"});
        }
        result.consistent = false;

        exml::DetailCallStats stats;
        const std::string summary = exml::inline_detail(result, "A", "B");
        check(ctx, stats.full_detail_calls == 0,
              "摘要路径偷调了完整明细（调用 " + std::to_string(stats.full_detail_calls) + " 次）");
        check(ctx, summary.find("仅 A 有 1000") != std::string::npos &&
                      summary.find("仅 B 有 500") != std::string::npos &&
                      summary.find("次数不同 200") != std::string::npos,
              "摘要没按计数拼：" + summary);

        const std::string full = exml::full_detail(result, "A", "B", &stats);
        check(ctx, stats.full_detail_calls == 1, "完整明细没被计数");
        check(ctx, full.find("id_a_999") != std::string::npos, "完整明细里没有逐条 id");
        // 摘要与完整明细必须**不是**同一个东西（否则就是又把 builder 塞进列里了）
        check(ctx, summary != full, "摘要与完整明细相同（10.3 的判据失效）");
    });

    // ⑩统计模式的**比较**本身：only_a/only_b/count_diff/consistent 四条判据
    // （行数一致性也参与 consistent —— 1.x 的 `compare_one` 就是这么定的）。
    reg.add("xml: 统计模式比较（only_a/only_b/次数/一致）", [](CheckContext& ctx) {
        namespace exml = xrkit::engine::xml;
        const std::string a =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<string_table>\n"
            "  <string id=\"a\"><text>1</text></string>\n"
            "  <string id=\"b\"><text>2</text></string>\n"
            "  <string id=\"b\"><text>3</text></string>\n"
            "</string_table>\n";
        const std::string b =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<string_table>\n"
            "  <string id=\"a\"><text>1</text></string>\n"
            "  <string id=\"b\"><text>2</text></string>\n"
            "  <string id=\"c\"><text>4</text></string>\n"
            "</string_table>\n";

        const exml::XmlCompareResult diff = exml::compare_stats("x.xml", a, b, "utf-8", "utf-8");
        check(ctx, diff.lines_a == 6 && diff.lines_b == 6,
              "行数不对（A=" + std::to_string(diff.lines_a) + " B=" +
                  std::to_string(diff.lines_b) + "）");
        check(ctx, diff.ids_a == 2 && diff.ids_b == 3,
              "ID 数不对（A=" + std::to_string(diff.ids_a) + " B=" + std::to_string(diff.ids_b) +
                  "）");
        check(ctx, diff.only_b.size() == 1 && diff.only_b[0] == "c", "only_b 不对");
        check(ctx, diff.only_a.empty(), "only_a 应为空");
        check(ctx, diff.count_diff.size() == 1 && diff.count_diff[0][0] == "b" &&
                      diff.count_diff[0][1] == "2" && diff.count_diff[0][2] == "1",
              "次数差异不对");
        check(ctx, !diff.consistent, "有差异却判成一致");

        // 完全相同的两份 ⇒ 一致；且**声明里的 id 不算数**（先剥声明）
        const exml::XmlCompareResult same = exml::compare_stats("x.xml", a, a, "utf-8", "utf-8");
        check(ctx, same.consistent, "同一份文本应判一致");
        check(ctx, same.ids_a == 2, "声明里的 id 被算进 ID 数了");

        // 只在一侧存在
        const exml::XmlCompareResult one = exml::only_one_side("only.xml", "b");
        check(ctx, one.only_one_side && !one.consistent &&
                      exml::inline_detail(one, "A", "B").find("仅 B") != std::string::npos,
              "单侧文件没被标成 only_side / 摘要不对");
    });

    // ⑩文本模式（ID 文本对比）：取值口径与"解析失败必须告警"两条。
    // 取值口径最阴的一条（门禁对拍抓出来的）：**判空看原始文本、存的是 strip 之后的值** ——
    // `<specific_character id=...>` 的直接文本是 "\n\t\t"（truthy ⇒ 1.x 记一条 id → ""），
    // 用 strip 判空会把整片 id 漏掉（实测真语料 32 个 id 全丢）。
    reg.add("xml: 文本模式取值与告警（⑩10.1/文本模式）", [](CheckContext& ctx) {
        namespace exml = xrkit::engine::xml;
        const std::string text =
            "<r>\n"
            "  <s id=\"a\"><text>hello &amp; bye</text></s>\n"
            "  <s id=\"b\" other=\"1\">own text</s>\n"
            "  <s id=\"ws\">\n\t\t</s>\n"          // 只有空白：1.x 仍记一条（值为 ""）
            "  <s id=\"kid\">pre<text>inner</text></s>\n"  // 优先取 <text>
            "  <s id=\"dup\"><text>first</text></s>\n"
            "  <s id=\"dup\"><text>second</text></s>\n"    // 后者覆盖前者
            "</r>";
        const exml::XmlTextSide side = exml::parse_text_by_id(text, "utf-8");
        check(ctx, side.ok && side.trusted, "正常 XML 应解析成功且可信：" + side.warning);
        check(ctx, side.id_text.count("a") != 0 && side.id_text.at("a") == "hello & bye",
              "实体没解码（&amp; → &）");
        check(ctx, side.id_text.count("b") != 0 && side.id_text.at("b") == "own text",
              "自有文本没取到");
        check(ctx, side.id_text.count("ws") != 0 && side.id_text.at("ws").empty(),
              "只有空白文本的元素被漏掉了（判空必须看原始文本）");
        check(ctx, side.id_text.count("kid") != 0 && side.id_text.at("kid") == "inner",
              "<text> 子元素没优先");
        check(ctx, side.id_text.count("dup") != 0 && side.id_text.at("dup") == "second",
              "重复 id 应后者覆盖前者");

        // 坏 XML：必须 trusted=false + 有理由（1.x 在这里 log_summary 告警）
        const exml::XmlTextSide broken =
            exml::parse_text_by_id("<r><s id=\"a\"><text>x</text></s>", "utf-8");
        check(ctx, !broken.trusted && !broken.warning.empty(),
              "标签没闭合却没被判成不可信");
        // 注释体里含 `--`：ElementTree 会抛 ⇒ 1.x 判不可信；我们**不许比它更宽**
        const exml::XmlTextSide bad_comment =
            exml::parse_text_by_id("<!-- a -- b --><r><s id=\"a\"><text>x</text></s></r>", "utf-8");
        check(ctx, !bad_comment.trusted, "注释体含 -- 时应判不可信（照 ElementTree）");

        // 比较：同 id 文本不同 ⇒ text_diff；只在一侧 ⇒ only_*
        const std::string other =
            "<r>\n  <s id=\"a\"><text>hello &amp; bye</text></s>\n"
            "  <s id=\"b\" other=\"1\">own TEXT</s>\n"
            "  <s id=\"ws\">\n\t\t</s>\n"
            "  <s id=\"kid\">pre<text>inner</text></s>\n"
            "  <s id=\"dup\"><text>second</text></s>\n"
            "  <s id=\"extra\"><text>only here</text></s>\n"
            "</r>";
        const exml::XmlCompareResult diff =
            exml::compare_text("x.xml", text, other, "utf-8", "utf-8");
        check(ctx, diff.only_b.size() == 1 && diff.only_b[0] == "extra", "only_b 不对");
        check(ctx, diff.text_diff.size() == 1 && diff.text_diff[0][0] == "b",
              "text_diff 不对（应只有 b）");
        check(ctx, !diff.consistent && diff.ids_a == 5 && diff.ids_b == 6,
              "文本模式统计不对（ids_a=" + std::to_string(diff.ids_a) + " ids_b=" +
                  std::to_string(diff.ids_b) + "）");
        check(ctx, exml::inline_detail(diff, "A", "B").find("文本不同 1") != std::string::npos,
              "摘要里没有文本不同计数");
        exml::DetailCallStats stats;
        const std::string full = exml::full_text_detail(diff, "A", "B", &stats);
        check(ctx, stats.full_detail_calls == 1 && full.find("首个差异@") != std::string::npos &&
                      full.find("only here") != std::string::npos,
              "文本模式完整明细不对");
    });

    // ⑧8.1/8.2/8.6/8.7 ffmpeg 定位：**只认两处**（配置里存在的文件 / 程序目录），
    // 不搜 PATH；探测惰性且失败也置位；8.x 拒用；只有手动指定的才写 user.ltx。
    reg.add("video: ffmpeg 只认两处、不搜 PATH（⑧8.1/8.2/8.6/8.7）", [](CheckContext& ctx) {
        namespace vid = xrkit::engine::video;
        namespace fsys = std::filesystem;
        std::error_code ec;
        const fsys::path root = fsys::temp_directory_path(ec) / "xrkit_selftest_video";
        fsys::remove_all(root, ec);
        fsys::create_directories(root / "app", ec);
        fsys::create_directories(root / "bogus_in_path", ec);
        {
            std::ofstream(root / "app" / "ffmpeg.exe") << "x";
            std::ofstream(root / "app" / "ffprobe.exe") << "x";
            std::ofstream(root / "manual_ffmpeg.exe") << "x";
            std::ofstream(root / "manual_ffprobe.exe") << "x";
            // 一个**只存在于"别处"**的同名程序：搜 PATH / 到处找都该找不到它
            std::ofstream(root / "bogus_in_path" / "ffmpeg.exe") << "x";
        }

        // ① 程序目录命中（自动找到的**不写盘** —— 8.7）
        const vid::FfmpegPaths local = vid::find_ffmpeg((root / "app").string(), {});
        check(ctx, local.found && local.has_probe && !local.ffmpeg_from_config,
              "程序目录里的 ffmpeg/ffprobe 没被认出来");
        check(ctx, local.ffmpeg.find("app") != std::string::npos,
              "拿到的不是程序目录里那一份：" + local.ffmpeg);

        // ② 配置里的路径**必须存在**才算数；不存在 ⇒ 落回程序目录
        vid::FfmpegConfig ghost;
        ghost.path = (root / "nowhere" / "ffmpeg.exe").string();
        ghost.probe = (root / "nowhere" / "ffprobe.exe").string();
        const vid::FfmpegPaths ghost_hit = vid::find_ffmpeg((root / "app").string(), ghost);
        check(ctx, ghost_hit.ffmpeg.find("app") != std::string::npos,
              "配置里的路径不存在时没有落回程序目录");
        check(ctx, ghost_hit.has_probe, "配置里的 probe 不存在时没有落回程序目录的 ffprobe");

        // ③ **手动优先**，且两侧独立解析（ffmpeg 来自配置、ffprobe 来自程序目录）
        vid::FfmpegConfig manual;
        manual.path = (root / "manual_ffmpeg.exe").string();
        const vid::FfmpegPaths mixed = vid::find_ffmpeg((root / "app").string(), manual);
        check(ctx, mixed.ffmpeg_from_config && mixed.ffmpeg == manual.path,
              "配置里的 ffmpeg 没有优先");
        check(ctx, !mixed.ffprobe_from_config && mixed.ffprobe.find("app") != std::string::npos,
              "两侧没有独立解析（ffprobe 应落回程序目录）");

        // ④ 8.1 的反面：**不搜 PATH** —— 只把程序放在一个别的目录里，必须判没找到
        const vid::FfmpegPaths nowhere =
            vid::find_ffmpeg((root / "bogus_in_path" / "empty_app").string(), {});
        check(ctx, !nowhere.found && nowhere.ffmpeg.empty(),
              "在程序目录之外找到了 ffmpeg（是不是偷偷搜了 PATH？）");

        // ⑤ 8.2 惰性 + 失败也置位：连探两次只算一次，且"没找到"要被记住
        vid::FfmpegLocator locator((root / "bogus_in_path" / "empty_app").string());
        check(ctx, locator.state() == vid::ProbeState::NotProbed,
              "探测之前状态不是 NotProbed（8.5：探测前不许画 ✗）");
        locator.ensure();
        const int after_first = locator.probe_count();
        check(ctx, locator.state() == vid::ProbeState::Missing,
              "找不到时状态应为 Missing");
        locator.ensure();
        check(ctx, after_first == 1 && locator.probe_count() == 1,
              "惰性探测失效（探了 " + std::to_string(locator.probe_count()) + " 次）");

        // ⑥ 8.6 版本守卫：8.x 拒用（theora 对 STALKER OGM 有 bug）
        check(ctx, vid::major_version("ffmpeg version 7.1-essentials_build") == 7,
              "版本号没解析出来（7.1 那串）");
        check(ctx, vid::major_version("ffmpeg version n8.0 Copyright") == 8,
              "版本号没解析出来（n8.0 那串）");
        check(ctx, vid::version_rejected("ffmpeg version 8.0.1") &&
                      !vid::version_rejected("ffmpeg version 7.1"),
              "8.x 守卫不对");
        check(ctx, vid::major_version("不是版本串") == -1,
              "无法识别的版本串应返回 -1");

        // ⑦ 8.7：**只有手动指定的**写进 user.ltx（自动找到的不写盘）。
        //    这里会真的动一下 exe 旁边的 user.ltx —— 先备份、测完原样写回。
        {
            const std::string ltx = xrkit::core::user_ltx_path();
            std::string backup;
            bool had_file = false;
            {
                std::ifstream in(xrkit::core::path_from_utf8(ltx), std::ios::binary);
                if (in) {
                    had_file = true;
                    backup.assign(std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>());
                }
            }
            vid::FfmpegLocator locator((root / "app").string());
            locator.ensure();  // 自动找到：**写盘了吗？**（不该写）
            const std::string auto_saved = xrkit::core::load_user_value("ffmpeg", "path");
            check(ctx, auto_saved != locator.paths().ffmpeg,
                  "自动查找到的路径被写进 user.ltx 了（8.7：只有手动指定才写）");

            std::string error;
            const std::string manual_path = (root / "manual_ffmpeg.exe").string();
            check(ctx, locator.set_manual(manual_path, "", error),
                  "手动指定写盘失败：" + error);
            check(ctx, xrkit::core::load_user_value("ffmpeg", "path") == manual_path,
                  "手动指定的路径没写进 user.ltx");
            // 手动优先：手动指定后重新探测必须拿到它
            locator.invalidate();
            check(ctx, locator.ensure().ffmpeg == manual_path,
                  "手动指定没有优先（探测拿到的还是程序目录那份）");
            // 不存在的文件必须拒绝
            check(ctx, !locator.set_manual((root / "nowhere" / "ffmpeg.exe").string(), "", error) &&
                          !error.empty(),
                  "手动指定了不存在的文件却没被拒");

            std::error_code cleanup;
            if (had_file) {
                std::ofstream out(xrkit::core::path_from_utf8(ltx), std::ios::binary | std::ios::trunc);
                out.write(backup.data(), static_cast<std::streamsize>(backup.size()));
            } else {
                fsys::remove(xrkit::core::path_from_utf8(ltx), cleanup);
            }
        }
        fsys::remove_all(root, ec);
    });

    // ⑪11.8 CJK 字形四态判定：真字体上逐态验（本机有 msyh=简体全、hpsimplifiedjpan=日语、
    // arial=无 CJK 三种典型形态）。缺字体时**只是不测**，不假绿也不判红。
    reg.add("font: CJK 字形四态判定（⑪11.8）", [](CheckContext& ctx) {
        namespace fnt = xrkit::engine::font;
        namespace fsys = std::filesystem;
        const auto exists = [](const char* path) {
            std::error_code ec;
            return fsys::is_regular_file(xrkit::core::path_from_utf8(path), ec);
        };
        // 判定失败（路径不存在）必须是 unknown，而不是 missing
        const fnt::CjkFontCheck bogus =
            fnt::classify_cjk_font("no_such_font_file_xyz.ttf", 40);
        check(ctx, bogus.state == fnt::CjkFontState::Unknown,
              "打不开的字体应判 unknown（与缺字形分开）");

        const char* msyh = "E:/Software/Games/STALKER/Localization/Tools/otherTools/"
                           "CN_Pack_Generator/msyh.ttf";
        const char* jpan = "C:/Windows/Fonts/hpsimplifiedjpan-regular.ttf";
        const char* latin = "C:/Windows/Fonts/arial.ttf";

        if (exists(msyh)) {
            const fnt::CjkFontCheck cjk = fnt::classify_cjk_font(msyh, 40);
            check(ctx, cjk.state == fnt::CjkFontState::Ok,
                  std::string("简体字体（msyh）应判 ok，实测 ") + fnt::cjk_state_mark(cjk.state));
            check(ctx, cjk.missing.empty(), "msyh 不该有缺字");
        } else {
            xrkit::core::log(xrkit::core::Level::Warn, "自检缺 msyh.ttf，跳过 11.8 的 ok 分支");
        }
        if (exists(jpan)) {
            // 日语字体按 JIS 收字：没有简体专有形、却有繁体形 ⇒ 必须是 japanese
            const fnt::CjkFontCheck jp = fnt::classify_cjk_font(jpan, 40);
            check(ctx, jp.state == fnt::CjkFontState::Japanese,
                  std::string("日语字体（hpsimplifiedjpan）应判 Japanese，实测 ") +
                      fnt::cjk_state_mark(jp.state));
            check(ctx, !jp.missing.empty() && !jp.fallbacks.empty(),
                  "日语字体应给出「缺哪些字 + 能用繁体顶哪些字」");
        } else {
            xrkit::core::log(xrkit::core::Level::Warn,
                             "自检缺 hpsimplifiedjpan，跳过 11.8 的 japanese 分支");
        }
        if (exists(latin)) {
            // 完全画不出汉字的字体：缺字连繁体也补不出来 ⇒ missing（fail-closed）
            const fnt::CjkFontCheck missing = fnt::classify_cjk_font(latin, 40);
            check(ctx, missing.state == fnt::CjkFontState::Missing,
                  std::string("无 CJK 的字体应判 missing，实测 ") +
                      fnt::cjk_state_mark(missing.state));
            check(ctx, missing.fallbacks.empty(), "无 CJK 的字体不该有可用的繁体替代");
        } else {
            xrkit::core::log(xrkit::core::Level::Warn, "自检缺 arial.ttf，跳过 11.8 的 missing 分支");
        }
        // 样本本身是判据的一部分：必须含简体特有的字（老样本漏了它们才会误判）
        const std::string sample = fnt::cjk_sample();
        check(ctx, sample.find("汉") != std::string::npos &&
                      sample.find("测") != std::string::npos &&
                      sample.find("这") != std::string::npos,
              "判定样本里缺了简体特有的字（会被日文字体蒙混过关）");
    });

    // ⑥6.7 路径越界判据（safe_out_path）：归档条目名是不可信输入，这条判据挡的是
    // **任意文件写**。1.x 只把理由写在注释里，2.0 把"拒因"带出来 —— 所以这里连
    // **理由**一起断言：改判定顺序、把某一条删掉，都能当场变红。
    reg.add("fs: 路径越界五条拒因逐条生效（⑥6.7）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        const std::string root = "C:/xrkit_out_root";

        struct Reject {
            const char* rel;
            const char* why;
        };
        const Reject rejects[] = {
            {"", "空"},
            {"/etc/passwd", "绝对路径"},
            {"//server/share/x", "UNC"},
            {"C:/windows/system32/x", "盘符"},
            {"../x", "上级"},
            {"a/../../x", "深层上级"},
            {"a/b/../../../x", "三段上级"},
            {"a/D:/b", "盘符相对（靠兜底复核拦）"},
        };
        for (const Reject& r : rejects) {
            const efs::OutPathResult got = efs::safe_out_path(root, r.rel);
            check(ctx, !got.ok, std::string("应拒而被接受：") + r.why + " [" + r.rel + "]");
            check(ctx, got.ok || !got.reason.empty(),
                  std::string("拒了但没给理由：") + r.why + " [" + r.rel + "]");
        }

        struct Accept {
            const char* rel;
            const char* want;  // 去掉 root 之后的相对路径（'/' 分隔）
        };
        const Accept accepts[] = {
            {"a/b.txt", "a/b.txt"},
            {"a//b.txt", "a/b.txt"},      // 空段被丢掉（1.x 就是这么做的，不能拒）
            {"a/./b.txt", "a/b.txt"},     // `.` 段同上
            {"a\\b.txt", "a/b.txt"},      // 反斜杠先归一
            {"deep/deeper/x.dds", "deep/deeper/x.dds"},
        };
        for (const Accept& a : accepts) {
            const efs::OutPathResult got = efs::safe_out_path(root, a.rel);
            check(ctx, got.ok, std::string("应接受而被拒：") + got.reason + " [" + a.rel + "]");
            if (!got.ok) continue;
            // 落盘路径必须以 root 的规范化结果为**组件级**前缀（不是字符串前缀）
            std::string tail = got.path;
            const size_t pos = tail.find("xrkit_out_root");
            if (pos == std::string::npos) {
                check(ctx, false, "落盘路径里找不到输出根: " + got.path);
                continue;
            }
            tail = tail.substr(pos + std::string("xrkit_out_root").size());
            while (!tail.empty() && (tail.front() == '/' || tail.front() == '\\')) tail.erase(0, 1);
            std::string want = a.want;
            for (char& c : tail) {
                if (c == '\\') c = '/';
            }
            check(ctx, tail == want, "规范化结果不符：得到 " + tail + "，期望 " + want);
        }
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
    std::string only_list;   // --unpack-tree 的 --only <清单文件>（每行一个镜像内路径）
    bool extract_all = false;
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
        if (a == "--only" && i + 1 < args.size()) {
            only_list = args[++i];
            continue;
        }
        if (a == "--all") {
            extract_all = true;
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

    // 整树导出（M2-6b1，对照表 ⑥6.7~6.10/6.14）。**不开外部 exe** 是 2.0 的硬口径
    // （1.x 靠 rdsquashfs --unpack-path / --cat，超时与"把整包收进内存"都栽过）。
    if (action == "--unpack-tree") {
        if (operands.size() < 2) {
            std::cerr << "UNPACK error reason=用法：--unpack-tree <镜像> <输出根> [--only <清单文件>]\n";
            return kExitUsage;
        }
        xrkit::engine::fs::ExtractTreeOptions options;
        if (!only_list.empty() && !extract_all) {
            std::ifstream in(xrkit::core::path_from_utf8(only_list), std::ios::binary);
            if (!in) {
                std::cout << "UNPACK error reason=读不到清单文件：" << only_list << "\n";
                return kExitFail;
            }
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                options.only.push_back(line);
            }
            // 6.10：**空清单 ≠ 全部**。1.x 的 `if not files:` 把两者混在一起，
            // 于是"什么都没勾"会退化成整镜像导出并被记为成功 —— 2.0 显式拒绝。
            options.selection = xrkit::engine::fs::Selection::Listed;
        }
        const xrkit::engine::fs::ExtractTreeResult res =
            xrkit::engine::fs::extract_tree(operands[0], operands[1], options);
        for (const xrkit::engine::fs::ExtractTreeFailure& f : res.failures) {
            std::cout << "UNPACK fail path=" << f.path << " reason=" << f.reason << "\n";
        }
        if (!res.ok) {
            std::cout << "UNPACK error reason=" << res.error << "\n";
            return kExitFail;
        }
        std::cout << "UNPACK ok files=" << res.files_written << " dirs=" << res.dirs_created
                  << " skipped_dirs=" << res.skipped_dirs << " bytes=" << res.bytes_written
                  << " failed=" << res.failures.size()
                  << " ms=" << static_cast<long long>(res.elapsed_ms) << "\n";
        // 有单项失败：**不谎报成功**（1.x 的"解包失败返回 0"就是这么被吞掉的）
        return res.failures.empty() ? kExitOk : kExitFail;
    }

    // 有损编解码探针（Python `errors="replace"` 的逐字节对拍用）。
    // 门禁 [12] 用 Python 同名 codec 驱动它：单字节 / UTF-8 截断 / GB 家族边界。
    if (action == "--codec-lossy") {
        if (operands.size() < 4) {
            std::cerr << "CODEC error reason=用法：--codec-lossy <decode|encode> <编码> <输入> <输出>\n";
            return kExitUsage;
        }
        const std::string& mode = operands[0];
        const std::string& encoding = operands[1];
        std::string body;
        {
            std::ifstream in(xrkit::core::path_from_utf8(operands[2]), std::ios::binary);
            if (!in) {
                std::cout << "CODEC error reason=读不到输入：" << operands[2] << "\n";
                return kExitFail;
            }
            body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::vector<uint8_t> out;
        std::string err;
        if (mode == "decode") {
            std::string text;
            if (!xrkit::engine::text::decode_lossy(
                    encoding, reinterpret_cast<const uint8_t*>(body.data()), body.size(), text,
                    err)) {
                std::cout << "CODEC error reason=" << err << "\n";
                return kExitFail;
            }
            out.assign(text.begin(), text.end());
        } else if (mode == "encode") {
            if (!xrkit::engine::text::encode_lossy(encoding, body, out, err)) {
                std::cout << "CODEC error reason=" << err << "\n";
                return kExitFail;
            }
        } else {
            std::cerr << "CODEC error reason=未知模式（decode|encode）\n";
            return kExitUsage;
        }
        std::ofstream file(xrkit::core::path_from_utf8(operands[3]), std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cout << "CODEC error reason=写不到输出：" << operands[3] << "\n";
            return kExitFail;
        }
        if (!out.empty()) {
            file.write(reinterpret_cast<const char*>(out.data()),
                       static_cast<std::streamsize>(out.size()));
        }
        file.close();
        std::cout << "CODEC ok mode=" << mode << " encoding=" << encoding << " bytes=" << out.size()
                  << "\n";
        return kExitOk;
    }

    // XML 声明规范化探针（1.x `_fix_xml_decl` 的逐字节对拍用）
    if (action == "--decl-fix") {
        if (operands.size() < 3) {
            std::cerr << "DECLFIX error reason=用法：--decl-fix <输入> <目标编码> <输出>\n";
            return kExitUsage;
        }
        std::ifstream in(xrkit::core::path_from_utf8(operands[0]), std::ios::binary);
        if (!in) {
            std::cout << "DECLFIX error reason=读不到输入：" << operands[0] << "\n";
            return kExitFail;
        }
        const std::string body((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        const std::string fixed = xrkit::engine::text::fix_xml_decl(body, operands[1]);
        std::ofstream file(xrkit::core::path_from_utf8(operands[2]), std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cout << "DECLFIX error reason=写不到输出：" << operands[2] << "\n";
            return kExitFail;
        }
        file.write(fixed.data(), static_cast<std::streamsize>(fixed.size()));
        file.close();
        std::cout << "DECLFIX ok bytes=" << fixed.size() << "\n";
        return kExitOk;
    }

    // 文本提取的请求守卫探针（⑦7.1/7.5）：页面与自检共用引擎那一份判据。
    if (action == "--text-plan") {
        if (operands.size() < 2) {
            std::cerr << "TEXTPLAN error reason=用法：--text-plan <源目录> <输出目录>\n";
            return kExitUsage;
        }
        xrkit::engine::text::ExtractRequest request;
        request.source_dir = operands[0];
        request.target_dir = operands[1];
        const std::string reason = xrkit::engine::text::validate_request(request);
        if (!reason.empty()) {
            std::cout << "TEXTPLAN reject reason=" << reason << "\n";
            return kExitFail;
        }
        xrkit::engine::text::ExtractKind kind = xrkit::engine::text::ExtractKind::Gameplay;
        std::string err;
        xrkit::engine::text::kind_from_dir_name(request.source_dir, kind, err);
        std::cout << "TEXTPLAN ok kind="
                  << (kind == xrkit::engine::text::ExtractKind::Gameplay ? "gameplay" : "scripts")
                  << "\n";
        return kExitOk;
    }

    // XML 统计模式探针（⑩10.6 行数口径 + ID 计数）：门禁拿 Python 的 `splitlines()`
    // 与 1.x 的 ID 正则当参照，逐文件对拍。
    if (action == "--xml-stats") {
        if (operands.empty()) {
            std::cerr << "XMLSTATS error reason=用法：--xml-stats <文件> [--encoding X]\n";
            return kExitUsage;
        }
        std::string body;
        {
            std::ifstream in(xrkit::core::path_from_utf8(operands[0]), std::ios::binary);
            if (!in) {
                std::cout << "XMLSTATS error reason=读不到文件：" << operands[0] << "\n";
                return kExitFail;
            }
            body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // 编码：显式给的直接严格解码；auto 走判定链（与 1.x decode_file 同义）
        std::string text;
        std::string used = text_encoding;
        std::string err;
        const std::vector<uint8_t> raw(body.begin(), body.end());
        if (text_encoding == "auto") {
            const xrkit::engine::text::SniffResult sniffed =
                xrkit::engine::text::sniff_encoding(raw, "auto");
            used = sniffed.encoding;
        }
        if (!xrkit::engine::text::decode_lossy(used, raw.data(), raw.size(), text, err)) {
            std::cout << "XMLSTATS error reason=" << err << "\n";
            return kExitFail;
        }
        const xrkit::engine::xml::XmlSideStats stats =
            xrkit::engine::xml::parse_stats_text(text, used);
        std::cout << "XMLSTATS ok lines=" << stats.lines << " ids=" << stats.id_counts.size()
                  << " enc=" << used << "\n";
        return kExitOk;
    }

    // XML 统计模式的**比较**探针：两侧同相对路径
    if (action == "--xml-compare") {
        if (operands.size() < 3) {
            std::cerr << "XMLCMP error reason=用法：--xml-compare <rel> <A 文件> <B 文件>\n";
            return kExitUsage;
        }
        const auto read_text = [](const std::string& path, std::string& text) {
            std::ifstream in(xrkit::core::path_from_utf8(path), std::ios::binary);
            if (!in) return false;
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            const std::vector<uint8_t> raw(body.begin(), body.end());
            std::string err;
            return xrkit::engine::text::decode_lossy("utf-8", raw.data(), raw.size(), text, err);
        };
        std::string text_a;
        std::string text_b;
        if (!read_text(operands[1], text_a) || !read_text(operands[2], text_b)) {
            std::cout << "XMLCMP error reason=读不到输入\n";
            return kExitFail;
        }
        const xrkit::engine::xml::XmlCompareResult result =
            xrkit::engine::xml::compare_stats(operands[0], text_a, text_b, "utf-8", "utf-8");
        std::cout << "XMLCMP ok consistent=" << (result.consistent ? 1 : 0)
                  << " lines_a=" << result.lines_a << " lines_b=" << result.lines_b
                  << " ids_a=" << result.ids_a << " ids_b=" << result.ids_b
                  << " only_a=" << result.only_a.size() << " only_b=" << result.only_b.size()
                  << " count_diff=" << result.count_diff.size() << "\n";
        return kExitOk;
    }

    // XML 文本模式的比较探针（ID 文本对比）
    if (action == "--xml-text") {
        if (operands.size() < 3) {
            std::cerr << "XMLTEXT error reason=用法：--xml-text <rel> <A 文件> <B 文件>\n";
            return kExitUsage;
        }
        const auto read_text = [](const std::string& path, std::string& text) {
            std::ifstream in(xrkit::core::path_from_utf8(path), std::ios::binary);
            if (!in) return false;
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            const std::vector<uint8_t> raw(body.begin(), body.end());
            std::string err;
            return xrkit::engine::text::decode_lossy("utf-8", raw.data(), raw.size(), text, err);
        };
        std::string text_a;
        std::string text_b;
        if (!read_text(operands[1], text_a) || !read_text(operands[2], text_b)) {
            std::cout << "XMLTEXT error reason=读不到输入\n";
            return kExitFail;
        }
        const xrkit::engine::xml::XmlCompareResult result =
            xrkit::engine::xml::compare_text(operands[0], text_a, text_b, "utf-8", "utf-8");
        std::cout << "XMLTEXT ok consistent=" << (result.consistent ? 1 : 0)
                  << " ids_a=" << result.ids_a << " ids_b=" << result.ids_b
                  << " only_a=" << result.only_a.size() << " only_b=" << result.only_b.size()
                  << " text_diff=" << result.text_diff.size()
                  << " untrusted=" << ((result.untrusted_a || result.untrusted_b) ? 1 : 0) << "\n";
        for (const std::array<std::string, 3>& item : result.text_diff) {
            std::cout << "XMLTEXT diff id=" << item[0] << "\ta=" << item[1] << "\tb=" << item[2]
                      << "\n";
        }
        for (const std::string& id : result.only_a) std::cout << "XMLTEXT only_a=" << id << "\n";
        for (const std::string& id : result.only_b) std::cout << "XMLTEXT only_b=" << id << "\n";
        return kExitOk;
    }

    // CSV 渲染探针（10.5 的机制对拍：BOM + \r\n + excel 引号规则）。
    // 表内容固定，只为让"引号/换行/BOM"这三件事能被 Python 的 csv 模块逐字节比对。
    if (action == "--xml-csv-probe") {
        if (operands.empty()) {
            std::cerr << "XMLCSV error reason=用法：--xml-csv-probe <输出 csv>\n";
            return kExitUsage;
        }
        xrkit::engine::xml::XmlCsvReport report;
        report.mode_name = "行数/ID 统计";
        report.folder_a = "A 目录";
        report.folder_b = "B 目录";
        report.filter_label = "全部";
        report.generated_at = "2026-09-24 00:00:00";
        report.columns = {"文件 (相对路径)", "状态", "差异详情"};
        report.rows = {
            {"a.xml", "一致", "无差异"},
            {"b,c.xml", "不一致", "仅 A 有 1；文本不同 2"},
            {"quote\".xml", "不一致", "有 \"引号\" 的摘要"},
            {"multi\nline.xml", "不一致", "摘要里有换行"},
            {"中文.xml", "一致", "无差异"},
            {"", "", ""},
        };
        report.consistent = 3;
        report.inconsistent = 3;
        report.only_a = 1;
        report.only_b = 0;
        report.encodings_used = "A=windows-1251 / B=utf-8";
        std::string error;
        if (!xrkit::engine::xml::write_csv_file(operands[0], report, error)) {
            std::cout << "XMLCSV error reason=" << error << "\n";
            return kExitFail;
        }
        std::cout << "XMLCSV ok bytes=" << xrkit::engine::xml::render_csv(report).size() << "\n";
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

    // 出包（M1-7 第⑦步）：整棵 `gamedata/` 产物树（10 个字号 + XML 转写 + 两个 ltx）。
    // 判据：与 1.x `build_package` 的产物树**逐字节一致**。
    if (action == "--build-font-pack") {
        if (operands.size() < 5) {
            std::cerr << "FONTPACK error reason=用法：--build-font-pack <SoC|ClS|CoP> <语言> "
                         "<xml目录> <字体> <输出目录> [--suffix s] [--offset n] "
                         "[--no-full-cyrillic]\n";
            return kExitUsage;
        }
        xrkit::engine::font::PackageOptions opt;
        opt.game = operands[0];
        opt.lang = operands[1];
        opt.xml_dir = operands[2];
        opt.font_path = operands[3];
        opt.out_dir = operands[4];
        for (size_t i = 5; i < operands.size(); ++i) {
            if (operands[i] == "--suffix" && i + 1 < operands.size()) {
                opt.suffix = operands[++i];
            } else if (operands[i] == "--offset" && i + 1 < operands.size()) {
                opt.offset = std::atoi(operands[++i].c_str());
            } else if (operands[i] == "--no-full-cyrillic") {
                opt.full_cyrillic = false;
            } else if (operands[i] == "--only-size" && i + 1 < operands.size()) {
                opt.only_sizes.push_back(std::atoi(operands[++i].c_str()));
            } else {
                std::cerr << "FONTPACK error reason=未知参数 " << operands[i] << "\n";
                return kExitUsage;
            }
        }
        const xrkit::engine::font::PackageResult r =
            xrkit::engine::font::build_package(opt);
        if (!r.ok) {
            std::cout << "FONTPACK error reason=" << r.error << "\n";
            return kExitFail;
        }
        for (const std::string& w : r.warnings) {
            std::cout << "FONTPACK warn " << w << "\n";
        }
        std::cout << "FONTPACK ok game=" << opt.game << " lang=" << opt.lang
                  << " out=" << r.out_gamedata << " fonts=" << r.fonts_written
                  << " xml=" << r.xml_written << " warnings=" << r.warnings.size() << "\n";
        return kExitOk;
    }

    // 主题令牌检查（M2 前置；数据层在 core，**不碰 Qt**）。
    // 锚点：① 两套模式都能给出全部颜色角色、且都是 #RRGGBB；② 字体角色齐全；
    //       ③ `px()` 的 HiDPI 缩放口径；④ user.ltx 的"读→改一项→整体写回、保留其它段"
    //       （在内存里跑，绝不碰真实 user.ltx）。
    if (action == "--themecheck") {
        int checks = 0;
        int failed = 0;
        auto check = [&](bool good, const std::string& what) {
            ++checks;
            if (!good) {
                ++failed;
                std::cout << "  THEMECHECK fail " << what << "\n";
            }
        };
        auto is_hex_color = [](const char* s) {
            if (s == nullptr || std::strlen(s) != 7 || s[0] != '#') return false;
            for (int i = 1; i < 7; ++i) {
                const char c = s[i];
                const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                                 (c >= 'A' && c <= 'F');
                if (!hex) return false;
            }
            return true;
        };
        const int roles = xrkit::core::theme_role_count();
        check(roles > 0, "颜色角色数 > 0");
        for (int i = 0; i < roles; ++i) {
            const char* role = xrkit::core::theme_role(i);
            check(role != nullptr && *role != '\0', "角色名非空");
            check(is_hex_color(xrkit::core::theme_color(xrkit::core::ThemeMode::Dark, role)),
                  std::string("暗色 ") + (role ? role : "?") + " 是 #RRGGBB");
            check(is_hex_color(xrkit::core::theme_color(xrkit::core::ThemeMode::Light, role)),
                  std::string("亮色 ") + (role ? role : "?") + " 是 #RRGGBB");
        }
        const int fonts = xrkit::core::theme_font_count();
        check(fonts > 0, "字体角色数 > 0");
        for (int i = 0; i < fonts; ++i) {
            const xrkit::core::ThemeFontSpec& f = xrkit::core::theme_font(i);
            check(f.role != nullptr && *f.role != '\0', "字体角色名非空");
            check(f.family != nullptr && *f.family != '\0', "字体族非空");
            check(f.size > 0, "字体字号 > 0");
        }
        // HiDPI：96 DPI 基准值 × 缩放
        xrkit::core::set_ui_scale(1.0);
        check(xrkit::core::px(24) == 24, "px(24) @1.0 = 24");
        xrkit::core::set_ui_scale(1.5);
        check(xrkit::core::px(24) == 36, "px(24) @1.5 = 36");
        xrkit::core::set_ui_scale(2.0);
        check(xrkit::core::px(24) == 48, "px(24) @2.0 = 48");
        xrkit::core::set_ui_scale(1.0);
        // 模式切换（外壳那边要"原地更新"，这里只验数据层的取色跟着模式走）
        xrkit::core::ThemeMode mode = xrkit::core::ThemeMode::Dark;
        check(xrkit::core::theme_mode_from_name("LIGHT", mode) &&
                  mode == xrkit::core::ThemeMode::Light,
              "模式名大小写不敏感");
        xrkit::core::set_theme_mode(xrkit::core::ThemeMode::Light);
        check(std::strcmp(xrkit::core::theme_color_current("bg"),
                          xrkit::core::theme_color(xrkit::core::ThemeMode::Light, "bg")) == 0,
              "切到亮色后取色跟着变");
        xrkit::core::set_theme_mode(xrkit::core::ThemeMode::Dark);
        check(xrkit::core::theme_color_current("__没有这个角色__") == nullptr,
              "未知角色返回 nullptr");
        // user.ltx：改一项、保留其它段（内存里跑）
        const std::string fixture =
            "[theme]\nmode = light\n\n[ffmpeg]\npath = D:/ffmpeg/bin/ffmpeg.exe\nprobe = 1\n\n"
            "[font]\nfile = msyh.ttf\n; 注释与垃圾行\n这不是键值对\n";
        xrkit::core::LtxData data = xrkit::core::parse_ltx(fixture);
        check(data.sections.size() == 3, "user.ltx 解析出 3 段");
        std::string rendered = xrkit::core::render_ltx(data);
        const xrkit::core::LtxData again = xrkit::core::parse_ltx(rendered);
        check(again.sections == data.sections && again.items == data.items,
              "user.ltx 渲染→再解析 幂等");
        check(rendered.find("[ffmpeg]") != std::string::npos &&
                  rendered.find("path = D:/ffmpeg/bin/ffmpeg.exe") != std::string::npos,
              "重写后其它段原样保留");
        std::cout << "THEMECHECK ok roles=" << roles << " fonts=" << fonts << " checks=" << checks
                  << " failed=" << failed << " mode=" << xrkit::core::theme_mode_name(
                         xrkit::core::theme_mode())
                  << " bg=" << xrkit::core::theme_color_current("bg") << "\n";
        return failed == 0 ? kExitOk : kExitFail;
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
