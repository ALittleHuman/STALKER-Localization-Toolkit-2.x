#pragma once
// xrkit::shell —— 页工厂（M2-6a）。
//
// 为什么要有工厂：Hub 只认"下标 → 页对象"这一件事，页的实现细节（M2-6b..g 各片
// 一片片填）不回流到 Hub。同时它把 5.1 的边界钉死在**一个地方**：
// 视口只由 `PageBase` 造，页面实现文件里不许自己造视口（门禁静态锁按
// `new ScrollViewport` + 左括号扫，所以本注释刻意不写全那个调用形态）。

class QWidget;

namespace xrkit::shell {

class PageBase;

// 按内置页下标造页（下标越界返回 nullptr）。parent 省略时由调用方接管所有权。
PageBase* create_page(int index, QWidget* parent = nullptr);

}  // namespace xrkit::shell
