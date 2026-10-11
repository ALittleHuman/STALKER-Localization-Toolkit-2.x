#pragma once
// xrkit::shell —— **屏幕自适应**（用户 2026-09-27："我电脑是2.5K屏，所以做一个自适应，
// 这样所有屏幕都能正常用。"）
//
// 口径（与 `docs/2.0.0-外壳规格.md` 配套）：
//   ① **缩放的唯一真值仍是 core**（`core::set_ui_scale()` / `core::px()`）；这里只负责
//      "启动时按当前屏幕把它设对"。设计稿像素一律按 96 DPI 定，几何一律过 `px()`。
//   ② ★★ P17-94（审计 `03 F-09`）：**不要再宣称"用逻辑 DPI"** —— 真机实测（见下面那条
//      `★ 口径更正`）证明 `logicalDotsPerInch()` 在 Windows/Qt6 下**恒为 96**，正确规则是
//      "看 `devicePixelRatio` ＋ 物理 DPI"。**不额外设 `QT_SCALE_FACTOR`**（避免双重缩放）这半句仍然成立。
//   ③ 缩放因子**吸附到常用档位**并 clamp 到 [1.0, 2.0]：避免 1.33/1.42 这类怪值把
//      像素取整搞出 1px 抖动。
//   ④ 窗口尺寸跟着屏幕走：默认约可用区 70%，介于"最小尺寸"与"可用区 90%（且不超过
//      1600×1000 逻辑像素）"之间 ⇒ 1366×768 不溢出、2.5K/4K 不会缩成一小块。

#include <QSize>

namespace xrkit::shell {

// 把"原始缩放"吸附到 {1.0, 1.25, 1.5, 1.75, 2.0} 并 clamp 到 [1.0, 2.0]（纯函数，可单测）
// ★ 口径更正（2026-09-27 真机实测）：**不能只看 logicalDotsPerInch** —— Windows/Qt6 下它
//   **恒为 96**（实测 150% 缩放的机器：dpi=96.0 / dpr=1.50 / pdpi=125.9）。正确规则：
//   dpr>1 ⇒ Qt 已在缩放 ⇒ 我们保持 1.0（否则双重缩放）；dpr≈1 ⇒ 按物理 DPI 自己放大。
double ui_scale_for(double device_pixel_ratio, double physical_dpi);

double snap_ui_scale(double raw);

// 屏幕可用区 + 最小尺寸 ⇒ 默认窗口尺寸（纯函数，可单测）
QSize adaptive_window_size(const QSize& available, const QSize& min_size);

// 主窗口的最小尺寸（设计稿像素，内部过 px()）——集中在一处，别在 main 里写死
QSize app_min_window_size();

// 按当前屏幕设全局 ui_scale（**规则见上面那条 `★ 口径更正`**：dpr ＋ 物理 DPI，不是逻辑 DPI）；返回实际生效的缩放因子。
// **只应在交互式启动路径调用**：离屏自检/`--layout-report` 要保持确定性，不调它。
double apply_screen_ui_scale();

}  // namespace xrkit::shell
