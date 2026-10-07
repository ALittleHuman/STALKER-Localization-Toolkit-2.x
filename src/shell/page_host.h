#pragma once
// xrkit::shell —— 页宿主注册表（插件改装 P1「Hub 机制层」的第一片）。
//
// 口径出处：`…_插件改装_交接\STALKER工具集_插件架构目标定义(待用户过目)_2026-10-06.md`
//   §一（Hub ＝ 容器 + 注册表 + 槽位装配 + 机制提供者，**不含业务**；官方六页也是"插件注册"）、
//   §四.2（**每类注册点 ↔ 消费点一一对应**：缺一即红）、
//   §五（覆盖用新 id + 显式 `replaces`，**禁止静默顶掉**、失败可回退）。
//
// 修前的事实（本条就是它的锁）：
//   · 六页硬编码在 `hub.cpp` 的 `kBuiltinPages[]` 里，**Hub 直接认识每个具体页类**
//     （`dynamic_cast<FsPage*>` / `dynamic_cast<XmlPage*>`）—— 与"Hub 不含业务"相反；
//   · 插件页走的是**另一条路**（`hub.cpp:render_plugin_slots()` 里的 `plugin_pages_` 列表）：
//     同一件事两份实现，插件的页与内置的页没有共用同一个机制。
//   ⇒ 本文件把"页"从"Hub 里的硬编码"变成**表里的一条注册**：id / title / subtitle /
//     order / replaces + builder；顺序、重复、覆盖、生命周期（注册 / 注销 / 回退 / 共存）
//     全在这张表上做，且**每一次拒绝与覆盖都留痕**（规格 §二 第②道闸「零静默」）。
//
// 边界（为什么放在 shell 而不是 engine）：builder 造的是 Qt 控件；但本类**不认识任何具体页类**，
//   也不碰控件树 —— 它只管"有哪些页、什么顺序、谁注册的"。具体页 → builder 的映射留在
//   `pages/page_factory.cpp` **一处**（内置六页），将来 Python 插件给的是同一形状的 builder。
//   ⇒ Hub 从此不再 include 任何 `pages/*_page.h`（静态锁见 `tests/check_page_host_lock.py`）。

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace xrkit::shell {

class PageBase;

// 内置页的 owner 哨兵（"内置"只许一份：与比较模式的 `<builtin>` 同字面量、同语义）。
inline constexpr const char* kBuiltinPageOwner = "<builtin>";

// ★ 排序"带"（band）：1.x `stalker_toolkit.py:215-221` 的口径是
//   「**内置六工具固定在前**，插件栏目按 order 升序稳定追加」。
//   2.x 把两者放进**同一张表** ⇒ 用带表达同一件事：`order` 只在**本带内**生效。
//   插件页要插到内置页中间，用 `replaces`（占被替换页的槽位）——那正是规格 §五 的显式覆盖。
inline constexpr int kBuiltinPageBand = 0;
inline constexpr int kPluginPageBand = 1000;

struct PageSpec {
    std::string id;   // 稳定标识（插件槽位 host / 日志 / 替换都按它走；空 ⇒ 拒绝注册）
    QString title;    // 界面上的标签名（空 ⇒ 拒绝注册）
    QString subtitle; // 一句说明（可空；六页当前全空，见 hub.cpp 的注释）
    int order = 0;    // 带内升序；同值按**注册顺序**稳定
    // 非空 ⇒ **显式**替换该 id：被替换页只隐藏、不删（回退＝撤掉替换者），
    // 且注册时就留一条可见的"X 被 Y 替换（可一键还原）"（禁止静默顶掉）。
    std::string replaces;
    int band = kPluginPageBand;  // 内置页注册时填 kBuiltinPageBand
};

// 造一页。parent 为空 ⇒ 由调用方（页栈）接管所有权。返回 nullptr 表示造不出来
// （由调用方兜底成占位页并留痕，**不许**变成"注册了却没出现"）。
using PageBuilder = std::function<QWidget*(QWidget* parent)>;

struct PageReg {
    std::string owner;  // 谁注册的（内置 = kBuiltinPageOwner；插件 = 插件名/dll 名）
    PageSpec spec;
    PageBuilder builder;
    size_t seq = 0;     // 注册序（稳定排序的第二关键字；0 起）
};

// 一条诊断（零静默通道）：
//   fatal = true  ⇒ 这次注册**被拒**（理由必须让用户看见，否则表现成"注册了却不生效"）；
//   fatal = false ⇒ 需要让用户看见的事实（例："插件页 X 替换内置页 Y（可一键还原）"）。
struct PageDiag {
    bool fatal = false;
    std::string owner;
    std::string message;
};

class PageRegistry {
public:
    // 注册一页。被拒（id/title 为空、id 重复、同一目标已被别人 `replaces`、自替换）⇒
    // 返回 false 且理由进 `take_diagnostics()`（**绝不静默**）。
    bool register_page(const std::string& owner, PageSpec spec, PageBuilder builder);
    // 按 owner 撤掉它注册的全部页（"插件卸载/重扫"的唯一入口）。返回撤掉的条数。
    size_t unregister_owner(const std::string& owner);
    // 按 id 撤掉一页。返回是否真撤掉（"一键还原被替换页"就是撤掉替换者）。
    bool unregister_id(const std::string& id);

    const PageReg* find(const std::string& id) const;
    bool contains(const std::string& id) const { return find(id) != nullptr; }
    size_t size() const { return regs_.size(); }
    std::vector<std::string> owners() const;  // 注册顺序去重

    // 全部页（含被替换的），按**生效槽位**排序（见 .cpp 的 slot 说明）。
    std::vector<const PageReg*> all_sorted() const;
    // **要显示出来的**页 ＝ 全部 − 被别人的 `replaces` 命中的那些
    //（只隐藏、不删 ⇒ 回退就是撤掉替换者）。
    std::vector<const PageReg*> visible_sorted() const;
    std::vector<const PageReg*> replaced() const;  // 被顶掉的（逐条可见，供界面/日志报出）

    // 零静默通道：取走并清空（Hub 负责写进日志栏/状态栏）。
    std::vector<PageDiag> take_diagnostics();

private:
    void reject(const std::string& owner, const std::string& message);
    void note(const std::string& owner, const std::string& message);

    std::vector<PageReg> regs_;
    std::vector<PageDiag> diags_;
    size_t next_seq_ = 0;
};

}  // namespace xrkit::shell
