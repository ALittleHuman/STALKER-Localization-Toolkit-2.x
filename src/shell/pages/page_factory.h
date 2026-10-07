#pragma once
// xrkit::shell —— 页工厂（M2-6a）＋ **内置六页的注册**（插件改装 P1）。
//
// 为什么工厂还在：Hub 只认"注册表里的页"，而**具体页类 → 控件**的映射必须有且只有一处实现
//（就是本文件）。它同时把 5.1 的边界钉死在这一个地方：视口只由 `PageBase` 造，页面实现
// 文件里不许自己造视口（门禁静态锁按 `new ScrollViewport` + 左括号扫，所以本注释刻意不写全
// 那个调用形态）。
//
// ★ P1：内置六页的**声明清单**（id/title/subtitle/order）也搬到了这里 —— 它属于页模块，
//   不属于 Hub。`register_builtin_pages()` 把清单逐条注册进 `PageRegistry`（带 =
//   `kBuiltinPageBand`），Hub 从此只消费注册表（静态锁：`hub.cpp` 里不许出现任何具体页类名，
//   见 `tests/check_page_host_lock.py`）。

#include "../page_host.h"

#include <QString>

#include <string>

class QWidget;

namespace xrkit::shell {

class PageBase;

// 内置页：id 是稳定标识（插件槽位 / 日志 / 替换都按它走），title 是界面上的名字。
struct PageInfo {
    const char* id;
    const char* title;
    const char* subtitle = "";  // 一句话说明本页能干什么（六页当前全空，见 page_factory.cpp）
    int order = 0;              // ★ P1：带内顺序（内置带 = kBuiltinPageBand）
};

int builtin_page_count();
const PageInfo& builtin_page(int index);

// 把内置六页注册进注册表（Hub 构造时调一次；owner = `kBuiltinPageOwner`）。
// 注册诊断（被拒的理由等）由调用方从 `PageRegistry::take_diagnostics()` 取走报出。
void register_builtin_pages(PageRegistry& pages);

// 按 id 造一页的内置实现（注册表用的 builder 形态）。id 不认识 ⇒ 返回 `PageBase` 兜底页；
// 造不出来才返回 nullptr。
QWidget* create_builtin_page_widget(const std::string& id, QWidget* parent);

// 按内置页下标造页（下标越界返回 nullptr）。parent 省略时由调用方接管所有权。
// ★ 保留它是因为**自检/工具要单独造一页来量**（不起整个 Hub）；Hub 自己不走这条捷径
//   —— 它只按注册表来（否则"注册↔消费"又被绕开了）。
PageBase* create_page(int index, QWidget* parent = nullptr);

}  // namespace xrkit::shell
