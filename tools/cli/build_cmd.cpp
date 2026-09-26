#include "build_cmd.h"

#include <cstdlib>
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
        std::cout << "BUILDPKG error reason=构建或打包失败（退出码 " << rc
                  << "）⇒ 版本已回退；日志=" << log << "\n";
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
    store.append(want_version, want_prerelease, want_index, rec);
    if (!xrkit::core::save_version_store(store_path, store, err)) {
        std::cout << "BUILDPKG error reason=写构建史失败：" << err << "\n";
        return kFail;
    }
    std::cout << "BUILDPKG ok version=" << full << " index=" << want_index << " log=" << log << "\n";
    return kOk;
}

}  // namespace xrkit_cli
