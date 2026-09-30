#include "build_cmd.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iostream>

#include "xrkit/core/build_version.h"
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
    cmd += "\" 2>&1 && powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\package.ps1";
    cmd += " -BuildIndex " + std::to_string(want_index);
    cmd += " -VersionString \"" + full + "\" >> \"";
    cmd += log;
    cmd += "\" 2>&1";
    std::cout << "BUILDPKG start version=" << full << " log=" << log << "\n";
    std::cout.flush();

#ifdef _WIN32
    const int rc = run_windowless_utf8(cmd);  // 无控制台窗口（用户要求：不要打开 cmd）
#else
    // 本项目只出 Windows 包（scripts/*.ps1 都是 PowerShell）⇒ 非 Windows 不做等价实现，
    // 直接失败。（保留 std::system 会让"无控制台窗口"静态锁永远红 —— 它就是拦这个的。）
    const int rc = -1;
#endif
    if (rc != 0) {
        // ③ 失败：版本/标记/序号**逐字节回退**（判据就是"与备份逐字节相同"）
        if (!xrkit::core::restore_version_store(backup, store_path, err)) {
            std::cout << "BUILDPKG error reason=回退失败：" << err << "\n";
            return kFail;
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
    store.append(want_version, want_prerelease, want_index, rec);
    if (!xrkit::core::save_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=写构建史失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG ok version=" << full << " index=" << want_index << " log=" << log << "\n";
    return kOk;
}

}  // namespace xrkit_cli
