#pragma once
// xrkit::engine::font —— **系统字体目录**（1.x `system_font_dirs()` 的等价实现）。
//
// 为什么在引擎层而不是页面里（2026-10-07 P7b 搬过来的）：
//   这份逻辑原本写在 `fontpack_page.cpp` 的匿名命名空间里，而**插件也要用同一份**
//   （`api.host.system_font_dirs()`）—— 在同一件事上再写一遍就是第二份，本仓已经因为
//   "旁边再写一份差不多的"被门禁拦下过三次（分栏 / 列策略 / 引擎门面住哪层）。
//
// 口径**逐条照 1.x**（不许顺手加东西）：Windows 取 `%SystemRoot%\Fonts`（**不写死 C:\Windows**，
// 环境变量被清掉时兜 `C:\Windows`）；macOS / Linux 的路径见实现。

#include <string>
#include <vector>

namespace xrkit::engine::font {

// 该平台的系统字体目录（**原样**，存在性/顺序由调用方决定 —— 页面会把"当前字体所在目录"
// 追加在后面并按"存在的排前面"排；那是页面的口径，不在这里）
std::vector<std::string> system_font_dirs();

}  // namespace xrkit::engine::font
