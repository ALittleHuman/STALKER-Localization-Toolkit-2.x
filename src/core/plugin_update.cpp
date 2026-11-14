#include "xrkit/core/plugin_update.h"

#include "xrkit/core/plugin_disabled.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace xrkit::core {
namespace {

fs::path state_path(const PluginUpdatePlan& plan) {
    return fs::path(plan.transaction) / "plugin_state";
}

UpdateResult fail(const std::string& what, const std::error_code& ec) {
    return UpdateResult::failure(what + ": " + ec.message());
}

bool write_state(const PluginUpdatePlan& plan, bool was_enabled, std::string* error) {
    std::ofstream out(state_path(plan), std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "无法写 plugin_state";
        return false;
    }
    out << "was_enabled=" << (was_enabled ? 1 : 0) << "\n";
    out << "key=" << plan.disabled_key << "\n";
    out.flush();
    if (!out) {
        if (error != nullptr) *error = "写 plugin_state 失败";
        return false;
    }
    return true;
}

bool read_state(const PluginUpdatePlan& plan, bool* was_enabled, std::string* error) {
    std::ifstream in(state_path(plan), std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "缺少 plugin_state，拒绝自动恢复";
        return false;
    }
    std::string line;
    bool got = false;
    while (std::getline(in, line)) {
        if (line.rfind("was_enabled=", 0) == 0) {
            *was_enabled = line.substr(12) == "1";
            got = true;
        }
    }
    if (!got) {
        if (error != nullptr) *error = "plugin_state 缺少 was_enabled";
        return false;
    }
    return true;
}

UpdateResult restore_enabled(const PluginUpdatePlan& plan, bool was_enabled) {
    std::vector<plugin::DisabledPluginEntry> entries =
        plugin::read_disabled_plugins(plan.plugins_dir);
    const bool currently_disabled = plugin::is_file_disabled(entries, plan.disabled_key);
    if (was_enabled && currently_disabled) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&plan](const auto& e) {
                                         return plugin::is_file_disabled({e}, plan.disabled_key);
                                     }),
                      entries.end());
    } else if (!was_enabled && !currently_disabled) {
        plugin::DisabledPluginEntry entry;
        entry.file = plan.disabled_key;
        entry.reason = "更新回滚：恢复原禁用状态";
        entries.push_back(std::move(entry));
    } else {
        return UpdateResult::success();  // 已经是目标状态
    }
    std::string error;
    if (!plugin::write_disabled_plugins(plan.plugins_dir, entries, error)) {
        return UpdateResult::failure("恢复插件启用状态失败：" + error);
    }
    return UpdateResult::success();
}

}  // namespace

PluginUpdateState plugin_update_begin(const PluginUpdatePlan& plan) {
    PluginUpdateState out;
    if (plan.plugins_dir.empty() || plan.target_dir.empty() || plan.staged_dir.empty() ||
        plan.transaction.empty() || plan.disabled_key.empty()) {
        out.error = "plugins_dir/target_dir/staged_dir/transaction/disabled_key 不能为空";
        return out;
    }

    std::vector<plugin::DisabledPluginEntry> entries =
        plugin::read_disabled_plugins(plan.plugins_dir);
    out.was_enabled = !plugin::is_file_disabled(entries, plan.disabled_key);
    if (out.was_enabled) {
        plugin::DisabledPluginEntry entry;
        entry.file = plan.disabled_key;
        entry.reason = "更新中（自动禁用；成功后恢复原状态）";
        entries.push_back(std::move(entry));
        std::string error;
        if (!plugin::write_disabled_plugins(plan.plugins_dir, entries, error)) {
            out.error = "自动禁用失败：" + error;
            return out;
        }
    }

    const UpdateResult prepared = update_prepare(
        UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
    if (!prepared.ok) {
        // 回滚刚写入的禁用状态（只有原本启用时才写过）。
        std::vector<plugin::DisabledPluginEntry> now =
            plugin::read_disabled_plugins(plan.plugins_dir);
        if (out.was_enabled) {
            now.erase(std::remove_if(now.begin(), now.end(),
                                     [&plan](const auto& e) {
                                         return plugin::is_file_disabled({e}, plan.disabled_key);
                                     }),
                      now.end());
            std::string ignored;
            plugin::write_disabled_plugins(plan.plugins_dir, now, ignored);
        }
        out.error = prepared.error;
        return out;
    }

    std::string error;
    if (!write_state(plan, out.was_enabled, &error)) {
        update_commit(UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
        // F-002：写状态失败也要把“自动禁用”撤掉，否则插件永远不再加载。
        if (out.was_enabled) {
            std::vector<plugin::DisabledPluginEntry> now =
                plugin::read_disabled_plugins(plan.plugins_dir);
            now.erase(std::remove_if(now.begin(), now.end(),
                                     [&plan](const auto& e) {
                                         return plugin::is_file_disabled({e}, plan.disabled_key);
                                     }),
                      now.end());
            std::string ignored;
            if (!plugin::write_disabled_plugins(plan.plugins_dir, now, ignored)) {
                error += "；恢复启用状态也失败：" + ignored;
            }
        }
        out.error = error;
        return out;
    }
    out.ok = true;
    return out;
}

UpdateResult plugin_update_apply(const PluginUpdatePlan& plan) {
    return update_replace(UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
}

namespace {

fs::path marker_path(const std::string& plugins_dir) {
    return fs::path(plugins_dir) / ".xrkit_update";
}

UpdateResult write_marker(const PluginUpdatePlan& plan, bool was_enabled) {
    std::ofstream out(marker_path(plan.plugins_dir), std::ios::binary | std::ios::app);
    if (!out) return UpdateResult::failure("无法写 .xrkit_update");
    out << plan.disabled_key << "\t" << (was_enabled ? 1 : 0) << "\t"
        << plan.transaction << "\t" << plan.target_dir << "\n";
    out.flush();
    if (!out) return UpdateResult::failure("写 .xrkit_update 失败");
    return UpdateResult::success();
}

UpdateResult remove_marker(const std::string& plugins_dir, const std::string& key) {
    std::ifstream in(marker_path(plugins_dir), std::ios::binary);
    if (!in) return UpdateResult::success();
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key + "\t", 0) != 0) lines.push_back(line);
    }
    in.close();
    std::error_code ec;
    fs::remove(marker_path(plugins_dir), ec);   // 空/失败都重建
    if (lines.empty()) return UpdateResult::success();
    std::ofstream out(marker_path(plugins_dir), std::ios::binary | std::ios::trunc);
    if (!out) return UpdateResult::failure("重写 .xrkit_update 失败");
    for (const std::string& kept : lines) out << kept << "\n";
    out.flush();
    if (!out) return UpdateResult::failure("重写 .xrkit_update 失败");
    return UpdateResult::success();
}

}  // namespace

std::vector<PendingPluginUpdate> pending_plugin_updates(const std::string& plugins_dir) {
    std::vector<PendingPluginUpdate> out;
    std::ifstream in(marker_path(plugins_dir), std::ios::binary);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
        PendingPluginUpdate item;
        item.key = line.substr(0, a);
        item.was_enabled = line.substr(a + 1, b - a - 1) == "1";
        item.transaction = line.substr(b + 1, c - b - 1);
        item.target = line.substr(c + 1);
        if (!item.key.empty() && !item.transaction.empty() && !item.target.empty()) {
            out.push_back(std::move(item));
        }
    }
    return out;
}

UpdateResult plugin_update_activate(const PluginUpdatePlan& plan) {
    bool was_enabled = false;
    std::string error;
    if (!read_state(plan, &was_enabled, &error)) return UpdateResult::failure(error);
    if (UpdateResult r = restore_enabled(plan, was_enabled); !r.ok) return r;
    return write_marker(plan, was_enabled);
}

UpdateResult plugin_update_commit(const PluginUpdatePlan& plan) {
    bool was_enabled = false;
    std::string error;
    if (!read_state(plan, &was_enabled, &error)) return UpdateResult::failure(error);
    if (UpdateResult r = restore_enabled(plan, was_enabled); !r.ok) return r;
    if (UpdateResult r = remove_marker(plan.plugins_dir, plan.disabled_key); !r.ok) return r;
    return update_commit(UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
}

UpdateResult plugin_update_rollback(const PluginUpdatePlan& plan) {
    bool was_enabled = false;
    std::string error;
    if (!read_state(plan, &was_enabled, &error)) return UpdateResult::failure(error);
    if (UpdateResult r = update_rollback(
            UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
        !r.ok) {
        return r;
    }
    if (UpdateResult r = restore_enabled(plan, was_enabled); !r.ok) return r;
    if (UpdateResult r = remove_marker(plan.plugins_dir, plan.disabled_key); !r.ok) return r;
    return update_commit(UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
}

UpdateResult plugin_update_recover(const PluginUpdatePlan& plan, bool success) {
    if (plan.transaction.empty() ||
        !fs::exists(fs::path(plan.transaction) / "pending")) {
        return UpdateResult::success();
    }
    return success ? plugin_update_commit(plan) : plugin_update_rollback(plan);
}

}  // namespace xrkit::core
