#pragma once
// xrkit::core::runtime —— 依赖路由（用户 2026-11-01 口径）。
//
// 目标：**开发期**把 ffmpeg / ffprobe / IXR python / ixr_run / fontlib 路由到
// 一个共享 runtime 根（`[runtime] root`），**正式版**把它写成 `@APP@`（工具目录），
// 这样 10 个构建不必各带一套 180 MB 的 ffmpeg。
//
// 解析优先级（用户钉死，不许加 PATH 兜底）：
//   ① 单项显式路径（`[runtime] ffmpeg=...`）
//   ② `[runtime] root` + 约定子路径（`bin/ffmpeg.exe` …）
//   ③ 工具目录默认（`<app>/bin/ffmpeg.exe`、`<app>/python/python.exe`、`<app>/tools/...`）
//   ④ 都没有 ⇒ 返回空串，由调用方**明确报错**（零静默）。
//
// 记法：`@APP@` = 工具目录；`${root}` = `[runtime] root` 的展开值（可嵌套）。

#include <string>

namespace xrkit::core {

// 展开 `@APP@` 与 `${root}`（`root_value` 是已展开的 root；可空）。
std::string expand_runtime_tokens(const std::string& value, const std::string& app_dir,
                                 const std::string& root_value = std::string());

// 读 `<app_dir>/xrkit_user.ltx` 的 `[runtime] root`（展开后）。读不到 ⇒ 空串。
std::string runtime_root(const std::string& app_dir);

// 依赖来源（给报错/自检用）
enum class RuntimeSource { Missing, Explicit, ExplicitMissing, Root, AppDefault };
const char* runtime_source_name(RuntimeSource source);

struct RuntimeDep {
    std::string path;             // 解析结果（空 = 没找到）
    RuntimeSource source = RuntimeSource::Missing;
};

// 解析一个依赖：
//   key        —— `[runtime]` 里的单项键（如 `ffmpeg`）
//   under_root —— 相对 root 的约定路径（如 `bin/ffmpeg.exe`）
//   under_app  —— 相对工具目录的默认路径（如 `bin/ffmpeg.exe`）
RuntimeDep resolve_runtime_dep(const std::string& app_dir, const std::string& key,
                               const std::string& under_root, const std::string& under_app);

}  // namespace xrkit::core
