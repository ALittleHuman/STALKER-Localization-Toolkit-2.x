// 禁用清单的唯一实现（读/写/查询）。格式见 include/xrkit/core/plugin_disabled.h。
#include "xrkit/core/plugin_disabled.h"

#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace xrkit::plugin {
namespace {

namespace fs = std::filesystem;

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim_copy(std::string s) {
    const std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

fs::path list_path(const std::string& dir) {
    return xrkit::core::path_from_utf8(dir) / "plugins.disabled";
}

}  // namespace

std::vector<DisabledPluginEntry> read_disabled_plugins(const std::string& dir) {
    std::vector<DisabledPluginEntry> out;
    std::error_code ec;
    const fs::path path = list_path(dir);
    if (!fs::is_regular_file(path, ec)) return out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        DisabledPluginEntry entry;
        const std::size_t first = line.find('\t');
        const std::size_t second = first == std::string::npos
                                       ? std::string::npos
                                       : line.find('\t', first + 1);
        entry.file = trim_copy(line.substr(0, first));
        if (first != std::string::npos) {
            entry.id = trim_copy(line.substr(
                first + 1, second == std::string::npos ? std::string::npos : second - first - 1));
            if (second != std::string::npos) entry.reason = trim_copy(line.substr(second + 1));
        }
        if (!entry.file.empty()) out.push_back(std::move(entry));
    }
    return out;
}

bool write_disabled_plugins(const std::string& dir,
                            const std::vector<DisabledPluginEntry>& entries,
                            std::string& error) {
    error.clear();
    std::string text;
    text += "# xrkit 插件禁用清单（TAB 分隔：file<TAB>id<TAB>reason）\n";
    text += "# 管理器/用户手改都认；宿主在 import 之前跳过这些文件。\n";
    for (const DisabledPluginEntry& entry : entries) {
        if (entry.file.empty()) continue;
        text += entry.file;
        text += '\t';
        text += entry.id;
        text += '\t';
        text += entry.reason;
        text += '\n';
    }
    std::error_code ec;
    fs::create_directories(list_path(dir).parent_path(), ec);
    const std::vector<uint8_t> data(text.begin(), text.end());
    return xrkit::core::write_file_atomic(xrkit::core::path_to_utf8(list_path(dir)), data, true,
                                          error);
}

bool is_file_disabled(const std::vector<DisabledPluginEntry>& entries,
                      const std::string& file) {
    const std::string want = lower_copy(trim_copy(file));
    for (const DisabledPluginEntry& entry : entries) {
        if (lower_copy(trim_copy(entry.file)) == want) return true;
    }
    return false;
}

}  // namespace xrkit::plugin
