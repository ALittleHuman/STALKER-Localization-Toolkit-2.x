// xrkit_app —— 2.x Qt6 外壳入口（M2）。
//
// 两种模式：
//   · 正常：起 Hub 主窗口，进事件循环。
//   · `--smoke`：**离屏**构造真实的控件树并跑自检，输出 ASCII 收尾行
//     `SHELL-SMOKE ok checks=N failed=M`，退出码 0/1 —— 门禁靠它判外壳（不靠人眼截图）。

#include "xrkit/core/theme.h"
#include "hub.h"
#include "smoke.h"
#include "screen_scale.h"
#include "theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <QTextStream>

int main(int argc, char** argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromUtf8(argv[i]);
    const bool smoke = args.contains(QStringLiteral("--smoke"));
    // ★ 屏幕自适应诊断：打印「启动时按当前屏幕算出来的缩放与窗口尺寸」然后退出。
    //   为什么要有它：离屏自检只验了纯函数；这条分支验的是**真实启动路径**
    //   （QScreen 逻辑 DPI → snap → set_ui_scale → 窗口尺寸），可配合 QT_SCALE_FACTOR 实测。
    const bool scale_report = args.contains(QStringLiteral("--scale-report"));

    // 离屏：不依赖真实显示器（门禁机器上也可能没有交互桌面）
    if (smoke && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // ★ 高 DPI 策略（屏幕自适应口径，2026-09-27）：让 Qt **保留分数缩放**。
    //   Qt6 默认 `Round` 会把 125%/133% 这类缩放先取整一次，而我们的 `snap_ui_scale()`
    //   还会再吸附一档 ⇒ 两处都在取整、结果不可预期。**必须在构造 QApplication 之前**设。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("STALKER Toolkit 2.x"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));

    if (scale_report) {
        const double applied = xrkit::shell::apply_screen_ui_scale();
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize avail = screen != nullptr ? screen->availableGeometry().size() : QSize(0, 0);
        const QSize win =
            xrkit::shell::adaptive_window_size(avail, xrkit::shell::app_min_window_size());
        const QSize minsz = xrkit::shell::app_min_window_size();
        QTextStream out(stdout);
        // ★ 关键取证（2026-09-27 实测发现的问题）：Windows 上 Qt6 的 **logicalDotsPerInch 恒为 96**
        //   （实测：150% 缩放的机器上 logical 可用区 1707x1019、logicalDotsPerInch 仍报 96.0）
        //   ⇒ 只看逻辑 DPI 的自适应**在真机上等于没做**。这里把三样都打出来，用来定正确口径：
        //   dpi=logicalDotsPerInch、pdpi=physicalDotsPerInch、dpr=devicePixelRatio、logical=屏幕逻辑尺寸。
        out << QStringLiteral("SCALE-APPLY dpi=%1 pdpi=%2 dpr=%3 scale=%4 screen=%5x%6 window=%7x%8 "
                              "min=%9x%10\n")
                   .arg(screen != nullptr ? screen->logicalDotsPerInch() : -1.0, 0, 'f', 1)
                   .arg(screen != nullptr ? screen->physicalDotsPerInch() : -1.0, 0, 'f', 1)
                   .arg(screen != nullptr ? screen->devicePixelRatio() : -1.0, 0, 'f', 2)
                   .arg(applied, 0, 'f', 2)
                   .arg(avail.width())
                   .arg(avail.height())
                   .arg(win.width())
                   .arg(win.height())
                   .arg(minsz.width())
                   .arg(minsz.height());
        out.flush();
        return 0;
    }

    if (smoke) {
        // 主题先按 user.ltx 的偏好铺上（读不到就用默认暗色）
        const std::string saved = xrkit::core::load_user_value("theme", "mode", "");
        xrkit::core::ThemeMode mode = xrkit::core::ThemeMode::Dark;
        if (!saved.empty()) xrkit::core::theme_mode_from_name(saved, mode);
        xrkit::shell::apply_theme(app, mode);

        const xrkit::shell::SmokeResult result = xrkit::shell::run_smoke();
        QTextStream out(stdout);
        for (const QString& note : result.notes) {
            out << "  SHELL-SMOKE note " << note << "\n";
        }
        for (const QString& failure : result.failures) {
            out << "  SHELL-SMOKE fail " << failure << "\n";
        }
        out << QStringLiteral("SHELL-SMOKE ok checks=%1 failed=%2 mode=%3\n")
                   .arg(result.checks)
                   .arg(result.failed)
                   .arg(QString::fromUtf8(xrkit::core::theme_mode_name(mode)));
        out.flush();
        return result.failed == 0 ? 0 : 1;
    }

    const std::string saved = xrkit::core::load_user_value("theme", "mode", "");
    xrkit::core::ThemeMode mode = xrkit::core::ThemeMode::Dark;
    if (!saved.empty()) xrkit::core::theme_mode_from_name(saved, mode);
    xrkit::shell::apply_theme(app, mode);

    // ★ 屏幕自适应（用户 2026-09-27："2.5K 屏…做一个自适应，所有屏幕都能正常用"）：
    //   ① 先按当前屏幕的逻辑 DPI 把全局 ui_scale 设对（**必须在建控件之前**，
    //      因为字体与几何都在构造时用 px() 换算）；
    //   ② 再按屏幕可用区给窗口一个合适的默认尺寸（小屏不溢出、大屏不缩成一小块）。
    xrkit::shell::apply_screen_ui_scale();

    xrkit::shell::Hub hub;
    xrkit::shell::apply_dark_titlebar(&hub, mode);
    {
        const QScreen* screen = QGuiApplication::primaryScreen();
        const QSize avail = screen != nullptr ? screen->availableGeometry().size()
                                              : QSize(1280, 800);
        hub.resize(xrkit::shell::adaptive_window_size(avail, xrkit::shell::app_min_window_size()));
        hub.setMinimumSize(xrkit::shell::app_min_window_size());
    }
    hub.show();
    return QApplication::exec();
}
