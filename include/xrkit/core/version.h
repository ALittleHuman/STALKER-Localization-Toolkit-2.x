#pragma once
// xrkit::core —— 版本信息。
//
// 与 1.x 的契约对应：版本号 + 预发布标记 + 构建计数都集中在**一处**，
// 由构建脚本写回（对照 1.x 的 toolkit_base.py 单点持有 + 原子写回）。

#include <string>

namespace xrkit::core {

inline constexpr int kVersionMajor = 2;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// 预发布标记："" = 正式版；非空形如 ALPHA.3 / BETA.1 / RC.2
inline constexpr const char* kPrerelease = "ALPHA.121";

// 构建计数：由 build 脚本自增并写回本行（M0 尚为 0）
inline constexpr int kBuildCount = 121;

// "2.0.0 ALPHA.0"（无预发布时为 "2.0.0"）
std::string version_string();

// "2.0.0" + 构建号："2.0.0+0"
std::string version_with_build();

// 契约自检用：版本串必须是 "主.次.补[ 标记]" 形态
bool version_string_is_well_formed(const std::string& s);

}  // namespace xrkit::core
