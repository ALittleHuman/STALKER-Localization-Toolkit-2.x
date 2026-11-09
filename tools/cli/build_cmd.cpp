#include "build_cmd.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

#include "xrkit/core/build_version.h"
#include "xrkit/core/sha256.h"
#include "xrkit/core/atomic_write.h"
#include "xrkit/core/version.h"

namespace xrkit_cli {
namespace {

constexpr int kOk = 0;
constexpr int kFail = 1;
constexpr int kUsage = 3;

}  // namespace


namespace {

// 仓库根：从**本 exe 所在目录**向上找带 `scripts\build.ps1` 的那一层。
// ★ 为什么必须这么做（用户实测反馈 2026-09-27）：GUI 双击启动时**工作目录是 exe 自己那层**
//   （部署副本在 `tools\build_gui\`），而 `scripts\build.ps1` 在仓库根 ⇒ 按相对路径调
//   会直接"命令找不到"，报成"构建失败且没有产出任何文件"。
std::filesystem::path repo_root() {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path dir = (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf).parent_path()
                                                        : std::filesystem::current_path(ec);
#else
    std::filesystem::path dir = std::filesystem::current_path(ec);
#endif
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (std::filesystem::exists(dir / "scripts" / "build.ps1", ec)) return dir;
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return std::filesystem::current_path(ec);  // 兜底：维持现状（就当是老行为）
}

}  // namespace

#ifdef _WIN32
namespace {

// ★ 用户 2026-09-27：「不要打开cmd」—— `std::system()` 会走 `cmd.exe /c`，
//   在 GUI（WIN32 子系统）里会**闪一个黑窗口**。这里改成用 `CreateProcessW`
//   + **`CREATE_NO_WINDOW`** 起同一个命令行：功能不变，但不创建控制台窗口。
int run_windowless_utf8(const std::string& command) {
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) return -1;
    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, wide.data(), wide_len);

    std::wstring line = L"cmd.exe /c ";
    line += wide;  // CreateProcessW 需要一个可写的命令行缓冲

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi) == FALSE) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

}  // namespace
#endif

// ★★ 2026-10-23（BETA.2 实测）：**打包链不许假设 `python` 能用**。
//   事实：本机 PATH 上的 `python` 是 WindowsApps 的**占位别名**（未安装 Store 版），
//   执行直接返回 9009（命令未找到）⇒ `gen_winres.py` 跑不了 ⇒ 打包失败。
//   口径：优先级 `XRKIT_PYTHON`（显式）→ `py -3`（Windows 启动器）→ `python`；
//   每个候选都**真跑一次** `-c "import sys"` 验活，不是"看文件名存在"。
static std::string python_prefix() {
    static std::string cached;
    if (!cached.empty()) return cached;
    const char* env = std::getenv("XRKIT_PYTHON");
    std::vector<std::string> candidates;
    if (env != nullptr && env[0] != '\0') candidates.emplace_back(env);
    candidates.emplace_back("py -3");
    candidates.emplace_back("python");
    for (const std::string& cand : candidates) {
        const std::string probe = cand + " -c \"import sys\" >nul 2>&1";
        if (run_windowless_utf8(probe) == 0) {
            cached = cand;
            return cached;
        }
    }
    cached = "python";   // 都不行：照旧报错，让上游的失败信息里出现真实退码
    return cached;
}


int run_write_version(const std::vector<std::string>& operands) {
    if (operands.size() < 3) {
        std::cerr << "BUILDPKG error reason=用法：--write-version <版本> <标记> <序号>\n";
        return kUsage;
    }
    const std::string pre = operands[1];
    const int idx = std::atoi(operands[2].c_str());
    // version.h 的 kPrerelease 只要 "<标记>.<序号>"
    const std::string full = pre.empty() ? std::to_string(idx) : pre + "." + std::to_string(idx);
    std::string err;
    if (!xrkit::core::save_version_header("include/xrkit/core/version.h", full, idx, err)) {
        std::cerr << "BUILDPKG error reason=写 version.h 失败：" << err << "\n";
        return kFail;
    }
    // ★ 用无窗口启动器（`std::system` 会弹控制台，静态锁也不许）
    const std::string cmd = "chcp 65001 >nul && " + python_prefix() +
                            " tools\\gen_winres.py >nul 2>&1";
    const int gen_rc = run_windowless_utf8(cmd);
    if (gen_rc != 0) {
        std::cerr << "BUILDPKG error reason=重生成 .rc 失败（gen_winres.py）\n";
        return kFail;
    }
    std::cout << "BUILDPKG wrote-version " << full << " build=" << idx << "\n";
    return kOk;
}

int run_build_package(const std::vector<std::string>& operands) {
    if (operands.size() < 4) {
        std::cerr << "BUILDPKG error reason=用法：--build-package <version.json> <版本> <标记> <序号>\n";
        return kUsage;
    }
    // ★ 先把工作目录切到**仓库根**：GUI 从部署目录双击启动时 CWD 不是仓库根，
    //   否则 `scripts\build.ps1` 找不到、`dist\` 也会落到别处（用户实测的失败就是这个）。
    {
        std::error_code root_ec;
        const std::filesystem::path root = repo_root();
        std::filesystem::current_path(root, root_ec);
        if (root_ec) {
            std::cout << "BUILDPKG error reason=切不到仓库根：" << root.string() << "\n";
            return kFail;
        }
    }
    const std::string store_path = operands[0];
    const std::string want_version = operands[1];
    const std::string want_prerelease = operands[2];
    const int want_index = std::atoi(operands[3].c_str());
    // ★ 2026-11-06：`--prebuilt` —— 版本号与两档产物**已由驱动脚本准备好**，
    //   本动作只做"算 sha + 打包 + 收尾"，不再自己编译（CLI 不能在打包过程中重编自己）。
    bool prebuilt = false;
    for (std::size_t i = 4; i < operands.size(); ++i) {
        if (operands[i] == "--prebuilt") {
            prebuilt = true;
        } else {
            std::cerr << "BUILDPKG error reason=未知参数 " << operands[i] << "\n";
            return kUsage;
        }
    }

    xrkit::core::VersionStore store;
    std::string err;
    if (!xrkit::core::load_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=" << err << "\n";
        return kFail;
    }

    // ① 备份 + 存版本（"每次开始构建时自动存，并保留上一次的备份"）
    const std::string backup = xrkit::core::version_backup_path(store_path);
    if (!xrkit::core::backup_version_store(store_path, backup, err)) {
        std::cout << "BUILDPKG error reason=备份失败：" << err << "\n";
        return kFail;
    }
    // ★★ P-806（2026-11-02）：**current 不再在这里落盘**。
    //   修前"取消/被杀"会留下 `current=75` 但产物没落地 ⇒ 包选择回退到旧包、门禁红，
    //   只能手工 git checkout 三件套复原（有副作用）。改成"产物落地、校验通过之后"再写 current
    //   （见函数末尾 `store.append(...)` 之前）；中途失败/取消 ⇒ version.json 从未前进。
    const std::string full = want_version + (want_prerelease.empty() ? "" : " " + want_prerelease) +
                             "." + std::to_string(want_index);

    // ── ★★ 2026-10-07（审计补充 H1-2，用户拍板"候选 A"）：版本号的**派生写回** ──────────
    //   真来源 = `version.json` 的 tree（最后一次成功发包）；`version.h` 是它的派生。
    //   写在这里的原因：exe 的资源（`assets/xrkit_winres.rc` ← `gen_winres.py` ← `version.h`）
    //   与 `core::version_string()` **都编自 version.h** ⇒ 必须在 `build.ps1` **之前**写好，
    //   否则编出来的包还会说 `ALPHA.0`（修前正是如此：包说 ALPHA.60、exe 说 ALPHA.0）。
    //   纪律：**原子写**（`save_version_header` 内部走 `write_file_atomic`）+ 失败**回滚**，
    //   并且把"要写的字节"分成两步（先算后写），任一步失败都回到失败前 ⇒ **序号不被吃掉**。
    //   注：这是**发版**路径；`version.h` 与 `version.json`（以及 .rc）因此变脏，
    //       按用户口径"**只有发版提交把这几份一起带上**"。
    // ★ 2026-11-04（审计）：同号重打守卫 + **半成品可识别可重打**。
    //   事故形态：打包被强杀（P-806 的 Stop-Job）⇒ 清理代码没机会跑 ⇒ 目录留下。
    //   · 意图文件 `.pending-<序号>` **先写**（放在父目录）⇒ 强杀在"建目录之前/之后"都认得出；
    //   · 目录里的 `INCOMPLETE` 覆盖"目录已建、marker 未写"的窗（双保险）；
    //   · 有标记 ⇒ 半成品 ⇒ 挪到 FAIL/stale_<序号>_<pid>_<时间戳>/ 留现场后继续；
    //   · 没标记且"长得像真包"（version.txt + exe）⇒ 拒绝重打；不像 ⇒ 也按半成品处理。
    const std::string pkg_dir_rel = (std::filesystem::path("dist") / want_version / want_prerelease /
                                     std::to_string(want_index)).generic_string();
    const std::string marker_rel = pkg_dir_rel + "/INCOMPLETE";
    const std::string pending_rel = (std::filesystem::path("dist") / want_version / want_prerelease /
                                     (".pending-" + std::to_string(want_index))).generic_string();
    {
        namespace stdfs = std::filesystem;
        std::error_code ec;
        const auto stamp_now = [] {
            char stamp[48] = {};
            std::time_t now = std::time(nullptr);
            std::tm tm_buf{};
            localtime_s(&tm_buf, &now);
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_buf);
            return std::string(stamp);
        };
        const bool has_pending = stdfs::exists(pending_rel, ec);
        const bool has_marker = stdfs::exists(marker_rel, ec);
        const bool looks_real = stdfs::exists(pkg_dir_rel + "/version.txt", ec) &&
                                stdfs::exists(pkg_dir_rel + "/xrkit_app.exe", ec);
        const bool dir_exists = stdfs::exists(pkg_dir_rel, ec);
        if (dir_exists && !has_marker && !has_pending && looks_real) {
            std::cout << "BUILDPKG error reason=序号已存在，拒绝重打：" << pkg_dir_rel
                      << "（有 version.txt+exe 且无 INCOMPLETE/.pending ⇒ 视为已发布的包）\n";
            return kFail;
        }
        if (dir_exists && (has_marker || has_pending || !looks_real)) {
            const stdfs::path stale = stdfs::path("dist") / "FAIL" /
                                      ("stale_" + std::to_string(want_index) + "_" +
                                       std::to_string(GetCurrentProcessId()) + "_" + stamp_now());
            stdfs::create_directories(stale.parent_path(), ec);
            stdfs::rename(pkg_dir_rel, stale, ec);
            if (ec) {
                std::cout << "BUILDPKG error reason=半成品挪不动（" << ec.message()
                          << "）：" << pkg_dir_rel << " —— 请手动删除后重打\n";
                return kFail;
            }
            std::cout << "BUILDPKG stale-moved " << stale.generic_string()
                      << "（识别到半成品标记，已留现场）\n";
        }
        // 意图文件：**先写它**，再建目录（强杀也不会堵序号）
        stdfs::create_directories(stdfs::path(pending_rel).parent_path(), ec);
        {
            std::ofstream pending(pending_rel, std::ios::binary);
            pending << "pending build " << want_index << "\n";
        }
        stdfs::create_directories(pkg_dir_rel, ec);
        std::ofstream marker(marker_rel, std::ios::binary);
        marker << "INCOMPLETE build " << want_index << "\n";
    }
    // FAIL/ 是"证据"，但证据会涨到吃满盘 ⇒ 超上限只提示，不自动删
    {
        namespace stdfs = std::filesystem;
        std::error_code ec;
        const stdfs::path fail_root = stdfs::path("dist") / "FAIL";
        std::size_t count = 0;
        unsigned long long total = 0;
        if (stdfs::is_directory(fail_root, ec)) {
            for (const auto& e : stdfs::directory_iterator(fail_root, ec)) {
                if (!e.is_directory(ec)) continue;
                ++count;
                for (const auto& sub : stdfs::recursive_directory_iterator(e.path(), ec)) {
                    if (sub.is_regular_file(ec)) total += sub.file_size(ec);
                }
            }
        }
        if (count > 5 || total > 500ull * 1024 * 1024) {
            std::cout << "BUILDPKG note FAIL 归档已有 " << count << " 项 / "
                      << (total / (1024 * 1024)) << " MB，请手动清理：dist/FAIL/\n";
        }
    }

    // 留原字节（回滚用）：读不到原文件 ⇒ 明确失败（不去猜）
    std::string header_before;
    std::string rc_before;
    {
        std::ifstream header_in("include/xrkit/core/version.h", std::ios::binary);
        std::ifstream rc_in("assets/xrkit_winres.rc", std::ios::binary);
        if (!header_in || !rc_in) {
            std::cout << "BUILDPKG error reason=读不到 version.h 或 .rc（写回与回滚都需要它们）\n";
            return kFail;
        }
        header_before.assign(std::istreambuf_iterator<char>(header_in),
                             std::istreambuf_iterator<char>());
        rc_before.assign(std::istreambuf_iterator<char>(rc_in), std::istreambuf_iterator<char>());
    }
    const std::string header_path = "include/xrkit/core/version.h";
    const std::string rc_path = "assets/xrkit_winres.rc";
    const std::string want_prerelease_full =
        want_prerelease.empty() ? std::string() : want_prerelease + "." + std::to_string(want_index);
    if (!xrkit::core::save_version_header(header_path, want_prerelease_full, want_index, err)) {
        std::cout << "BUILDPKG error reason=写回 version.h 失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG wrote-header prerelease=" << want_prerelease_full
              << " build=" << want_index << "\n";
    // `.rc` 必须与 version.h 同源（否则门禁的 `gen_winres.py --check` 会红）⇒ 立刻重生成。
    {
        const int gen_rc =
            run_windowless_utf8("chcp 65001 >nul && " + python_prefix() + " tools\\gen_winres.py >nul 2>&1");
        if (gen_rc != 0) {
            // 回滚（version.h + .rc）后失败：不许留一个"版本串一半新一半旧"的树
            std::string rollback_err;
            const bool h_ok = xrkit::core::write_file_atomic(
                header_path, std::vector<uint8_t>(header_before.begin(), header_before.end()), true,
                rollback_err);
            const bool rc_ok = xrkit::core::write_file_atomic(
                rc_path, std::vector<uint8_t>(rc_before.begin(), rc_before.end()), true,
                rollback_err);
            std::cout << "BUILDPKG error reason=重生成 " << rc_path << " 失败（退出码 " << gen_rc
                      << "）—— 已回滚 version.h/.rc（" << (h_ok ? "h=ok" : "h=fail")
                      << " " << (rc_ok ? "rc=ok" : "rc=fail") << "）\n";
            return kFail;
        }
    }

    // ② 真编译 + 打包（日志落 dist\；GUI 侧只是调本动作，不自己拼参数）
    // 构建日志不再散在 dist 根：统一进 dist\logs\（dist 根只留版本树 + FAIL）
    // ★ 用户 2026-09-29：「为什么 dist 里面还是有 build_pkg_13？」⇒ 日志不再放 dist，
    //   统一进 **build\logs\**（构建中间区）；dist 只留版本树 + FAIL。
    const std::string log = "build\\logs\\build_pkg_" + std::to_string(want_index) + ".log";
    // 日志目录先建出来（重定向到不存在的目录会直接失败）
    {
        std::error_code mk_ec;
        std::filesystem::create_directories("build\\logs", mk_ec);
    }
    std::string cmd;
    // 先把控制台代码页切 UTF-8（`chcp 65001`）⇒ 子进程 PowerShell 写进日志的中文不再乱码
    cmd += "chcp 65001 >nul && powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\build.ps1";
    cmd += " -Preset msvc-shell > \"";
    cmd += log;
    cmd += "\" 2>&1";
    std::cout << "BUILDPKG start version=" << full << " log=" << log << "\n";
    std::cout.flush();

#ifdef _WIN32
    int rc = 0;
    if (!prebuilt) {
        rc = run_windowless_utf8(cmd);  // 无控制台窗口（用户要求：不要打开 cmd）
    } else {
        std::cout << "BUILDPKG prebuilt 跳过编译（产物已由驱动脚本准备好）\n";
    }
    // （旧警告已由 --write-version + --prebuilt 驱动取代；这里不再猜测版本。）
    //   原因：`--build-package` 是 **CLI 自己**在跑；先 bump 版本号、再重编 CLI 会撞上
    //   "正在运行的 exe 被自己占用"（实测 LNK1104）。⇒ 这里不做自重编，只把事实说出来：
    //   包内 CLI 是上一次 `build.ps1 -Preset msvc-release` 的产物（版本可能比包小 1）。
    //   要同版：先 `scripts\build.ps1 -Preset msvc-release`，再打包。
    std::cout << "BUILDPKG warn 包内 CLI 版本可能落后一版（它无法在打包过程中重编自己；"
                 "要同版请先 build msvc-release）\n";
#else
    // 本项目只出 Windows 包（scripts/*.ps1 都是 PowerShell）⇒ 非 Windows 不做等价实现，
    // 直接失败。（保留 std::system 会让"无控制台窗口"静态锁永远红 —— 它就是拦这个的。）
    const int rc = -1;
#endif
    // ── ★★ 2026-10-23（Q2）：**包内 exe 的 sha256** 是硬不变量 ─────────────────
    //   口径：`--build-package` 是 `exe_sha256=` 的**唯一写入者**；`package.ps1` 只**核对**
    //   （它拿到这里算好的期望值，对**舞台里的 exe** 现算一遍，不一致就 throw）。
    //   为什么在 build 之后、package 之前算：此时 exe 刚编出来，正是要打进包的那一份。
    std::string pkg_sha;
    if (rc == 0) {
        std::string sha_error;
        pkg_sha = xrkit::core::sha256_file("build\\msvc-shell\\src\\shell\\xrkit_app.exe", sha_error);
        if (pkg_sha.empty()) {
            std::cout << "BUILDPKG error reason=算包内 exe 的 sha256 失败：" << sha_error << "\n";
            rc = 1;   // 走统一失败分支（回滚三件套 + 归档现场）
        } else {
            std::cout << "BUILDPKG exe-sha256=" << pkg_sha << "\n";
            std::cout.flush();
            std::string cmd2;
            cmd2 += "chcp 65001 >nul && powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\package.ps1";
            cmd2 += " -BuildIndex " + std::to_string(want_index);
            cmd2 += " -VersionString \"" + full + "\"";
            cmd2 += " -ExeSha256 \"" + pkg_sha + "\" >> \"";
            cmd2 += log;
            cmd2 += "\" 2>&1";
            rc = run_windowless_utf8(cmd2);
            if (rc == 0) {
                std::error_code mark_ec;
                std::filesystem::remove(marker_rel, mark_ec);
                std::error_code pend_ec;
                std::filesystem::remove(pending_rel, pend_ec);
                if (mark_ec || pend_ec) {
                    // ★ 2026-11-04（HA）：包是好的但标记删不掉 ⇒ 下次会被当半成品挪走 ⇒ 必须判红
                    std::cout << "BUILDPKG error reason=删不掉标记（INCOMPLETE/.pending）："
                              << marker_rel << " / " << pending_rel << "\n";
                    rc = 1;
                }
            }
        }
    }

fail_archive:;
    if (rc != 0) {
        // ③ 失败：版本/标记/序号**逐字节回退**（判据就是"与备份逐字节相同"）
        if (!xrkit::core::restore_version_store(backup, store_path, err)) {
            std::cout << "BUILDPKG error reason=回退失败：" << err << "\n";
            rc = 1;
            // ★★ 2026-10-31（注入审计 P-770）：原来这里 `goto fail_archive` ⇒ 回到失败块
            //   再试一次回退，成了**自循环**（7 小时空转那条教训）。不再 goto：
            //   记 rc=1 后继续往下走（header/.rc 回滚 + 失败归档），由失败块统一 return。
        }
        // ★★ 2026-10-07（H1-2）：`version.h` / `.rc` 也一起回滚 —— 否则"这次发包失败"
        //   会把发版序号留在源码里（下次门禁看到 version.h=N 而 tree 还是 N-1 ⇒ 红），
        //   也就是用户说的"先自增再干活把序号吃掉"。回滚按**原字节**写回并逐字节校验。
        {
            std::string rollback_err;
            const bool h_ok = xrkit::core::write_file_atomic(
                header_path, std::vector<uint8_t>(header_before.begin(), header_before.end()), true,
                rollback_err);
            const bool rc_ok = xrkit::core::write_file_atomic(
                rc_path, std::vector<uint8_t>(rc_before.begin(), rc_before.end()), true,
                rollback_err);
            std::string got_prerelease;
            int got_index = -1;
            std::string read_err;
            const bool match = xrkit::core::load_version_header(header_path, got_prerelease,
                                                               got_index, read_err);
            std::cout << "BUILDPKG rollback-header "
                      << (h_ok && rc_ok && match ? "ok" : "FAIL")
                      << " prerelease=" << got_prerelease << " build=" << got_index << "\n";
        }
        // ★ 2026-11-04（审计）：失败**不删证据**，挪到 FAIL/ 留现场；
        //   挪不动就大声说"半成品仍在，请手动删"（下一轮会被 version-consistency 判据抓到）。
        {
            namespace stdfs = std::filesystem;
            std::error_code ec;
            if (stdfs::exists(pkg_dir_rel, ec)) {
                char stamp[32] = {};
                std::time_t now = std::time(nullptr);
                std::tm tm_buf{};
                localtime_s(&tm_buf, &now);
                std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H_%M_%S", &tm_buf);
                const stdfs::path dest = stdfs::path("dist") / "FAIL" /
                                         ("build_" + std::to_string(want_index) + "_" + stamp);
                stdfs::create_directories(dest.parent_path(), ec);
                stdfs::rename(pkg_dir_rel, dest, ec);
                if (ec) {
                    std::cout << "BUILDPKG cleanup FAIL " << pkg_dir_rel
                              << " —— 半成品仍在（" << ec.message() << "），请手动删除"
                              << "；下轮 version-consistency 会判红\n";
                } else {
                    std::cout << "BUILDPKG cleanup ok moved-to=" << dest.generic_string() << "\n";
                }
            }
        }
        // ④ 失败流（用户 2026-09-24 口述 spec）：失败件进 dist\FAIL\、**加构建时间戳重命名**；
        //    **没产出任何文件 ⇒ 不建目录**（日志直接放 FAIL\ 下）；日志名与文件夹名一致。
        namespace stdfs = std::filesystem;
        std::error_code ec;
        char stamp[32] = {};
        {
            std::time_t now = std::time(nullptr);
            std::tm tm_buf{};
            localtime_s(&tm_buf, &now);
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H_%M_%S", &tm_buf);
        }
        const std::string base = "build_" + std::to_string(want_index) + "_" + stamp;
        const stdfs::path fail_root = stdfs::path("dist") / "FAIL";
        stdfs::create_directories(fail_root, ec);
        // 舞台目录里有东西 ⇒ 说明构建出了产物但打包或自检失败 ⇒ 整个移进 FAIL 下该次目录
        const stdfs::path stage = stdfs::path("build") / "package_stage";
        const stdfs::path log_path2 = stdfs::path(log);
        bool produced = false;
        if (stdfs::is_directory(stage, ec)) {
            for (stdfs::directory_iterator it(stage, ec), end; it != end; it.increment(ec)) {
                produced = true;
                break;
            }
        }
        if (produced) {
            const stdfs::path dir = fail_root / base;
            stdfs::create_directories(dir, ec);
            if (stdfs::exists(log_path2, ec)) {
                stdfs::rename(log_path2, dir / (base + ".log"), ec);
            }
            const stdfs::path moved = dir / "package_stage";
            stdfs::rename(stage, moved, ec);
            std::cout << "BUILDPKG error reason=构建或打包失败（退出码 " << rc
                      << "）⇒ 版本已回退；失败件已归档：" << (fail_root / base).string() << "\n";
        } else {
            // 什么都没产出 ⇒ **不建目录**（spec：没有 build 出任何文件就把那个构建文件夹删掉）
            const stdfs::path only_log = fail_root / (base + ".log");
            if (stdfs::exists(log_path2, ec)) {
                stdfs::rename(log_path2, only_log, ec);
            }
            std::cout << "BUILDPKG error reason=构建失败且没有产出任何文件（退出码 " << rc
                      << "）⇒ 版本已回退、未留构建目录；日志=" << only_log.string() << "\n";
        }
        return kFail;   // 终点：失败块自己**永远返回**，不许再 goto（否则会自循环）
    }

    // ④ 成功：追加进构建史（树结构）
    xrkit::core::BuildRecord rec;
    {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &now);
        char when[32] = {};
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_buf);
        rec.built = when;
    }
    // 构建史里的 `commit` 要的是**源提交**（不是引擎版本串）。package.ps1 已经把
    // `commit=<短 sha>` 写进产物的 version.txt，这里直接读它 —— 保证与成品里那份**同一个值**，
    // 不再各自算一遍（找不到就退回引擎版本串，绝不写空）。
    // ★★ 2026-09-30 修（真缺陷，构建史 13~16 的 `commit` 全是引擎版本串 `2.0.0 ALPHA.0`）：
    //   package.ps1 **结尾会把舞台 `Move-Item` 到** `<dist>\<主版本>\<标记>\<序号>\`
    //   ⇒ 旧写法读 `build\package_stage\version.txt` 时那个目录**已经不存在了** ⇒ 永远读不到。
    //   改成读**成品目录里那一份**（同一条落盘规则、同一个文件 ⇒ 与成品逐字一致，正是本意）。
    rec.commit = xrkit::core::version_string();
    {
        const std::filesystem::path version_txt = std::filesystem::path("dist") / want_version /
                                                  want_prerelease / std::to_string(want_index) /
                                                  "version.txt";
        std::ifstream vf(version_txt);
        std::string line;
        while (std::getline(vf, line)) {
            const std::string key = "commit=";
            if (line.rfind(key, 0) == 0) {
                std::string value = line.substr(key.size());
                while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
                    value.pop_back();
                }
                if (!value.empty()) rec.commit = value;
                break;
            }
        }
    }
    rec.artifacts.push_back(log);
    {
        // ★ 2026-09-28 用户口径：「构建出来不要使用压缩包的形式…压缩包方便分发但不方便测试。
        //   构建成功的不要加时间戳」⇒ 主交付是**一个能直接跑的目录**（成功件名确定性、
        //   不带时间戳），压缩包只是顺带给分发；版本层级＝主版本 → 标记。
        //   与 package.ps1 落盘规则一致：dist\<主版本>\<标记>\STALKER_Toolkit_<主版本>_<标记>_<commit>（不带时间戳）
        // 与 package.ps1 的落盘规则一致：dist\<主版本>\<标记>\<标记>.<序号>\ （只要目录）
        const std::string dir_hint = "dist\\" + want_version + "\\" + want_prerelease + "\\" +
                                     std::to_string(want_index);   // 只留序号（上层已是 主版本/标记）
        rec.artifacts.push_back(dir_hint);   // 可直接双击跑的目录（历史版本按序号共存）
    }
    // ── ★★ 2026-10-23（Q2 的"唯一写入者"这一步）：把 `exe_sha256=` 写进**最终落点**的 version.txt。
    //   时机：包已经落到 `dist\\<主版本>\\<标记>\\<序号>\\` 之后 ⇒ 算的是**包内那份**而不是舞台那份。
    //   纪律：原子写；写后**读回比对**；再对包内 exe 现算一遍，三者必须一致。
    {
        const std::filesystem::path final_dir = std::filesystem::path("dist") / want_version /
                                                want_prerelease / std::to_string(want_index);
        const std::filesystem::path final_exe = final_dir / "xrkit_app.exe";
        const std::filesystem::path final_txt = final_dir / "version.txt";
        std::string sha_error;
        const std::string landed_sha = xrkit::core::sha256_file(final_exe.string(), sha_error);
        if (landed_sha.empty() || landed_sha != pkg_sha) {
            std::cout << "BUILDPKG error reason=包内 exe 的 sha256 与构建产物不一致（" 
                      << (landed_sha.empty() ? sha_error : landed_sha) << " vs " << pkg_sha
                      << "）⇒ 包不可信\n";
            rc = 1;
            goto fail_archive;
        }
        // ★★ 2026-10-24（BETA.2 实测后的修订）：**不再改写 version.txt**。
        //   事实：`package.ps1` 刚写的 `version.txt` 会被杀软短暂占用，
        //   "写 .part → rename 覆盖"这一步连试 5 次都 `Access is denied`，
        //   于是打包明明成功（`PACKAGE OK` 已打）却在这里失败并回滚。
        //   口径不变、实现更稳：**值仍由本函数算**（唯一真值），
        //   `package.ps1` 把它写进 `version.txt`（并已核对过舞台 exe），
        //   本函数在落盘后**只读**核对三件事：
        //     ① 包内 exe 的 sha256 == 本函数算出的期望值；
        //     ② `version.txt` 的 `exe_sha256=` == 同一个值；
        //     ③ 两者都在**最终落点**上（不是舞台）。
        std::ifstream in(final_txt);
        if (!in) {
            rc = 1;
            goto fail_archive;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "exe_sha256=";
        const size_t at = text.find(key);
        if (at == std::string::npos) {
            std::cout << "BUILDPKG error reason=包内 version.txt 没有 exe_sha256=（package.ps1 没写？）\n";
            rc = 1;
            goto fail_archive;
        }
        const size_t eol = text.find_first_of("\r\n", at);
        const std::string got = text.substr(at + key.size(),
                                            (eol == std::string::npos ? text.size() : eol) -
                                                (at + key.size()));
        if (got != landed_sha) {
            std::cout << "BUILDPKG error reason=version.txt 的 exe_sha256=「" << got
                      << "」≠ 包内 exe 的 sha256「" << landed_sha << "」\n";
            rc = 1;
            goto fail_archive;
        }
    }

    store.append(want_version, want_prerelease, want_index, rec);
    // ★ P-806：current 与 tree 在同一次写里前进（产物已落地并校验过），
    //   中途取消/失败 ⇒ 这里根本走不到 ⇒ current 不会指向不存在的包。
    store.current_version = want_version;
    store.current_prerelease = want_prerelease;
    store.current_index = want_index;
    if (!xrkit::core::save_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=写构建史失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG ok version=" << full << " index=" << want_index << " log=" << log << "\n";
    return kOk;
}

}  // namespace xrkit_cli
