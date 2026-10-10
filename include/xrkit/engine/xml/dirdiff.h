#pragma once
// xrkit::engine::xml —— **目录对比**（结果表那一整块业务）。
//
// 2026-10-08（P11b）从 `src/shell/pages/xml_page.cpp` 下沉到这里。为什么必须下沉：
//   · 这套规则（递归列 `*.xml` → 按相对路径配对 → 逐文件读文本 → 按模式比 → 逐行给 kind）
//     是**产品口径**，不是界面；
//   · 插件版的 XML 页（`plugins/xml_page.py`）和 CLI 参照都要它 —— 留在页面里，
//     插件就只能"再写一遍差不多的"，那正是本仓门禁反复拦下的形态。
// 逐条保真搬过来（含 2026-10-06 的三条审计修正）：
//   · **每个文件之前**轮询 `should_stop` ⇒ 取消时 rows 里都是"比完了的完整结果"；
//   · 读不出文本 ⇒ 记一条 `untrusted_*` + `warning`（**绝不静默当成一致**）；
//   · 插件注册的 `compare` **真的被调用**（内置模式的回调留空 ⇒ 走内置两支 text/stats）。

#include "xrkit/engine/xml/compare.h"

#include <functional>
#include <string>
#include <vector>

namespace xrkit::engine::xml {

// 结果表的一行：`kind` 与页面/CSV 用的是**同一套字符串**
//   `same` / `diff` / `only_a` / `only_b`
struct DirDiffRow {
    XmlCompareResult result;
    std::string kind;
};

struct DirDiffOptions {
    std::string dir_a;
    std::string dir_b;
    std::string enc_a = "auto";
    std::string enc_b = "auto";
    std::string mode_key = "stats";   // "stats" / "text" / 插件模式的内部键
    // 插件注册模式的回调（内置模式留空 ⇒ 引擎按 `mode_key` 走内置两支）
    std::function<XmlCompareResult(const std::string& rel, const std::string& text_a,
                                   const std::string& text_b, const std::string& enc_a,
                                   const std::string& enc_b)> compare;
    std::function<bool()> should_stop;   // 每个文件之前轮询
};

struct DirDiffReport {
    std::vector<DirDiffRow> rows;
    int total = 0;            // 配对后的文件数（含取消时没比到的）
    bool cancelled = false;
    // ★★ 2026-10-10（P17-10，审计 02 F-05 Medium）：**遍历出错必须带出来**。
    //   事实（修前）：`list_xml_files` 里 `if (ec) break;` —— 某侧目录遍历中途出错
    //   （权限/被独占/路径过长）就静默截断 ⇒ 那一侧后续文件全缺 ⇒ 它们会被判成
    //   "只在一侧存在"或干脆消失 ⇒ 结果表给出**虚假的"一致/缺失"结论**，而页面/CSV
    //   一个告警都没有。校对工具的价值就在"不漏"，静默漏文件属实质缺陷。
    //   现在：每侧的第一条出错理由（含目录与 `ec.message()`）落这里；**非空即"本次结论不可信"**，
    //   调用方（CLI 退码 / 页面状态栏 / CSV）必须把它显示出来。
    std::vector<std::string> errors;
    bool listings_ok() const { return errors.empty(); }
};

// 递归列出 `*.xml`：返回**相对路径**（'/' 分隔）按字节序升序（确定性）。
// ★ P17-10（02 F-05）：`errors` 非空 ⇒ 遍历中途出过错（**保留了已经列到的部分**，
//   但调用方必须把"结论不可信"这件事报出去；别把部分列表当完整列表用）。
std::vector<std::string> list_xml_files(const std::string& root,
                                        std::vector<std::string>* errors = nullptr);

// 比两个目录（`total` = 两侧相对路径的并集大小）。
DirDiffReport compare_dirs(const DirDiffOptions& options);

// 命中判定：`row_kinds` 空 ⇒ 全过；`differ_types` 非空 ⇒ 必须**同时**命中
//（"only" = 有仅一侧的 id；"text" = 有文本差异）—— 与页面那套筛选口径逐字一致。
bool row_matches_filter(const XmlFilter& filter, const std::string& kind,
                        const XmlCompareResult& result);

}  // namespace xrkit::engine::xml
