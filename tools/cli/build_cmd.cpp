#include "build_cmd.h"

#include <cstdlib>
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

int run_build_package(const std::vector<std::string>& operands) {
    if (operands.size() < 4) {
        std::cerr << "BUILDPKG error reason=用法：--build-package <version.json> <版本> <标记> <序号>\n";
        return kUsage;
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
    const std::string log = "dist\\build_pkg_" + std::to_string(want_index) + ".log";
    std::string cmd;
    cmd += "powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\build.ps1";
    cmd += " -Preset msvc-shell > \"";
    cmd += log;
    cmd += "\" 2>&1 && powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\package.ps1";
    cmd += " -BuildIndex " + std::to_string(want_index);
    cmd += " -VersionString \"" + full + "\" >> \"";
    cmd += log;
    cmd += "\" 2>&1";
    std::cout << "BUILDPKG start version=" << full << " log=" << log << "\n";
    std::cout.flush();

    const int rc = std::system(cmd.c_str());
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
    // `commit=<短 sha>` 写进舞台的 version.txt，这里直接读它 —— 保证与成品里那份**同一个值**，
    // 不再各自算一遍（找不到就退回引擎版本串，绝不写空）。
    rec.commit = xrkit::core::version_string();
    {
        std::ifstream vf("build\\package_stage\\version.txt");
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
        // 与 package.ps1 的命名一致：STALKER_Toolkit_<版本_标记.序号>_<commit>.zip
        // （GUI 的构建史条目点开就是这个文件）
        std::string tag = want_version + "_" + want_prerelease;
        if (want_index > 0) tag += "." + std::to_string(want_index);
        for (char& c : tag) {
            if (c == ' ') c = '_';
        }
        std::string zip_name_hint = "dist\\STALKER_Toolkit_" + tag + "_" + rec.commit + ".zip";
        rec.artifacts.push_back(zip_name_hint);
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
