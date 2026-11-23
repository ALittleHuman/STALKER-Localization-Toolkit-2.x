// xrkit_smoke —— 外壳自检的独立入口（RC2 起从本体拆出）。
// 本体 xrkit_app.exe 不再包含 smoke.cpp / xrkit_smoke.exe；本 exe 只用于构建、门禁和包内临时自检，
// 不随发布包发放。

#include "smoke.h"
#include "theme.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/plugin/py_host.h"

#include <QApplication>
#include <QGuiApplication>
#include <QTextStream>

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    qputenv("XRKIT_SMOKE", "1");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("STALKER Toolkit 2.x"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));

    struct PythonShutdownGuard {
        ~PythonShutdownGuard() { xrkit::plugin::shutdown_python(); }
    } python_shutdown_guard;

    const xrkit::core::ThemeMode mode = xrkit::core::load_theme_mode();
    xrkit::shell::apply_theme(app, mode);
    const xrkit::shell::SmokeResult result = xrkit::shell::run_smoke();
    QTextStream out(stdout);
    for (const QString& note : result.notes) out << "  SHELL-SMOKE note " << note << "\n";
    for (const QString& failure : result.failures) out << "  SHELL-SMOKE fail " << failure << "\n";
    out << QStringLiteral("SHELL-SMOKE ok checks=%1 failed=%2 not_covered=%3 mode=%4\n")
               .arg(result.checks)
               .arg(result.failed)
               .arg(result.not_covered)
               .arg(QString::fromUtf8(xrkit::core::theme_mode_name(mode)));
    out.flush();
    return result.failed == 0 ? 0 : 1;
}