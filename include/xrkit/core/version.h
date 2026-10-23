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
inline constexpr const char* kPrerelease = "ALPHA.253";

// 构建计数：**由打包器自增并就地改写下面那一行**（`build_version.cpp` 的 `kBuildCountLine`）。
//   ★ 2026-10-10（P17-36，审计 04 F-12）：原注释写的是**当时那个数字**（它早就过期了），
//     后来一路涨到三位数却没跟着改 —— 这类「写死当前值」的注释必然过期，所以这里改成**不写数字**。
//   ★ 另：`version.json` 的 `tree.2.0.0.ALPHA` 里**缺序号 17 与 107** 属**历史断号**（不是缺陷），
//     别去「补号」—— 补了会让包目录名与既有发布记录对不上。
inline constexpr int kBuildCount = 253;

// "2.0.0 ALPHA.0"（无预发布时为 "2.0.0"）
std::string version_string();

// "2.0.0" + 构建号："2.0.0+0"
std::string version_with_build();

// 契约自检用：版本串必须是 "主.次.补[ 标记]" 形态
bool version_string_is_well_formed(const std::string& s);

}  // namespace xrkit::core
