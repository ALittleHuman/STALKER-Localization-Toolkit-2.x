#include "xrkit/engine/font/system_dirs.h"

#include <cstdlib>
#include <string>

namespace xrkit::engine::font {
namespace {

// 环境变量（Windows 上大小写不敏感，但 `std::getenv` 只认传进去的那个写法 ⇒ 两种都试）
std::string env_value(const char* name, const char* fallback_name = nullptr) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
        return std::string(value);
    }
    if (fallback_name != nullptr) {
        if (const char* value = std::getenv(fallback_name); value != nullptr && *value != '\0') {
            return std::string(value);
        }
    }
    return std::string();
}

std::string join(const std::string& base, const char* leaf) {
    if (base.empty()) return std::string();
    if (base.back() == '/' || base.back() == '\\') return base + leaf;
    return base + "\\" + leaf;
}

}  // namespace

std::vector<std::string> system_font_dirs() {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    std::string win = env_value("SystemRoot", "windir");
    if (win.empty()) win = "C:\\Windows";   // 最后兜底（环境变量被清掉时）
    dirs.push_back(join(win, "Fonts"));
#elif defined(__APPLE__)
    dirs.push_back("/System/Library/Fonts");
    dirs.push_back("/Library/Fonts");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back(std::string(home) + "/Library/Fonts");
    }
#else
    dirs.push_back("/usr/share/fonts");
    dirs.push_back("/usr/local/share/fonts");
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        dirs.push_back(std::string(home) + "/.fonts");
    }
#endif
    return dirs;
}

}  // namespace xrkit::engine::font
