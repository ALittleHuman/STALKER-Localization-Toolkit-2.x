#include "xrkit/core/update_transaction.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace xrkit::core {
namespace {

UpdateResult fail(const std::string& what, const std::error_code& ec) {
    return UpdateResult::failure(what + ": " + ec.message());
}

fs::path backup_path(const UpdatePaths& paths) {
    return fs::path(paths.transaction) / "backup";
}

fs::path pending_path(const UpdatePaths& paths) {
    return fs::path(paths.transaction) / "pending";
}

bool inside(const fs::path& child, const fs::path& parent) {
    const fs::path c = fs::weakly_canonical(child);
    const fs::path p = fs::weakly_canonical(parent);
    auto ci = c.begin();
    auto pi = p.begin();
    for (; pi != p.end(); ++pi, ++ci) {
        if (ci == c.end() || *ci != *pi) return false;
    }
    return true;
}

UpdateResult copy_tree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        if (ec) return fail("检查源目录失败", ec);
        return UpdateResult::failure("源目录不存在：" + from.string());
    }
    fs::remove_all(to, ec);
    if (ec) return fail("清理目标目录失败", ec);
    fs::create_directories(to, ec);
    if (ec) return fail("创建目标目录失败", ec);
    fs::copy(from, to,
             fs::copy_options::recursive | fs::copy_options::copy_symlinks |
                 fs::copy_options::overwrite_existing,
             ec);
    if (ec) return fail("复制目录失败", ec);
    return UpdateResult::success();
}

UpdateResult validate(const UpdatePaths& paths) {
    if (paths.target.empty() || paths.staged.empty() || paths.transaction.empty()) {
        return UpdateResult::failure("target/staged/transaction 不能为空");
    }
    if (inside(paths.transaction, paths.target) || inside(paths.target, paths.transaction)) {
        return UpdateResult::failure("transaction 不能与被替换目录互相包含");
    }
    if (inside(paths.staged, paths.target) || inside(paths.target, paths.staged)) {
        return UpdateResult::failure("staged 不能与被替换目录互相包含");
    }
    return UpdateResult::success();
}

}  // namespace

UpdateResult update_prepare(const UpdatePaths& paths) {
    if (UpdateResult v = validate(paths); !v.ok) return v;
    std::error_code ec;
    fs::remove_all(paths.transaction, ec);
    if (ec) return fail("清理事务目录失败", ec);
    fs::create_directories(paths.transaction, ec);
    if (ec) return fail("创建事务目录失败", ec);

    const fs::path backup = backup_path(paths);
    if (fs::exists(paths.target, ec)) {
        if (ec) return fail("检查旧目录失败", ec);
        if (UpdateResult r = copy_tree(paths.target, backup); !r.ok) {
            fs::remove_all(paths.transaction, ec);
            return r;
        }
    } else {
        fs::create_directories(backup, ec);
        if (ec) return fail("创建空备份失败", ec);
    }

    std::ofstream pending(pending_path(paths), std::ios::binary | std::ios::trunc);
    if (!pending) return UpdateResult::failure("无法写 pending 标记");
    pending << paths.target << "\n";
    pending.flush();
    if (!pending) return UpdateResult::failure("写 pending 标记失败");
    return UpdateResult::success();
}

UpdateResult update_replace(const UpdatePaths& paths) {
    if (UpdateResult v = validate(paths); !v.ok) return v;
    std::error_code ec;
    if (!fs::exists(backup_path(paths), ec)) {
        return UpdateResult::failure("没有备份，拒绝替换（先 prepare）");
    }
    fs::remove_all(paths.target, ec);
    if (ec) return fail("删除旧目标失败", ec);
    return copy_tree(paths.staged, paths.target);
}

UpdateResult update_rollback(const UpdatePaths& paths) {
    if (UpdateResult v = validate(paths); !v.ok) return v;
    std::error_code ec;
    if (!fs::exists(backup_path(paths), ec)) {
        return UpdateResult::failure("没有备份，无法回滚");
    }
    fs::remove_all(paths.target, ec);
    if (ec) return fail("回滚时删除目标失败", ec);
    if (UpdateResult r = copy_tree(backup_path(paths), paths.target); !r.ok) return r;
    // 恢复完成：pending 失效，防止下次启动重复回滚；事务目录留给 commit/诊断。
    fs::remove(pending_path(paths), ec);
    return UpdateResult::success();
}

UpdateResult update_commit(const UpdatePaths& paths) {
    if (paths.transaction.empty()) return UpdateResult::failure("transaction 为空");
    std::error_code ec;
    fs::remove_all(paths.transaction, ec);
    if (ec) return fail("删除事务目录失败", ec);
    return UpdateResult::success();
}

UpdateResult update_recover(const UpdatePaths& paths, bool success) {
    if (UpdateResult v = validate(paths); !v.ok) return v;
    std::error_code ec;
    if (!fs::exists(pending_path(paths), ec)) {
        if (ec) return fail("检查 pending 失败", ec);
        return UpdateResult::success();  // 没有未完成事务
    }
    return success ? update_commit(paths) : update_rollback(paths);
}

}  // namespace xrkit::core
