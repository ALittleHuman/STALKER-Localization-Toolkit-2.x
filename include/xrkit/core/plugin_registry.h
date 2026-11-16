// 线上插件注册表（plugins.json）与本地插件列表的合并逻辑。
//
// 为什么单独抽成不依赖 Qt 的头文件：
//   ① 本仓库的全局口径是 core/engine/cli 三者**不许依赖 Qt**（见顶层
//      CMakeLists 的说明），所以这段纯逻辑必须能用 std 写；
//   ② 这段逻辑是"全新用户能不能从界面里装上插件"的关键路径，
//      值得有独立可编译的回归测试，而不是只能靠 GUI 点。
//
// 平台（tools/plugin_manager/main.cpp）读到的注册表字段：
//   id / version / package / sha256 / entry / target
//   - package 不以 http 开头时，平台会拼到
//     https://raw.githubusercontent.com/ALittleHuman/
//     STALKER-Localization-Toolkit-2.x-Network-Plugin/main/ 后面；
//   - entry 是**文件名**（不是路径），解压后在目录树里递归按文件名找；
//     找到后，它所在的整个目录被整体替换到 plugins/<target>/；
//   - target 缺省取 id。
#pragma once

#include <string>
#include <vector>

namespace xrkit::core {

// 注册表里的一条插件条目。
struct RegistryEntry {
    std::string id;
    std::string version;
    std::string package;
    std::string sha256;
    std::string entry;   // 压缩包内的入口文件名（不是路径）
    std::string target;  // 安装目标目录名；为空则取 id
    std::string name;
    std::string note;

    // 与平台既有语义一致：target 缺省取 id。
    std::string effective_target() const { return target.empty() ? id : target; }
};

// 本地已安装插件的最小信息（只用于判断"是否已装"）。
struct LocalPluginRef {
    std::string id;
    std::string version;
};

// 注册表 ∖ 本地：返回"本地还没装、可以安装"的条目（保持注册表顺序）。
inline std::vector<RegistryEntry> registry_missing(
        const std::vector<LocalPluginRef>& local,
        const std::vector<RegistryEntry>& registry) {
    std::vector<RegistryEntry> out;
    for (const RegistryEntry& e : registry) {
        if (e.id.empty()) continue;
        bool installed = false;
        for (const LocalPluginRef& l : local) {
            if (!l.id.empty() && l.id == e.id) {
                installed = true;
                break;
            }
        }
        if (!installed) out.push_back(e);
    }
    return out;
}

// 安装按钮的条目选择：selected_id 为空 ⇒ 取注册表第一条（平台原行为）；
// 否则按 id 精确匹配。命中返回 true 并写出条目。
inline bool registry_pick(const std::vector<RegistryEntry>& registry,
                          const std::string& selected_id,
                          RegistryEntry* out) {
    for (const RegistryEntry& e : registry) {
        if (selected_id.empty() || e.id == selected_id) {
            if (out) *out = e;
            return true;
        }
    }
    return false;
}

}  // namespace xrkit::core
