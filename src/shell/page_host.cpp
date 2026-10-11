#include "page_host.h"

#include <algorithm>
#include <set>

namespace xrkit::shell {
namespace {

// 排序关键字：带 → order → 注册序。
struct Slot {
    int band = 0;
    int order = 0;
    size_t seq = 0;
};

bool slot_less(const Slot& a, const Slot& b) {
    if (a.band != b.band) return a.band < b.band;
    if (a.order != b.order) return a.order < b.order;
    return a.seq < b.seq;
}

}  // namespace

void PageRegistry::reject(const std::string& owner, const std::string& message) {
    // 拒绝 = 注册没生效 ⇒ 必须能说得出来（否则表现成"注册了却不生效"，规格 §二 第②道闸）
    diags_.push_back(PageDiag{true, owner, message});
}

void PageRegistry::note(const std::string& owner, const std::string& message) {
    diags_.push_back(PageDiag{false, owner, message});
}

bool PageRegistry::register_page(const std::string& owner, PageKind kind, PageSpec spec,
                                PageBuilder builder) {
    // ★★ P17-88（§四·8② 的**运行期那一半**）：**注册入口对"跨用"当场拒绝** —— 先于其它判据，
    //   因为"这一页是谁的"是**权限**问题（冒充内置页＝静默夺权），不是格式问题。
    //   口径（用户 2026-10-09）：「`owner`/`kind` 是权限边界……注册入口对跨用**当场拒绝**」。
    {
        const bool owner_is_builtin = (owner == kBuiltinPageOwner);
        if (kind == PageKind::Builtin && !owner_is_builtin) {
            reject(owner, "页「" + spec.id + "」自报 kind=Builtin，但 owner 不是内置哨兵「" +
                              kBuiltinPageOwner + "」⇒ 拒绝（不许冒充内置页）");
            return false;
        }
        if (kind == PageKind::Plugin && owner_is_builtin) {
            reject(owner, "页「" + spec.id + "」自报 kind=Plugin，但 owner 是内置哨兵「" +
                              kBuiltinPageOwner + "」⇒ 拒绝（不许把插件页挂到内置身份上）");
            return false;
        }
    }
    if (spec.id.empty()) {
        reject(owner, "页 id 不能为空（它是插件槽位 host / 日志 / 替换的唯一锚点）");
        return false;
    }
    if (spec.title.isEmpty()) {
        reject(owner, "页「" + spec.id + "」的 title 不能为空（标签条上要看得见）");
        return false;
    }
    if (!builder) {
        reject(owner, "页「" + spec.id + "」没有 builder（注册了也造不出控件）");
        return false;
    }
    const PageReg* existing = find(spec.id);
    if (existing != nullptr) {
        reject(owner, "页 id「" + spec.id + "」重复（已由 " + existing->owner + " 注册）");
        return false;
    }
    if (!spec.replaces.empty()) {
        if (spec.replaces == spec.id) {
            reject(owner, "页「" + spec.id + "」不能替换它自己");
            return false;
        }
        // 同一目标只允许一个替换者：多个人抢同一个槽位必须**当场拒绝**，不许"谁后注册谁赢"
        //（那正是"静默顶掉"的形状）。
        for (const PageReg& reg : regs_) {
            if (reg.spec.replaces == spec.replaces) {
                reject(owner, "页「" + spec.replaces + "」已被 " + reg.owner + " 的页「" +
                                  reg.spec.id + "」替换（同一目标只允许一个替换者）");
                return false;
            }
        }
    }

    PageReg reg;
    reg.owner = owner;
    reg.kind = kind;   // ★★ P17-88：身份**随注册一起记下**（判据/宿主读它，不再靠哨兵字符串推断）
    reg.spec = std::move(spec);
    reg.builder = std::move(builder);
    reg.seq = next_seq_++;
    regs_.push_back(std::move(reg));

    if (!regs_.back().spec.replaces.empty()) {
        const std::string target = regs_.back().spec.replaces;
        if (find(target) != nullptr) {
            note(owner, "页「" + target + "」被 " + owner + " 的页「" + regs_.back().spec.id +
                            "」替换（被替换页只隐藏、没删，可一键还原）");
        } else {
            note(owner, owner + " 的页「" + regs_.back().spec.id + "」声明替换「" + target +
                            "」，但那个 id 现在还没注册 ⇒ 暂不生效");
        }
    }
    return true;
}

size_t PageRegistry::unregister_owner(const std::string& owner) {    const size_t before = regs_.size();
    regs_.erase(std::remove_if(regs_.begin(), regs_.end(),
                               [&owner](const PageReg& reg) { return reg.owner == owner; }),
                regs_.end());
    return before - regs_.size();
}

bool PageRegistry::unregister_id(const std::string& id) {
    for (auto it = regs_.begin(); it != regs_.end(); ++it) {
        if (it->spec.id == id) {
            regs_.erase(it);
            return true;
        }
    }
    return false;
}

const PageReg* PageRegistry::find(const std::string& id) const {
    for (const PageReg& reg : regs_) {
        if (reg.spec.id == id) return &reg;
    }
    return nullptr;
}

// ★★ P17-88：这一页自报的身份。**找不到** ⇒ 按 `Plugin` 返回（保守：未知的东西**不许**算内置页）。
PageKind PageRegistry::page_kind(const std::string& id) const {
    const PageReg* reg = find(id);
    return reg == nullptr ? PageKind::Plugin : reg->kind;
}

std::vector<std::string> PageRegistry::owners() const {
    std::vector<std::string> out;
    for (const PageReg& reg : regs_) {
        if (std::find(out.begin(), out.end(), reg.owner) == out.end()) out.push_back(reg.owner);
    }
    return out;
}

std::vector<const PageReg*> PageRegistry::all_sorted() const {
    // 生效槽位：声明了 `replaces` 且目标已注册 ⇒ **继承目标的槽位**（替换＝占位，不是"另起一页"）；
    // 目标还没注册 ⇒ 用自己声明的槽位（注册诊断里已经说过"暂不生效"）。
    // 刻意**只查一层**：替换者替换的如果是另一个替换者，取的是它**声明**的槽位 —— 链式替换
    // 谁占谁的位没有唯一合理答案，写死一层比"猜一个"可控。
    const auto slot_of = [this](const PageReg& reg) -> Slot {
        if (!reg.spec.replaces.empty()) {
            const PageReg* target = find(reg.spec.replaces);
            if (target != nullptr) return Slot{target->spec.band, target->spec.order, target->seq};
        }
        return Slot{reg.spec.band, reg.spec.order, reg.seq};
    };
    std::vector<const PageReg*> out;
    out.reserve(regs_.size());
    for (const PageReg& reg : regs_) out.push_back(&reg);
    std::sort(out.begin(), out.end(), [&slot_of](const PageReg* a, const PageReg* b) {
        return slot_less(slot_of(*a), slot_of(*b));
    });
    return out;
}

std::vector<const PageReg*> PageRegistry::visible_sorted() const {
    std::set<std::string> hidden;
    for (const PageReg& reg : regs_) {
        if (!reg.spec.replaces.empty() && find(reg.spec.replaces) != nullptr) {
            hidden.insert(reg.spec.replaces);
        }
    }
    std::vector<const PageReg*> out;
    for (const PageReg* reg : all_sorted()) {
        if (hidden.count(reg->spec.id) == 0) out.push_back(reg);
    }
    return out;
}

std::vector<const PageReg*> PageRegistry::replaced() const {
    std::set<std::string> hidden;
    for (const PageReg& reg : regs_) {
        if (!reg.spec.replaces.empty() && find(reg.spec.replaces) != nullptr) {
            hidden.insert(reg.spec.replaces);
        }
    }
    std::vector<const PageReg*> out;
    for (const PageReg* reg : all_sorted()) {
        if (hidden.count(reg->spec.id) != 0) out.push_back(reg);
    }
    return out;
}

std::vector<PageDiag> PageRegistry::take_diagnostics() {
    std::vector<PageDiag> out;
    out.swap(diags_);
    return out;
}

}  // namespace xrkit::shell
