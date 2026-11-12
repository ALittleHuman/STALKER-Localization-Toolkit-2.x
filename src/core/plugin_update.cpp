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
        out.error = error;
        return out;
    }
    out.ok = true;
    return out;
}

UpdateResult plugin_update_apply(const PluginUpdatePlan& plan) {
    return update_replace(UpdatePaths{plan.target_dir, plan.staged_dir, plan.transaction});
}

UpdateResult plugin_update_commit(const PluginUpdatePlan& plan) {
    bool was_enabled = false;
    std::string error;
    if (!read_state(plan, &was_enabled, &error)) return UpdateResult::failure(error);
    if (UpdateResult r = restore_enabled(plan, was_enabled); !r.ok) return r;
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
