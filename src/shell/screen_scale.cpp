#include "screen_scale.h"

#include "xrkit/core/theme.h"

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <array>
#include <cmath>

namespace xrkit::shell {
namespace {

constexpr std::array<double, 5> kSnapSteps = {1.0, 1.25, 1.5, 1.75, 2.0};

// 默认窗口占可用区比例 / 上限 / 下限兜底
constexpr double kDefaultFraction = 0.70;
constexpr double kMaxFraction = 0.90;
constexpr int kMaxLogicalWidth = 1600;   // 逻辑像素上限：再大的屏也别把窗口拉到满屏
constexpr int kMaxLogicalHeight = 1000;

// 设计稿最小窗口（逻辑像素，内部过 px()）
constexpr int kMinDesignWidth = 900;
constexpr int kMinDesignHeight = 520;

}  // namespace

double snap_ui_scale(double raw) {
    if (!(raw > 0.0) || std::isnan(raw)) return 1.0;  // 0/负数/NaN ⇒ 回落到 100%
    const double clamped = std::min(std::max(raw, kSnapSteps.front()), kSnapSteps.back());
    double best = kSnapSteps.front();
    double best_diff = std::abs(clamped - best);
    for (double step : kSnapSteps) {
        const double diff = std::abs(clamped - step);
        if (diff < best_diff) {
            best = step;
            best_diff = diff;
        }
    }
    return best;
}

QSize adaptive_window_size(const QSize& available, const QSize& min_size) {
    const int avail_w = std::max(1, available.width());
    const int avail_h = std::max(1, available.height());
    const int min_w = std::max(1, min_size.width());
    const int min_h = std::max(1, min_size.height());

    const int cap_w = std::min(static_cast<int>(avail_w * kMaxFraction), kMaxLogicalWidth);
    const int cap_h = std::min(static_cast<int>(avail_h * kMaxFraction), kMaxLogicalHeight);

    int w = static_cast<int>(avail_w * kDefaultFraction);
    int h = static_cast<int>(avail_h * kDefaultFraction);
    w = std::max(w, min_w);   // 小于"最小尺寸"就没法用了
    h = std::max(h, min_h);
    // 上限：不超过可用区 90%。**若最小尺寸本身就超过上限**（极端小屏），
    // 以最小尺寸为准（宁可溢出，也不能把窗口压到内容放不下）——这条要有断言覆盖。
    w = std::min(w, std::max(min_w, cap_w));
    h = std::min(h, std::max(min_h, cap_h));
    return QSize(w, h);
}

QSize app_min_window_size() {
    return QSize(xrkit::core::px(kMinDesignWidth), xrkit::core::px(kMinDesignHeight));
}

double ui_scale_for(double device_pixel_ratio, double physical_dpi) {
    // Qt 已经在按 OS 缩放渲染（dpr>1）⇒ 我们**不叠加**，否则几何被放大两次
    if (device_pixel_ratio > 1.01) return 1.0;
    // dpr≈1：系统没帮我们放大（典型：4K 屏但系统缩放 100%）⇒ 按物理 DPI 自己放大
    if (!(physical_dpi > 0.0)) return 1.0;
    return snap_ui_scale(physical_dpi / 96.0);
}

double apply_screen_ui_scale() {
    double dpr = 1.0;
    double pdpi = 96.0;
    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        if (screen->devicePixelRatio() > 0.0) dpr = screen->devicePixelRatio();
        const double reported = screen->physicalDotsPerInch();
        if (reported > 0.0) pdpi = reported;  // 拿不到就按 96（＝100%）走
    }
    const double scale = ui_scale_for(dpr, pdpi);
    xrkit::core::set_ui_scale(scale);
    return scale;
}

}  // namespace xrkit::shell
