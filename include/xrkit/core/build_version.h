#pragma once
// xrkit::core —— 构建版本与构建史（**唯一事实来源**：`version.json`）。
//
// 用户 2026-09-27 口径（v2）：
//   · 版本 / 标记 / 计数都写进 `version.json`，**别处不许复制版本串**；
//   · `version.json` 用**树的结构**记录构建的历史版本：tree[版本][标记][序号] = {built, commit, artifacts}；
//   · **"下一个构建的序号" = 该版本 + 该标记下现有最大序号 + 1**；
//   · 失败 ⇒ 用备份**逐字节回退**（所以本模块同时提供备份/还原）；
//   · buildGUI **只能下拉选**版本与标记，序号只读自动填，并显示完整版本号。
//
// 本模块 Qt-free（`core` 层铁律），CLI 与 GUI 都调它 —— **只有一份实现**。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xrkit::core {

// 单次构建的记录
struct BuildRecord {
    std::string built;                    // "2026-09-27 14:05:33"
    std::string commit;                   // 短 sha（构建时取）
    std::vector<std::string> artifacts;   // 产物路径（zip / 成品目录）
};

// version.json 的内存形态
struct VersionStore {
    std::string current_version;      // 界面上选中的版本号（如 "2.0.0"）
    std::string current_prerelease;   // 选中的标记（ALPHA / BETA / RC）
    int current_index = 0;            // **下一个构建的序号**（只读显示的就是它）
    std::map<std::string, std::map<std::string, std::map<int, BuildRecord>>> tree;

    // 顶层版本号（字典序，稳定）
    std::vector<std::string> versions() const;
    // 某版本下已有的标记（加入"标准三档" ALPHA/BETA/RC，保序去重）
    std::vector<std::string> prereleases(const std::string& version) const;
    // 下一个构建的序号 = max(已有序号) + 1；该版本+标记下还没构建过 ⇒ 1
    int next_index(const std::string& version, const std::string& prerelease) const;
    // 追加一条构建记录（成功后调用）
    void append(const std::string& version, const std::string& prerelease, int index,
                const BuildRecord& record);
    // 完整版本号：如 "2.0.0 BETA.4"（序号为 0 时只显示 "2.0.0 BETA"）
    std::string full_version() const;
};

// 读 / 写 / 备份 / 还原（路径用 UTF-8；内部走 native 宽串，避免 ACP 丢字符）
bool load_version_store(const std::string& path, VersionStore& out, std::string& error);
bool save_version_store(const std::string& path, const VersionStore& store, std::string& error);
bool backup_version_store(const std::string& path, const std::string& backup_path,
                          std::string& error);
bool restore_version_store(const std::string& backup_path, const std::string& path,
                           std::string& error);

// 备份文件路径的约定：<version.json>.bak（GUI 与 CLI 共用同一条约定，别各写一份）
std::string version_backup_path(const std::string& path);

// ── ★★ 2026-10-07（审计补充 H1-2；用户拍板"候选 A"）版本号只许一份真来源 ────────────
//
// 事实（修前）：`version.json` 与 `include/xrkit/core/version.h` 是**两个**"唯一事实来源"，
// 且**没有任何检查比对**它们：exe 的资源与 `core::version_string()` 编自 version.h 的常量
// （`kPrerelease = "ALPHA.0"`、`kBuildCount = 0`，**从来没被写回**过），而包目录名 /
// `version.txt` 来自 version.json ⇒ 实测同一个交付物里"包说 `ALPHA.60`、包内 exe 说
// `ALPHA.0`"，而门禁那两条 `WINRES` 判据全绿（只在各自源内自比）。
//
// 用户 2026-10-07 裁决：
//   · **真来源 = `version.json` 的 tree**（最后一次成功发包：版本+标记+序号）；
//     `version.h` 是它的**派生**（kPrerelease = "<标记>.<序号>"、kBuildCount = 序号）；
//   · 写回时机 = `--build-package` 写 `version.json` 之后、调 `build.ps1` **之前**
//     （exe 的资源与 `core::version_string()` 都编自 version.h ⇒ 必须先写）；
//   · **原子写**（走 `write_file_atomic`，同目录 .part → rename；半写的 version.h
//     会编出坏版本串，同族审计项 2-5）；
//   · 失败路径必须**回滚**（调用方先留原字节）⇒ **序号不被吃掉**；
//   · 判据只对 **包内 exe** + `version.h`/`.rc` 断言，**不**对 build 目录的 exe 断言
//     （开发构建用缺省值合法）；期望值**从 tree 现算**，**不读 `current.index`**
//     （它是"下一个号"，门禁与构建 GUI 都会推它）。

// 把 `<标记>.<序号>` / 序号写回 `path`（= `include/xrkit/core/version.h`）。
// 只改 `kPrerelease` 与 `kBuildCount` 两行，其余字节原样；写盘前后各自校验一次。
// `prerelease` 传**完整串**（形如 "ALPHA.60"；正式版传空串）。失败 ⇒ false + error。
bool save_version_header(const std::string& path, const std::string& prerelease, int build_count,
                         std::string& error);

// 读回 `kPrerelease` / `kBuildCount` 的当前值（回滚断言 / 判据探针 / 幂等检查用）。
bool load_version_header(const std::string& path, std::string& prerelease, int& build_count,
                         std::string& error);

// 从 store 的 tree 里取"**最后一次成功发包**"的 (version, prerelease, index)。
// 空 tree ⇒ false（还没发过包）。★ 故意**不**用 `current_index`。
bool last_release(const VersionStore& store, std::string& version, std::string& prerelease,
                  int& index, std::string& error);

// `last_release()` 的拼串口径（= `version.txt` 的 `version=` 行 / exe 的 ProductVersion）：
// 有标记 ⇒ "<version> <标记>.<序号>"；无标记 ⇒ "<version>"。
std::string release_version_string(const std::string& version, const std::string& prerelease,
                                  int index);

}  // namespace xrkit::core
