#pragma once
// xrkit::core —— 文件级更新事务（本体 updater / 插件目录更新共用）。
//
// 一个事务目录（临时目录）：
//   <transaction>/backup/   旧 target 的完整拷贝
//   <transaction>/pending   未提交标记（存在 ⇒ 下次启动需要恢复）
//
// 顺序（调用方负责退出进程/重启）：
//   prepare → replace → 启动新版本 → 健康检查通过 ⇒ commit
//                              └─ 失败/超时 ⇒ rollback
//
// 本模块只搬文件，不 import 插件、不碰 Qt、不判断“健康”。
// 插件启用状态由调用方另存并恢复（见 plugins.disabled）。

#include <string>

namespace xrkit::core {

struct UpdatePaths {
    std::string target;       // 被替换的目录（安装目录或单个插件目录）
    std::string staged;       // 新版本目录（完整内容）
    std::string transaction;  // 临时事务目录（backup/pending 所在）
};

struct UpdateResult {
    bool ok = false;
    std::string error;

    static UpdateResult success() { return {true, {}}; }
    static UpdateResult failure(std::string why) { return {false, std::move(why)}; }
};

// 创建事务目录，把 target 完整复制到 <transaction>/backup，并写 pending。
UpdateResult update_prepare(const UpdatePaths& paths);

// 删除 target，把 staged 完整复制到 target。失败时不要假装成功。
UpdateResult update_replace(const UpdatePaths& paths);

// 用 backup 恢复 target（先删 target，再复制）。成功后保留事务目录，
// 由调用方在确认可启动后 commit；恢复失败则保留现场供诊断。
UpdateResult update_rollback(const UpdatePaths& paths);

// 新版本已通过健康检查：删除 backup/pending/整个事务目录。
UpdateResult update_commit(const UpdatePaths& paths);

// 启动时检查 pending：有就按 success 决定 commit 或 rollback。
// 没有 pending ⇒ 返回 success（幂等）。
UpdateResult update_recover(const UpdatePaths& paths, bool success);

}  // namespace xrkit::core
