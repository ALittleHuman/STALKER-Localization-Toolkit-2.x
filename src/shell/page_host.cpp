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

bool PageRegistry::register_page(const std::string& owner, PageSpec spec, PageBuilder builder) {
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

size_t PageRegistry::unregister_owner(const std::string& owner) {
    const size_t before = regs_.size();
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
