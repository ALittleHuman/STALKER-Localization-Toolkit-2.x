#pragma once
// xrkit::core —— 插件目录级更新（多文件）＋ plugins.disabled 状态事务。
//
// 一次更新：
//   begin    记下原启用状态 → 若原启用则写 disabled（下次启动不加载旧版）
//            → 备份到临时事务目录 → 写 pending
//   apply    替换整个插件目录
//   commit   新版本加载成功后：恢复原启用状态 → 删除备份/事务
//   rollback 启动失败：恢复旧目录 + 恢复原启用状态 → 删除事务
//
// 不做进程内 unload：调用方必须重启本体；本模块只负责文件与禁用状态。
// 网络/下载由调用方（联网插件）负责，本模块不联网。

#include <string>

#include "xrkit/core/update_transaction.h"

namespace xrkit::core {

struct PluginUpdatePlan {
    std::string plugins_dir;   // 插件根目录
    std::string target_dir;    // 被替换的插件目录（多文件）
    std::string staged_dir;    // 新版本完整目录
    std::string transaction;   // 临时事务目录
    std::string disabled_key;  // plugins.disabled 里的文件键
};

struct PluginUpdateState {
    bool ok = false;
    bool was_enabled = false;
    std::string error;
};

// 记录原状态、自动禁用（如原启用）、prepare 文件事务。
PluginUpdateState plugin_update_begin(const PluginUpdatePlan& plan);

// 替换插件目录；失败不自动回滚（调用方应调用 rollback）。
UpdateResult plugin_update_apply(const PluginUpdatePlan& plan);

// 新版本加载成功：恢复原启用状态（原启用 ⇒ 删除 disabled 条目），提交事务。
UpdateResult plugin_update_commit(const PluginUpdatePlan& plan);

// 失败：恢复旧目录 + 原启用状态，并清理事务。
UpdateResult plugin_update_rollback(const PluginUpdatePlan& plan);

// 启动时恢复：有 pending 时按 success 提交/回滚；没有则成功。
UpdateResult plugin_update_recover(const PluginUpdatePlan& plan, bool success);

}  // namespace xrkit::core
