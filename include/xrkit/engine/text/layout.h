#pragma once
// ★ 2026-11-09：STALKER 各作的配置目录名不统一 —— SoC 用 `config`（单数），
//   CoP/ClS/IX-Ray 与多数模组用 `configs`。文本/ORI 链原先只认 `configs`，
//   在 SoC 原版布局上会报"找不到 configs"。这里提供唯一探测口径（configs 优先），
//   调用方不要再自己拼 `"configs"`。
#include <filesystem>
#include <string>

namespace xrkit::engine::text {

// 返回 base 下的配置目录名：`configs`（优先）或 `config`；都没有返回空串。
inline std::string config_dir_name(const std::filesystem::path& base) {
    std::error_code ec;
    if (std::filesystem::is_directory(base / "configs", ec)) return "configs";
    if (std::filesystem::is_directory(base / "config", ec)) return "config";
    return std::string();
}

}  // namespace xrkit::engine::text
