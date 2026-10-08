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

#include "xrkit/engine/font/xml_bytes.h"
#include "xrkit/core/check.h"
#include "xrkit/core/log.h"
#include "xrkit/core/path.h"
#include "xrkit/core/atomic_write.h"
#include "build_cmd.h"
#include "version_store_check.h"
#include "xrkit/core/build_version.h"
#include "xrkit/core/theme.h"
#include "xrkit/core/version.h"
#include "xrkit/engine/engine.h"
#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/extract_tree.h"
#include "xrkit/engine/fs/lzhuf.h"
#include "xrkit/engine/fs/overlay.h"
#include "xrkit/engine/plugin/abi_host.h"
#include "xrkit/engine/plugin/contract.h"
#include "xrkit/engine/plugin/loader.h"
#include "xrkit/engine/plugin/py_host.h"
#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/font/charset.h"
#include "xrkit/engine/font/glyph.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"
#include "xrkit/engine/font/render.h"
#include "xrkit/engine/font/resample.h"
#include "xrkit/engine/text/batch.h"
#include "xrkit/engine/text/convert.h"
#include "xrkit/engine/text/encoding.h"
#include "xrkit/engine/text/extract.h"
#include "xrkit/engine/text/sniff.h"
#include "xrkit/engine/third_party_probe.h"
#include "xrkit/engine/video/ffmpeg.h"
#include "xrkit/engine/video/transcode.h"
#include "xrkit/engine/xml/compare.h"
#include "xrkit/engine/xml/dirdiff.h"
#include "xrkit/engine/patch/patch.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int kExitOk = 0;
[[maybe_unused]] constexpr int kExitFail = 1;
// 2 = "没出错，但结论不是正常路径"：`--bench` 未实现；`--sniff` **低置信**（纯猜/无证据）。
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
           "  --unpack-tree <镜像> <输出根> [--only <清单文件>] [--verify-readback]\n"
           "                                              整树导出（**原样摊开**：条目直接落在输出根下；\n"
           "                                              只认 SquashFS；安全路径 + 原子写；空清单＝拒绝）\n"
           "                                              --verify-readback：写后校验回到**读回逐字节复核**\n"
           "                                              （默认是「边写边算哈希」＝一遍 I/O，不读回；\n"
           "                                              那条路少一次读盘，真机 14.9k 文件上省 ≈ 18 s）\n"
           "  --unpack-package <包|目录> [输出根] [--only <清单文件>] [DB格式]\n"
           "        （**输出根可省**：省略 ⇒ 与 GUI 同一条引擎规则 —— 第一个源所在目录；没有源 ⇒ 当前目录）\n"
           "                                              **与 GUI「解包」等价**：任何包类型（SquashFS +\n"
           "                                              DB 六格式）或一个目录（递归找包）；落点＝包名去扩\n"
           "                                              展名、磁盘父目录不进产物、包内已自带同名壳不叠一层\n"
           "                                              （与 GUI 共用同一份落点实现）\n"
           "  --pack-db <输出> <格式> <输入目录>          按目录打包 DB（逐字节对拍用）\n"
           "  --pack-sqfs <输出> <输入目录> [--pack-dirs]  自写 SquashFS 封包（xz 块，逐字节对拍用）\n"
           "  --decode-text <输入> <输出>                 六编码链解码（把正文写成 UTF-8；对拍用）\n"
           "  --codec-lossy <decode|encode> <编码> <输入> <输出>\n"
           "                                              有损编解码（Python errors=replace；对拍用）\n"
           "  --codec-lossy <decode|encode> <编码> <输入> strict <输出>\n"
           "                                              严格语义（decode_strict / encode_strict；判据用）\n"
           "  --codec-encode-table                        big5/shift_jis/euc-kr 反向编码表整表导出（对拍用）\n"
           "  --decl-fix <输入> <目标编码> <输出>          XML 声明规范化（1.x _fix_xml_decl；对拍用）\n"
           "  --sniff <输入> <输出> [--encoding <手动指定>]  判定链（六步）判定 + 解码（对拍用）\n"
           "  --ui-scale-probe                            读 user.ltx 的 [theme] scale（只读；判据用）\n"
           "  --extract-text <源目录> <输出目录> [--prefix p] [--id-prefix p] [--clean]\n"
           "                                              文本提取（gameplay|scripts 一个分支）\n"
           "  --text-plan <源目录> <输出目录>              文本提取的请求守卫（7.1 文件夹名 / 7.5 覆盖源目录）\n"
           "  --plugin-init-rollback <夹具目录>            插件 init 返回 0 必须回滚注册（S1-2 判据探针；\n"
           "                                              目录里只放坏 init 夹具 DLL，按 owner 逐容器断言零残留）\n"
           "  --status-line [--module-subset <a,b,c>]    引擎状态行（模块计数**从模块表算出来**，不是常量自报）。\n"
           "                                              --module-subset 把「当前用到的模块集」收窄（诊断用；\n"
           "                                              给空串 ⇒ 一块都不生效，判据据此证明那一串会跟着变）\n"
           "  --video-plan                                视频转码 argv + 摘要（PLAN/PLANSUM；与 1.x 对拍）\n"
           "  --video-probe <ffprobe-json 文件>           视频参数解析（与 1.x parse_video_info 对拍）\n"
           "  --video-encoders <ffmpeg -encoders 输出>    可用编码器解析（与 1.x _ensure_encoders 对拍）\n"
           "  --video-output-check <源> <参考OGM|-> <输出>  输出目标守卫（argv 恒带 -y：输出=源/参考\n"
           "                                              或已存在时必须能判出来，不许静默覆盖）\n"
           "  --glyph-dump <字体> <字号> <输出> [字符集文件]  导出 FreeType 栅格（与 Pillow 对拍）\n"
           "  --font-metrics <输出>                       导出字库几何/宽度表（与 1.x font_pack 对拍）\n"
           "  --font-render <字体> <字号> <x2> <fmt> <字符集文件> <输出前缀>\n"
           "                                              渲染一个字号的 DDS + ini（与 1.x 逐字节对拍）\n"
           "  --resample-dump <输入raw> <源宽> <源高> <目标宽> <目标高> <输出raw>\n"
           "                                              Pillow LANCZOS 重采样（与 Pillow 对拍）\n"
           "  --extract-chars <xml目录> [输出文件] [--no-full-cyrillic]\n"
           "                                              字符集提取（每行一个十六进制码位）\n"
           "  --convert-batch <源> <输出目录> <源编码> <目标编码> [--ext .xml] [--no-recursive]\n"
           "                  [--backup] [--exclude <模式>]\n"
           "                  批量编码转换（与内置 convert 页同一个引擎实现；判据对拍用它）\n"
           "  --build-font-pack <SoC|ClS|CoP> <语言> <xml目录> <字体> <输出目录>\n"
           "                    [--suffix s] [--offset n] [--no-full-cyrillic]\n"
           "                                              出整棵 gamedata 汉化包（与 1.x 对拍）\n"
           // ★★ 2026-10-05（小件 ①）：`--build-package` **实现早已存在**（`tools/cli/build_cmd.cpp`，
           //   `run_cli()` 里的分派也在），但帮助的动作表里**漏写了这一条** ⇒ `--help` 上
           //   看不见它。用户口径是"CLI 是能力全集"，能力清单里漏一条就是"这功能好像没有"。
           //   用法逐字照 `run_build_package()` 的操作数顺序与 `build_cmd.h` 的契约：
           //   <version.json> <版本> <标记> <序号>（版本口径只有 `version.json` 一处）。
           "  --build-package <version.json> <版本> <标记> <序号>\n"
           "                                              构建 + 打包（GUI 的「构建」按钮走的就是本动作；\n"
           "                                              只此一份实现 —— GUI 只是调它，不自己拼参数）\n"
           "  --python-host   起一次 CPython 插件宿主并打印状态（插件改装 P2；\n"
           "                  `--python-home <目录>` 可显式指定发行包自带的 python/）\n"
           "  --py-plugins [目录]\n"
           "                  Python 插件加载链：起宿主 → 扫 `*.py` → `register(api)` →\n"
           "                  进注册表 → **真消费一次**（调 fs 工具栏那条命令）→ 逐条报账\n"
           "  --ui-facade     控件门面能力表：打印 `xrkit` 模块**实际暴露**的名字（判据与预期表对账，\n"
           "                  多一个即红）＋ 门面可用性（无 GUI 进程里 available=0 且给出理由）\n"
           "  --py-plugin-call [目录] [JSON]\n"
           "                  **无界面**调用插件的非 UI 入口：插件导出 `headless(api, args)`，\n"
           "                  结果以 `PYPLUGINCALL result=<json>` 一行交回（门禁判「产物逐字节\n"
           "                  一致」走这条，因为它不需要界面）\n"
           "  --version       打印版本\n"
           "  --help, -h      本帮助\n"
           "选项:\n"
           "  --plugins <目录> 插件目录（默认 exe 旁边的 plugins/，与 GUI 同一份口径）。\n"
           "                  装了插件 ⇒ 读包的每个入口都过**同一枚解密闸门**（CLI 的动作路径\n"
           "                  以前从不建注册表 ⇒ 加密包会被当普通包读）；被闸门拒绝时\n"
           "                  打印与 GUI **同一条**理由并非零退出。目录不存在/加载失败只记错，\n"
           "                  不阻塞动作（与 GUI 的启动口径一致）。\n"
           "  --python-home <目录>\n"
           "                  CPython 宿主用哪个 Python home（仅 `--python-host` 用；不给则按\n"
           "                  exe 旁 python/ → 系统安装 的顺序解析，口径只有一处）\n"
           "  --verbose, -v   逐条打印自检结果\n"
           "\n"
           "退出码: 0=通过  1=有失败  2=显式跳过（未实现）  3=用法错误\n";
}

// ★ 写盘探针的**作用域闸门**（见 register_core_checks 里那条原子写自校验锁）：
//   `--selftest --verify-readback` 会把探针装在**进程级**，而这一轮自检里别的检查
//   （散装层样本、解密闸门）也用 `write_file_atomic` —— 不给闸门就会把**别人的**检查搞红。
//   只有那条自校验锁自己会把它置位，所以注入永远只影响它自己。
bool g_atomic_probe_armed = false;

// ★★ 2026-10-05（本批小件 ② 行为型锁）：`WriteProbe::record` 的落点 —— 记下每次原子写
//   **实际用的档位**（顺序即调用顺序）。必须是**普通函数指针**能访问到的东西（见锁里的 lambda）。
std::vector<xrkit::core::WriteVerify> g_verify_probe_log;

// 自校验锁**自带**的探针（`--selftest` 单独跑也照样验）。
// ★ 必须是**静态存活期**的对象：`set_write_probe` 存的是函数指针的**值**，但探针结构体本身
//   若是个临时量就会当场析构 —— 之后再取指针就是悬垂（2026-10-05 实测：调用点一条都没触发，
//   注入静默失效，锁"看起来绿"）。CLI 那条 `--verify-readback` 注入同理（见 run_cli，用 static）。
xrkit::core::WriteProbe make_atomic_selftest_probe() {
    xrkit::core::WriteProbe probe;
    probe.tamper_temp_file = [](const std::string& path_utf8) -> bool {
        if (!g_atomic_probe_armed) return false;
        std::filesystem::path p = xrkit::core::path_from_utf8(path_utf8);
        std::fstream io(p, std::ios::binary | std::ios::in | std::ios::out);
        if (!io) return false;
        io.seekp(0, std::ios::beg);
        const char bad = '\x5A';
        io.write(&bad, 1);
        return static_cast<bool>(io);
    };
    return probe;
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

    reg.add("engine: 五个模块都已实现 ⇒ 就绪状态要如实上报", [](CheckContext& ctx) {
        const bool is_ready = xrkit::engine::ready();
        check(ctx, is_ready,
              "五个模块（fs/font/text/xml/video）都已实现，ready() 必须为真（M0 的恒 false 占位已过时）");
        const std::string line = xrkit::engine::status_line();
        check(ctx, line.rfind("engine id=", 0) == 0, "status_line 形态不符: " + line);
        // ★★ 2026-10-06（审计第 4 组）：**不再**断言字面量 `modules=5/5` —— 那条断言正是
        //   "常量自报"能长期活着的原因（它把"自报"当成契约钉住了）。现在的口径：
        //   ① `status_line()` 里的 `modules=<能跑>/<当前用到>` 必须与模块表**算出来的一致**
        //      （模块表由 `probe_<模块>()` 逐个探针支撑，"在编"这件事有链接期证据）；
        //   ② 模块**清单**必须与 `src/engine/` 下的模块一一对应（名字对不上即红）；
        //   ③ `ready` 必须由"全部模块都能跑"推出来（不是也无条件为 1）。
        const int used = xrkit::engine::module_count();
        const int on = xrkit::engine::module_ready_count();
        const int all = xrkit::engine::all_module_count();
        const std::string want = "modules=" + std::to_string(on) + "/" + std::to_string(used);
        check(ctx, line.find(want) != std::string::npos,
              "status_line 的模块计数必须与模块表一致（期望 " + want + "，实得: " + line +
                  "）—— 修前这里是字面量 modules=5/5，与引擎真实状态无关");
        check(ctx, line.find(std::string("ready=") + (is_ready ? "1" : "0")) != std::string::npos,
              "status_line 的 ready 位必须与 ready() 同源，实得: " + line);
        check(ctx, used == all,
              "默认路径下『当前用到的模块数』必须等于『编译进来的模块总数』（实得 " +
                  std::to_string(used) + " / " + std::to_string(all) +
                  "）—— 不等说明模块表被收窄却没走 set_module_subset");
        // 清单逐名核对：六个模块名必须与 `src/engine/` 的目录一一对应
        // （少一个 = 有人加了模块目录却没进模块表；多一个 = 表里写了不存在的模块）。
        const std::vector<std::string> names = xrkit::engine::module_names();
        const char* expected[] = {"fs", "font", "text", "xml", "video", "plugin"};
        for (const char* name : expected) {
            bool found = false;
            for (const std::string& got : names) {
                if (got == name) found = true;
            }
            check(ctx, found, std::string("模块表里缺 ") + name + "（src/engine/ 下确实有这个模块）");
        }
        check(ctx, names.size() == 6,
              "模块表条数应为 6（fs/font/text/xml/video/plugin），实测 " +
                  std::to_string(names.size()));
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
        // ★ 2026-10-06（④）：格式表现在是 **auto + 6 种 DB + SquashFS = 8 条**
        //   （1.x `fs_app.py:154/193` 的下拉第一项就是 "auto"）。
        check(ctx, items.size() == 8, "格式表条数不对（auto + 6 种 DB + SquashFS）");
        check(ctx, !items.empty() && items.front().is_auto &&
                       efs::format_key_is_auto(items.front().key),
              "auto 不是格式表第一项（1.x 的下拉默认值就是 auto）");
        int sqfs_count = 0;
        int auto_count = 0;
        for (const efs::FsFormatItem& item : items) {
            if (item.is_auto) {
                ++auto_count;
                check(ctx, efs::format_key_is_auto(item.key), "auto 键没被认出来");
                // auto ＝**不给格式**：它不是 DB 格式、也不是 SquashFS
                check(ctx, !efs::format_key_to_db(item.key).has_value(),
                      "auto 被当成 DB 格式了");
                check(ctx, !efs::format_key_is_sqfs(item.key), "auto 被当成 SquashFS 了");
                continue;
            }
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
        check(ctx, auto_count == 1, "auto 在格式表里不是唯一一条");
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
        // 1.x 曾漏掉 cp865 → 整篇乱码。判据落在"末位 latin-1 永远成功且码位齐全"。
        // ★ 2026-10-06 **按新口径显式改写**这条期望（原因：单字节家族置信度系统上线）：
        //   旧链：`bytes(range(256))` 落第 6 步默认 windows-1251，候选全失败 ⇒ **兜底**
        //         latin-1，`forced=1`；
        //   新链：统计给出决定性西欧（`statscore=-0.314`、证据 128 字节），而 cp1252 含
        //         未定义字节（81/8D/8F/90/9D）解不开 ⇒ **统计自己选** latin-1，`forced=0`。
        //   两者正文**逐字节相同**（256 个码位）—— 变的是"是选的还是兜底的"，
        //   而这正是新口径要如实区分的东西（`forced` 只表示"落到 latin-1 兜底"）。
        std::vector<uint8_t> all_bytes(256);
        for (int i = 0; i < 256; ++i) all_bytes[static_cast<int>(i)] = static_cast<uint8_t>(i);
        const txt::DecodedBytes fallback = txt::decode_bytes(all_bytes);
        check(ctx, fallback.ok && fallback.used_encoding == std::string(txt::kLatin1) &&
                       fallback.chars == 256,
              "任意字节没有落到 latin-1 并解出 256 个码位（9.3）：实得 used=" +
                  fallback.used_encoding + " chars=" + std::to_string(fallback.chars));
        // 零静默：`forced`/`mism` 两个标志必须与"编码名、实际使用"自洽 ——
        // forced 只表示**兜底**（不是"选了 latin-1"），mism 只表示两者分叉。
        check(ctx, fallback.forced == false && fallback.used_mismatch == false,
              "全字节样本的 forced/mism 标志不自洽（9.3/9.9：选中 ≠ 兜底）");

        // ★★ 2026-10-06 用户口径（⑨9.8/9.9）：声明与"鲁棒"一起进**置信度系统**。
        // ① 谎报 windows-1251 的西语（≥2 个高位字节）⇒ 统计**否决声明**且必须可见
        {
            const std::string lie = "<?xml version=\"1.0\" encoding=\"windows-1251\" ?>";
            std::vector<uint8_t> body(lie.begin(), lie.end());
            const std::string spa = "El ca\xF1\xF3n apunta lejos \xA1" "cuidado!";
            body.insert(body.end(), spa.begin(), spa.end());
            const txt::SniffResult r = txt::sniff_encoding(body);
            check(ctx, r.step == txt::SniffStep::Statistic && r.override_decl &&
                           r.encoding == std::string(txt::kWindows1252),
                  "谎报 windows-1251 的西语没被统计否决（9.8）：step=" +
                      std::to_string(static_cast<int>(r.step)) + " enc=" + r.encoding);
        }
        // ② 反向锁：**诚实**声明 windows-1251 + 真西里尔字节 ⇒ 仍按 1251，不许被统计带偏
        {
            const std::string ok_decl = "<?xml version=\"1.0\" encoding=\"windows-1251\" ?>";
            std::vector<uint8_t> body(ok_decl.begin(), ok_decl.end());
            const std::string rus =
                "\xCF\xF0\xE8\xE2\xE5\xF2, \xF1\xF2\xE0\xEB\xEA\xE5\xF0! \xCE\xEF\xE0\xF1\xED\xEE\xF1\xF2\xFC.";
            body.insert(body.end(), rus.begin(), rus.end());
            const txt::SniffResult r = txt::sniff_encoding(body);
            check(ctx, r.step == txt::SniffStep::XmlDeclaration && !r.override_decl &&
                           r.encoding == std::string(txt::kWindows1251),
                  "诚实声明 windows-1251 的真俄文被统计带偏了（9.8 反向锁）：enc=" + r.encoding);
        }
        // ③ 纯猜必须**可见**：无声明、无证据 ⇒ 第 6 步 + low_confidence（零静默）
        {
            const std::vector<uint8_t> guess = {'a', 'b', 'c', 0x01, 0x1B, 'd'};
            const txt::SniffResult r = txt::sniff_encoding(guess);
            check(ctx, r.step == txt::SniffStep::Default && r.low_confidence,
                  "纯猜（第 6 步默认）没有置低置信标记（9.10 零静默）");
            const std::string flags = txt::sniff_flags_label(r);
            check(ctx, flags.find("低置信") != std::string::npos,
                  "低置信的可见标记没给出（9.10）：flags='" + flags + "'");
        }

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

    // ★★ 2026-10-06（D2 批）：编码转换页"批处理层"的判据面（`engine::text::batch`）。
    //
    // 这批修的是审计里 9 处"不必要阉割"，**最容易悄悄回去的就是"子目录被静默漏掉"**：
    // 旧实现是 `QDir::entryList(QDir::Files)` ⇒ 只读顶层、且一个字都不说。所以这里的
    // 判据按"能不能判红"来设计：
    //   ① 递归真的进去了（深层文件出现在结果里）、并且**报了几层/几个**；
    //   ② 非递归时只认顶层，且把"被挡掉的"计入 `skipped_depth`（不是默默少几个）；
    //   ③ 后缀过滤 / 排除模式 / 跳备份区各自的**计数**对得上；
    //   ④ 备份 → 改坏 → 恢复，**逐字节回到原样**；清空真的删且报数；
    //   ⑤ 导出统计是 utf-8-sig（BOM 在最前面），字段与 1.x 逐行同序；
    //   ⑥ 遍历不存在的目录**必须报错**（旧 `collect_tree` 是静默返回空）。
    reg.add("text: 编码转换批处理（递归/排除/备份三件/统计 CSV）", [](CheckContext& ctx) {
        namespace fsys = std::filesystem;
        namespace etxt = xrkit::engine::text;
        std::error_code ec;
        const fsys::path root = fsys::temp_directory_path(ec) / "xrkit_selftest_convert_batch";
        fsys::remove_all(root, ec);
        fsys::create_directories(root / "sub" / "deep", ec);
        fsys::create_directories(root / "backup", ec);
        {
            std::ofstream(root / "top.xml") << "<a>1</a>";
            std::ofstream(root / "skip.txt") << "not xml";
            std::ofstream(root / "sub" / "mid.xml") << "<b>2</b>";
            std::ofstream(root / "sub" / "deep" / "leaf.xml") << "<c>3</c>";
            std::ofstream(root / "sub" / "deep" / "temp.xml") << "<d>4</d>";
            // 备份区里的东西**不许**参与候选（1.x 按段跳过整个 backup 子树）
            std::ofstream(root / "backup" / "old.xml") << "<e>5</e>";
        }

        // ① 递归（默认开）+ ② 台账：层数 3（sub/deep/leaf.xml）⇒ 4 个 xml 候选
        etxt::CollectOptions co;
        co.extensions = {".xml"};
        co.recursive = true;
        etxt::CollectReport rep;
        const auto deep_files = etxt::collect_files(root.string(), co, &rep);
        check(ctx, rep.depth == 3, "递归层数不对（实测 " + std::to_string(rep.depth) + "）");
        check(ctx, rep.selected == 4,
              "递归候选数不对（实测 " + std::to_string(rep.selected) + "，期望 4）");
        check(ctx, rep.skipped_ext >= 1, "后缀不符没记账（skip.txt）");
        check(ctx, rep.skipped_backup >= 1, "备份区里的文件没被跳过记账");
        bool saw_deep = false;
        for (const etxt::CollectedFile& f : deep_files) {
            if (f.rel == "sub/deep/leaf.xml") saw_deep = true;
        }
        // ★ 这一条就是"子目录被静默漏掉"那个缺陷的锁：非递归时它必须**不在**结果里，
        //   递归时必须在。把递归关掉/写成顶层列举 ⇒ 上面那两条立刻红。
        check(ctx, saw_deep, "递归没进子目录：结果里找不到 sub/deep/leaf.xml");

        // ② 非递归：只剩顶层 1 个，且被挡掉的计入 skipped_depth（不是静默少几个）
        etxt::CollectOptions flat = co;
        flat.recursive = false;
        etxt::CollectReport rep_flat;
        const auto flat_files = etxt::collect_files(root.string(), flat, &rep_flat);
        check(ctx, rep_flat.selected == 1,
              "非递归候选数不对（实测 " + std::to_string(rep_flat.selected) + "，期望 1）");
        check(ctx, rep_flat.skipped_depth == 3,
              "非递归挡掉的文件数没记账（实测 " + std::to_string(rep_flat.skipped_depth) +
                  "，期望 3）");
        for (const etxt::CollectedFile& f : flat_files) {
            check(ctx, f.rel != "sub/deep/leaf.xml", "非递归模式仍然进了子目录：" + f.rel);
        }

        // ③ 排除模式：文件名 + 相对路径两种写法都要命中；`\` 分隔符写法也认
        check(ctx, etxt::is_excluded("sub/deep/temp.xml", "temp.xml", "*temp*"),
              "文件名通配没命中");
        check(ctx, etxt::is_excluded("sub/deep/temp.xml", "temp.xml", "sub/deep/*"),
              "'/' 相对路径通配没命中");
        check(ctx, etxt::is_excluded("sub/deep/temp.xml", "temp.xml", "sub\\deep\\*"),
              "'\\' 相对路径通配没命中（Windows 习惯写法）");
        check(ctx, !etxt::is_excluded("sub/deep/temp.xml", "temp.xml", "other*"),
              "不该命中的模式命中了");
        etxt::CollectOptions excl = co;
        excl.exclude_patterns = "*temp*";
        etxt::CollectReport rep_excl;
        const auto excl_files = etxt::collect_files(root.string(), excl, &rep_excl);
        check(ctx, rep_excl.selected == 3 && rep_excl.skipped_excluded == 1,
              "排除后计数不对（选中 " + std::to_string(rep_excl.selected) + " / 排除 " +
                  std::to_string(rep_excl.skipped_excluded) + "）");
        check(ctx, rep_excl.excluded.size() == 1, "被排除的项没逐条留痕");

        // ⑥ 遍历不存在的目录：必须留下错误（旧实现静默返回空）
        etxt::CollectReport rep_missing;
        const auto none = etxt::collect_files((root / "nope").string(), co, &rep_missing);
        check(ctx, none.empty() && !rep_missing.errors.empty(),
              "遍历不存在的目录没有报错（静默）");

        // ④ 备份三件：backup → 改坏 → restore 回原样；clear 报数
        const std::string top = (root / "top.xml").string();
        const std::string original = "<a>1</a>";
        const etxt::BackupOutcome b1 = etxt::backup_file(top, root.string(), "backup", ".bak");
        check(ctx, b1.ok && !b1.existed && b1.bytes == original.size(),
              "备份没成功：" + b1.error);
        check(ctx, b1.path.find("backup") != std::string::npos &&
                       b1.path.find(".bak") != std::string::npos,
              "备份落点不对：" + b1.path);
        // 再备份一次 ⇒ 已有备份，跳过（不覆盖）
        const etxt::BackupOutcome b2 = etxt::backup_file(top, root.string(), "backup", ".bak");
        check(ctx, b2.ok && b2.existed, "重复备份没有被判为'已有备份'");

        std::ofstream(top, std::ios::binary | std::ios::trunc) << "<a>WRECKED</a>";
        etxt::BackupTaskOptions bopt;
        bopt.source_root = root.string();
        bopt.extensions = {".xml"};
        int restores_seen = 0;
        etxt::BatchHooks hooks;
        hooks.message = [&](const std::string&) { ++restores_seen; };
        const etxt::BackupTaskResult restored = etxt::restore_backups(bopt, hooks);
        check(ctx, restored.total >= 1 && restored.failed == 0,
              "恢复失败（候选 " + std::to_string(restored.total) + " / 失败 " +
                  std::to_string(restored.failed) + "）");
        {
            std::ifstream in(top, std::ios::binary);
            const std::string back((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            check(ctx, back == original,
                  "恢复后内容不是原样（实测 " + std::to_string(back.size()) + " 字节）");
        }
        check(ctx, restores_seen >= 2, "恢复过程没有逐条留痕（零静默）");

        // 恢复的**反面**：目标不存在 ⇒ 该条失败 + 给理由（1.x 同）
        fsys::create_directories(root / "backup" / "sub", ec);
        std::ofstream(root / "backup" / "sub" / "mid.xml.bak", std::ios::binary) << "<b>2</b>";
        fsys::remove(root / "sub" / "mid.xml", ec);
        const etxt::BackupTaskResult restored2 = etxt::restore_backups(bopt, hooks);
        check(ctx, restored2.failed >= 1, "原文件不存在时没报失败（静默跳过）");

        // 清空：真的删掉且报数
        const etxt::BackupTaskResult cleared = etxt::clear_backups(bopt, hooks);
        check(ctx, cleared.total >= 1 && cleared.failed == 0 && cleared.ok == cleared.total,
              "清空备份计数不对（候选 " + std::to_string(cleared.total) + " / 成功 " +
                  std::to_string(cleared.ok) + "）");
        const etxt::BackupTaskResult cleared2 = etxt::clear_backups(bopt, hooks);
        check(ctx, cleared2.total == 0, "清空之后还能列出备份（残留）");

        // 取消：恢复/清空在**文件边界**也要真停（D1 的取消锁在本层的落点）
        std::ofstream(root / "top.xml", std::ios::binary | std::ios::trunc) << "<a>1</a>";
        etxt::backup_file((root / "top.xml").string(), root.string(), "backup", ".bak");
        etxt::BatchHooks cancel_hooks;
        cancel_hooks.should_stop = []() { return true; };
        const etxt::BackupTaskResult cancelled = etxt::clear_backups(bopt, cancel_hooks);
        check(ctx, cancelled.cancelled && cancelled.ok == 0 && cancelled.total >= 1,
              "清空备份没在文件边界响应取消（已删 " + std::to_string(cancelled.ok) + "）");

        // ⑤ 统计 CSV：utf-8-sig（BOM 在最前面）+ 字段逐行同序
        etxt::ConvertStats st;
        st.generated_at = "2026-10-06 12:00:00";
        st.source = root.string();
        st.source_enc = "auto";
        st.target_enc = "utf-8";
        st.candidates = 4;
        st.processed = 4;
        st.success = 3;
        st.skipped = 1;
        st.duration = etxt::format_duration(3725.0);
        check(ctx, st.duration == "1:02:05", "耗时格式不是 Python timedelta 口径：" + st.duration);
        check(ctx, etxt::format_duration(90061.0) == "1 day, 1:01:01",
              "跨天耗时格式不对：" + etxt::format_duration(90061.0));
        const fsys::path csv = root / "stats.csv";
        std::string csv_error;
        check(ctx, etxt::write_stats_csv(csv.string(), st, &csv_error),
              "统计 CSV 写失败：" + csv_error);
        {
            std::ifstream in(csv, std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            check(ctx, body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
                           static_cast<unsigned char>(body[1]) == 0xBB &&
                           static_cast<unsigned char>(body[2]) == 0xBF,
                  "统计 CSV 不是 utf-8-sig（缺 BOM）");
            check(ctx, body.find("生成时间,2026-10-06 12:00:00\n") != std::string::npos,
                  "统计 CSV 缺'生成时间'那行");
            check(ctx, body.find("输出目录,原地转换\n") != std::string::npos,
                  "统计 CSV 的'原地转换'口径不对");
            check(ctx, body.find("耗时,1:02:05\n") != std::string::npos,
                  "统计 CSV 的耗时那行不对");
            check(ctx, body.find("处理模式,文件夹\n") != std::string::npos,
                  "统计 CSV 的处理模式那行不对");
        }

        // 端到端：真的转换一批 ⇒ **部分失败 ⇒ 退出码非 0**；输出保持相对结构
        {
            const fsys::path src = root / "e2e";
            fsys::create_directories(src / "sub", ec);
            std::ofstream(src / "a.xml") << "<?xml version='1.0' encoding='windows-1251'?>\n<x/>";
            std::ofstream(src / "sub" / "b.xml") << "<?xml version=\"1.0\" encoding=\"gbk\"?>\n<y/>";
            const fsys::path out_dir = root / "e2e_out";
            etxt::BatchOptions bo;
            bo.source = src.string();
            bo.output_dir = out_dir.string();
            bo.extensions = {".xml"};
            bo.target_encoding = "utf-8";
            bo.backup = false;
            int messages = 0;
            etxt::BatchHooks bh;
            bh.message = [&](const std::string&) { ++messages; };
            const etxt::BatchOutcome e2e = etxt::convert_batch(bo, bh);
            check(ctx, e2e.exit_code == 0 && e2e.success == 2 && e2e.failed == 0,
                  "端到端转换不为全绿（成功 " + std::to_string(e2e.success) + " / 失败 " +
                      std::to_string(e2e.failed) + " / 退出码 " +
                      std::to_string(e2e.exit_code) + "）");
            // 输出保持**相对结构**（否则不同子目录的同名文件会互相覆盖）
            check(ctx, fsys::exists(out_dir / "sub" / "b.xml"),
                  "输出目录没有保留相对结构（sub/b.xml 不在）");
            check(ctx, messages >= 5, "批处理没有逐条留痕（零静默）");

            // 反面：源路径不存在 ⇒ 退出码 2 + 报错（不许静默成功）
            bo.source = (root / "e2e_missing").string();
            const etxt::BatchOutcome bad = etxt::convert_batch(bo, bh);
            check(ctx, bad.exit_code == 2 && !bad.collect.errors.empty(),
                  "源路径不可用时没有如实报错");

            // ★ 零静默的收口：**部分失败 ⇒ 退出码非 0**。
            //   造法：把输出目录指到一个"已经是文件"的路径 ⇒ 建目录必失败 ⇒ 该文件计入 failed。
            //   （这条锁把 `out.exit_code = bad ? 1 : 0` 改成恒 0 就会立刻红。）
            bo.source = src.string();
            bo.output_dir = (root / "e2e_blocker").string();
            {
                std::ofstream blocker(bo.output_dir, std::ios::binary);
                blocker << "not a dir";
            }
            const etxt::BatchOutcome partial = etxt::convert_batch(bo, bh);
            check(ctx, partial.failed > 0 && partial.exit_code == 1,
                  "部分失败没有如实收口（失败 " + std::to_string(partial.failed) + " / 退出码 " +
                      std::to_string(partial.exit_code) + "，期望失败>0 且退出码 1）");
        }
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

    // ⑫ 插件契约表：槽位（HOSTS/LOCATIONS/AREA_*）与 api.ui 的 16 方法。
    // 判据要点：① 表是**从 1.x 源码按 AST 读出来的生成物**（门禁 [13] 的 --check 锁住）；
    // ② 条数不对/空名字要**报错**，不许静默降级（12.3）；③ 槽位精确匹配（12.4）。
    reg.add("plugin: 契约表与槽位精确匹配（⑫12.1/12.3/12.4）", [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        std::string why;
        // ★★ 2026-10-07（零静默）：修前这一行是
        //     `check(ctx, plug::contract_ok(why), "契约表自检失败：" + why);`
        //   —— C++ **不保证实参求值顺序**（MSVC 实测从右往左）⇒ 那句说明**先于**
        //   `contract_ok(why)` 拼好 ⇒ `why` 还是空的。后果在 2026-10-07 的 D2 批门禁里
        //   实测到了：判据确实红了，但行尾是「契约表自检失败：」**后面什么都没有**，
        //   排障时看不出是哪一条（HOSTS/LOCATIONS/AREAS/api.ui/空键）。
        //   现在先算结论、再拼说明（判据的**理由**也是判据的一部分）。
        const bool contract_ok = plug::contract_ok(why);
        check(ctx, contract_ok, "契约表自检失败：" + why);

        check(ctx, plug::host_count() == 6, "槽位表应有 6 个组件（⑫12.1）");
        // 六个组件的键必须与内置页 id 一致（插件槽位就是按页 id 寻址的）
        const char* expected_hosts[] = {"fs", "convert", "text", "xml", "video", "font"};
        int host_bad = 0;
        for (const char* key : expected_hosts) {
            if (!plug::known_host(key)) ++host_bad;
        }
        check(ctx, host_bad == 0, "六页的 host 键没有全部认出来（缺 " +
                                      std::to_string(host_bad) + " 个）");
        check(ctx, !plug::known_host("fontpack"),
              "host 键认了 'fontpack'（1.x 的 host 是 'font' —— 不许跨槽回退/别名）");
        check(ctx, !plug::known_host("") && plug::host_label("nope") == nullptr,
              "未知 host 应判不合法");
        check(ctx, plug::host_label("xml") != nullptr, "host 标签取不到");

        check(ctx, plug::location_count() == 2 && plug::known_location("toolbar") &&
                      plug::known_location("context"),
              "挂载点表应是 toolbar/context 两项（⑫12.4）");
        check(ctx, !plug::known_location("sidebar"), "未知挂载点应判不合法");

        // AREA_* 槽位（下拉/面板区）：至少要有 top_bar/filter/format 这几个真用到的
        int area_bad = 0;
        for (const char* key : {"top_bar", "bottom", "format", "pack_format", "source_enc",
                                "game", "lang", "size", "suffix", "filter"}) {
            if (!plug::known_area(key)) ++area_bad;
        }
        check(ctx, area_bad == 0,
              "AREA_* 槽位缺 " + std::to_string(area_bad) + " 个（从 1.x 源码读表）");
        // ★★ 2026-10-06（审计第 4 组：「`contract_ok()` 不校 `area` 条数」）：上面那条按
        //   **清单逐名**核对，但它**不校条数** —— 表里多一个 area（或把 `AREA_BOTTOM` 删掉后
        //   又从别处补一个）它照样绿。条数是 `docs/2.0.0-设计口径.md` 写死的契约：10 个。
        //   与 `contract_ok()` 里新加的那条**同一口径**（两处都判，互相兜底）。
        check(ctx, plug::area_count() == 10,
              "槽位表 AREAS 应有 10 个区（含 AREA_BOTTOM），实测 " +
                  std::to_string(plug::area_count()));
        check(ctx, plug::known_area("bottom"),
              "AREA_BOTTOM=\"bottom\" 是 2.x 相对 1.x 明文列出的唯一新增项，不许被删掉");

        // api.ui 的 16 方法：9 造控件 + 7 改控件/取输入（12.3）
        check(ctx, plug::ui_method_count() == 16, "api.ui 契约应有 16 个方法");
        for (const char* name : {"label", "button", "row", "checkbox", "entry", "text", "combo",
                                 "drop_box", "pack", "set_text", "set_state", "text_view", "alive",
                                 "ask_yes_no", "ask_open_file", "ask_save_file"}) {
            check(ctx, plug::find_ui_method(name) != nullptr,
                  std::string("api.ui 契约里缺方法：") + name);
        }
        check(ctx, plug::find_ui_method("draw_chart") == nullptr,
              "契约里不该有 1.x 没有的方法");
        const plug::UiMethodSpec* button = plug::find_ui_method("button");
        check(ctx, button != nullptr && std::string(button->params).find("on_click") !=
                                            std::string::npos,
              "参数名也是契约（button 应有 on_click）");
        const plug::UiMethodSpec* label = plug::find_ui_method("label");
        check(ctx, label != nullptr && std::string(label->params).find("parent") !=
                                           std::string::npos &&
                      std::string(label->params).find("style") != std::string::npos,
              "参数名也是契约（label 应有 parent/style）");
    });

    // ⑫12.1/12.4~12.9 注册表：**10 个注册点逐点验"真的被消费"**
    // （1.x 的判据原话：注册成功但无人消费 = FAIL），外加槽位精确匹配、排序规则、
    // when 三形态、scan 不清 compare_modes、回调异常不冒出去。
    reg.add("plugin: 十个注册点与消费端（⑫12.1/12.4~12.9）", [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        namespace exml = xrkit::engine::xml;
        plug::Registry reg;

        // ── 1) 解密器 / 2) 格式：两个"能力"注册点 ──
        bool decryptor_used = false;
        check(ctx, reg.register_decryptor("t.dll", [](const std::string& path) {
                  return path.find("zzzz") != std::string::npos;
              },
              [&decryptor_used](const std::string& path, std::string& out) {
                  decryptor_used = path.empty();
                  out = "x";
                  return true;
              }),
              "注册解密器失败");
        check(ctx, reg.find_decryptor("a/zzzz.sq_base") != nullptr &&
                      reg.find_decryptor("a/plain.sqfs") == nullptr,
              "解密器没有被消费端按 check 命中（12.1）");
        // check 抛异常 ⇒ 跳过这一个（1.x `find_decryptor` 的 try/except）
        plug::Registry bad_dec;
        bad_dec.register_decryptor("bad.dll", [](const std::string&) -> bool {
            throw std::runtime_error("boom");
        }, [](const std::string&, std::string&) { return true; });
        check(ctx, bad_dec.find_decryptor("x") == nullptr, "抛异常的 check 没被跳过");

        check(ctx, reg.register_format("t.dll", "myfmt", [](const std::string&) { return true; }),
              "注册格式失败");
        check(ctx, reg.formats().size() == 1 && reg.formats()[0].name == "myfmt",
              "格式没有被消费端看到（12.1）");

        // ── 3) 命令 + 4) 挂载点 ──
        const auto cmd = reg.register_command("t.dll", "export_csv", "导出 CSV", [] {});
        check(ctx, cmd.has_value() && *cmd == "t.dll:export_csv",
              "命令 id 应是 <插件>:<动作名>（12.1）");
        check(ctx, !reg.register_command("t.dll", "bad name!", "x", [] {}).has_value(),
              "非法动作名应被拒");
        check(ctx, !reg.register_command("t.dll", "export_csv", "x", [] {}).has_value(),
              "重复命令应被拒");
        check(ctx, !reg.register_command("t.dll", "ok_name", "x", nullptr).has_value(),
              "空 handler 应被拒");
        check(ctx, !reg.register_menu_item("t.dll", "nope", "toolbar", *cmd),
              "未知 host 应被拒（12.4）");
        check(ctx, !reg.register_menu_item("t.dll", "xml", "sidebar", *cmd),
              "未知 location 应被拒（12.4）");
        check(ctx, reg.registration_errors().size() >= 5,
              "被拒的注册必须留下 registration_errors（不许静默）");
        check(ctx, reg.register_menu_item("t.dll", "xml", "toolbar", *cmd, 10), "挂载点注册失败");
        check(ctx, reg.register_menu_item("t.dll", "xml", "toolbar", "t.dll:missing", 0) == false,
              "挂载点引用未注册命令应被拒");
        check(ctx, reg.menu_items_for("xml", "toolbar").size() == 1 &&
                      reg.menu_items_for("xml", "context").empty() &&
                      reg.menu_items_for("fs", "toolbar").empty(),
              "挂载点是**精确匹配**（不跨槽回退，12.4）");

        // ── 5) 下拉条目 + 6) 新建下拉 ──
        check(ctx, reg.register_option_entry("t.dll", "xml", "filter", "我的筛选"), "条目注册失败");
        check(ctx, reg.register_option_entry("t.dll", "xml", "filter", "我的筛选"),
              "重复条目注册失败");
        check(ctx, !reg.register_option_entry("t.dll", "nope", "filter", "x"), "未知 host 应被拒");
        const std::vector<std::string> entries = reg.option_entries_for("xml", "filter");
        check(ctx, entries.size() == 1 && entries[0] == "我的筛选",
              "下拉条目没被消费或没保序去重（12.5）");
        check(ctx, reg.register_option("t.dll", "我的下拉", {"a", "b"}, [] {}, "fs", "top_bar", {},
                                       5),
              "新建下拉注册失败");
        check(ctx, reg.options_for("fs", "top_bar").size() == 1 &&
                      reg.options_for("fs", "bottom").empty(),
              "新建下拉没被消费（12.1）");

        // ── 7) 面板 ──
        check(ctx, reg.register_panel("t.dll", "我的面板", [](void*) { return nullptr; }, "xml",
                                      "top_bar"),
              "面板注册失败");
        check(ctx, !reg.register_panel("t.dll", "坏面板", nullptr, "xml", "top_bar"),
              "空 builder 应被拒");
        check(ctx, reg.panels_for("xml", "top_bar").size() == 1 &&
                      reg.panels_for("xml", "bottom").empty(),
              "面板没被消费（12.1）");

        // ── 8) Hub 页（工具）──
        check(ctx, reg.register_tool("t.dll", "我的页", [](void*) { return nullptr; }, 1),
              "工具页注册失败");
        check(ctx, !reg.register_tool("t.dll", "坏页", nullptr),
              "空 builder 应被拒");
        check(ctx, reg.tools_sorted().size() == 1 && reg.tools_sorted()[0]->name == "我的页",
              "工具页没被消费（12.1）");

        // ── 9) 比较模式（唯一哨兵 + 内置优先 + order 稳定排序）──
        auto make_mode = [](const std::string& name, const std::string& key, int order) {
            plug::CompareModeReg mode;
            mode.spec.name = name;
            mode.spec.key = key;
            mode.spec.order = order;
            mode.spec.columns = {{"rel", "文件", 100}, {"detail", "差异", 100}};
            mode.compare = [](const std::string& rel, const std::string&, const std::string&,
                              const std::string&, const std::string&) {
                exml::XmlCompareResult r;
                r.rel = rel;
                r.consistent = true;
                return r;
            };
            return mode;
        };
        check(ctx, reg.register_compare_mode("t.dll", make_mode("我的模式", "mine", 30)),
              "比较模式注册失败");
        check(ctx, !reg.register_compare_mode("t.dll", make_mode("", "", 0)),
              "空名字/空键的模式应被拒");
        plug::CompareModeReg no_compare = make_mode("无比较", "nocompare", 0);
        no_compare.compare = nullptr;
        check(ctx, !reg.register_compare_mode("t.dll", no_compare), "缺 compare 的模式应被拒");
        // 内置模式（哨兵由注册表统一）：插件与内置同名时**内置赢**（与注册顺序无关）
        plug::CompareModeReg clashing = make_mode("行数/ID 统计", "plugin_stats", 1);
        check(ctx, reg.register_compare_mode(exml::kBuiltinPlugin, clashing),
              "内置模式注册失败");
        int modes_seen = 0;
        bool builtin_won = false;
        for (const plug::CompareModeReg* mode : reg.compare_modes_for()) {
            if (mode->spec.name == "行数/ID 统计") {
                ++modes_seen;
                builtin_won = mode->spec.plugin == exml::kBuiltinPlugin &&
                              mode->spec.key == "plugin_stats";
            }
        }
        check(ctx, modes_seen == 1 && builtin_won,
              "同名模式下内置没有赢 / 没有去重（12.5/12.8）");
        check(ctx, reg.compare_mode("mine") != nullptr && reg.compare_mode("没有这个") == nullptr,
              "按内部键取模式失败");
        const std::vector<const plug::CompareModeReg*> ordered = reg.compare_modes_for();
        check(ctx, ordered.size() == 2 && ordered[0]->spec.key == "plugin_stats",
              "比较模式没按 order 升序（内置那条 order=1 应在前）");

        // ── 10) scan()：清插件贡献但**不动** compare_modes（12.7）──
        const size_t before = reg.compare_modes_for().size();
        reg.scan_clear();
        check(ctx, reg.command_count() == 0 && reg.formats().empty() &&
                      reg.menu_items_for("xml", "toolbar").empty() &&
                      reg.registration_errors().empty(),
              "scan 没有清掉插件贡献");
        check(ctx, reg.compare_modes_for().size() == before,
              "scan() 把 compare_modes 也清了（12.7：内置与插件共用一张表）");

        // ── 12.6 when 三形态 ──
        plug::WhenSpec always;
        check(ctx, plug::when_ok(always, {}), "空 when 应恒真");
        plug::WhenSpec match;
        match.match.push_back({"mode", std::string("文本")});
        plug::Context hit;
        hit["mode"] = std::string("文本");
        plug::Context miss;
        miss["mode"] = std::string("统计");
        check(ctx, plug::when_ok(match, hit) && !plug::when_ok(match, miss) &&
                      !plug::when_ok(match, {}),
              "dict 形式 when 不满足时判真（缺键应不满足）");
        plug::WhenSpec null_match;
        null_match.match.push_back({"k", plug::ContextValue{}});  // 期望 None
        check(ctx, plug::when_ok(null_match, {}),
              "期望 None 时缺键应命中（Python 的 ctx.get(k) == None 语义）");
        plug::WhenSpec boom;
        boom.predicate = [](const plug::Context&) -> bool { throw std::runtime_error("x"); };
        check(ctx, !plug::when_ok(boom, {}), "when 回调抛异常应判 False");
        plug::WhenSpec pred;
        pred.predicate = [](const plug::Context& c) {
            const auto it = c.find("loaded");
            return it != c.end() && it->second.has_value() && *it->second == "1";
        };
        plug::Context loaded;
        loaded["loaded"] = std::string("1");
        check(ctx, plug::when_ok(pred, loaded) && !plug::when_ok(pred, {}),
              "可调用 when 没生效");

        // ── 12.9 回调抛异常：绝不冒出去，也绝不静默 ──
        plug::Registry throwing;
        throwing.invoke("t.dll", "命令执行", [] { throw std::runtime_error("kaboom"); });
        check(ctx, throwing.registration_errors().size() == 1 &&
                      throwing.registration_errors()[0].message.find("kaboom") != std::string::npos,
              "插件回调抛异常没被捕获记账（12.9）");
    });

    // 视频进度/取消（口径见 docs/2.0.0-视频模块规格.md §3；1.x convert() 340–372 行）
    reg.add("video: 进度取样与取消令牌（照 1.x 语义）", [](CheckContext& ctx) {
        namespace vid = xrkit::engine::video;
        // time= 的第 4 组按**厘秒**算；percent 上限 99.5（1.x 原文 min(..., 99.5)）
        const vid::ProgressSample half =
            vid::progress_from_line("frame= 100 time=00:00:05.00 bitrate=1", 10.0);
        check(ctx, half.matched && half.percent > 49.9 && half.percent < 50.1,
              "time=00:00:05.00 / 10s 应为 ~50%");
        const vid::ProgressSample cs =
            vid::progress_from_line("time=00:00:01.50", 10.0);
        check(ctx, cs.matched && cs.percent > 14.9 && cs.percent < 15.1,
              "厘秒要按 /100 算（1.50s / 10s = 15%）");
        const vid::ProgressSample capped =
            vid::progress_from_line("time=01:00:00.00", 10.0);
        check(ctx, capped.matched && capped.percent == 99.5, "超过 100% 要封顶 99.5");
        const vid::ProgressSample none = vid::progress_from_line("frame= 1 fps=0", 10.0);
        check(ctx, !none.matched, "没有 time= 的行不该算进度");
        const vid::ProgressSample nodur = vid::progress_from_line("time=00:00:05.00", 0.0);
        check(ctx, !nodur.matched, "duration<=0 时不算进度（1.x 的条件）");
        // 首处匹配（Python `search` 语义），后面的 time= 不覆盖
        const vid::ProgressSample first =
            vid::progress_from_line("time=00:00:01.00 x time=00:00:09.00", 10.0);
        check(ctx, first.matched && first.percent > 9.9 && first.percent < 10.1,
              "一行里多个 time= 只取第一处（search 语义）");
        vid::CancelToken token;
        check(ctx, !token.cancelled(), "取消令牌初始应为未取消");
        token.cancel();
        check(ctx, token.cancelled(), "cancel() 后应为已取消");
        token.reset();
        check(ctx, !token.cancelled(), "只有 reset() 能清零（1.x convert() 不清零）");
    });

    // XML 声明改写要**大小写不敏感**（1.x 用 re.I；Hana 第二次审计 §四-2）
    reg.add("font: XML 声明改写（大写声明/大写 ENCODING 也要认）", [](CheckContext& ctx) {
        namespace ef = xrkit::engine::font;
        const std::string upper =
            "<?XML version=\"1.0\" ENCODING=\"windows-1251\"?>\n<root/>";
        const std::string fixed = ef::rewrite_xml_declaration_encoding(upper);
        check(ctx, fixed.find("utf-8") != std::string::npos,
              "大写 XML 声明没被改写成 utf-8（大小写敏感的老毛病）：" + fixed);
        check(ctx, fixed.find("<root/>") != std::string::npos, "声明之外的正文被弄坏了");
        // 小写声明当然也要认（回归）
        const std::string lower = "<?xml version=\"1.0\" encoding='cp1251'?>\n<a/>";
        const std::string fixed2 = ef::rewrite_xml_declaration_encoding(lower);
        check(ctx, fixed2.find("utf-8") != std::string::npos, "小写声明改写失败：" + fixed2);
        // 没有声明 ⇒ 原样返回
        const std::string plain = "<a b=\"1\"/>";
        check(ctx, ef::rewrite_xml_declaration_encoding(plain) == plain, "没有声明时不该动内容");
    });

    // 散装层（用户 2026-09-26"最后并入层"决策）：真实目录 → 与包同形的条目表
    xrkit_cli::register_version_store_check(reg);
    reg.add("fs: 散装层条目收集（与包同形、目录也算、排序确定）", [](CheckContext& ctx) {
        namespace efs = xrkit::engine::fs;
        namespace core = xrkit::core;
        std::error_code ec;
        const std::filesystem::path work = std::filesystem::temp_directory_path(ec) /
                                           "xrkit_selftest_loose";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work / "sub", ec);
        std::filesystem::create_directories(work / "empty", ec);
        {
            std::string err;
            check(ctx, core::write_file_atomic(core::path_to_utf8(work / "a.txt"),
                                               std::vector<uint8_t>{'A'}, true, err),
                  "准备散装层样本失败");
            check(ctx, core::write_file_atomic(core::path_to_utf8(work / "sub" / "b.txt"),
                                               std::vector<uint8_t>{'B'}, true, err),
                  "准备子目录样本失败");
        }
        std::string error;
        const auto entries = efs::loose_entries(core::path_to_utf8(work), error);
        check(ctx, error.empty(), "loose_entries 报错：" + error);
        std::vector<std::string> files;
        std::vector<std::string> dirs;
        for (const auto& item : entries) {
            (item.is_dir ? dirs : files).push_back(item.path);
        }
        check(ctx, files.size() == 2 && files[0] == "a.txt" && files[1] == "sub/b.txt",
              "散装层文件条目不对（应为 a.txt 与 sub/b.txt）");
        check(ctx, std::find(dirs.begin(), dirs.end(), std::string("empty")) != dirs.end() &&
                      std::find(dirs.begin(), dirs.end(), std::string("sub")) != dirs.end(),
              "散装层目录条目不对（empty/sub 都要在，空目录也算）");
        check(ctx, std::is_sorted(entries.begin(), entries.end()),
              "散装层条目未按字节序排序（渲染确定性要求）");
        // 不存在的目录 ⇒ 明确报错（不是静默空表）
        const auto missing = efs::loose_entries(core::path_to_utf8(work / "nope"), error);
        check(ctx, !error.empty() && missing.empty(),
              "不存在的散装目录必须报错，不许静默返回空表");
        std::filesystem::remove_all(work, ec);
    });

    // 字库字符集口径（用户 2026-09-26：日文层砍掉 ⇒ 假名不进字符集）
    reg.add("font: 字符集剔掉日文假名（汉字/西里尔保留）", [](CheckContext& ctx) {
        std::set<uint32_t> chars{0x0041,     // A
                                 0x0410,     // А（西里尔）
                                 0x00C9,     // É（西欧）
                                 0x6C49,     // 汉（译文汉字 —— 必须留）
                                 0x3042,     // あ
                                 0x30A2,     // ア
                                 0x31F0,     // ㇰ（片假名扩展）
                                 0xFF76};    // ｶ（半角片假名）
        xrkit::engine::font::strip_japanese_kana(chars);
        check(ctx, chars.count(0x3042) == 0 && chars.count(0x30A2) == 0 &&
                      chars.count(0x31F0) == 0 && chars.count(0xFF76) == 0,
              "日文假名（含扩展/半角）必须被剔掉");
        check(ctx, chars.count(0x0041) == 1 && chars.count(0x0410) == 1 &&
                      chars.count(0x00C9) == 1 && chars.count(0x6C49) == 1,
              "ASCII/西里尔/西欧/译文汉字都不许被误剔");
    });

    // ⑫12.1 decryptor 消费端（用户 2026-09-26：NLC 解密是外部插件；工具内不放逻辑）
    reg.add("plugin: decryptor 消费端（加载 DB 前问插件）", [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        namespace core = xrkit::core;
        std::error_code ec;
        const std::filesystem::path work = std::filesystem::temp_directory_path(ec) /
                                           "xrkit_selftest_decrypt";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work, ec);
        const std::string dir = core::path_to_utf8(work);
        const std::string enc = dir + "/gamedata.db_enc";
        {
            std::vector<uint8_t> cipher{0xDE, 0xAD, 0xBE, 0xEF};
            std::string err;
            check(ctx, core::write_file_atomic(enc, cipher, true, err), "准备加密样本失败");
        }

        // ① 没有解密器 ⇒ 原样放行（行为与不做这件事一致）
        {
            plug::Registry empty;
            const plug::DecryptGateResult r = plug::gate_db_path(empty, enc, dir);
            check(ctx, r.ok && !r.used_plugin && r.path == enc,
                  "没有解密器时不该改变路径/判定");
        }

        // ② 命中且解密成功 ⇒ 明文落盘、路径换成解密产物
        {
            plug::Registry registry;
            check(ctx,
                  registry.register_decryptor(
                      "nlc.dll", [](const std::string& p) { return p.size() > 4; },
                      [](const std::string&, std::string& out) {
                          out = "DECRYPTED-DB";
                          return true;
                      }),
                  "注册解密器失败");
            const plug::DecryptGateResult r = plug::gate_db_path(registry, enc, dir);
            check(ctx, r.ok && r.used_plugin && r.path != enc && r.path.find(".decrypted") !=
                                                                      std::string::npos,
                  "命中解密器后没换成解密产物");
            if (r.ok && r.used_plugin) {
                std::ifstream in(core::path_from_utf8(r.path), std::ios::binary);
                const std::string got((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
                check(ctx, got == "DECRYPTED-DB", "解密结果内容不对");
            }
        }

        // ③ 命中但解密失败 ⇒ **可见失败**（不许回落）
        {
            plug::Registry registry;
            registry.register_decryptor("bad.dll",
                                        [](const std::string&) { return true; },
                                        [](const std::string&, std::string&) { return false; });
            const plug::DecryptGateResult r = plug::gate_db_path(registry, enc, dir);
            check(ctx, !r.ok && !r.error.empty() && r.path == enc,
                  "解密失败必须 visible（ok=false + 理由），不许静默回落");
        }
        std::filesystem::remove_all(work, ec);
    });

    // ★ 原子写的**路径编码安全**（2026-09-25 HanaAgent 审计查出的【高】缺陷的锁）：
    //   临时文件路径若经 `Path::string()`（CP_ACP）转窄串，ACP 表示不了的字符会变成 `?`
    //   ⇒ 打不开、原子写失败。这里用**ACP 表示不了的字符（韩文）**真写一次：旧代码必红。
    //   ★ 样本选择有讲究：**西里尔在 GBK(936) 里是有映射的**（GB2312 收俄文字母）⇒ 拿"俄文
    //   文件名"当样本会**假绿**（实测：注入旧代码仍全过）；韩文/泰文不在 GBK 里，在 ACP=936
    //   与 ACP=1252 两种机器上都表示不了，才是能失败的样本。
    reg.add("core: 原子写在非 ACP 路径下必须成功（韩文）", [](CheckContext& ctx) {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) /
            xrkit::core::path_from_utf8("xrkit_selftest_한글");
        const std::filesystem::path file = dir / xrkit::core::path_from_utf8("파일.txt");
        const std::vector<uint8_t> data{0x41, 0x00, 0x42, 0xFF};
        std::string error;
        const bool ok = xrkit::core::write_file_atomic(xrkit::core::path_to_utf8(file), data,
                                                       /*atomic=*/true, error);
        check(ctx, ok, "非 ACP 字符（韩文）的路径原子写失败：" + error);
        if (ok) {
            std::ifstream in(file, std::ios::binary);
            const std::string got((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            check(ctx, got.size() == data.size() &&
                          std::memcmp(got.data(), data.data(), data.size()) == 0,
                  "写回内容与输入不一致（非 ACP 路径）");
            std::filesystem::path part = file;
            part += L".part";
            check(ctx, !std::filesystem::exists(part), "原子写留下了 .part");
        }
        std::filesystem::remove(file, ec);
        std::filesystem::remove(dir, ec);
    });
    // ★★ 2026-10-05（用户裁决 ③）：解包原子写的**写后校验**改造 —— 三条锁各自"先红后绿"。
    //
    // 为什么默认路径要换：旧的"读回逐字节复核"是**按文件个数付钱**（真机 `gamedata.sq_base_configs`
    // 14,861 文件上实测 ≈ 17.9 s：31.1 s → 13.25 s 的差，而 rds 参照只要 13.8 s）。
    // 新默认：**边写边算哈希 + 实际写出字节数**，写完与内存那份数据比对（零额外 I/O）。
    //
    // 这几条锁都靠 `core::WriteProbe` 注入（进程外的测试改不到"写到一半的数据"/"比对用的那份"）：
    //   ① 正常写 ⇒ 绿、不留 `.part`；
    //   ② 注入"期望哈希不对" ⇒ 流式自校验**必须**报错 + 不 rename + 清 `.part`；
    //   ③ 撤掉注入 ⇒ 绿（证明②的红来自注入，不是"永远为红"）；
    //   ④ 写完之后动临时文件 ⇒ `WriteVerify::Readback` **必须**抓住（证明它真在读回）。
    // ★ 注入只改**比对用的期望值**、只动**临时文件**，真实产物一个字节都不动 —— 所以
    //   "没写对就不 rename" 能被老实观测到（不像"改内存里那份"会让产物必然变脏）。
    reg.add("core: 原子写自校验（边写边算哈希 / 读回开关，各自先红后绿）", [](CheckContext& ctx) {
        namespace core = xrkit::core;
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "xrkit_selftest_atomic_verify";
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        // ★ 两条锁各用**自己的文件**：④ 会把临时文件改脏（那是它的目的），
        //   若与②③ 共用一个落点，后面的断言看到的是被改脏的产物（2026-10-05 实测踩到）。
        const std::filesystem::path stream_file = dir / "stream.bin";
        const std::filesystem::path readback_file = dir / "readback.bin";
        const auto part_of = [](const std::filesystem::path& p) {
            std::filesystem::path q = p;
            q += L".part";
            return q;
        };
        std::vector<uint8_t> data(4096);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 13);
        const auto read_back = [](const std::filesystem::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        };
        const std::string want(reinterpret_cast<const char*>(data.data()), data.size());

        // 本检查**自带**一份探针（不依赖命令行开关）：这样 `--selftest` 单独跑也照样验。
        // 每次都用**全新**的 `WriteProbe` 覆盖（`set_write_probe` 是进程级的 ⇒ 不覆盖就会把
        // 命令行 `--verify-readback` 装的那一份留在外面）。探针函数的存活期由
        // `make_atomic_selftest_probe()` 的静态量保证（见它的注释）。
        //   Arm::Off        零注入
        //   Arm::TempFile   写完之后动一下 `.part`（只该被**读回**档抓住）
        //   Arm::BadHash    把比对用的期望哈希换掉（只该被**流式自校验**抓住）
        enum class Arm { Off, TempFile, BadHash };
        const auto arm = [](Arm kind) {
            static const xrkit::core::WriteProbe kSelftest = make_atomic_selftest_probe();
            xrkit::core::WriteProbe probe;
            if (kind == Arm::TempFile) {
                probe.tamper_temp_file = kSelftest.tamper_temp_file;
            } else if (kind == Arm::BadHash) {
                // 一个**合法但不同**的哈希值（真值由同一函数算出，这里取它的按位取反，
                // 保证非 0、且几乎不可能与任何真值相同）。
                probe.tamper_expected_hash = ~0xA5A5A5A5A5A5A5A5ULL;
            }
            g_atomic_probe_armed = (kind != Arm::Off);
            xrkit::core::set_write_probe(probe);
        };
        const core::WriteProbe saved = core::set_write_probe(core::WriteProbe{});

        // ── 流式档（默认路径）：绿 → 注入红 → 还原绿 ──
        // ① 正常写：必须绿，且不留 `.part`
        {
            std::string error;
            const bool ok = core::write_file_atomic(core::path_to_utf8(stream_file), data,
                                                    /*atomic=*/true, error,
                                                    core::WriteVerify::Streamed);
            check(ctx, ok, "流式自校验的正常写失败：" + error);
            check(ctx, read_back(stream_file) == want, "流式档写出的内容与输入不一致");
            check(ctx, !std::filesystem::exists(part_of(stream_file)),
                  "流式档正常写之后留下了 .part");
        }
        // ② 注入"期望哈希不对" ⇒ 流式自校验必须红，且不许 rename、要清 `.part`
        std::filesystem::remove(stream_file, ec);   // 先删目标，好断言"没被 rename 出来"
        {
            arm(Arm::BadHash);
            std::string error;
            const bool ok = core::write_file_atomic(core::path_to_utf8(stream_file), data,
                                                    /*atomic=*/true, error,
                                                    core::WriteVerify::Streamed);
            const bool part_left = std::filesystem::exists(part_of(stream_file));
            const bool target_left = std::filesystem::exists(stream_file);
            arm(Arm::Off);
            check(ctx, !ok, "注入「期望哈希不对」竟被判为成功（自校验是永远为真的锁）");
            check(ctx, !error.empty(), "自校验失败却没有理由（零静默：要给逐条原因）");
            check(ctx, !target_left, "自校验失败却还是 rename 出了目标文件");
            check(ctx, !part_left, "自校验失败却留下了 .part");
            check(ctx, error.find("自校验不一致") != std::string::npos,
                  "自校验失败的理由不对：" + error);
        }
        // ③ 撤掉注入 ⇒ 绿：证明②的红来自注入，不是永远为红
        {
            arm(Arm::Off);
            std::string error;
            const bool ok = core::write_file_atomic(core::path_to_utf8(stream_file), data,
                                                    /*atomic=*/true, error,
                                                    core::WriteVerify::Streamed);
            check(ctx, ok, "撤掉注入后还是红（锁有残留状态）：" + error);
            check(ctx, read_back(stream_file) == want, "撤掉注入后内容不对");
        }

        // ── 读回档（开关路径）：注入"改临时文件的一个字节" ⇒ 必须抓住 ──
        std::filesystem::remove(readback_file, ec);
        {
            arm(Arm::TempFile);
            std::string error;
            const bool ok = core::write_file_atomic(core::path_to_utf8(readback_file), data,
                                                    /*atomic=*/true, error,
                                                    core::WriteVerify::Readback);
            const bool part_left = std::filesystem::exists(part_of(readback_file));
            const bool target_left = std::filesystem::exists(readback_file);
            arm(Arm::Off);
            check(ctx, !ok, "读回档没抓住「临时文件被改了一个字节」—— 它没有真在读回");
            check(ctx, !error.empty(), "读回档失败了却没有理由");
            check(ctx, !part_left, "读回档失败却留下了 .part");
            check(ctx, !target_left, "读回档失败却还是 rename 出了目标文件");
        }
        // ⑤ 同一枚文件的**正常**读回档：必须绿、内容逐字节一致（对照：开关本身不改变产物）
        {
            std::string error;
            const bool ok = core::write_file_atomic(core::path_to_utf8(readback_file), data,
                                                    /*atomic=*/true, error,
                                                    core::WriteVerify::Readback);
            check(ctx, ok, "读回档的正常写失败：" + error);
            check(ctx, read_back(readback_file) == want, "读回档写出的内容与输入不一致");
            check(ctx, !std::filesystem::exists(part_of(readback_file)),
                  "读回档正常写之后留下了 .part");
        }

        core::set_write_probe(saved);
        std::filesystem::remove_all(dir, ec);
    });

    // ★★ 2026-10-05（本批小件 ②）**性能锁换成"行为型"**。
    //
    // 旧判据是**耗时上界**（"默认档不比参照慢出量级"）—— 现场：本机余量只有 8%，
    // 而**旧默认档 47 s 也照样能过** ⇒ 既假红又没用。
    // 新判据是**结构事实**，与机器快慢无关：用 `WriteProbe::record` 抓"这一次实际用的是
    // 哪一档"，断言 **默认档 ⇒ Streamed（零额外读盘）/ `--verify-readback` ⇒ Readback**。
    //   · 为什么能判红：把 `extract_tree` 里 `options.verify_readback ? Readback : Streamed`
    //     写死成任一侧 ⇒ 这里立刻红（耗时上界看不出来）。
    //   · 为什么走**真引擎**（`extract_package`）而不是直接调 `write_file_atomic`：
    //     要钉的是"解包那条路"的档位，不是"这个开关本身能不能传值"。
    //   · 夹具是一枚**自造的真 DB 包**（引擎自己的写侧产出）⇒ 不依赖语料/外部样本。
    reg.add("core: 解包写后校验档位＝行为型锁（默认 Streamed / --verify-readback Readback）",
            [](CheckContext& ctx) {
                namespace core = xrkit::core;
                namespace efs = xrkit::engine::fs;
                std::error_code ec;
                const std::filesystem::path dir =
                    std::filesystem::temp_directory_path(ec) / "xrkit_selftest_verify_mode";
                std::filesystem::remove_all(dir, ec);
                std::filesystem::create_directories(dir, ec);
                const std::filesystem::path pkg = dir / "xr.db";

                std::vector<efs::DbPackItem> items;
                efs::DbPackItem one;
                one.path = "a.txt";
                one.data = {'A', 'B', 'C'};
                items.push_back(one);
                const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
                bool wrote = false;
                {
                    std::ofstream out(pkg, std::ios::binary | std::ios::trunc);
                    wrote = static_cast<bool>(out);
                    if (wrote) {
                        out.write(reinterpret_cast<const char*>(raw.data()),
                                  static_cast<std::streamsize>(raw.size()));
                    }
                }
                check(ctx, wrote && std::filesystem::exists(pkg), "行为型锁：夹具包没造出来");

                // 探针逐次回调 ⇒ 记下**每一次**实际用的档位（顺序即调用顺序）。
                //   ★ 存活期：与 `write_file_atomic` 的回调签名一致（普通函数指针）。
                auto record = +[](const std::string&, core::WriteVerify mode) {
                    g_verify_probe_log.push_back(mode);
                };
                core::WriteProbe probe;
                probe.record = record;
                g_verify_probe_log.clear();
                const core::WriteProbe saved_probe = core::set_write_probe(probe);

                const auto unpack_with = [&](bool readback, const std::string& out_name) {
                    efs::ExtractTreeOptions options;
                    options.verify_readback = readback;
                    return efs::extract_package(core::path_to_utf8(pkg),
                                                core::path_to_utf8(dir / out_name), options);
                };

                // ① 默认档（用户没给开关）
                const efs::ExtractTreeResult first = unpack_with(false, "out_default");
                const std::vector<core::WriteVerify> after_default = g_verify_probe_log;
                check(ctx, first.ok && first.files_written == 1,
                      "行为型锁：默认档解包没写出 1 个文件（files=" +
                          std::to_string(first.files_written) + " error=" + first.error + "）");
                // ② 读回档（`--verify-readback` 那一档）
                const efs::ExtractTreeResult second = unpack_with(true, "out_readback");
                const std::vector<core::WriteVerify> after_readback = g_verify_probe_log;
                core::set_write_probe(saved_probe);

                // 判据：两次各写一次、各报一次 ⇒ 共 2 条；**第 1 条必须是 Streamed、第 2 条必须是
                // Readback**（顺序与调用顺序一致）。
                check(ctx, after_default.size() == 1,
                      "行为型锁：默认档应当只报一次写档位（实测 " +
                          std::to_string(after_default.size()) + " 次）");
                check(ctx, after_default.size() == 1 &&
                               after_default[0] == core::WriteVerify::Streamed,
                      "行为型锁：**默认档必须走 Streamed**（零额外读盘）—— 实测走了 Readback 或没报");
                check(ctx, after_readback.size() == 2 &&
                               after_readback[1] == core::WriteVerify::Readback,
                      "行为型锁：**--verify-readback 必须走 Readback** —— 实测档位没换");
                check(ctx, second.ok && second.files_written == 1,
                      "行为型锁：读回档解包没写出 1 个文件（error=" + second.error + "）");
                g_verify_probe_log.clear();
                std::filesystem::remove_all(dir, ec);
            });

    // ★★ 2026-10-05（**落点覆盖必须可见 / 零静默**）。
    // 现场：一次默认落点解包把 **3.9 万个文件**盖在模组目录上，工具一个字都没说。
    // 口径（用户没改）：**默认落点就是来源所在目录**、**不弹模态确认框**（那会改交互口径）；
    // 但"任何改写都必须可见：**逐条 + 计数**"（零静默红线）。
    // 判据：对同一枚夹具**解两遍** —— 第二遍必须逐条报出被覆盖的文件，且**计数非零**。
    //   · 判红办法：把 `result.overwritten.push_back(...)` 那两处去掉（或把 `had_file` 写死
    //     false）⇒ 第二遍 `overwritten_size()==0` ⇒ 立刻红。
    reg.add("core: 落点覆盖可见（逐条 + 计数；零静默）", [](CheckContext& ctx) {
        namespace core = xrkit::core;
        namespace efs = xrkit::engine::fs;
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) / "xrkit_selftest_overwrite";
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path pkg = dir / "ov.db";

        std::vector<efs::DbPackItem> items;
        efs::DbPackItem one;
        one.path = "dir/keep.txt";
        one.data = {'K'};
        items.push_back(one);
        const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
        bool wrote = false;
        {
            std::ofstream out(pkg, std::ios::binary | std::ios::trunc);
            wrote = static_cast<bool>(out);
            if (wrote) {
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
        }
        check(ctx, wrote, "覆盖锁：夹具包没造出来");

        // 落点照**引擎**那条规则算（与 GUI/CLI 同一个函数）：不带壳 ⇒ <根>/<包名>
        const std::string target = efs::unpack_target_dir(
            core::path_to_utf8(dir / "out"), core::path_to_utf8(pkg), {"dir/keep.txt"});
        const efs::ExtractTreeResult first = efs::extract_package(
            core::path_to_utf8(pkg), target, efs::ExtractTreeOptions{});
        check(ctx, first.ok && first.files_written == 1,
              "覆盖锁：第一遍解包没成功（error=" + first.error + "）");
        check(ctx, first.overwritten_size() == 0,
              "覆盖锁：**第一遍**不该报覆盖（落点本来是空的）—— 实测 " +
                  std::to_string(first.overwritten_size()) + " 条");
        // 第二遍：落点上已经有那个文件了 ⇒ 必须**逐条**报出来 + 计数非零
        const efs::ExtractTreeResult second = efs::extract_package(
            core::path_to_utf8(pkg), target, efs::ExtractTreeOptions{});
        check(ctx, second.ok && second.files_written == 1,
              "覆盖锁：第二遍解包没成功（error=" + second.error + "）");
        check(ctx, second.overwritten_size() == 1,
              "覆盖锁：第二遍必须报出**恰好 1 条**被覆盖的文件 —— 实测 " +
                  std::to_string(second.overwritten_size()) + " 条（静默覆盖＝红）");
        check(ctx, second.overwritten_size() == 1 && second.overwritten[0] == "dir/keep.txt",
              "覆盖锁：被覆盖的路径要**逐条**报对（期望 dir/keep.txt）");
        std::filesystem::remove_all(dir, ec);
    });

    // ⑫12.1/12.2/12.3 纯 C ABI：**像插件那样**通过函数表把 10 个注册点全跑一遍，
    // 再从消费端把每一条捞回来。无 Qt 路径（`api.ui == nullptr`）——UI 那 16 个方法
    // 由外壳注入，这里证的是"注册面 + 回调 + when 数据化"这套 ABI 形状真的通。
    reg.add("plugin: C ABI 十个注册点端到端（⑫12.1）", [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        plug::Registry reg;
        plug::PluginApiAdapter adapter(reg, "demo.dll", /*ui=*/nullptr);
        const stk_api& api = adapter.api();

        check(ctx, api.abi_version == STK_ABI_VERSION, "ABI 版本号不对");
        check(ctx, api.host != nullptr && api.ui == nullptr,
              "无 Qt 路径下 host 要有、ui 可以是空");
        check(ctx, api.register_decryptor != nullptr && api.register_command != nullptr &&
                      api.register_tool != nullptr && api.register_compare_mode != nullptr &&
                      api.register_option_entry != nullptr && api.register_option != nullptr &&
                      api.register_panel != nullptr && api.register_menu_item != nullptr &&
                      api.add_error != nullptr,
              "注册面函数表有空洞（10 个注册点必须齐）");

        // ① 解密器（裸函数指针跨 ABI）
        const auto check_fn = +[](void*, const char* path) -> int {
            return std::string(path).find("zzzz") != std::string::npos ? 1 : 0;
        };
        const auto decrypt_fn = +[](void*, const char*, void** data, size_t* size) -> int {
            static const char kPayload[] = "decrypted";
            *data = const_cast<char*>(kPayload);
            *size = sizeof(kPayload) - 1;
            return 1;
        };
        check(ctx, api.register_decryptor(api.host, "nlc", check_fn, decrypt_fn, nullptr) == 1,
              "① 解密器注册失败");
        const plug::DecryptorReg* dec = reg.find_decryptor("a/zzzz.sq_base");
        check(ctx, dec != nullptr, "① 解密器没被消费端命中");
        if (dec != nullptr) {
            std::string out;
            check(ctx, dec->decrypt("a/zzzz.sq_base", out) && out == "decrypted",
                  "① 解密回调跨 ABI 传值失败");
        }

        // ② 格式
        const auto format_fn = +[](void*, const char* path) -> int {
            return std::string(path).rfind(".myfmt", 0) == 0 ? 1 : 0;
        };
        check(ctx, api.register_format(api.host, "myfmt", format_fn, nullptr) == 1,
              "② 格式注册失败");
        check(ctx, reg.formats().size() == 1 && reg.formats()[0].handler(".myfmt-x") &&
                      !reg.formats()[0].handler("other.bin"),
              "② 格式没被消费（handler 应按注册的判据命中）");

        // ③ 命令（when 用**数据**表达）
        const auto command_fn = +[](void* user) { *static_cast<int*>(user) += 1; };
        int runs = 0;
        const char* when_keys[] = {"mode"};
        const char* when_values[] = {"文本"};
        stk_when when{};
        when.keys = when_keys;
        when.values = when_values;
        when.count = 1;
        const char* command_id =
            api.register_command(api.host, "demo_action", "演示动作", command_fn, &runs, &when);
        check(ctx, command_id != nullptr && std::string(command_id) == "demo.dll:demo_action",
              "③ 命令 id 形态不对（应是 <插件>:<动作名>）");
        check(ctx, api.register_command(api.host, "bad name", "x", command_fn, &runs, nullptr) ==
                          nullptr,
              "③ 非法动作名没被拒");

        // ④ 挂载点（精确匹配 + when 生效）
        check(ctx, api.register_menu_item(api.host, "xml", "toolbar", command_id, 5) == 1,
              "④ 挂载点注册失败");
        check(ctx, api.register_menu_item(api.host, "nope", "toolbar", command_id, 0) == 0,
              "④ 非法 host 没被拒");
        plug::Context hit;
        hit["mode"] = std::string("文本");
        plug::Context miss;
        miss["mode"] = std::string("统计");
        const auto items_hit = reg.menu_items_for("xml", "toolbar", hit);
        const auto items_miss = reg.menu_items_for("xml", "toolbar", miss);
        check(ctx, items_hit.size() == 1 && items_hit[0]->id == "demo.dll:demo_action",
              "④ 挂载点没被消费");
        check(ctx, items_miss.empty(), "④ ABI 传过来的 when 没生效（应只在 mode=文本 时出现）");
        if (!items_hit.empty()) {
            items_hit[0]->handler();
            check(ctx, runs == 1, "③ 命令回调跨 ABI 调用失败");
        }

        // ⑤ 下拉条目 / ⑥ 新建下拉
        check(ctx, api.register_option_entry(api.host, "xml", "filter", "插件筛选项", nullptr) == 1,
              "⑤ 下拉条目注册失败");
        check(ctx, reg.option_entries_for("xml", "filter").size() == 1 &&
                      reg.option_entries_for("xml", "filter")[0] == "插件筛选项",
              "⑤ 下拉条目没被消费");
        const char* choices[] = {"甲", "乙"};
        check(ctx, api.register_option(api.host, "插件下拉", choices, 2, nullptr, nullptr, "fs",
                                       "top_bar", nullptr, 3) == 1,
              "⑥ 新建下拉注册失败");
        check(ctx, reg.options_for("fs", "top_bar").size() == 1 &&
                      reg.options_for("fs", "top_bar")[0]->choices.size() == 2,
              "⑥ 新建下拉没被消费");

        // ⑦ 面板 / ⑧ Hub 页
        const auto builder = +[](void*, stk_widget*) -> stk_widget* { return nullptr; };
        check(ctx, api.register_panel(api.host, "插件面板", builder, nullptr, "xml", "top_bar",
                                      nullptr, 1) == 1,
              "⑦ 面板注册失败");
        check(ctx, reg.panels_for("xml", "top_bar").size() == 1, "⑦ 面板没被消费");
        check(ctx, api.register_tool(api.host, "插件页", builder, nullptr, 2) == 1,
              "⑧ Hub 页注册失败");
        check(ctx, reg.tools_sorted().size() == 1 && reg.tools_sorted()[0]->name == "插件页",
              "⑧ Hub 页没被消费");

        // ⑨ 比较模式：回调通过**宿主提供的 sink** 回传（跨 ABI 没有所有权问题）
        const auto compare_fn = +[](void* user, const char*, const char*, const char*, const char*,
                                    const char*, stk_result_sink sink, void* sink_user) {
            *static_cast<bool*>(user) = true;
            sink(sink_user, "consistent", "0");
            sink(sink_user, "only_a", "id_only_a");
            sink(sink_user, "text_diff", "id_x\t甲\t乙");
        };
        bool compare_ran = false;
        const char* col_keys[] = {"rel", "status", "detail"};
        const char* col_headings[] = {"文件", "状态", "差异"};
        const int col_widths[] = {280, 60, 400};
        const char* filter_labels[] = {"全部", "仅不一致"};
        const char* filter_kinds[] = {"same|diff|only_a|only_b", "diff"};
        check(ctx, api.register_compare_mode(api.host, "插件模式", "plugin_mode", 7, col_keys,
                                             col_headings, col_widths, 3, filter_labels, 2,
                                             filter_kinds, compare_fn, &compare_ran) == 1,
              "⑨ 比较模式注册失败");
        const plug::CompareModeReg* mode = reg.compare_mode("插件模式");
        check(ctx, mode != nullptr && mode->spec.columns.size() == 3 &&
                      mode->spec.filters.size() == 2 &&
                      mode->spec.filters[1].row_kinds.size() == 1 &&
                      mode->spec.filters[1].row_kinds[0] == "diff",
              "⑨ 比较模式声明（列/筛选）没被消费");
        if (mode != nullptr && mode->compare) {
            const xrkit::engine::xml::XmlCompareResult result =
                mode->compare("x.xml", "<a/>", "<b/>", "utf-8", "utf-8");
            check(ctx, compare_ran && !result.consistent && result.only_a.size() == 1 &&
                          result.only_a[0] == "id_only_a" && result.text_diff.size() == 1 &&
                          result.text_diff[0][0] == "id_x" && result.text_diff[0][2] == "乙",
                  "⑨ 插件比较回调的 sink 数据没被正确解析回结果");
        }

        // ⑩ 插件自报错误：走同一条留痕通道（Hub 启动时可见）
        const size_t errors_before = reg.registration_errors().size();
        api.add_error(api.host, "插件自己发现的异常");
        check(ctx, reg.registration_errors().size() == errors_before + 1 &&
                      reg.registration_errors().back().message.find("插件自己发现的异常") !=
                          std::string::npos,
              "⑩ 插件自报错误没进 registration_errors");
        check(ctx, reg.registration_errors().size() >= 2, "被拒的注册没有全部留痕");
    });

    // ⑫12.4 + 用户口径：**插件加载失败不阻塞启动**；加载成功的要真的注册进表。
    // 夹具是同一构建里编出来的 `xrkit_test_plugin.dll`（**用 C 编译**，证明 ABI 是纯 C）。
    reg.add("plugin: DLL 加载与注册（⑫12.4）", [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        plug::Registry registry;

        // ① 加载不存在的 DLL：只报错、不崩
        plug::LoadResult broken;
        {
            plug::PluginApiAdapter adapter(registry, "broken.dll", nullptr);
            broken = plug::load_plugin("no_such_plugin_xyz.dll", adapter);
        }
        check(ctx, !broken.ok && !broken.error.empty(),
              "加载不存在的 DLL 应失败并给出理由（不许崩）");

        // ② 真 DLL：与 CLI 同目录（构建时就放好了）
        const std::string dll = xrkit::core::app_dir() + "/xrkit_test_plugin.dll";
        std::error_code ec;
        const bool present =
            std::filesystem::is_regular_file(xrkit::core::path_from_utf8(dll), ec);
        if (!present) {
            // 自家产物缺失 ⇒ 如实报红（不是 SKIP）
            check(ctx, false, "找不到测试插件 " + dll + "（构建没编出来？）");
            return;
        }
        {
            plug::PluginApiAdapter adapter(registry, "xrkit_test_plugin.dll", nullptr);
            const plug::LoadResult loaded = plug::load_plugin(dll, adapter);
            check(ctx, loaded.ok && loaded.init_rc == 1, "加载测试插件失败：" + loaded.error);
            // ★★ 2026-10-06（审计第 4 组：「`LoadResult::registrations` **死字段**」）：
            //   修前**只有失败分支**写这个字段，成功加载时恒为 0 ⇒ 它名义上是"这次加载
            //   带来的注册条数"，实际上消费端读到的永远是 0。这里按**同一份来源**
            //   （`residue_for(owner).registrations()`）核对：成功路径必须如实报出条数。
            const size_t truth =
                registry.residue_for("xrkit_test_plugin.dll").registrations();
            check(ctx, loaded.registrations > 0 && loaded.registrations == truth,
                  "成功加载时 registrations 必须是**真条数**（实测 " +
                      std::to_string(loaded.registrations) + "，按 owner 逐容器核对为 " +
                      std::to_string(truth) + "）—— 修前成功路径恒为 0（死字段）");
            if (loaded.ok) new plug::PluginApiAdapter(std::move(adapter));
        }
        check(ctx, plug::loaded_count() >= 1, "加载成功后句柄没被记账");

        // ③ 它注册的东西必须**真的在表里**（插件名做前缀 / 做哨兵）
        check(ctx, registry.has_command("xrkit_test_plugin.dll:hello"),
              "插件注册的命令没进表");
        check(ctx, registry.formats().size() == 1 &&
                      registry.formats()[0].plugin == "xrkit_test_plugin.dll",
              "插件注册的格式没进表");
        plug::Context demo;
        demo["mode"] = std::string("demo");
        plug::Context other;
        other["mode"] = std::string("别的");
        check(ctx, registry.menu_items_for("xml", "toolbar", demo).size() == 1 &&
                      registry.menu_items_for("xml", "toolbar", other).empty(),
              "插件挂载点/它的 when 没生效（应只在 mode=demo 时出现）");
        check(ctx, registry.option_entries_for("xml", "filter").size() == 1 &&
                      registry.option_entries_for("xml", "filter")[0] == "demo 筛选项",
              "插件下拉条目没进表");
        check(ctx, registry.panels_for("xml", "top_bar").size() == 1, "插件面板没进表");
        const plug::CompareModeReg* mode = registry.compare_mode("demo 模式");
        check(ctx, mode != nullptr && mode->spec.plugin == "xrkit_test_plugin.dll",
              "插件比较模式没进表（哨兵应是插件名，不是内置）");
        if (mode != nullptr && mode->compare) {
            const auto diff = mode->compare("a.xml", "AAA", "BBB", "utf-8", "utf-8");
            const auto same = mode->compare("a.xml", "SAME", "SAME", "utf-8", "utf-8");
            check(ctx, !diff.consistent && diff.only_a.size() == 1 &&
                          diff.only_a[0] == "a.xml" && same.consistent,
                  "插件比较回调经 sink 回传的结果不对（回调没跨 DLL 边界跑通）");
        }
        // ④ 命令回调真的能跨 DLL 调用（不崩）
        const auto items = registry.menu_items_for("xml", "toolbar", demo);
        if (!items.empty()) {
            items[0]->handler();
            check(ctx, true, "插件命令回调跨 DLL 调用没有崩");
        }
        check(ctx, registry.registration_errors().empty(),
              "这一段不该产生 registration_errors（插件注册都是合法的）");
        plug::unload_all();
        check(ctx, plug::loaded_count() == 0, "unload_all 没把句柄清掉");
    });

    // ★★ 2026-10-06（F1 命令 id 悬空）：**把 `abi_host.h` 那条承诺落成锁** ——
    //   `register_command` 返回的 `const char*`（命令 id）必须**在宿主生命周期内一直有效**。
    //   插件的典型写法就是"先注册全部命令、再统一挂 menu item"（id 要留着后面用），
    //   而修前 `private: std::vector<std::string> owned_` 会在注册第 2 个命令时扩容搬走
    //   旧元素；id 形如 `"p:c0"` 落在 MSVC `std::string` 的 SSO 阈值（15 字节）内 ⇒
    //   字符数据内联在对象里 ⇒ 早先返回的 `c_str()` 指向**已被释放的旧数组**（UAF）。
    //   判据三条一起才可信：① 3 个 id 逐个仍可读且**内容正确**；② 指针两两不同；
    //   ③ 中间在堆上**涂抹同尺寸块**（把扩容时释放的那些数组复用掉）——没有 ③，悬空指针
    //     常常还"恰好"读得出旧内容（SSO 的 move 是 memcpy，旧缓冲里可能还留着原文）。
    //   ⇒ 改回 `std::vector` 立刻红（先证红后改绿的口径：本轮实测过）。
    reg.add("plugin: 命令 id 指针在注册第 2/3 个命令后仍然有效（⑫12.2 契约锁）",
            [](CheckContext& ctx) {
        namespace plug = xrkit::plugin;
        plug::Registry reg2;
        // 插件名**故意取短**：id 是 `p:c0` 这种 SSO 串 —— bug 只在 SSO 串上真会读到脏数据
        // （长 id 的字符数据在堆上，move 只搬指针 ⇒ 内容看着还对，这也是它一直没被发现的原因）
        plug::PluginApiAdapter adapter(reg2, "p", /*ui=*/nullptr);
        const stk_api& api = adapter.api();
        const auto handler = +[](void*) {};
        const char* want_ids[3] = {"p:c0", "p:c1", "p:c2"};
        const char* ids[3] = {nullptr, nullptr, nullptr};
        for (int i = 0; i < 3; ++i) {
            ids[i] = api.register_command(api.host, want_ids[i] + 2, "标签", handler, nullptr,
                                          nullptr);
        }
        check(ctx, ids[0] != nullptr && ids[1] != nullptr && ids[2] != nullptr,
              "注册 3 个命令时有返回值是空的（夹具前提不成立）");
        {
            // 堆涂抹：尺寸覆盖 `vector` 扩容时释放过的那几档（1/2/3 个 string ⇒ 32/64/96 B）
            std::vector<std::unique_ptr<char[]>> noise;
            const size_t sizes[] = {96, 64, 32, 128, 48, 64, 32};
            for (int round = 0; round < 24; ++round) {
                for (size_t n : sizes) {
                    auto block = std::unique_ptr<char[]>(new char[n]);
                    std::memset(block.get(), 'X', n);
                    noise.push_back(std::move(block));
                }
            }
            check(ctx, noise.size() == 24 * 7 && noise.back().get()[0] == 'X',
                  "堆涂抹夹具本身没问题（哨兵）");
        }
        for (int i = 0; i < 3; ++i) {
            const std::string got = ids[i] != nullptr ? std::string(ids[i]) : std::string("<null>");
            check(ctx, ids[i] != nullptr && got == want_ids[i],
                  "命令 id 指针失效：第 " + std::to_string(i) + " 个读到「" + got + "」，期望「" +
                      want_ids[i] + "」（`owned_` 必须是「插入不搬家」的容器）");
        }
        check(ctx, ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2],
              "3 个命令 id 的指针不该相同");
        check(ctx, adapter.owned_strings().size() == 3 && adapter.owned_strings().front() == "p:c0",
              "注册期字符串台账内容/数量不对");
    });

    // ⑥6.7 路径越界判据（safe_out_path）：归档条目名是不可信输入，这条判据挡的是
    // **任意文件写**。1.x 只把理由写在注释里，2.0 把"拒因"带出来 —— 所以这里连
    // **理由**一起断言：改判定顺序、把某一条删掉，都能当场变红。
    reg.add("fs: 路径越界拒因逐条生效（⑥6.7；含 F-1 的尾随空格/点与段内冒号）", [](CheckContext& ctx) {
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
            // ★★ 2026-10-06（F-1 路径逃逸）：`..` 的拦截**不能再按字节精确比** ——
            //   Windows 落盘时会剥掉组件末尾的空格/点，`".. "` / `".. ."` 会变成 `..`。
            //   这一组是"先证红再改绿"的那批输入；判红锁另见 tests/check_fs_hardening.py
            //   （端到端：自造镜像 + 落盘 + 域外零文件）。
            {".. /x", "尾随空格绕过 `..`"},
            {".. ./x", "尾随空格+点绕过 `..`"},
            {"a/.. /b", "中段尾随空格绕过 `..`"},
            {".. /", "尾随空格的 `..`（末尾空段）"},
            {".. .", "整条就是带尾随点的 `..`"},
            {".../x", "剥掉尾随点后为空的分段"},
            {"   /x", "全空格分段"},
            {"ok.txt:evil", "段内冒号（NTFS 数据流 ADS）"},
            {"a/b:c", "中段冒号（NTFS 数据流 ADS）"},
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
            // ★ F-1 同批：尾随空格/点**不是** `..` ⇒ 不能拒，但落点必须按 Windows 的
            //   有效名归一（`a. ` 与 `a` 在盘上是**同一条**路径）—— 否则"报出来的落点"
            //   与"盘上真发生的"又对不上了（`ArtifactTally` 会把同一处算成两个落点）。
            {"a. /b.txt", "a/b.txt"},
            {"a/b.txt.", "a/b.txt"},
            {"a/b.txt ", "a/b.txt"},
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

// ── P16-1b：站点表的**文本形态**（只有 CLI/判据用；插件侧直接给值类型）──────────
//   每行一条：`site id=… kind=… offset=0x… expect="41 8B" patched="E8 …" [helper=A]
//   [tail="…"] [before="…"] [after="…"] [layout_delta=0x…] [layout_section=.text] [note="…"]`
//   取值可加双引号（含空格时必须加）；`#` 开头是注释。
namespace {

std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quote = false;
    for (char c : line) {
        if (c == '"') { in_quote = !in_quote; cur.push_back(c); continue; }
        if (!in_quote && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string strip_quotes(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

bool parse_u64(const std::string& text, uint64_t& out) {
    try {
        size_t used = 0;
        out = std::stoull(text, &used, 0);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

bool parse_patch_sites(const std::string& text,
                       std::vector<xrkit::engine::patch::PatchSite>& out, std::string& error) {
    int line_no = 0;
    for (const std::string& raw_line : [&] {
             std::vector<std::string> lines;
             std::string cur;
             for (char c : text) {
                 if (c == '\n') { lines.push_back(cur); cur.clear(); }
                 else if (c != '\r') cur.push_back(c);
             }
             if (!cur.empty()) lines.push_back(cur);
             return lines;
         }()) {
        ++line_no;
        const std::string line = [&] {
            std::string t = raw_line;
            const size_t hash = t.find('#');
            if (hash != std::string::npos) t = t.substr(0, hash);
            return t;
        }();
        const std::vector<std::string> tokens = split_tokens(line);
        if (tokens.empty()) continue;
        if (tokens[0] != "site") {
            error = "第 " + std::to_string(line_no) + " 行不是 `site` 开头";
            return false;
        }
        xrkit::engine::patch::PatchSite site;
        for (size_t i = 1; i < tokens.size(); ++i) {
            const size_t eq = tokens[i].find('=');
            if (eq == std::string::npos) {
                error = "第 " + std::to_string(line_no) + " 行的 token 缺 `=`：" + tokens[i];
                return false;
            }
            const std::string key = tokens[i].substr(0, eq);
            const std::string val = strip_quotes(tokens[i].substr(eq + 1));
            if (key == "id") site.id = val;
            else if (key == "kind") site.kind = val;
            else if (key == "offset") {
                if (!parse_u64(val, site.offset)) {
                    error = "第 " + std::to_string(line_no) + " 行 offset 解析不了：" + val;
                    return false;
                }
            } else if (key == "expect") site.expect_hex = val;
            else if (key == "patched") site.patched_hex = val;
            else if (key == "helper") site.helper = val;
            else if (key == "tail") site.tail_hex = val;
            else if (key == "before") site.before_hex = val;
            else if (key == "after") site.after_hex = val;
            else if (key == "layout_delta") {
                if (!parse_u64(val, site.layout_delta)) {
                    error = "第 " + std::to_string(line_no) + " 行 layout_delta 解析不了：" + val;
                    return false;
                }
            } else if (key == "layout_section") site.layout_section = val;
            else if (key == "note") site.note = val;
            else {
                error = "第 " + std::to_string(line_no) + " 行有未知键：" + key;
                return false;
            }
        }
        if (site.id.empty() || site.kind.empty()) {
            error = "第 " + std::to_string(line_no) + " 行缺 id 或 kind";
            return false;
        }
        out.push_back(site);
    }
    if (out.empty()) {
        error = "一份站点都没解析到（空表不算一份表）";
        return false;
    }
    return true;
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
    // --unpack-tree 的**写后校验档位**（用户裁决 ③）：默认＝边写边算哈希（零额外 I/O）；
    // 显式给本开关才回到"读回逐字节复核"。也用于 `--selftest` 的注入自证（见下方安装点）。
    bool verify_readback = false;
    std::string plugins_dir;   // --plugins <目录>（空 ⇒ exe 旁边的 plugins/，与 GUI 同口径）
    bool plugins_given = false;
    std::string python_home;   // --python-home <目录>（P2：CPython 宿主的 home，仅探针用）
    // ★★ 2026-10-06（审计第 4 组）：见 `--module-subset` 的解析处与 `--status-line` 动作。
    std::string module_subset;
    bool module_subset_given = false;
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
        if (a == "--plugins" && i + 1 < args.size()) {
            plugins_dir = args[++i];
            plugins_given = true;
            continue;
        }
        if (a == "--plugins") {
            std::cerr << "PLUGINS error reason=用法：--plugins <目录>\n";
            return kExitUsage;
        }
        // ★★ 插件改装 P2：CPython 宿主的 Python home（只给 `--python-host` 用；
        //   不给 ⇒ `resolve_python_home("")` 按"exe 旁 python/ → 系统安装"解析）。
        if (a == "--python-home" && i + 1 < args.size()) {
            python_home = args[++i];
            continue;
        }
        if (a == "--python-home") {
            std::cerr << "PYHOST error reason=用法：--python-home <目录>\n";
            return kExitUsage;
        }
        if (a == "--all") {
            extract_all = true;
            continue;
        }
        if (a == "--verify-readback") {
            verify_readback = true;
            continue;
        }
        // ★★ 2026-10-06（审计第 4 组：「`engine::status_line()` 的**常量自报**」）：
        //   诊断开关 —— 把"当前引擎用到的模块集"真的收窄成这一串（逗号分隔；可给空串）。
        //   为什么必须有它：修前 `status_line()` 返回**字面量** `modules=5/5`，任何判据都
        //   反驳不了它（改代码不改它、摘模块不改它）。有了这个开关，"模块集变了 ⇒ 那一串
        //   必须跟着变"就成了**可判红**的判据（见 `--status-line` 与门禁的 `engine-status` 步）。
        //   它只影响诊断动作，产品路径从不调 `set_module_subset` ⇒ 行为逐字节不变。
        if (a == "--module-subset" && i + 1 < args.size()) {
            module_subset = args[++i];
            module_subset_given = true;
            continue;
        }
        if (a == "--module-subset") {
            std::cerr << "MODULES error reason=用法：--module-subset <a,b,c>（可给空串 ⇒ 一块都不生效）\n";
            return kExitUsage;
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

    // ★★ 2026-10-06（审计第 4 组）：把诊断用的模块集收窄**落进引擎**（默认路径不调它）。
    //   只影响 `--status-line` 这类诊断动作读到的数字；产品动作（封包/解包/…）不看模块表。
    if (module_subset_given) {
        std::vector<std::string> names;
        std::string piece;
        for (char ch : module_subset) {
            if (ch == ',') {
                if (!piece.empty()) names.push_back(piece);
                piece.clear();
                continue;
            }
            piece.push_back(ch);
        }
        if (!piece.empty()) names.push_back(piece);
        xrkit::engine::set_module_subset(names);
    }

    // ── ★ 2026-10-04 动作分派前：建**同一个**插件注册表并注入引擎（⑫12.1 的消费端）──
    // 用户口径（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。**」
    // 修前的缺陷：闸门只在外壳里，CLI 的动作路径**从不建注册表**、也从不加载插件 ⇒ 加密包
    // 被 CLI 当普通包读（同一引擎、两个前端两套行为）。
    // 现在与 GUI 共用一条口径：
    //   · 目录 = `--plugins <目录>` 优先，否则 `core::app_dir()/plugins`（GUI 的 `Hub::load_plugins` 同源）；
    //   · **加载失败只记错、不阻塞**动作（1.x 口径，也是 GUI 的口径）；
    //   · 没装解密器 ⇒ 没有任何命中 ⇒ 一切照旧（默认 `plugins/` 常为空 ⇒ 行为与修前一致）。
    //   · 用完复位：注册表是**局部对象**，动作跑完就得把钩子收回来（不许留野指针）。
    xrkit::plugin::Registry plugin_registry;
    {
        const std::string dir = plugins_given ? plugins_dir : (xrkit::core::app_dir() + "/plugins");
        // ★★ P2b：与 GUI 走**同一个**统一入口（`*.dll` + `*.py`）—— 修前两边各调
        //   `load_plugins_from_dir`，Python 插件一旦加进来就会出现"GUI 有、CLI 没有"的分叉。
        const std::vector<xrkit::plugin::RegistrationError> plugin_errors =
            xrkit::plugin::load_all_plugins_from_dir(dir, plugin_registry, /*ui=*/nullptr);
        if (plugins_given) {
            // 显式指定了插件目录才报账（默认路径下保持 CLI 输出与修前逐字节一致）
            std::cout << "PLUGINS dir=" << dir << " errors=" << plugin_errors.size() << "\n";
            for (const xrkit::plugin::RegistrationError& e : plugin_errors) {
                std::cout << "PLUGINS warn plugin=" << e.plugin << " reason=" << e.message << "\n";
            }
        }
    }
    // RAII：钩子在**任何** return 路径上都会被收回（注册表是这个函数的局部对象，
    // 钩子绝不允许活得比它久）。作用域要罩住后面整段动作分派。
    const xrkit::plugin::ActiveRegistryScope active_plugin_registry(&plugin_registry);

    // ★★ 2026-10-05（用户裁决 ③）：原子写"写后校验"的**先红后绿取证**。
    //
    // 取证不需要额外的命令行开关：自检里那条锁（`core: 原子写自校验…`）自己把探针装上、
    // 验完再还原，**跑 `--selftest --verbose` 就能看到它一路 PASS**（包括"注入 ⇒ 必须红"）。
    // 这里**不**再装第二份进程级探针 —— 自检每条锁都会用全新 `WriteProbe` 覆盖，
    // 装在更外层的探针反正会被覆盖（2026-10-05 实测：`--selftest --verify-readback` 与
    // `--selftest` 的输出逐字节相同，装了等于没装）。宁可不留这种"看起来有用"的死开关。
    (void)verify_readback;

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
    // ★★ 2026-10-06（审计处理批·2-4）：`user.ltx` 的 `[theme] scale` 读侧探针。
    //   为什么要有它：这条链的**唯一**消费点在外壳（`shell/theme.cpp apply_startup_ui`），
    //   而 CLI 一直没有任何入口能读它 ⇒ "越界/垃圾值回落屏幕自适应"这条口径**判不了红**
    //   （审计 §5.8 / A2-9：`scale = 1e400` ⇒ `strtod` 给 +∞ ⇒ `px()` 里 double→int 越界转换 UB）。
    //   本动作只做**读**：把 `load_ui_scale()` 的原始串与解析结果打出来（不改任何文件）。
    if (action == "--ui-scale-probe") {
        const std::string raw = xrkit::core::load_user_value(xrkit::core::kPrefSection,
                                                            xrkit::core::kPrefScaleKey, "");
        const double scale = xrkit::core::load_ui_scale();
        std::cout << "UISCALE ok raw=" << (raw.empty() ? "-" : raw) << " scale=" << scale
                  << " path=" << xrkit::core::user_ltx_path() << "\n";
        return kExitOk;
    }

    if (action == "--video-plan") {
        // 判据面：把 `tests/probe_video_argv.py` 里那 7 组用例（输入与 1.x 实调时**同一张表**）
        // 交给引擎算 argv，逐行打印，供 `tests/golden_video_argv.py` 与基线对拍。
        namespace vid = xrkit::engine::video;
        struct Case {
            const char* name;
            bool has_ref;
            bool ref_has_video;
            const char* ref_codec;
            int ref_w, ref_h;
            double ref_fps;
            long long ref_vbitrate;
            bool ref_has_audio;
            int ref_ar, ref_ac;
            long long ref_abitrate;
            bool src_parsed;
            bool src_has_video;
            int src_w, src_h;
            double src_fps;
            bool src_has_audio;
            int encoder_set;  // 0=三个都有 1=无 libxvid 2=只有 mpeg4
        };
        const Case cases[] = {
            {"theora_ref_bitrate_src_same_audio", true, true, "theora", 640, 480, 25.0, 800000, true,
             44100, 2, 128000, true, true, 640, 480, 25.0, true, 0},
            {"theora_ref_no_bitrate_src_diff_noaudio", true, true, "h264", 0, 0, 0.0, 0, false, 0, 0,
             0, true, true, 320, 240, 30.0, true, 0},
            {"mpeg4_ref_no_bitrate_src_noaudio", true, true, "mpeg4", 640, 480, 25.0, 0, true, 0, 0,
             0, true, true, 640, 480, 25.0, false, 0},
            {"mpeg4_ref_fallback_noxvid", true, true, "mpeg4", 640, 480, 25.0, 0, true, 0, 0,
             0, true, true, 640, 480, 25.0, true, 1},
            {"last_resort_only_mpeg4", true, true, "theora", 640, 480, 25.0, 800000, true, 44100, 2,
             128000, true, true, 640, 480, 25.0, true, 2},
            {"ffprobe_failed_keeps_audio", true, true, "theora", 640, 480, 25.0, 800000, true, 44100,
             2, 128000, false, false, 0, 0, 0.0, false, 0},
            {"no_reference_at_all", false, false, "", 0, 0, 0.0, 0, false, 0, 0, 0, true, true, 640,
             480, 25.0, true, 0},
        };
        for (const Case& item : cases) {
            vid::TranscodeRequest request;
            request.ffmpeg = "FFMPEG";
            request.input = "IN.avi";
            request.output = "OUT.ogm";
            request.has_reference = item.has_ref;
            if (item.has_ref) {
                request.reference.has_video = item.ref_has_video;
                request.reference.has_audio = item.ref_has_audio;
                request.reference.video.codec = item.ref_codec;
                request.reference.video.width = item.ref_w;
                request.reference.video.height = item.ref_h;
                request.reference.video.fps = item.ref_fps;
                request.reference.video.bitrate = item.ref_vbitrate;
                request.reference.audio.sample_rate = item.ref_ar;
                request.reference.audio.channels = item.ref_ac;
                request.reference.audio.bitrate = item.ref_abitrate;
            }
            request.source.parsed = item.src_parsed;
            request.source.has_video = item.src_has_video;
            request.source.has_audio = item.src_has_audio;
            request.source.video.width = item.src_w;
            request.source.video.height = item.src_h;
            request.source.video.fps = item.src_fps;
            if (item.encoder_set == 0) {
                request.available_encoders = {"libtheora", "libxvid", "mpeg4"};
            } else if (item.encoder_set == 1) {
                request.available_encoders = {"libtheora", "mpeg4"};
            } else {
                request.available_encoders = {"mpeg4"};
            }
            const vid::TranscodePlan plan = vid::plan_transcode(request);
            std::string joined;
            for (const std::string& part : plan.argv) {
                if (!joined.empty()) joined += '\x1f';
                joined += part;
            }
            std::cout << "PLAN\t" << item.name << "\t" << joined << "\n";
            // PLANSUM：界面「视频参数」面板读的那些值（scale/fps/码率/音频）。
            // 期望值由 `tests/golden_video_argv.py` **从 1.x 的 argv 反推**（不是我手写）⇒
            // 面板显示的东西与真正交给 ffmpeg 的东西不可能对不上。
            std::cout << "PLANSUM\t" << item.name << "\t" << plan.video_encoder
                      << "|scale=" << (plan.scale_applied ? 1 : 0)
                      << "|fps=" << (plan.fps_applied ? 1 : 0) << "|vbr=" << plan.video_bitrate
                      << "|abr=" << plan.audio_bitrate << "|ar=" << plan.audio_sample_rate
                      << "|ac=" << plan.audio_channels << "|an=" << (plan.audio_disabled ? 1 : 0)
                      << "\n";
        }
        return kExitOk;
    }
    if (action == "--video-probe") {
        // 解析判据：把**录下来的真 ffprobe 输出**（tests/golden/video_probe/）喂给
        // `parse_ffprobe_json`，逐字段打印；`tests/golden_video_probe.py` 拿 1.x 的
        // `parse_video_info` 吃同一份字节，两边比字段（期望值不是我手写的）。
        if (operands.empty()) {
            std::cerr << "VIDEOPROBE error reason=用法：--video-probe <ffprobe-json 文件>\n";
            return kExitUsage;
        }
        const std::vector<uint8_t> raw = read_all(operands[0]);
        const std::string text(raw.begin(), raw.end());
        const xrkit::engine::video::MediaInfo info =
            xrkit::engine::video::parse_ffprobe_json(text);
        char buffer[128];
        auto emit = [](const char* key, const char* value) {
            std::cout << "PROBE\t" << key << "\t" << value << "\n";
        };
        emit("parsed", info.parsed ? "1" : "0");
        emit("has_video", info.has_video ? "1" : "0");
        emit("has_audio", info.has_audio ? "1" : "0");
        std::snprintf(buffer, sizeof(buffer), "%.6f", info.duration);
        emit("duration", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.6f", info.size_mb);
        emit("size_mb", buffer);
        emit("video.codec", info.video.codec.c_str());
        std::snprintf(buffer, sizeof(buffer), "%d", info.video.width);
        emit("video.width", buffer);
        std::snprintf(buffer, sizeof(buffer), "%d", info.video.height);
        emit("video.height", buffer);
        std::snprintf(buffer, sizeof(buffer), "%.6f", info.video.fps);
        emit("video.fps", buffer);
        std::snprintf(buffer, sizeof(buffer), "%lld", info.video.bitrate);
        emit("video.bitrate", buffer);
        emit("audio.codec", info.audio.codec.c_str());
        std::snprintf(buffer, sizeof(buffer), "%d", info.audio.channels);
        emit("audio.channels", buffer);
        std::snprintf(buffer, sizeof(buffer), "%d", info.audio.sample_rate);
        emit("audio.sample_rate", buffer);
        std::snprintf(buffer, sizeof(buffer), "%lld", info.audio.bitrate);
        emit("audio.bitrate", buffer);
        return kExitOk;
    }
    if (action == "--video-encoders") {
        // 解析判据：`ffmpeg -encoders` 的**真输出**喂给 `parse_encoder_list`。
        if (operands.empty()) {
            std::cerr << "VIDEOENC error reason=用法：--video-encoders <ffmpeg -encoders 输出文件>\n";
            return kExitUsage;
        }
        const std::vector<uint8_t> raw = read_all(operands[0]);
        const std::string text(raw.begin(), raw.end());
        const std::vector<std::string> names = xrkit::engine::video::parse_encoder_list(text);
        std::cout << "ENCODERS\t" << names.size() << "\n";
        for (const std::string& name : names) std::cout << "ENC\t" << name << "\n";
        return kExitOk;
    }
    if (action == "--video-output-check") {
        // ★ 2026-10-07（审计补充 F1-2 / F1-3）：`-y` 恒在 argv 里 ⇒ "输出已存在 / 输出就是
        //   源 / 输出就是参考"这三件必须由**引擎**在起跑前判定。这里只把结论打成机器可解析的
        //   形态，供 `tests/check_video_overwrite_guard.py` 判红（页面侧另有一条行为锁）。
        //   用法：--video-output-check <源文件> <参考OGM|-> <输出文件>
        if (operands.size() < 3) {
            std::cerr << "VIDEOOUTPUT error reason=用法：--video-output-check <源文件> "
                         "<参考OGM|-> <输出文件>\n";
            return kExitUsage;
        }
        namespace vid = xrkit::engine::video;
        vid::TranscodeRequest request;
        request.input = operands[0];
        request.reference_path = (operands[1] == "-") ? std::string() : operands[1];
        request.output = operands[2];
        const vid::OutputConflict conflict = vid::check_output_conflict(request);
        const char* name = "none";
        switch (conflict) {
            case vid::OutputConflict::SameAsSource: name = "same-as-source"; break;
            case vid::OutputConflict::SameAsReference: name = "same-as-reference"; break;
            case vid::OutputConflict::Exists: name = "exists"; break;
            case vid::OutputConflict::None: name = "none"; break;
        }
        std::cout << "VIDEOOUTPUT conflict=" << name << "\n";
        std::cout << "VIDEOOUTPUT reason=" << vid::output_conflict_reason(conflict, request) << "\n";
        return kExitOk;
    }
    if (action == "--status-line") {
        // ★★ 2026-10-06（审计第 4 组）：「`engine::status_line()` 的常量自报」的判据探针。
        // 输出**机器可解析**的形态：`STATUSLINE line=<原始行> used=<当前用到> on=<能跑> all=<编译进来的总数>
        // ready=<0|1> names=<清单>` —— 门禁/自检据此判"模块集被收窄后，那一串有没有跟着变"
        // （修前它恒等于字面量 `modules=5/5`）。
        const std::string line = xrkit::engine::status_line();
        const int on = xrkit::engine::module_ready_count();
        const int used = xrkit::engine::module_count();
        const int all = xrkit::engine::all_module_count();
        std::cout << "STATUSLINE line=" << line << " used=" << used << " on=" << on
                  << " all=" << all << " ready=" << (xrkit::engine::ready() ? 1 : 0) << " names=";
        const std::vector<std::string> names = xrkit::engine::module_names();
        for (size_t i = 0; i < names.size(); ++i) std::cout << (i ? "," : "") << names[i];
        std::cout << "\n";
        return kExitOk;
    }
    if (action == "--version-store") {
        if (operands.empty()) {
            std::cerr << "VERSIONSTORE error reason=用法：--version-store <version.json> [set <版本> <标记>]\n";
            return kExitUsage;
        }
        const std::string store_path = operands[0];
        xrkit::core::VersionStore store;
        std::string verr;
        if (!xrkit::core::load_version_store(store_path, store, verr)) {
            // ★ 2026-09-27 实测踩到：**解析失败 ≠ 文件不存在**。原先把两者混在一起，
            //   一份带 BOM（或写坏）的 version.json 会被"当成首次使用"**静默覆盖**，
            //   等于把用户的构建史丢掉。存在但读不出来 ⇒ 如实报错、**一个字节都不写**。
            if (std::filesystem::exists(xrkit::core::path_from_utf8(store_path))) {
                std::cout << "VERSIONSTORE error reason=解析失败（已保留原文件，不覆盖）：" << verr
                          << "\n";
                return kExitFail;
            }
            store = xrkit::core::VersionStore{};
            store.current_version = "2.0.0";
            store.current_prerelease = "BETA";
            store.current_index = 1;
            if (!xrkit::core::save_version_store(store_path, store, verr)) {
                std::cout << "VERSIONSTORE error reason=" << verr << "\n";
                return kExitFail;
            }
            std::cout << "VERSIONSTORE created=" << store_path << "\n";
        }
        if (operands.size() >= 2 && operands[1] == "write-header") {
            // ★★ 2026-10-07（H1-2，用户候选 A）：把 tree 里**最后一次成功发包**的
            //   「标记.序号」写回 `include/xrkit/core/version.h`（真来源＝version.json 的 tree；
            //   version.h 是派生）。**故意不读 `current.index`** —— 那是"下一个号"，
            //   门禁与构建 GUI 都会推它（实测 59 → 60）。
            //   写回之后必须重跑 `python tools/gen_winres.py` 让 `.rc` 同源（本动作不代跑，
            //   因为它不该替调用方决定生成器怎么跑；`--build-package` 里是代跑的）。
            std::string last_version;
            std::string last_prerelease;
            int last_index = 0;
            if (!xrkit::core::last_release(store, last_version, last_prerelease, last_index, verr)) {
                std::cout << "VERSIONSTORE error reason=" << verr << "\n";
                return kExitFail;
            }
            const std::string header = "include/xrkit/core/version.h";
            const std::string want = last_prerelease.empty()
                                         ? std::string()
                                         : last_prerelease + "." + std::to_string(last_index);
            std::string herr;
            if (!xrkit::core::save_version_header(header, want, last_index, herr)) {
                std::cout << "VERSIONSTORE error reason=" << herr << "\n";
                return kExitFail;
            }
            std::string got;
            int got_index = -1;
            if (!xrkit::core::load_version_header(header, got, got_index, herr)) {
                std::cout << "VERSIONSTORE error reason=" << herr << "\n";
                return kExitFail;
            }
            std::cout << "VERSIONSTORE ok wrote-header path=" << header
                      << " prerelease=" << got << " build=" << got_index
                      << " release=" << xrkit::core::release_version_string(
                             last_version, last_prerelease, last_index)
                      << "（接着必须重跑 python tools/gen_winres.py 让 .rc 同源）\n";
            return kExitOk;
        }
        if (operands.size() >= 4 && operands[1] == "set") {
            store.current_version = operands[2];
            store.current_prerelease = operands[3];
            store.current_index = store.next_index(operands[2], operands[3]);
            if (!xrkit::core::save_version_store(store_path, store, verr)) {
                std::cout << "VERSIONSTORE error reason=" << verr << "\n";
                return kExitFail;
            }
        } else {
            store.current_index = store.next_index(store.current_version, store.current_prerelease);
        }
        size_t history = 0;
        for (const auto& v : store.tree) {
            for (const auto& m : v.second) history += m.second.size();
        }
        std::cout << "VERSIONSTORE ok version=" << store.current_version
                  << " prerelease=" << store.current_prerelease
                  << " next=" << store.current_index
                  << " full=" << store.full_version()
                  << " versions=" << store.tree.size()
                  << " history=" << history << "\n";
        return kExitOk;
    }

    if (action == "--build-package") {
        return xrkit_cli::run_build_package(operands);
    }

    if (action == "--exe-version") {
        // ★★ 2026-10-07（H1-2，用户候选 A）：直接读 exe 的 VERSIONINFO —— 这是"对外可见的
        //   版本身份"。打包时用它断言"**包内** xrkit_app.exe 的 ProductVersion == 本次发版串"
        //   （修前：包说 ALPHA.60、包内 exe 说 ALPHA.0，而门禁只在各自源内自比 ⇒ 全绿）。
        //   为什么在 C++ 里读：打包链（`--build-package` / `package.ps1`）不该新增 Python 依赖。
        //   输出：`EXEVERSION product=<...> file=<...> numeric=<a.b.c.d>`（机器可解析）。
        if (operands.empty()) {
            std::cerr << "EXEVERSION error reason=用法：--exe-version <exe>\n";
            return kExitUsage;
        }
#ifdef _WIN32
        const std::filesystem::path exe = xrkit::core::path_from_utf8(operands[0]);
        DWORD handle = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(exe.c_str(), &handle);
        if (size == 0) {
            std::cout << "EXEVERSION error reason=读不到版本资源（文件不存在 / 没有 VERSIONINFO）："
                      << operands[0] << "\n";
            return kExitFail;
        }
        std::vector<uint8_t> block(size);
        if (::GetFileVersionInfoW(exe.c_str(), 0, size, block.data()) == FALSE) {
            std::cout << "EXEVERSION error reason=GetFileVersionInfoW 失败："
                      << static_cast<unsigned long>(::GetLastError()) << "\n";
            return kExitFail;
        }
        struct LangAndCodePage {
            WORD language;
            WORD code_page;
        };
        LangAndCodePage* translation = nullptr;
        UINT translation_len = 0;
        if (::VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation",
                             reinterpret_cast<void**>(&translation), &translation_len) == FALSE ||
            translation_len < sizeof(LangAndCodePage)) {
            std::cout << "EXEVERSION error reason=没有 VarFileInfo\\Translation\n";
            return kExitFail;
        }
        const auto string_field = [&](const wchar_t* name) -> std::string {
            wchar_t sub[128];
            std::swprintf(sub, 128, L"\\StringFileInfo\\%04x%04x\\%s", translation->language,
                          translation->code_page, name);
            wchar_t* value = nullptr;
            UINT value_len = 0;
            if (::VerQueryValueW(block.data(), sub, reinterpret_cast<void**>(&value), &value_len) ==
                    FALSE ||
                value == nullptr) {
                return std::string();
            }
            return xrkit::core::path_to_utf8(std::filesystem::path(value));
        };
        VS_FIXEDFILEINFO* fixed = nullptr;
        UINT fixed_len = 0;
        std::string numeric;
        if (::VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_len) !=
                FALSE &&
            fixed != nullptr) {
            numeric = std::to_string(HIWORD(fixed->dwFileVersionMS)) + "." +
                      std::to_string(LOWORD(fixed->dwFileVersionMS)) + "." +
                      std::to_string(HIWORD(fixed->dwFileVersionLS)) + "." +
                      std::to_string(LOWORD(fixed->dwFileVersionLS));
        }
        std::cout << "EXEVERSION product=" << string_field(L"ProductVersion")
                  << " file=" << string_field(L"FileVersion") << " numeric=" << numeric
                  << " path=" << operands[0] << "\n";
        return kExitOk;
#else
        std::cout << "EXEVERSION error reason=只在 Windows 上有 VERSIONINFO\n";
        return kExitSkip;
#endif
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

        std::optional<xrkit::engine::fs::DbFormat> fmt;
        // ★ 2026-10-06（1.x `fs_app.py:154` 的下拉默认值就是 "auto"）：CLI 侧要**认** auto，
        //   且它与"不传格式"必须**逐值等价**（同一条自洽判定；歧义时由引擎**显式拒绝**）。
        //   判"是不是 auto"只有引擎那一个谓词，别在这里手写字符串比较。
        if (operands.size() >= 2 && !xrkit::engine::fs::format_key_is_auto(operands[1])) {
            fmt = xrkit::engine::fs::db_format_from_any(operands[1]);
            if (!fmt) {
                std::cout << "DUMP error reason=未知格式名: " << operands[1] << "\n";
                return kExitUsage;
            }
        }
        // ★ 2026-09-30 用户第四轮 ⑦：DB 列条目走 **seek 式**（只读块头 + 尾部条目表）。
        //   旧写法 `read_all` 把整文件读进内存 ⇒ 1900 MB 的包 8,063 ms；顺带把
        //   "1.9 GB 读不进来"那条旧观察也一并消掉（那次是**路径写错**：实际文件在
        //   `IWP\database\patch\` 下，`read_all` 打不开 ⇒ 返回空 ⇒ 报了"读不到文件或文件为空"）。
        const xrkit::engine::fs::DbListing res = xrkit::engine::fs::list_db_seek(target, fmt);
        if (!res.ok) {
            std::cout << "DUMP error reason=" << res.error << "\n";
            return kExitFail;
        }
        // TSV：<d|f> TAB 路径 TAB size_real TAB size_comp TAB offset TAB use_lzhuf
        for (const xrkit::engine::fs::DbEntry& e : res.entries) {
            std::cout << (e.is_dir ? 'd' : 'f') << '\t' << e.path << '\t' << e.size_real << '\t'
                      << e.size_comp << '\t' << e.offset << '\t' << (e.use_lzhuf ? 1 : 0) << '\n';
        }
        std::cout << "DUMP ok kind=db format=" << xrkit::engine::fs::db_format_key(res.format)
                  << " entries=" << res.entries.size()
                  << " ms=" << static_cast<long long>(res.elapsed_ms)
                  << " seek=" << (res.used_seek ? 1 : 0)
                  << " header_bytes=" << res.header_bytes
                  << " file_bytes=" << res.file_size << "\n";
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
            // ★ 2026-09-30 第四轮 ⑦：列条目与取那一条都走 seek（大 DB 不再整包进内存）
            std::optional<xrkit::engine::fs::DbFormat> fmt;
            if (operands.size() >= 4) fmt = xrkit::engine::fs::db_format_from_any(operands[3]);
            const xrkit::engine::fs::DbListing list = xrkit::engine::fs::list_db_seek(source, fmt);
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
            const xrkit::engine::fs::DbExtract r =
                xrkit::engine::fs::extract_entry_seek(source, *found);
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
            std::cerr << "UNPACK error reason=用法：--unpack-tree <镜像> <输出根> [--only <清单文件>] "
                         "[--verify-readback]\n";
            return kExitUsage;
        }
        xrkit::engine::fs::ExtractTreeOptions options;
        options.verify_readback = verify_readback;   // 默认 false ⇒ 边写边算哈希
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
                  << " dups=" << res.duplicate_records
                  << " failed=" << res.failures.size()
                  << " ms=" << static_cast<long long>(res.elapsed_ms) << "\n";
        // 有单项失败：**不谎报成功**（1.x 的"解包失败返回 0"就是这么被吞掉的）
        return res.failures.empty() ? kExitOk : kExitFail;
    }

    // ★★ 2026-10-04 用户拍的**架构红线**（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的
    //   细节功能比 CLI 还多的。**」⇒ 本条动作＝GUI「解包」按钮的等价物：
    //   · 源可以是**一枚包**（SquashFS / DB 六格式都行）或**一个目录**（递归找包，规则＝引擎的
    //     `is_package_file_name`，与 GUI 拖入文件夹同规则）；
    //   · 落点＝**包名去扩展名**、**磁盘父目录不进产物**、**包内已自带同名壳时不再叠一层**
    //     —— 与 GUI **共用** `fs::plan_unpack_targets()`（此前这条只写在 `FsPage::plan_unpack()`
    //     里 ⇒ CLI 复现不了 ⇒ 真机产物布局只能在 GUI/合成夹具上验）。
    //   与 `--unpack-tree` 的分工：后者是"**原样摊开一棵树**"（SquashFS，条目直接落在输出根下，
    //   保持既有行为兼容）；要"和 GUI 一样"就用本条。
    if (action == "--unpack-package") {
        if (operands.size() < 1) {
            std::cerr << "UNPACK-PKG error reason=用法：--unpack-package <包|目录> [输出根] "
                         "[--only <清单文件>] [--verify-readback] [DB格式]\n";
            return kExitUsage;
        }
        const std::string& src = operands[0];
        // ★★ 2026-10-04：**输出根可省**（用户红线「GUI 的功能，CLI 肯定要全部拥有」）——
        //   省略时走引擎同一条规则 `resolve_output_root`（第一个源所在目录；没有源 ⇒ 当前目录），
        //   与 GUI 的「解包到」留空行为**逐字符一致**。
        //   歧义消解：`operands[1]` 若**能当 DB 格式名**且后面没有第 3 个操作数 ⇒ 它是格式、不是根。
        bool out_root_given = false;
        std::string out_root;
        std::string fmt_operand;
        if (operands.size() >= 2) {
            // ★ 2026-10-06：`auto` 也算"格式位置上的操作数"（它＝不给格式）——
            //   否则 `--unpack-package <包> auto` 会把 auto 当成**输出根**（静默错落点）。
            const bool second_is_fmt =
                (xrkit::engine::fs::format_key_is_auto(operands[1]) ||
                 xrkit::engine::fs::format_key_to_db(operands[1]).has_value()) &&
                operands.size() == 2;
            if (second_is_fmt) {
                fmt_operand = operands[1];
            } else {
                out_root = operands[1];
                out_root_given = true;
                if (operands.size() >= 3) fmt_operand = operands[2];
            }
        }
        std::optional<xrkit::engine::fs::DbFormat> fallback;
        if (!fmt_operand.empty() && !xrkit::engine::fs::format_key_is_auto(fmt_operand)) {
            fallback = xrkit::engine::fs::format_key_to_db(fmt_operand);
            if (!fallback.has_value()) {
                std::cerr << "UNPACK-PKG error reason=不认识的 DB 格式：" << fmt_operand << "\n";
                return kExitUsage;
            }
        }
        (void)out_root_given;
        // ① 收源（目录 ⇒ 递归找包；文件 ⇒ 它自己）
        std::vector<std::string> images;
        std::error_code ec;
        const std::filesystem::path src_path = xrkit::core::path_from_utf8(src);
        if (std::filesystem::is_directory(src_path, ec)) {
            for (std::filesystem::recursive_directory_iterator it(src_path, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                const std::string f = xrkit::core::path_to_utf8(it->path());
                if (xrkit::engine::fs::is_package_file_name(f)) images.push_back(f);
            }
            std::sort(images.begin(), images.end());
        } else {
            images.push_back(src);
        }
        // ★★ 输出根缺省：与 GUI **同一条引擎规则**（省略根 ⇒ 第一个源所在目录；没有源 ⇒ 当前目录）。
        if (out_root.empty()) {
            const std::string front = images.empty() ? std::string() : images.front();
            out_root = xrkit::engine::fs::resolve_output_root(
                front, xrkit::core::path_to_utf8(std::filesystem::current_path()));
        }
        std::cout << "UNPACK-PKG root=" << out_root << "\n";
        // ② `--only` 清单读一次（空清单**不等于**全部 —— 与 `--unpack-tree` 同一口径）
        std::vector<std::string> only_paths;
        if (!only_list.empty() && !extract_all) {
            std::ifstream in(xrkit::core::path_from_utf8(only_list), std::ios::binary);
            if (!in) {
                std::cout << "UNPACK-PKG error reason=读不到清单文件：" << only_list << "\n";
                return kExitFail;
            }
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) only_paths.push_back(line);
            }
        }
        // ③ 多源 ⇒ 落点（**引擎那一份**）；条目表用于判断"包内是否已自带同名壳"
        std::vector<std::pair<std::string, std::vector<std::string>>> plan_input;
        for (const std::string& img : images) {
            std::string err;
            const std::vector<xrkit::engine::fs::EntryInfo> entries =
                xrkit::engine::fs::package_entries(img, fallback, err);
            if (!err.empty()) {
                // ★ 2026-10-04：改前这里**把 `err` 直接丢掉**（列不出条目也照样往下排）⇒
                //   解密闸门拒绝的理由会消失。现在如实打印理由并**非零退出**
                //   （与 GUI 同一条文案：理由由引擎给出，前端只负责显示）。
                std::cout << "UNPACK-PKG error image=" << img << " reason=" << err << "\n";
                return kExitFail;
            }
            std::vector<std::string> paths;
            paths.reserve(entries.size());
            for (const xrkit::engine::fs::EntryInfo& e : entries) paths.push_back(e.path);
            plan_input.emplace_back(img, std::move(paths));
        }
        const std::vector<xrkit::engine::fs::UnpackTarget> plan =
            xrkit::engine::fs::plan_unpack_targets(out_root, plan_input);
        // ④ 逐包导出（`extract_package` 按类型分派 ⇒ SquashFS 与 DB 六格式同一条路）
        //   ★★ 2026-10-04 **两种口径的分工**（判红锁②，别混）：
        //     · `entries=` = **产物口径**（同路径的多条记录只算一条）—— 它和后一行
        //       `files=/dirs=/bytes=`、以及右栏"解包产物预览"是同一把尺子；
        //     · `records=` = **包内记录数**（忠实、不去重）—— 与 `--dump` 的 `entries=` 同口径。
        //   为什么必须分开报：`IWP\database\maps\*.db` 的 HEADER 表把除根记录外的每条记录登记了
        //   两遍（同路径/同 offset ⇒ 同一份数据，独立解码已证实重复在包内）⇒ 报"记录数"会正好是
        //   盘上真相的 2 倍 −1。**`--dump` 保持忠实报记录数**（它是协议层工具，另有用途）。
        uint64_t files = 0, dirs = 0, bytes = 0, failed = 0, done = 0, dups = 0, overwritten = 0;
        for (size_t i = 0; i < plan.size(); ++i) {
            std::cout << "UNPACK-PKG plan image=" << plan[i].image << " target=" << plan[i].target
                      << " entries=" << xrkit::engine::fs::unique_entry_count(plan_input[i].second)
                      << " records=" << plan_input[i].second.size() << "\n";
            xrkit::engine::fs::ExtractTreeOptions options;
            // ★★ 2026-10-05（小件 ③）：**`--verify-readback` 也接到本动作**。
            //   用户红线（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能
            //   比 CLI 还多的。**」的同一面 —— **CLI 是能力全集**，那么"两个入口该一致"：
            //   本动作就是 GUI「解包」按钮的等价物（`--unpack-tree` 是"原样摊开"那条老路），
            //   而写后校验档位此前**只有 `--unpack-tree` 能选** ⇒ 同一个按钮的等价物少一档。
            options.verify_readback = verify_readback;   // 默认 false ⇒ 边写边算哈希
            if (!only_paths.empty()) {
                // ★★ 2026-10-06（审计处理批·1-4 / S1-7）：修前这里是**自己抄的一份比对** ——
                //   `std::find(plan_input[i].second, one)` 只比**镜像内路径**，而页面/用户
                //   手里那份勾选是**落点路径**（带包名壳）⇒ 一条都不中 ⇒ `options.only` 空
                //   ⇒ 下面 `if (options.only.empty()) continue;` 把整包静默跳过 ⇒
                //   `UNPACK-PKG ok files=0`、退 0，用户看不到任何理由。
                //   现在改用引擎的**唯一一份**匹配规则 `selection_matches()`（落点路径与
                //   镜像内路径都认），与 GUI「解包选中」逐条同源。
                std::vector<std::string> paths_for_match;
                paths_for_match.reserve(plan_input[i].second.size());
                for (const std::string& p : plan_input[i].second) paths_for_match.push_back(p);
                for (const std::string& one : only_paths) {
                    if (xrkit::engine::fs::selection_matches(plan[i].image, paths_for_match, one)) {
                        options.only.push_back(one);
                    }
                }
                if (options.only.empty()) continue;   // 这一包里没有清单中的项 ⇒ 不排它（同 GUI 勾选模式）
                options.selection = xrkit::engine::fs::Selection::Listed;
            }
            const xrkit::engine::fs::ExtractTreeResult res =
                xrkit::engine::fs::extract_package(plan[i].image, plan[i].target, options, fallback);
            for (const xrkit::engine::fs::ExtractTreeFailure& f : res.failures) {
                std::cout << "UNPACK-PKG fail image=" << plan[i].image << " path=" << f.path
                          << " reason=" << f.reason << "\n";
            }
            if (!res.ok) {
                std::cout << "UNPACK-PKG error image=" << plan[i].image << " reason=" << res.error << "\n";
                ++failed;
                continue;
            }
            ++done;
            files += res.files_written;
            dirs += res.dirs_created;
            bytes += res.bytes_written;
            dups += res.duplicate_records;
            failed += res.failures.size();
            // ★★ 2026-10-05（**落点覆盖必须可见 / 零静默**）：逐条报出被**覆盖**的已存在文件
            //   + 计数。起因：一次默认落点解包把 3.9 万个文件盖在模组目录上，工具一个字没说。
            //   ★ **不改默认落点**（默认＝来源所在目录，用户口径），也**不弹模态确认框**
            //     （那会改交互口径）—— 只做"看得见"：目标在解包前已打印（上面的 plan 行），
            //     这里逐条报"哪一条被盖了"。
            for (const std::string& p : res.overwritten) {
                std::cout << "UNPACK-PKG overwrite image=" << plan[i].image << " path=" << p << "\n";
            }
            overwritten += res.overwritten_size();
        }
        // ★★ 2026-10-06（审计处理批·1-4 的 CLI 侧 / 零静默）：`--only` 清单**一条都没命中
        //   任何包**时，修前是"逐包 `continue` 掉 ⇒ files=0、failed=0"，输出里只有
        //   `UNPACK-PKG ok sources=N done=0 files=0` —— 字面看像"成功但什么都没做"。
        //   这里补一条**明确的原因行**（`tests/check_unpack_rules.py` 的反向锁正是靠
        //   "一个包都没排上"来做到"只看计划、不落盘"，所以这条**不改退出码**：
        //   退出码仍由 `failed` 决定，与既有真机反向锁的契约一致）。
        //   ★ 引擎侧"对不上的逐条记 failures"才是硬判据（见 `check_unpack_only_semantics.py`）。
        if (!only_paths.empty() && files == 0) {
            std::cout << "UNPACK-PKG note reason=--only 清单一条都没命中（清单 " << only_paths.size()
                      << " 条，plan " << plan.size() << " 个包都没排上）—— 本次没有产物写出\n";
        }
        std::cout << "UNPACK-PKG ok sources=" << plan.size() << " done=" << done << " files=" << files
                  << " dirs=" << dirs << " bytes=" << bytes << " dups=" << dups
                  << " overwritten=" << overwritten
                  << " failed=" << failed << "\n";
        return failed == 0 ? kExitOk : kExitFail;
    }

    // 有损编解码探针（Python `errors="replace"` 的逐字节对拍用）。
    // 门禁 [12] 用 Python 同名 codec 驱动它：单字节 / UTF-8 截断 / GB 家族边界。
    // ★★ 2026-10-06（审计处理批·2-1/2-2）：补 `strict` 模式 —— "**严格**解码/编码"这条
    //   路在此之前**没有任何黑盒入口**：`--decode-text` 走的是六编码链（latin-1 兜底、
    //   永不失败），`--codec-lossy` 走的是 replace 语义 ⇒ 严格路径的边界（越界读表 /
    //   有损分支死循环）谁也判不了红。语义与 lossy 一一对应：
    //     `--codec-lossy decode strict <编码> <输入> <输出>` = `decode_strict`（必抛/必失败）
    //     `--codec-lossy encode strict <编码> <输入> <输出>` = `encode_strict`（表示不了即失败）
    if (action == "--codec-lossy") {
        if (operands.size() < 4) {
            std::cerr << "CODEC error reason=用法：--codec-lossy <decode|encode> <编码> <输入> <输出>"
                         "（“严格”语义：加第四个位置参数 strict）\n";
            return kExitUsage;
        }
        const std::string& mode = operands[0];
        const std::string& encoding = operands[1];
        // 位置与帮助行一一对应：`<decode|encode> <编码> <输入> [strict] <输出>`
        const bool strict = operands.size() >= 5 && operands[3] == "strict";
        const size_t out_arg = strict ? 4 : 3;
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
            const bool decoded =
                strict ? xrkit::engine::text::decode_strict(
                             encoding, reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                             text, err)
                       : xrkit::engine::text::decode_lossy(
                             encoding, reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                             text, err);
            if (!decoded) {
                std::cout << "CODEC error mode=" << (strict ? "strict" : "lossy")
                          << " reason=" << err << "\n";
                return kExitFail;
            }
            out.assign(text.begin(), text.end());
        } else if (mode == "encode") {
            const bool encoded = strict
                                     ? xrkit::engine::text::encode_strict(encoding, body, out, err)
                                     : xrkit::engine::text::encode_lossy(encoding, body, out, err);
            if (!encoded) {
                std::cout << "CODEC error mode=" << (strict ? "strict" : "lossy")
                          << " reason=" << err << "\n";
                return kExitFail;
            }
        } else {
            std::cerr << "CODEC error reason=未知模式（decode|encode）\n";
            return kExitUsage;
        }
        std::ofstream file(xrkit::core::path_from_utf8(operands[out_arg]),
                           std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cout << "CODEC error reason=写不到输出：" << operands[out_arg] << "\n";
            return kExitFail;
        }
        if (!out.empty()) {
            file.write(reinterpret_cast<const char*>(out.data()),
                       static_cast<std::streamsize>(out.size()));
        }
        file.close();
        std::cout << "CODEC ok mode=" << (strict ? "strict" : "lossy") << " encoding=" << encoding
                  << " bytes=" << out.size() << "\n";
        return kExitOk;
    }

    // ★ D3 批：big5 / shift_jis / euc-kr 的**反向编码表**整表导出（对拍用）。
    //   为什么要有它：`--codec-lossy encode` 一次只编一个字符，把三张表的**每一个**
    //   可编码码位逐条过一遍要 ~2.9 万次进程启动（实测每次约 15 ms ⇒ 十分钟级），
    //   塞不进门禁 [12]。整表导出把"逐项核对反向表"压成**一次调用**，判据强度不变：
    //   Python 侧枚举同名 codec 能编出的**全部**码位，逐条与本表比对（`--full`）。
    //   输出（稳定可解析）：`T <编码> <十进制码位> <小写十六进制字节>`，末行 `T END`。
    if (action == "--codec-encode-table") {
        struct DumpSpec {
            const char* name;
            int which;
        };
        static const DumpSpec kDump[] = {
            {xrkit::engine::text::kBig5, 0},
            {xrkit::engine::text::kShiftJis, 1},
            {xrkit::engine::text::kEucKr, 2},
        };
        for (const DumpSpec& spec : kDump) {
            auto rows = xrkit::engine::text::codec_encode_table(spec.which);
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& row : rows) {
                std::cout << "T " << spec.name << " " << row.first << " ";
                static const char* kHex = "0123456789abcdef";
                for (unsigned char b : row.second) {
                    std::cout << kHex[(b >> 4) & 0xF] << kHex[b & 0xF];
                }
                std::cout << "\n";
            }
        }
        std::cout << "T END\n";
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

    // ★★ S1-2（插件 `init` 返回 0 ⇒ 必须**回滚**它 this-init 期间的注册）：判据探针。
    //
    // 夹具 = 一枚"前 6 条注册成功、最后一条 `register_compare_mode` 必失败 ⇒ 入口 `return 0`"
    // 的 DLL（`tools/test_plugin/test_plugin_badinit.c`，由 tools/cli/CMakeLists 编到 exe 旁边）。
    // 修前宿主只 `FreeLibrary`、不回滚 ⇒ 表里留着指向**已 unmap 代码段**的裸函数指针（点一下 AV）。
    //
    // 断言（缺一条不算过）：
    //   ① 宿主**如实报**失败（理由里写明"入口返回 0"）、`loaded_count()` **不含**它；
    //   ② ★主判据：按 **owner 名**逐容器**零残留**（9 个注册容器，逐项 `k=v` 打印）
    //      ＋ `registration_count()` 回到加载前的 mark；
    //   ③ 留痕仍在（零静默）：失败理由必须看得见 —— `errors_` 不参与"零残留"（见 registry.h）。
    //   ★ 判据不是"之后调它不崩"：`FreeLibrary` 未必立刻 unmap，"不崩≠干净"（草案已注明）。
    // 退出码：0=全绿；1=有断言不成立；2=夹具缺失（外部前提，门禁按档位记未覆盖）；3=用法错误。
    // ★★ 插件改装 P2b/P2c/P3：宿主、插件加载链、**控件门面能力表**三个诊断动作。
    //   实现在**引擎层唯一一份**（`py_host.cpp::run_python_host_probe`）——
    //   因为发行包里只有 `xrkit_app.exe`（没有 CLI），"包内 Python 能不能起来、能不能加载 .py"
    //   必须由 GUI exe 的同名开关也能跑；两处各写一遍必然分叉（本仓最贵的一类返工）。
    //   退出码：0=通 / 1=有失败 / 2=构建期没带 Python（显式跳过）/ 3=用法错误。
    if (action == "--python-host" || action == "--py-plugins" || action == "--ui-facade" ||
        action == "--py-plugin-call") {
        return xrkit::plugin::run_python_host_probe(args, [](const std::string& line) {
            std::cout << line << "\n";
        });
    }

    if (action == "--plugin-init-rollback") {
        if (operands.empty()) {
            std::cerr << "PLUGINROLLBACK error reason=用法：--plugin-init-rollback <只放夹具的目录>\n";
            return kExitUsage;
        }
        const std::string dir = operands[0];
        // owner = 目录里**第一个** *.dll（升序 —— 与 `load_plugins_from_dir` 的加载/命名口径同源）
        std::string owner;
        {
            std::error_code ec;
            std::vector<std::string> names;
            for (std::filesystem::directory_iterator it(xrkit::core::path_from_utf8(dir), ec), end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                std::string name = xrkit::core::path_to_utf8(it->path().filename());
                // ★★ S2-2（B 批）：探针与 `load_plugins_from_dir` **共用同一份**命名判据
                //   （修前两处各抄一份大小写敏感的 `.dll` 判定 ⇒ `NLC.DLL` 连探针都找不到）。
                if (xrkit::plugin::is_plugin_dll_name(name)) {
                    names.push_back(name);
                }
            }
            std::sort(names.begin(), names.end());
            if (!names.empty()) owner = names.front();
        }
        if (owner.empty()) {
            std::cout << "PLUGINROLLBACK skip reason=目录里没有 *.dll（夹具没准备）：" << dir << "\n";
            return kExitSkip;
        }
        xrkit::plugin::Registry registry;
        const size_t mark = registry.registration_count();
        const size_t loaded_before = xrkit::plugin::loaded_count();
        const std::vector<xrkit::plugin::RegistrationError> load_errors =
            xrkit::plugin::load_plugins_from_dir(dir, registry, /*ui=*/nullptr);
        const size_t loaded_after = xrkit::plugin::loaded_count();
        const xrkit::plugin::PluginResidue residue = registry.residue_for(owner);
        std::cout << "PLUGINROLLBACK info dir=" << dir << " dll=" << owner << " mark=" << mark
                  << " count=" << registry.registration_count() << " loaded_before=" << loaded_before
                  << " loaded_after=" << loaded_after << " errors=" << load_errors.size() << "\n";
        // 逐项可见：脏在哪个容器一眼看得出（不是只报一句"总数不对"）
        std::cout << "PLUGINROLLBACK residue " << residue.describe() << "\n";
        for (const xrkit::plugin::RegistrationError& e : load_errors) {
            std::cout << "PLUGINROLLBACK load-error plugin=" << e.plugin << " reason=" << e.message
                      << "\n";
        }
        for (const xrkit::plugin::RegistrationError& e : registry.registration_errors()) {
            if (e.plugin == owner) {
                std::cout << "PLUGINROLLBACK error plugin=" << e.plugin << " reason=" << e.message
                          << "\n";
            }
        }
        std::vector<std::string> bad;
        const auto want = [&bad](bool cond, const std::string& what) {
            if (!cond) bad.push_back(what);
        };
        bool reported = false;
        for (const xrkit::plugin::RegistrationError& e : load_errors) {
            if (e.plugin == owner && e.message.find("入口返回 0") != std::string::npos) {
                reported = true;
            }
        }
        want(reported, "宿主没如实报『入口返回 0』（或 owner 名不是 " + owner + "）");
        want(loaded_after == loaded_before, "加载失败却把句柄记进了已加载名单");
        want(residue.clean(), "表里还留着属于它的注册（按 owner 逐容器）");
        want(residue.compare_modes == 0, "内置/别家的比较模式被算成它的残留（口径错了）");
        want(registry.registration_count() == mark, "registration_count() 没回到加载前的 mark");
        want(residue.errors > 0, "失败留痕被一并抹掉了（零静默：理由必须看得见）");
        if (!bad.empty()) {
            std::cout << "PLUGINROLLBACK fail count=" << bad.size() << " why=";
            for (size_t i = 0; i < bad.size(); ++i) std::cout << (i ? "；" : "") << bad[i];
            std::cout << "\n";
            return kExitFail;
        }
        std::cout << "PLUGINROLLBACK ok owner=" << owner << " rolled_and_clean=1\n";
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
    if (action == "--xml-compare-dirs") {
        if (operands.size() < 2) {
            std::cerr << "XMLDIRS error reason=用法：--xml-compare-dirs <A 目录> <B 目录> "
                         "[--mode stats|text] [--enc-a X] [--enc-b Y]\n";
            return kExitUsage;
        }
        xrkit::engine::xml::DirDiffOptions opt;
        opt.dir_a = operands[0];
        opt.dir_b = operands[1];
        for (size_t i = 2; i < operands.size(); ++i) {
            if (operands[i] == "--mode" && i + 1 < operands.size()) {
                opt.mode_key = operands[++i];
            } else if (operands[i] == "--enc-a" && i + 1 < operands.size()) {
                opt.enc_a = operands[++i];
            } else if (operands[i] == "--enc-b" && i + 1 < operands.size()) {
                opt.enc_b = operands[++i];
            } else {
                std::cerr << "XMLDIRS error reason=未知参数 " << operands[i] << "\n";
                return kExitUsage;
            }
        }
        const xrkit::engine::xml::DirDiffReport report =
            xrkit::engine::xml::compare_dirs(opt);
        // 逐行输出（**判据对拍用的就是这些行**：rel / kind / 两侧行数 / id 数 / 差异数）
        for (const xrkit::engine::xml::DirDiffRow& row : report.rows) {
            std::cout << "XMLDIRS row rel=" << row.result.rel << " kind=" << row.kind
                      << " lines_a=" << row.result.lines_a << " lines_b=" << row.result.lines_b
                      << " ids_a=" << row.result.ids_a << " ids_b=" << row.result.ids_b
                      << " count_diff=" << row.result.count_diff.size()
                      << " text_diff=" << row.result.text_diff.size()
                      << " only_a=" << row.result.only_a.size()
                      << " only_b=" << row.result.only_b.size()
                      << " consistent=" << (row.result.consistent ? 1 : 0)
                      << " untrusted="
                      << ((row.result.untrusted_a || row.result.untrusted_b) ? 1 : 0) << "\n";
        }
        std::cout << "XMLDIRS ok total=" << report.total
                  << " rows=" << report.rows.size()
                  << " cancelled=" << (report.cancelled ? 1 : 0) << "\n";
        return kExitOk;
    }

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
        // ★ 2026-10-06（用户红线）：「**不能静默报错，不然出问题了还不知道**」——
        //   封包侧接住 `utf8_to_cp1251` 的返回值（语义＝"这次编码有没有发生顶替"），
        //   把**逐条改写清单 + 计数**打出来。**不阻塞、不加 --strict-names**：包照常产出。
        std::vector<xrkit::engine::fs::DbNameRewrite> rewrites;
        const std::vector<uint8_t> bytes = xrkit::engine::fs::pack_db(items, *fmt, &rewrites);
        // ★★ 2026-10-04（用户红线「GUI 的功能，CLI 肯定要全部拥有」）：输出 operand **可以是目录、
        //   也可以没有扩展名** ⇒ 与 GUI 走同一条引擎规则补落点与扩展名（`xdb→.xdb.db`、`sqfs→.sqfs`）。
        //   给了带扩展名的完整路径 ⇒ 原样不动（不破坏既有用法）。
        const xrkit::engine::fs::PackTarget tgt = xrkit::engine::fs::resolve_pack_target(
            operands[0], xrkit::core::path_to_utf8(root.filename()), operands[1]);
        std::cout << "PACKDB target=" << tgt.path << "\n";
        std::ofstream out(path_from_utf8(tgt.path), std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cout << "PACKDB error reason=无法写输出文件\n";
            return kExitFail;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        std::cout << "PACKDB ok items=" << items.size() << " bytes=" << bytes.size() << "\n";
        // 名字改写台账（**清零静默**）：只在真发生改写时打，逐条给「源名 → 写进包里的名字」。
        // 计数行**恒在**（=0 也要说）——"没打印"与"没发生"必须能区分开。
        if (!rewrites.empty()) {
            std::cout << "PACKDB warning names_rewritten=" << rewrites.size()
                      << " reason=cp1251 表示不了的字符被逐码点顶替成 '?'"
                         "（引擎不认中文名；包照常产出，请核对下列名字）\n";
            for (const xrkit::engine::fs::DbNameRewrite& r : rewrites) {
                std::cout << "PACKDB rewrite src=" << r.source << " -> written=" << r.written
                          << "\n";
            }
        } else {
            std::cout << "PACKDB warning names_rewritten=0\n";
        }
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
        // ★★ 2026-10-04（用户红线「GUI 的功能，CLI 肯定要全部拥有」）：输出 operand **可以是目录、
        //   也可以没有扩展名** ⇒ 与 GUI 走同一条引擎规则（sqfs ⇒ `.sqfs`）。给了带扩展名的完整路径 ⇒ 原样不动。
        const std::filesystem::path root = path_from_utf8(operands[1]);
        const xrkit::engine::fs::PackTarget tgt = xrkit::engine::fs::resolve_pack_target(
            operands[0], xrkit::core::path_to_utf8(root.filename()), "sqfs");
        std::cout << "PACK target=" << tgt.path << "\n";
        const std::string& out_path = tgt.path;
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
        // ★ 补充 G3【S1】（B 批）：守卫必须判**过滤后**的 `args`，不是过滤前的 `operands` ——
        //   `operands = {"--no-full-cyrillic"}` 时 `args` 为空，而下面取 `args[0]` ⇒
        //   空 vector 取下标（实测 exit=0xC0000005 访问违例 ×3/3）。帮助行恰好把两者并列展示，
        //   很容易这么敲；门禁的 `golden_font_charset` 只用正常形态，抓不到。
        if (args.empty()) {
            std::cerr << "CHARSET error reason=用法：--extract-chars <xml目录> [输出文件] "
                         "[--no-full-cyrillic]\n";
            return kExitUsage;
        }
        const std::string& xml_dir = args[0];
        xrkit::engine::font::CharsetResult result;
        if (!xrkit::engine::font::extract_chars(xml_dir, full_cyrillic, result)) {
            std::cout << "CHARSET error reason="
                      << (result.error.empty() ? ("目录不存在：" + xml_dir) : result.error) << "\n";
            return kExitFail;
        }
        // ★★ 2026-10-07（审计补充 A1-4）：**打不开的文件必须计入失败**（原来被当成空文件，
        //   整份语料的字符被静默丢弃、退出码照样 0）。
        if (result.files_unreadable > 0) {
            std::cout << "CHARSET error reason=有 " << result.files_unreadable
                      << " 个 XML 读不出来（被占用/权限不足/路径过长）：";
            for (size_t i = 0; i < result.guessed.size(); ++i) {
                if (i > 0) std::cout << " | ";
                std::cout << result.guessed[i];
            }
            std::cout << "\n";
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
                  << " unreadable=" << result.files_unreadable
                  << " guessed=" << result.guessed.size()
                  << " full_cyrillic=" << (full_cyrillic ? 1 : 0) << "\n";
        return kExitOk;
    }

    // 出包（M1-7 第⑦步）：整棵 `gamedata/` 产物树（10 个字号 + XML 转写 + 两个 ltx）。
    // 判据：与 1.x `build_package` 的产物树**逐字节一致**。
    if (action == "--convert-batch") {
        if (operands.size() < 4) {
            std::cerr << "CONVERTBATCH error reason=用法：--convert-batch <源> <输出目录> "
                         "<源编码> <目标编码> [--ext .xml] [--no-recursive] [--backup] "
                         "[--exclude <模式>]\n";
            return kExitUsage;
        }
        xrkit::engine::text::BatchOptions opt;
        opt.source = operands[0];
        opt.output_dir = operands[1];
        opt.source_encoding = operands[2];
        opt.target_encoding = operands[3];
        for (size_t i = 4; i < operands.size(); ++i) {
            if (operands[i] == "--ext" && i + 1 < operands.size()) {
                opt.extensions.push_back(operands[++i]);
            } else if (operands[i] == "--no-recursive") {
                opt.recursive = false;
            } else if (operands[i] == "--backup") {
                opt.backup = true;
            } else if (operands[i] == "--no-backup") {
                opt.backup = false;
            } else if (operands[i] == "--exclude" && i + 1 < operands.size()) {
                opt.exclude_patterns = operands[++i];
            } else {
                std::cerr << "CONVERTBATCH error reason=未知参数 " << operands[i] << "\n";
                return kExitUsage;
            }
        }
        const xrkit::engine::text::BatchOutcome outcome =
            xrkit::engine::text::convert_batch(opt, {});
        std::cout << "CONVERTBATCH ok single_file=" << (outcome.single_file ? 1 : 0)
                  << " depth=" << outcome.collect.depth << " dirs=" << outcome.collect.dirs
                  << " files_seen=" << outcome.collect.files_seen
                  << " selected=" << outcome.collect.selected
                  << " skipped_ext=" << outcome.collect.skipped_ext
                  << " skipped_excluded=" << outcome.collect.skipped_excluded
                  << " skipped_backup=" << outcome.collect.skipped_backup
                  << " candidates=" << outcome.candidates << " processed=" << outcome.processed
                  << " success=" << outcome.success << " skipped=" << outcome.skipped
                  << " failed=" << outcome.failed << " backed_up=" << outcome.backed_up
                  << " exit=" << outcome.exit_code << "\n";
        return outcome.exit_code == 0 ? kExitOk : 1;
    }

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
        // ★ 2.x 配色断言（2026-09-27 界面重构口径；用户"配色也改一下"）——
        //   不是"看起来还行"，而是**能量、能判红**：正文/次要文字对比度、强调色与背景亮度差、
        //   当前导航项与窗口底必须可区分。改坏任意一条 ⇒ 这里当场判红。
        {
            struct Rgb {
                int r = 0, g = 0, b = 0;
            };
            auto rgb_of = [&](xrkit::core::ThemeMode m, const char* role) {
                Rgb out;
                const char* hex = xrkit::core::theme_color(m, role);
                if (hex == nullptr || std::strlen(hex) != 7) return out;
                auto hv = [](char c) {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 0;
                };
                out.r = hv(hex[1]) * 16 + hv(hex[2]);
                out.g = hv(hex[3]) * 16 + hv(hex[4]);
                out.b = hv(hex[5]) * 16 + hv(hex[6]);
                return out;
            };
            auto rel_lum = [](const Rgb& c) {  // WCAG 相对亮度
                auto ch = [](int v) {
                    const double s = static_cast<double>(v) / 255.0;
                    return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
                };
                return 0.2126 * ch(c.r) + 0.7152 * ch(c.g) + 0.0722 * ch(c.b);
            };
            auto contrast = [&](const Rgb& a, const Rgb& b) {
                const double la = rel_lum(a);
                const double lb = rel_lum(b);
                const double hi = la > lb ? la : lb;
                const double lo = la > lb ? lb : la;
                return (hi + 0.05) / (lo + 0.05);
            };
            auto same = [](const Rgb& a, const Rgb& b) {
                return a.r == b.r && a.g == b.g && a.b == b.b;
            };
            auto brief = [](double v) { return std::to_string(v).substr(0, 4); };
            const struct {
                xrkit::core::ThemeMode mode;
                const char* label;
            } modes[] = {{xrkit::core::ThemeMode::Dark, "dark"},
                         {xrkit::core::ThemeMode::Light, "light"}};
            for (const auto& m : modes) {
                const Rgb card = rgb_of(m.mode, "surface2");
                const double c_text = contrast(rgb_of(m.mode, "text"), card);
                const double c_dim = contrast(rgb_of(m.mode, "text_dim"), card);
                check(c_text >= 4.5, std::string(m.label) + " 正文/卡片对比度>=4.5 实测" + brief(c_text));
                check(c_dim >= 3.0, std::string(m.label) + " 次要文字/卡片对比度>=3.0 实测" + brief(c_dim));
                const Rgb accent = rgb_of(m.mode, "accent");
                const Rgb bg = rgb_of(m.mode, "bg");
                const double delta = std::abs(rel_lum(accent) - rel_lum(bg));
                check(delta >= 0.25,
                      std::string(m.label) + " 强调色与背景亮度差>=25% 实测" + brief(delta));
                check(!same(rgb_of(m.mode, "selected"), bg),
                      std::string(m.label) + " 当前导航项与窗口底不同色");
            }
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
    if (action == "--patch-identify") {
        if (operands.size() < 2) {
            std::cerr << "PATCH error reason=用法：--patch-identify <exe> <站点表文件>\n";
            return kExitUsage;
        }
        std::ifstream site_in(path_from_utf8(operands[1]), std::ios::binary);
        if (!site_in) {
            std::cout << "PATCH error reason=读不到站点表：" << operands[1] << "\n";
            return kExitFail;
        }
        const std::string site_text((std::istreambuf_iterator<char>(site_in)),
                                    std::istreambuf_iterator<char>());
        std::vector<xrkit::engine::patch::PatchSite> sites;
        std::string perr;
        if (!parse_patch_sites(site_text, sites, perr)) {
            std::cout << "PATCH error reason=站点表解析失败：" << perr << "\n";
            return kExitFail;
        }
        const xrkit::engine::patch::PatchIdentifyResult r =
            xrkit::engine::patch::identify_file(operands[0], sites);
        if (!r.error.empty()) {
            std::cout << "PATCH error reason=" << r.error << "\n";
            return kExitFail;
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), " delta=0x%llX", static_cast<unsigned long long>(r.delta));
        std::cout << "PATCH layout parsed=" << (r.layout.parsed ? 1 : 0)
                  << " hard_ok=" << (r.layout.hard_ok ? 1 : 0)
                  << " inside_text=" << (r.layout.targets_inside_text ? 1 : 0) << buf << "\n";
        std::cout << "PATCH layout_note " << r.layout.note << "\n";
        std::cout << "PATCH verdict key=" << r.verdict_key << " text=" << r.verdict_text
                  << " thunk_intact=" << (r.thunk_intact ? 1 : 0)
                  << " cave_all_zero=" << (r.cave_all_zero ? 1 : 0)
                  << " file_size=" << r.file_size
                  << " diff_bytes=" << r.reference_diff_bytes << "\n";
        std::cout << "PATCH count orig=" << r.n_orig << " patched=" << r.n_patched
                  << " kept=" << r.n_kept << " broken=" << r.n_broken
                  << " other=" << r.n_other << " context=" << r.n_context
                  << " range=" << r.n_range << "\n";
        for (const xrkit::engine::patch::PatchItemState& item : r.items) {
            std::snprintf(buf, sizeof(buf), "0x%08llX", static_cast<unsigned long long>(item.offset));
            std::cout << "PATCH item id=" << item.id << " kind=" << item.kind
                      << " offset=" << buf << " state=" << item.state
                      << " expect=\"" << item.expect_hex << "\""
                      << " patched=\"" << item.patched_hex << "\""
                      << " actual=\"" << item.actual_hex << "\"";
            if (item.call_target >= 0) {
                std::snprintf(buf, sizeof(buf), " target=0x%08llX",
                              static_cast<unsigned long long>(item.call_target));
                std::cout << buf;
            }
            if (item.call_target_defect >= 0) {
                std::snprintf(buf, sizeof(buf), " target_defect=0x%08llX",
                              static_cast<unsigned long long>(item.call_target_defect));
                std::cout << buf;
            }
            std::cout << "\n";
            if (!item.context_problem.empty()) {
                std::cout << "PATCH item_note id=" << item.id << " " << item.context_problem
                          << "\n";
            }
        }
        std::cout << "PATCH verdict_detail " << r.detail << "\n";
        return kExitOk;
    }
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
        // ★ 2026-10-06 置信度系统上屏（零静默）：
        //   conf/lowconf/overdecl/statn/statscore = 判定的"不确定那部分"（用户口径③④）
        //   mism = **判定结论 ≠ 实际使用编码**（旧版在这里静默分叉过）
        //   rc：0 = 判定完成且不是低置信；2 = 判定完成但**低置信**（纯猜/无证据）——
        //       退出码如实反映语义，低置信不再与诚实判定同形。
        char conf_buf[16];
        char score_buf[16];
        std::snprintf(conf_buf, sizeof(conf_buf), "%.3f", sr.confidence);
        std::snprintf(score_buf, sizeof(score_buf), "%+.3f", sr.stat_score);
        std::cout << "SNIFF ok step=" << static_cast<int>(sr.step) << " enc=" << sr.encoding
                  << " used=" << db.used_encoding << " forced=" << (db.forced ? 1 : 0)
                  << " chars=" << db.chars << " bytes=" << db.text.size()
                  << " conf=" << conf_buf
                  << " lowconf=" << (sr.low_confidence ? 1 : 0)
                  << " overdecl=" << (sr.override_decl ? 1 : 0)
                  << " statn=" << sr.stat_evidence
                  << " statscore=" << score_buf
                  << " mism=" << (db.used_mismatch ? 1 : 0) << "\n";
        return sr.low_confidence ? kExitSkip : kExitOk;
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
        // ★★ S1-1（零静默）：**台账先印、结论后判** —— 修前 `!st.ok` 直接 return，
        //   于是"哪些文件没写出去"这条台账与计数**一起消失**（只剩一行理由）。
        //   现在失败也照印台账，结论行按实情给 `TEXT ok` / `TEXT fail`。
        for (const std::string& s : st.skipped) {
            std::cout << "TEXT skip " << s << "\n";
        }
        // ★ 本批：**因保护而没替换**的命中逐条可见（零静默）—— 注释里 / 更长标识符内部。
        //   计数同时进 ASCII 收尾行 `replskip=`，供判据脚本直接判（不用去解析中文正文）。
        for (const std::string& s : st.repl_skipped) {
            std::cout << "TEXT repl-skip " << s << "\n";
        }
        std::cout << (st.ok ? "TEXT ok " : "TEXT fail ")
                  << "kind=" << type_dir << " files=" << st.files
                  << " strings=" << st.strings << " chars=" << st.chars
                  << " skipped=" << st.skipped.size()
                  << " replskip=" << st.repl_skipped.size()
                  << " writefail=" << st.write_failures
                  << " xml=" << (st.texts_xml_name.empty() ? "-" : st.texts_xml_name)
                  << " ms=" << static_cast<long long>(st.elapsed_ms) << "\n";
        if (!st.ok) {
            std::cout << "TEXT error reason=" << st.error << "\n";
            return kExitFail;
        }
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
