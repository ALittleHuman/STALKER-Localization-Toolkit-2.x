#include "build_cmd.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>
#include <iterator>
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
    store.current_version = want_version;
    store.current_prerelease = want_prerelease;
    store.current_index = want_index;
    if (!xrkit::core::save_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=写回失败：" << err << "\n";
        return kFail;
    }
    const std::string full = store.full_version();

    // ── ★★ 2026-10-07（审计补充 H1-2，用户拍板"候选 A"）：版本号的**派生写回** ──────────
    //   真来源 = `version.json` 的 tree（最后一次成功发包）；`version.h` 是它的派生。
    //   写在这里的原因：exe 的资源（`assets/xrkit_winres.rc` ← `gen_winres.py` ← `version.h`）
    //   与 `core::version_string()` **都编自 version.h** ⇒ 必须在 `build.ps1` **之前**写好，
    //   否则编出来的包还会说 `ALPHA.0`（修前正是如此：包说 ALPHA.60、exe 说 ALPHA.0）。
    //   纪律：**原子写**（`save_version_header` 内部走 `write_file_atomic`）+ 失败**回滚**，
    //   并且把"要写的字节"分成两步（先算后写），任一步失败都回到失败前 ⇒ **序号不被吃掉**。
    //   注：这是**发版**路径；`version.h` 与 `version.json`（以及 .rc）因此变脏，
    //       按用户口径"**只有发版提交把这几份一起带上**"。
    const std::string header_path = "include/xrkit/core/version.h";
    const std::string rc_path = "assets/xrkit_winres.rc";
    const std::string want_prerelease_full =
        want_prerelease.empty() ? std::string() : want_prerelease + "." + std::to_string(want_index);
    // 留原字节（回滚用）：读不到原文件 ⇒ 明确失败（不去猜）
    std::string header_before;
    std::string rc_before;
    {
        std::ifstream header_in(header_path, std::ios::binary);
        std::ifstream rc_in(rc_path, std::ios::binary);
        if (!header_in || !rc_in) {
            std::cout << "BUILDPKG error reason=读不到 " << header_path << " 或 " << rc_path
                      << "（写回与回滚都需要它们）\n";
            return kFail;
        }
        header_before.assign(std::istreambuf_iterator<char>(header_in),
                             std::istreambuf_iterator<char>());
        rc_before.assign(std::istreambuf_iterator<char>(rc_in), std::istreambuf_iterator<char>());
    }
    if (!xrkit::core::save_version_header(header_path, want_prerelease_full, want_index, err)) {
        std::cout << "BUILDPKG error reason=写回 version.h 失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG wrote-header prerelease=" << want_prerelease_full
              << " build=" << want_index << "\n";
    // `.rc` 必须与 version.h 同源（否则门禁的 `gen_winres.py --check` 会红）⇒ 立刻重生成。
    {
        const int gen_rc =
            run_windowless_utf8("chcp 65001 >nul && python tools\\gen_winres.py >nul 2>&1");
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
    int rc = run_windowless_utf8(cmd);  // 无控制台窗口（用户要求：不要打开 cmd）
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
        }
    }

    if (rc != 0) {
        // ③ 失败：版本/标记/序号**逐字节回退**（判据就是"与备份逐字节相同"）
        if (!xrkit::core::restore_version_store(backup, store_path, err)) {
            std::cout << "BUILDPKG error reason=回退失败：" << err << "\n";
            return kFail;
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
        return kFail;
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
            return kFail;
        }
        std::ifstream in(final_txt);
        if (!in) {
            std::cout << "BUILDPKG error reason=读不到 " << final_txt.string() << "（写 exe_sha256 需要它）\n";
            return kFail;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string key = "exe_sha256=";
        const std::string line = key + landed_sha + "\n";
        const size_t at = text.find(key);
        if (at != std::string::npos) {
            const size_t eol = text.find('\n', at);
            text.replace(at, (eol == std::string::npos ? text.size() : eol + 1) - at, line);
        } else {
            if (!text.empty() && text.back() != '\n') text.push_back('\n');
            text += line;
        }
        std::string werr;
        if (!xrkit::core::write_file_atomic(final_txt.string(),
                                            std::vector<uint8_t>(text.begin(), text.end()), true, werr)) {
            std::cout << "BUILDPKG error reason=写 " << final_txt.string() << " 失败：" << werr << "\n";
            return kFail;
        }
        std::ifstream check_stream(final_txt);
        std::string check((std::istreambuf_iterator<char>(check_stream)),
                          std::istreambuf_iterator<char>());
        if (check.find(line) == std::string::npos) {
            std::cout << "BUILDPKG error reason=写回 exe_sha256 后读不到（原子写失败？）\n";
            return kFail;
        }
        std::cout << "BUILDPKG wrote exe_sha256=" << landed_sha << " into "
                  << final_txt.string() << "\n";
    }

    store.append(want_version, want_prerelease, want_index, rec);
    if (!xrkit::core::save_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=写构建史失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG ok version=" << full << " index=" << want_index << " log=" << log << "\n";
    return kOk;
}

}  // namespace xrkit_cli
