#include "smoke.h"

#include "xrkit/core/theme.h"
#include "hub.h"
#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStringList>
#include <QWidget>

namespace xrkit::shell {
namespace {

// 判据用的期望值：**契约**（页 id 是插件槽位/日志的稳定标识），不是"再抄一份清单"。
// 清单本身从 `builtin_page()`（真实清单）读，两边交叉核对。
constexpr const char* kExpectedIds[] = {"fs", "convert", "text", "xml", "video", "fontpack"};
constexpr const char* kExpectedTitles[] = {"文件系统", "编码转换", "文本提取",
                                           "XML校对", "视频转换", "汉化包生成"};
constexpr int kExpectedCount = 6;

}  // namespace

SmokeResult run_smoke() {
    SmokeResult result;
    const auto check = [&result](bool good, const QString& what) {
        ++result.checks;
        if (!good) {
            ++result.failed;
            result.failures.append(what);
        }
    };

    // ① 真实清单 vs 契约（少一页/改个名字都要红）
    check(builtin_page_count() == kExpectedCount,
          QStringLiteral("内置页数 = %1（实际 %2）").arg(kExpectedCount).arg(builtin_page_count()));
    for (int i = 0; i < kExpectedCount && i < builtin_page_count(); ++i) {
        check(QString::fromUtf8(builtin_page(i).id) == QString::fromUtf8(kExpectedIds[i]),
              QStringLiteral("第 %1 页 id = %2").arg(i).arg(QString::fromUtf8(kExpectedIds[i])));
        check(QString::fromUtf8(builtin_page(i).title) == QString::fromUtf8(kExpectedTitles[i]),
              QStringLiteral("第 %1 页标题 = %2")
                  .arg(i)
                  .arg(QString::fromUtf8(kExpectedTitles[i])));
    }

    // ② Hub 真的按这份清单建了控件（列表项 = 页栈页数 = 清单条数）
    Hub hub;
    check(hub.page_count() == builtin_page_count(), QStringLiteral("页栈页数 = 内置页数"));
    check(hub.page_list()->count() == builtin_page_count(), QStringLiteral("列表项数 = 内置页数"));
    for (int i = 0; i < builtin_page_count(); ++i) {
        check(hub.page_title(i) == QString::fromUtf8(builtin_page(i).title),
              QStringLiteral("列表第 %1 项标题与清单一致").arg(i));
        check(hub.page_id(i) == QString::fromUtf8(builtin_page(i).id),
              QStringLiteral("第 %1 页 id 可查").arg(i));
    }
    check(hub.stack()->currentIndex() == 0, QStringLiteral("默认停在第一页"));

    // ③ 日志栏存在、只读（1.x 口径：日志不归主窗口那根滚动条管）
    check(hub.log_view() != nullptr, QStringLiteral("日志栏存在"));
    check(hub.log_view() != nullptr && hub.log_view()->isReadOnly(),
          QStringLiteral("日志栏只读"));
    hub.log_line(QStringLiteral("smoke"));
    check(hub.log_view() != nullptr && hub.log_view()->toPlainText().contains("smoke"),
          QStringLiteral("日志能追加"));

    // ④ 主题：令牌 → QPalette/QSS；**切换是原地更新**（同一个 widget 的 palette 跟着变）
    QWidget probe;
    probe.show();
    apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
    const QColor dark_window = probe.palette().color(QPalette::Window);
    apply_theme(*qApp, xrkit::core::ThemeMode::Light);
    const QColor light_window = probe.palette().color(QPalette::Window);
    check(dark_window.name().compare(
              QString::fromUtf8(xrkit::core::theme_color(xrkit::core::ThemeMode::Dark, "bg")),
              Qt::CaseInsensitive) == 0,
          QStringLiteral("暗色 QPalette::Window = 令牌 bg"));
    check(light_window.name().compare(
              QString::fromUtf8(xrkit::core::theme_color(xrkit::core::ThemeMode::Light, "bg")),
              Qt::CaseInsensitive) == 0,
          QStringLiteral("亮色 QPalette::Window = 令牌 bg"));
    check(dark_window != light_window, QStringLiteral("切主题后同一控件底色确实变了"));
    check(!qApp->styleSheet().isEmpty(), QStringLiteral("QSS 已应用"));
    apply_theme(*qApp, xrkit::core::ThemeMode::Dark);

    // ⑤ HiDPI：外壳里所有硬编码尺寸都要过 px()
    xrkit::core::set_ui_scale(1.0);
    check(px(24) == 24, QStringLiteral("px(24) @1.0 = 24"));
    xrkit::core::set_ui_scale(1.5);
    check(px(24) == 36, QStringLiteral("px(24) @1.5 = 36"));
    xrkit::core::set_ui_scale(1.0);

    // ⑥ user.ltx 落在 **exe 所在目录**（1.x `app_dir()/user.ltx`），且只读不写
    const QString path = QString::fromUtf8(xrkit::core::user_ltx_path());
    check(QFileInfo(path).fileName().compare(QStringLiteral("user.ltx"), Qt::CaseInsensitive) == 0,
          QStringLiteral("user.ltx 文件名"));
    check(QFileInfo(path).absolutePath() == QCoreApplication::applicationDirPath(),
          QStringLiteral("user.ltx 与 exe 同目录"));

    return result;
}

}  // namespace xrkit::shell
