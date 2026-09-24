#include "xrkit/core/version.h"

namespace xrkit::core {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string semver() {
    return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." +
           std::to_string(kVersionPatch);
}

}  // namespace

std::string version_string() {
    std::string s = semver();
    if (kPrerelease != nullptr && kPrerelease[0] != '\0') {
        s += " ";
        s += kPrerelease;
    }
    return s;
}

std::string version_with_build() { return semver() + "+" + std::to_string(kBuildCount); }

bool version_string_is_well_formed(const std::string& s) {
    // 形态：主.次.补[ 标记]，三段都是非空数字串
    size_t i = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t start = i;
        while (i < s.size() && is_digit(s[i])) ++i;
        if (i == start) return false;  // 空数字段
        if (part < 2) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    if (i == s.size()) return true;   // 正式版
    if (s[i] != ' ') return false;    // 标记前必须是空格
    return i + 1 < s.size();          // 标记不能为空
}

}  // namespace xrkit::core
