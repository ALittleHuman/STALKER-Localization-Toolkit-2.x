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

}  // namespace xrkit::core
