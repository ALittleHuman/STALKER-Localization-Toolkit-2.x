#include "smoke.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/version.h"
#include "xrkit/core/log.h"
// ★ 2026-10-06（审计 3-3）：自检要直接断言"两种路径写法判成同一份"这条判重口径
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/lzhuf.h"   // ★ P17-17（01 F-14）：编码上限判据
#include "xrkit/engine/fs/untrusted_limits.h"   // ★ P17-17：那三个上限的具名常量
#include "xrkit/engine/fs/squashfs.h"
#include "xrkit/engine/plugin/abi_host.h"
#include "xrkit/engine/plugin/loader.h"
// ★ P17-14（01 F-15）：保留设备名判定与写盘拦阻要看真实现
#include "xrkit/core/atomic_write.h"
#include "xrkit/core/build_version.h"   // ★ P17-15（01 F-11）：JSON 深度上限判据
#include "xrkit/engine/process/run.h"   // ★ P17-11（01 F-04）：命令行引号判据要 `run()` / 拼命令行
#include "xrkit/engine/text/encoding.h"
#include "hub.h"
#include "dialog_start.h"
#include "drop_filter.h"
#include "font_picker.h"
#include "xrkit/engine/font/system_dirs.h"
#include "drop_zone.h"
#include "log_box.h"
#include "plugin_ui.h"
#include "pages/convert_page.h"
#include "pages/fs_page.h"
#include "pages/fontpack_page.h"
#include "pages/page_base.h"
#include "pages/page_factory.h"
#include "pages/text_page.h"
#include "pages/video_page.h"
#include "pages/xml_page.h"
#include "scroll_viewport.h"
#include "split_pane.h"
#include "task_shell.h"
#include "facade_tree.h"
#include "row_action_delegate.h"
#include "theme.h"
#include "virtual_list.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QDropEvent>
#include <QUrl>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QVariant>
#include <QScrollBar>
#include <QSet>
#include <QStackedWidget>
#include <QElapsedTimer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#if defined(_WIN32)
// 末尾「⑬ 版本资源」那一组要读**本进程 exe 的真实资源字节**（VERSIONINFO）。
//   · 只在 Win32 下有意义（本仓库只出 Windows 包；PE 子系统之类判据本来也都在 Win32 上）；
//   · NOMINMAX 与全局口径一致（根 CMakeLists 的 xrkit_flags）—— 这里再兜一次：没有它的话
//     windows.h 会定义 min/max 宏，把本文件里的 std::max / std::min 全弄坏（会编译失败）。
//   · 不用 WIN32_LEAN_AND_MEAN：它会把 winver.h 排除掉，而本组要的 GetFileVersionInfoW /
//     VerQueryValueW 就在 winver.h 里。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>
#endif

#include <cstdio>
#include <QHeaderView>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

namespace xrkit::shell {
namespace {

// 判据用的期望值：**契约**（页 id 是插件槽位/日志的稳定标识），不是"再抄一份清单"。
// 清单本身从 `builtin_page()`（真实清单）读，两边交叉核对。
constexpr const char* kExpectedIds[] = {"fs", "convert", "text", "xml", "video", "fontpack"};
constexpr const char* kExpectedTitles[] = {"文件系统", "编码转换", "文本提取",
                                           "XML校对", "视频转换", "汉化包生成"};
constexpr int kExpectedCount = 6;

// ★★ 2026-10-02 包列表**树化**（用户：「左边的包列表的结构应该也是一个文件树」）之后，
//   "第 0 行"不再等于"第 0 个源" —— 源节点散在树里、中间还夹着**纯分组节点**。
//   所以夹具要按**节点上的源下标**（`data(0, Qt::UserRole)` ≥0）来找源，别再数顶层行。
// 包列表树里的**源节点**总数（纯分组节点不算）
int package_source_node_count(QTreeWidget* list) {
    int total = 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toInt() >= 0) ++total;
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
    return total;
}

// 选中包列表树里**所有源节点**（旧夹具用 `topLevelItem(i)->setSelected(true)`，树化后那样选不到源）
void select_all_package_sources(QTreeWidget* list) {
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item->data(0, Qt::UserRole).toInt() >= 0) item->setSelected(true);
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
}

// 按名字找源节点（树里到处找）
QTreeWidgetItem* find_package_source_node(QTreeWidget* list, const QString& name_contains) {
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (found == nullptr && item->data(0, Qt::UserRole).toInt() >= 0 &&
            item->text(0).contains(name_contains, Qt::CaseInsensitive)) {
            found = item;
        }
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    if (list != nullptr) {
        for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
    }
    return found;
}

#if defined(_WIN32)
// 读**本进程那个 exe**的版本资源里某一个字符串字段（读不到返回空串）。
// 为什么直读 Win32 而不另起进程问 PowerShell：字段里有中文，管道/控制台码页会把它读坏 ——
// 判据要的是**逐字**相等，只能自己读资源。
QString exe_version_field(const QString& name) {
    const QString path = QCoreApplication::applicationFilePath();
    const auto* wide_path = reinterpret_cast<LPCWSTR>(path.utf16());
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide_path, &handle);
    if (size == 0) return QString();
    std::vector<unsigned char> buffer(size);
    if (!GetFileVersionInfoW(wide_path, 0, size, buffer.data())) return QString();
    struct LangPair {
        WORD language;
        WORD code_page;
    };
    LangPair* pair = nullptr;
    UINT pair_size = 0;
    if (!VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&pair), &pair_size) ||
        pair == nullptr || pair_size < sizeof(LangPair)) {
        return QString();
    }
    const QString sub = QStringLiteral("\\StringFileInfo\\%1%2\\%3")
                            .arg(pair->language, 4, 16, QLatin1Char('0'))
                            .arg(pair->code_page, 4, 16, QLatin1Char('0'))
                            .arg(name);
    void* value = nullptr;
    UINT value_size = 0;
    if (!VerQueryValueW(buffer.data(), reinterpret_cast<LPCWSTR>(sub.utf16()), &value,
                        &value_size) ||
        value == nullptr) {
        return QString();
    }
    return QString::fromWCharArray(static_cast<const wchar_t*>(value));
}
#endif

// ★★ 2026-10-06（审计处理批·0-3 之 B1-7）：`user.ltx` 的"备份→改动→复原"三处用例，
//   修前判据是 `QFile(ltx).size() == ltx_before.size()`，两个漏洞：
//     ① **读备份失败时** `ltx_before` 是空 ⇒ 判据退化成 `0 == 0` ⇒ 数据被破坏却判"还原成功"；
//     ② 即便读成功也**只比长度不比内容** ⇒ 同长度写坏（截断/错位）照样绿。
//   这里按"内容逐字节相等"判，并把"备份到底读到没有"作为**前置**（没读到就明说无从校验）。
//   返回：{0,1} = 是否还原成原样；false 时 `why` 写明原因（不存在/长度变/内容变/备份缺失）。
bool user_ltx_restored(const QString& path, bool existed, const QByteArray& before, QString* why) {
    const bool now_exists = QFileInfo::exists(path);
    if (now_exists != existed) {
        if (why != nullptr) *why = QStringLiteral("存在状态变了（原 %1 / 现 %2）")
                                      .arg(existed ? 1 : 0).arg(now_exists ? 1 : 0);
        return false;
    }
    if (!existed) return true;                      // 原本就没有 ⇒ 也不该被留下
    if (before.isEmpty()) {
        if (why != nullptr) *why = QStringLiteral("**备份没读到**（无从校验，不许算还原成功）");
        return false;
    }
    QFile back(path);
    if (!back.open(QIODevice::ReadOnly)) {
        if (why != nullptr) *why = QStringLiteral("复原后读不回来（打不开）");
        return false;
    }
    const QByteArray after = back.readAll();
    if (after != before) {
        if (why != nullptr) *why = QStringLiteral("内容不一致（原 %1 字节 / 现 %2 字节）")
                                      .arg(before.size()).arg(after.size());
        return false;
    }
    return true;
}

// 取样式表里**某条规则**的规则体（`{` 到配对 `}`，`.simplified()`）。
//   `last` = true ⇒ 取**最后一条**同名规则。★ 这是 QSS 的语义要求：Qt 里后写的规则赢，
//   所以"生效的那一条"永远是最后一条（`theme.cpp:447-451` 的注释自己写明了这点）。
//   ★★ 2026-10-06（审计处理批·0-3 之 B1-3/B1-4）：修前 QSS 锁用的是 `indexOf`
//      （取**第一条**）、表头锁用的是 `lastIndexOf` 之后的 `qss.mid(at)`（取**到文末的整段**），
//      两个都抓不住"生效的那一条" ⇒ 把生效规则改回缺陷写法/整条删掉，锁仍然绿。
//   ★ 选择器匹配必须**以规则头为准**（选择器后面只能有空白、然后就是 `{`）：
//      `QComboBox QAbstractItemView` 是 `QComboBox QAbstractItemView::item { … }` 的**真前缀**，
//      用纯子串查找会取到错误的那条规则（实测：下拉③ 因此读到 `::item` 的规则体而误判红）。
QString qss_rule_body(const QString& sheet, const QString& selector, bool last) {
    int found = -1;
    for (int at = sheet.indexOf(selector); at >= 0; at = sheet.indexOf(selector, at + 1)) {
        int k = at + selector.size();
        while (k < sheet.size() && (sheet.at(k) == QLatin1Char(' ') || sheet.at(k) == QLatin1Char('\n') ||
                                    sheet.at(k) == QLatin1Char('\t') || sheet.at(k) == QLatin1Char('\r'))) {
            ++k;
        }
        if (k < sheet.size() && sheet.at(k) == QLatin1Char('{')) {
            found = at;
            if (!last) break;
        }
    }
    if (found < 0) return QString();
    const int open = sheet.indexOf(QLatin1Char('{'), found);
    if (open < 0) return QString();
    // 括号配平地找闭合符（QSS 规则体里可以是任意声明，别用"第一个 }"糊过去）
    int depth = 0;
    for (int i = open; i < sheet.size(); ++i) {
        if (sheet.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (sheet.at(i) == QLatin1Char('}')) {
            --depth;
            if (depth == 0) return sheet.mid(open + 1, i - open - 1).simplified();
        }
    }
    return QString();
}

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

    // ★★ 2026-10-06（③）：落盘日志的目录 —— **先把自检的落盘目录改成临时目录**，
    //   免得门禁每跑一次就往构建目录里写一堆 `logs/log_*.txt`。默认口径先在这里钉住。
    const QString smoke_log_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_logs"));
    check(hub_log_dir() == QDir(QCoreApplication::applicationDirPath()).filePath(
                               QStringLiteral("logs")),
          QStringLiteral("③ 默认落盘目录 = exe 目录/logs（1.x `app_dir()/logs`；实得 %1）")
              .arg(hub_log_dir()));
    QDir(smoke_log_dir).removeRecursively();
    set_hub_log_dir(smoke_log_dir);

    // ★★ 2026-10-10（P17-11，审计 01 F-04 Medium）：**命令行引号必须按 Windows 规矩翻倍反斜杠**。
    //   事实（修前）：`quote_arg` 只把 `"` 转义成 `\"`，反斜杠原样输出 ⇒ 参数**以反斜杠结尾**时
    //   （`C:\dir with space\`）得到 `"C:\dir with space\"` —— 收尾那个反斜杠把**收尾引号**吃掉，
    //   `CommandLineToArgvW` 会把后面的参数全并进这一个参数（命令跑歪）。
    //   期望值**不是手写的**：逐条由 Python 的 `subprocess.list2cmdline` 生成
    //   （1.x 拼命令行用的就是它 ⇒ 这是"与 1.x 同口径"的机器口径），
    //   并且另外在 4000 条随机串（字母集 `a b 空格 反斜杠 引号 制表符`）上做过逐字节对拍：
    //   0 条不一致（见批报告 P17-11）。判红办法：把反斜杠翻倍那两处删掉 ⇒ 立刻红。
    {
        struct QuoteCase {
            const char* arg;
            const char* want;
        };
        const QuoteCase quote_cases[] = {
            {"C:\\plain", "C:\\plain"},
            {"C:\\a b\\", "\"C:\\a b\\\\\""},
            {"E:\\dir with space\\", "\"E:\\dir with space\\\\\""},
            {"tail\\", "tail\\"},
            {"sp ace\\", "\"sp ace\\\\\""},
            {"end with 3\\\\\\", "\"end with 3\\\\\\\\\\\\\""},
            {"a b\\\\\"c", "\"a b\\\\\\\\\\\"c\""},
            {"x\"y", "x\\\"y"},
            {"quote\"and\\", "quote\\\"and\\"},
            {"", "\"\""},
            {"has space", "\"has space\""},
            {"tab\there", "\"tab\there\""},
        };
        int bad_quotes = 0;
        QString first_bad;
        for (const QuoteCase& item : quote_cases) {
            const std::string got =
                xrkit::engine::process::build_command_line("", {std::string(item.arg)});
            // `build_command_line("")` 会先拼一个空程序名（`""`）再加一个空格 ⇒ 取空格之后那一段
            const std::string actual = got.size() > 3 ? got.substr(3) : got;
            if (actual != item.want) {
                ++bad_quotes;
                if (first_bad.isEmpty()) {
                    first_bad = QStringLiteral("arg=%1 实得=%2 期望=%3")
                                    .arg(QString::fromUtf8(item.arg),
                                         QString::fromStdString(actual),
                                         QString::fromUtf8(item.want));
                }
            }
        }
        check(bad_quotes == 0,
              QStringLiteral("P17-11 01 F-04：命令行引号与 `subprocess.list2cmdline` 逐字节一致"
                             "（%1 条里 %2 条不符%3）")
                  .arg(static_cast<int>(sizeof(quote_cases) / sizeof(quote_cases[0])))
                  .arg(bad_quotes)
                  .arg(first_bad.isEmpty() ? QString() : QStringLiteral("；首条：") + first_bad));
        // 端到端那一半：真起一个进程，把"以反斜杠结尾 + 含空格"的参数原样交回
        // （`cmd.exe /c echo` 的解析规则与 CRT 不同，所以用**我们自己**的 exe 当子进程不现实；
        //   这里用 `cmd /c` 只是为了证明"命令拼出来能被系统接受"，argv 精度由上面那张表负责）。
        xrkit::engine::process::RunOptions echo;
        echo.program = "cmd.exe";
        echo.args = {"/c", "echo", "C:\\dir with space\\"};
        const xrkit::engine::process::RunResult echoed = xrkit::engine::process::run(echo);
        check(echoed.exit_code == 0,
              QStringLiteral("P17-11 01 F-04 端到端：以反斜杠结尾且含空格的参数能让子进程正常起来"
                             "（exit=%1 err=%2）")
                  .arg(echoed.exit_code)
                  .arg(QString::fromStdString(echoed.error)));
    }

    // ★★ 2026-10-10（P17-14，审计 01 F-15 Low）：**Windows 保留设备名不许当文件名**。
    //   事实（修前）：`write_file_atomic` 直接往目标写 ⇒ 名字是 `CON`/`NUL`/`COM1` 这类
    //   **设备**时，写入**不报错而是被设备吃掉**（`NUL` 直接吞、`CON` 打到控制台），
    //   调用方还会以为产物落盘了 —— 一条静默数据丢失路径。
    //   判据两半：① 名字判定表（`console.txt`/`COM0`/`CON_X` 合法，`CON`/`con.txt`/`Com1.log` 非法）；
    //             ② `write_file_atomic` 真调一次 ⇒ 必须返回 false 且理由里带"保留设备名"，
    //                并且**不许**把文件/设备产物留在磁盘上。
    //   判红办法：把 `write_file_atomic` 里那道拦删掉 ⇒ ②立刻红（那条写入会返回 true）。
    {
        struct NameCase {
            const char* name;
            bool reserved;
        };
        const NameCase name_cases[] = {
            {"CON", true},       {"con", true},      {"CON.txt", true},
            {"Com1.log", true},  {"LPT9", true},     {"NUL", true},
            {"console.txt", false}, {"COM0", false}, {"CON_X", false},
            {"COM10", false},    {"normal.xml", false}, {"", false},
        };
        int bad_names = 0;
        QString bad_detail;
        for (const NameCase& item : name_cases) {
            const bool got = xrkit::core::is_windows_reserved_name(item.name);
            if (got != item.reserved) {
                ++bad_names;
                if (bad_detail.isEmpty()) {
                    bad_detail = QStringLiteral("%1 判成 %2（期望 %3）")
                                     .arg(QString::fromUtf8(item.name))
                                     .arg(got ? 1 : 0)
                                     .arg(item.reserved ? 1 : 0);
                }
            }
        }
        check(bad_names == 0,
              QStringLiteral("P17-14 01 F-15：保留设备名判定表（12 条里 %1 条不符%2）")
                  .arg(bad_names)
                  .arg(bad_detail.isEmpty() ? QString() : QStringLiteral("；首条：") + bad_detail));

        const QString guard_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_reserved"));
        QDir(guard_dir).removeRecursively();
        QDir().mkpath(guard_dir);
        std::string guard_error;
        const std::vector<uint8_t> guard_data{1, 2, 3};
        const bool wrote = xrkit::core::write_file_atomic(
            (guard_dir + QStringLiteral("/CON.txt")).toStdString(), guard_data, true, guard_error);
        check(!wrote && guard_error.find("保留设备名") != std::string::npos,
              QStringLiteral("P17-14 01 F-15：写 `CON.txt` 必须被拒且给出理由（wrote=%1 err=%2）")
                  .arg(wrote ? 1 : 0)
                  .arg(QString::fromStdString(guard_error)));
        const std::vector<uint8_t> control_data{4, 5, 6};
        std::string control_error;
        const bool control_ok = xrkit::core::write_file_atomic(
            (guard_dir + QStringLiteral("/normal.txt")).toStdString(), control_data, true,
            control_error);
        check(control_ok, QStringLiteral("P17-14 01 F-15 对照：普通名字仍要能写（ok=%1 err=%2）")
                              .arg(control_ok ? 1 : 0)
                              .arg(QString::fromStdString(control_error)));
        QDir(guard_dir).removeRecursively();
    }

    // ★★ 2026-10-10（P17-15，审计 01 F-11 Low）：**JSON 递归深度必须有上限**。
    //   事实（修前）：`JParser::value()` 对 `{`/`[` 递归下降，深度只受输入长度限制 ⇒
    //   一份 `[[[[…` 的深层嵌套（几十 KB 文本）就能把 1 MB 线程栈打穿 ⇒ **崩而不是报错**
    //   （`version.json` 是"用户/第三方放到程序目录"的不可信输入）。
    //   判据两半：① 8000 层嵌套 ⇒ 必须**返回 false 并给出理由**（而不是崩）；
    //             ② 对照：仓库里真实的 `version.json` 仍要能正常读进来（防"一刀切拒掉"）。
    {
        const QString deep_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_json_depth"));
        QDir(deep_dir).removeRecursively();
        QDir().mkpath(deep_dir);
        const QString deep_path = deep_dir + QStringLiteral("/deep.json");
        std::string deep_text(8000, '[');
        deep_text.append(8000, ']');
        std::string write_error;
        const bool deep_written = xrkit::core::write_file_atomic(
            deep_path.toStdString(),
            std::vector<uint8_t>(deep_text.begin(), deep_text.end()), true, write_error);
        xrkit::core::VersionStore deep_store;
        std::string deep_error;
        const bool deep_loaded =
            deep_written && xrkit::core::load_version_store(deep_path.toStdString(), deep_store,
                                                            deep_error);
        check(!deep_loaded && deep_error.find("嵌套层数超过上限") != std::string::npos,
              QStringLiteral("P17-15 01 F-11：8000 层嵌套 JSON 必须报错而不是崩"
                             "（loaded=%1 err=%2）")
                  .arg(deep_loaded ? 1 : 0)
                  .arg(QString::fromStdString(deep_error)));
        xrkit::core::VersionStore real_store;
        std::string real_error;
        // 对照（防"一刀切拒掉"）：**上限之内**的嵌套（60 层 < 64）不许报深度错
        // —— 它可能因为"形状不是 version.json"而以别的理由失败，那是**另一条**判据的事，
        //    这里只要求"不是因为深度"。
        const QString shallow_path = deep_dir + QStringLiteral("/shallow.json");
        std::string shallow_text(60, '[');
        shallow_text.append(60, ']');
        std::string shallow_write_error;
        const bool shallow_written = xrkit::core::write_file_atomic(
            shallow_path.toStdString(),
            std::vector<uint8_t>(shallow_text.begin(), shallow_text.end()), true,
            shallow_write_error);
        const bool shallow_loaded =
            shallow_written && xrkit::core::load_version_store(shallow_path.toStdString(),
                                                               real_store, real_error);
        check(shallow_written && real_error.find("嵌套层数超过上限") == std::string::npos,
              QStringLiteral("P17-15 01 F-11 对照：上限之内的嵌套不许被判成「超深」"
                             "（60 层 loaded=%1 err=%2）")
                  .arg(shallow_loaded ? 1 : 0)
                  .arg(QString::fromStdString(real_error)));
        QDir(deep_dir).removeRecursively();
    }

    // ★★ 2026-10-10（P17-17，审计 01 F-14 Low）：**lzhuf 编码缓冲尺寸必须先判上限**。
    //   事实（修前）：`out(size * 2 + 64)` 直接按输入长度×2 分配（`size * 2` 可能回绕），
    //   且 `static_cast<int>(size)`/`static_cast<int>(out.size())` 是窄化 ⇒ 荒谬入参会带着
    //   错尺寸进 LZHUF 内部写（越界写的典型形状），而不是当场被拒。
    //   判据：① 超上限 / 空指针 ⇒ **当场返回空**（故意用 `nullptr` + 超大 size 调，能安全地
    //   证明"先判后碰"：真去碰了就会在这里崩）；② 对照：正常输入仍要能编码-解码往返。
    {
        const std::vector<uint8_t> too_big = xrkit::engine::fs::lzhuf_encode(
            nullptr, static_cast<size_t>(XRKIT_LZHUF_MAX_DECLARED_BYTES) + 1);
        check(too_big.empty(),
              QStringLiteral("P17-17 01 F-14：超上限入参必须当场返回空（实得 %1 字节，"
                             "真去碰了 nullptr 这里就崩了）")
                  .arg(too_big.size()));
        const std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        const std::vector<uint8_t> encoded =
            xrkit::engine::fs::lzhuf_encode(payload.data(), payload.size());
        std::vector<uint8_t> decoded;
        std::string dec_error;
        const bool dec_ok = !encoded.empty() &&
                            xrkit::engine::fs::lzhuf_decode(encoded.data(), encoded.size(), decoded,
                                                            dec_error);
        check(dec_ok && decoded == payload,
              QStringLiteral("P17-17 01 F-14 对照：正常输入仍要能往返（encoded=%1 dec_ok=%2 err=%3）")
                  .arg(encoded.size())
                  .arg(dec_ok ? 1 : 0)
                  .arg(QString::fromStdString(dec_error)));
    }

    // ① 真实清单 vs 契约（少一页/改个名字都要红）
    {
        // ★ 2026-09-27（用户："先把我截图的文字全删了"）：副标题**允许留空**（他截到的那两句已删）
        //   ⇒ 判据从"六页都要有"改成"**要么没有、要么有实质文案**"（留空串/空白串占位＝红）。
        //   这是**用户的删除指令**造成的口径变更，不是为了让谁变绿而放松 —— 原来那条
        //   "六页都要有说明文案"是上一轮 `docs/2.0.0-界面重构口径(排版与配色).md` 的口径，
        //   现被用户当面推翻（他的话就是更高一级的口径）。
        int bad_sub = 0;
        for (int i = 0; i < builtin_page_count(); ++i) {
            const char* sub = builtin_page(i).subtitle;
            if (sub == nullptr || *sub == '\0') continue;  // 没有文案 = 合法（用户删掉的那些）
            bool blank = true;
            for (const char* p = sub; *p != '\0'; ++p) {
                if (*p != ' ' && *p != '\t') blank = false;
            }
            if (blank) ++bad_sub;
        }
        check(bad_sub == 0,
              QStringLiteral("① 副标题要么没有、要么有实质文案（空白占位 %1 条）").arg(bad_sub));
    }
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

    // ★ 2026-09-27（用户："先把我截图的文字全删了"）：**被他点名删掉的那些文字不许回来** ——
    //   六页卡片顶部那几段说明、两页的副标题、占位符/下拉项里的括注。
    //   判据是**逐字串**（他给的正是字面文字），命中即红；不抽样：六页的每个 QLabel / 占位符 /
    //   下拉项都扫一遍。
    {
        const QStringList banned = {
            QStringLiteral("六步判定链"),      // 编码转换页说明 + 下拉项括注
            QStringLiteral("可拖进来"),        // 四个占位符的括注
            QStringLiteral("留空＝原地转换"),  // 输出目录占位符括注
            QStringLiteral("只认两处"),        // 视频页说明
            // ⚠ 这里**不能**用 `行数/ID 统计`：那是 XML 页比较模式下拉里的**合法模式名**
            //   （引擎的模式声明），我第一版写成它，锁当场把我自己拦下来了 ⇒ 换成说明段里
            //   独有的那半句。这条经验：**锁的判据要挑"只有被删对象才有"的字串**。
            QStringLiteral("看条数与出现次数"),  // XML 页说明（独有的那半句）
            QStringLiteral("只看文件夹自己的名字"),  // 文本提取页说明
            QStringLiteral("与 1.x 逐字节一致"),     // 汉化包页说明
            QStringLiteral("产出可直接分发的压缩包"),  // 汉化包页副标题
            QStringLiteral("勾选后解包或重打包"),      // 文件系统页副标题
            // ★ 2026-10-04（用户：「**我截图的文字全部删除。**」）四页副标题也删了 ⇒ 一并进锁。
            //   判据仍照 2026-09-27 那条经验：**挑只有被删对象才有的字串**
            //   （用 `批量转换` 会打到合法文案；cp1251 / ffmpeg / XML 这类词更是满地都是）。
            QStringLiteral("逐条对比转换前后"),  // 编码转换页副标题（独有的那半句）
            QStringLiteral("提取可翻译文本"),      // 文本提取页副标题（独有的那半句）
            QStringLiteral("结构与条目差异"),      // XML 校对页副标题（独有的那半句）
            QStringLiteral("缺失时可见跳过"),      // 视频转换页副标题（独有的那半句）
        };
        QStringList hit;
        // ① 六页的 QLabel（含卡片里的说明段落）
        for (int i = 0; i < builtin_page_count(); ++i) {
            QWidget* page = create_page(i);
            if (page == nullptr) continue;
            for (QLabel* label : page->findChildren<QLabel*>()) {
                for (const QString& text : banned) {
                    if (label->text().contains(text)) hit << text;
                }
            }
            // ② 输入框占位符
            for (QLineEdit* edit : page->findChildren<QLineEdit*>()) {
                for (const QString& text : banned) {
                    if (edit->placeholderText().contains(text)) hit << text;
                }
            }
            // ③ 下拉项（编码转换页的「自动（六步判定链）」就是这一类）
            for (QComboBox* combo : page->findChildren<QComboBox*>()) {
                for (int k = 0; k < combo->count(); ++k) {
                    for (const QString& text : banned) {
                        if (combo->itemText(k).contains(text)) hit << text;
                    }
                }
            }
            delete page;
        }
        // ④ 副标题（六页清单）
        for (int i = 0; i < builtin_page_count(); ++i) {
            const char* sub = builtin_page(i).subtitle;
            if (sub == nullptr) continue;
            for (const QString& text : banned) {
                if (QString::fromUtf8(sub).contains(text)) hit << text;
            }
        }
        check(hit.isEmpty(),
              QStringLiteral("① 用户点名删掉的文字又回来了：%1")
                  .arg(hit.join(QStringLiteral(" / "))));
    }

    // ② Hub 真的按这份清单建了控件（列表项 = 页栈页数 = 清单条数）
    //   ★★ P16-2a/P16-2b（**切打包口径**）：同一页槽位有**两种合法实现** ——
    //      开发态（插件未随目录/未随包）＝**内置页**；开发版包里随包装载＝**插件版页**
    //      （`replaces` 把内置页顶替掉、只隐藏）。所以这里**逐槽位显式列出两种 id 与两种标题**，
    //      第三种一律红 —— 不许用通配/前缀/“有个 id 就算”的放宽写法。
    //      这就是设计笔记预告的 P7c「半迁移 vs 内置六页判据」冲突的**一次面对**。
    struct SlotExpect {
        const char* builtin_id;
        const char* builtin_title;
        const char* plugin_id;
        const char* plugin_title;
    };
    static const SlotExpect kSlots[] = {
        {"fs", "文件系统", "fs_py", "文件系统"},
        {"convert", "编码转换", "convert_py", "编码转换"},
        {"text", "文本提取", "text_py", "文本提取"},
        // ★ 这一槽位两版标题**不同**：内置是「XML校对」（无空格），插件版是「XML 校对」（有空格）。
        //   如实登记两版，不许谁去"顺手统一"—— 两版都要能在各自形态下判绿。
        {"xml", "XML校对", "xml_py", "XML 校对"},
        {"video", "视频转换", "video_py", "视频转换"},
        {"fontpack", "汉化包生成", "fontpack_py", "汉化包生成"},
    };
    // 额外插件页（`replaces` 留空的**新增页**）——同样显式点名（它只在插件随包时出现）
    static const SlotExpect kExtraPages[] = {
        {"", "", "toolchain_py", "汉化产线"},
    };
    Hub hub;
    const int slot_count = static_cast<int>(sizeof(kSlots) / sizeof(kSlots[0]));
    const int extra_count = static_cast<int>(sizeof(kExtraPages) / sizeof(kExtraPages[0]));
    check(slot_count == builtin_page_count(),
          QStringLiteral("显式清单覆盖全部内置页槽位（%1 vs %2）")
              .arg(slot_count).arg(builtin_page_count()));
    const bool extra_loaded = hub.page_count() > slot_count;
    const int want_pages = slot_count + (extra_loaded ? extra_count : 0);
    check(hub.page_count() == want_pages,
          QStringLiteral("页栈页数 = 六槽位%1").arg(extra_loaded ? QStringLiteral(" + 新增页") : QString()));
    check(hub.page_tabs()->count() == want_pages,
          QStringLiteral("标签项数 = 六槽位%1").arg(extra_loaded ? QStringLiteral(" + 新增页") : QString()));
    for (int i = 0; i < slot_count; ++i) {
        const SlotExpect& slot = kSlots[i];
        const QString id = hub.page_id(i);
        const bool is_plugin = (id == QString::fromUtf8(slot.plugin_id));
        const bool is_builtin = (id == QString::fromUtf8(slot.builtin_id));
        check(is_builtin || is_plugin,
              QStringLiteral("第 %1 页 id 必须是显式清单里的两种之一（内置 %2 / 插件 %3），实得 %4")
                  .arg(i).arg(slot.builtin_id, slot.plugin_id, id));
        const QString want_title = QString::fromUtf8(is_plugin ? slot.plugin_title
                                                               : slot.builtin_title);
        check(hub.page_title(i) == want_title,
              QStringLiteral("列表第 %1 项标题要跟它那一版的清单一致（要 %2，实得 %3）")
                  .arg(i).arg(want_title, hub.page_title(i)));
    }
    if (extra_loaded) {
        for (int k = 0; k < extra_count; ++k) {
            const int i = slot_count + k;
            check(hub.page_id(i) == QString::fromUtf8(kExtraPages[k].plugin_id),
                  QStringLiteral("新增页第 %1 项的 id 要按显式清单（要 %2，实得 %3）")
                      .arg(k).arg(kExtraPages[k].plugin_id, hub.page_id(i)));
            check(hub.page_title(i) == QString::fromUtf8(kExtraPages[k].plugin_title),
                  QStringLiteral("新增页第 %1 项的标题要按显式清单（要 %2，实得 %3）")
                      .arg(k).arg(kExtraPages[k].plugin_title, hub.page_title(i)));
        }
    }
    check(hub.stack()->currentIndex() == 0, QStringLiteral("默认停在第一页"));

    // ★★ 插件改装 P1（Hub 机制层）：**页注册表是"页"的唯一一份状态**。
    //   修前的事实（这就是本块要锁住的东西）：
    //     · 内置六页硬编码在 `hub.cpp:kBuiltinPages[]` + `page_factory.cpp:create_page()` 的
    //       if 链里，插件页另走 `hub.cpp` 的 `plugin_pages_` 列表 ⇒ **同一件事两份实现**；
    //     · Hub 为了给 fs/xml 注入注册表得 `dynamic_cast<FsPage*>`/`<XmlPage*>` ⇒ "Hub 不含业务"不成立。
    //   规格 §四.2 的判据：**每类注册点 ↔ 消费点一一对应**（缺一即红）；§五：覆盖必须显式、
    //   失败可回退。所以本块逐条量：注册→出现、注销→消失、重复→被拒且留痕、
    //   `replaces`→只隐藏且能一键还原、**追加插件页不许重建已有页**。
    {
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pagehost"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub host_hub(nullptr, empty);
        const int builtin_count = host_hub.page_count();
        check(builtin_count == builtin_page_count(),
              QStringLiteral("P1 页注册表：内置页数 = 清单条数（%1 / %2）")
                  .arg(builtin_count)
                  .arg(builtin_page_count()));
        // ① 三处计数**同长同序**：标签条 / 页栈 / 页表（修前插件页会打破这个不变量）
        check(host_hub.page_tabs()->count() == builtin_count &&
                  host_hub.stack()->count() == builtin_count,
              QStringLiteral("P1 标签条/页栈/页表三处计数一致（%1 / %2 / %3）")
                  .arg(host_hub.page_tabs()->count())
                  .arg(host_hub.stack()->count())
                  .arg(builtin_count));
        bool builtin_ok = true;
        for (int i = 0; i < builtin_count; ++i) {
            if (host_hub.page_id(i) != QString::fromUtf8(builtin_page(i).id)) builtin_ok = false;
            if (host_hub.page_owner(i) != QString::fromUtf8(kBuiltinPageOwner)) builtin_ok = false;
        }
        check(builtin_ok, QStringLiteral("P1 内置页逐条来自注册表（id/owner 与清单一致）"));

        // ② 注册一页 ⇒ **消费点必须出现**（注册成功但没人渲染 = FAIL）
        QWidget* const first_page_before = host_hub.stack()->widget(0);
        PageSpec spec;
        spec.id = "probe_page";
        spec.title = QStringLiteral("探针页");
        spec.order = 7;   // 插件带内顺序（与内置带无关）
        const bool registered = host_hub.pages().register_page(
            "probe.dll", spec, [](QWidget*) -> QWidget* { return new QWidget(); });
        check(registered, QStringLiteral("P1 注册一页失败"));
        check(host_hub.sync_pages() == 1,
              QStringLiteral("P1 sync_pages 只新建缺的那一页（返回值应为 1）"));
        check(host_hub.page_count() == builtin_count + 1 &&
                  host_hub.page_tabs()->count() == builtin_count + 1 &&
                  host_hub.stack()->count() == builtin_count + 1,
              QStringLiteral("P1 注册页没出现在标签条/页栈（%1）").arg(host_hub.page_count()));
        check(host_hub.page_id(builtin_count) == QStringLiteral("probe_page") &&
                  host_hub.page_title(builtin_count) == QStringLiteral("探针页") &&
                  host_hub.page_owner(builtin_count) == QStringLiteral("probe.dll"),
              QStringLiteral("P1 插件页的 id/标题/owner 不对（实得 %1 / %2 / %3）")
                  .arg(host_hub.page_id(builtin_count))
                  .arg(host_hub.page_title(builtin_count))
                  .arg(host_hub.page_owner(builtin_count)));
        check(host_hub.stack()->widget(0) == first_page_before,
              QStringLiteral("P1 ★追加插件页**不许重建**已有页（0 号页对象必须原样）"));
        check(host_hub.plugin_pages_added() == 1,
              QStringLiteral("P1 插件页计数 = 1（实得 %1）").arg(host_hub.plugin_pages_added()));

        // ③ 幂等：再同步一次不新建、不重复
        check(host_hub.sync_pages() == 0 && host_hub.page_count() == builtin_count + 1,
              QStringLiteral("P1 重同步把页装了两遍（pages=%1）").arg(host_hub.page_count()));

        // ④ 生命周期：注销该 owner ⇒ 消费点跟着消失，计数回到原位
        check(host_hub.pages().unregister_owner("probe.dll") == 1,
              QStringLiteral("P1 按 owner 注销没撤到页"));
        host_hub.sync_pages();
        check(host_hub.page_count() == builtin_count &&
                  host_hub.page_tabs()->count() == builtin_count &&
                  host_hub.stack()->count() == builtin_count &&
                  host_hub.plugin_pages_added() == 0,
              QStringLiteral("P1 注销后三处计数没回到原位（%1 / %2 / %3）")
                  .arg(host_hub.page_count())
                  .arg(host_hub.page_tabs()->count())
                  .arg(host_hub.stack()->count()));

        // ⑤ 重复 id **被拒且留痕**（零静默：修前那种"注册了却不生效"不许回来）
        PageSpec dup;
        dup.id = "fs";
        dup.title = QStringLiteral("假的文件系统");
        check(!host_hub.pages().register_page("probe2.dll", dup,
                                              [](QWidget*) -> QWidget* { return new QWidget(); }),
              QStringLiteral("P1 重复 id 必须被拒"));
        bool dup_diag = false;
        for (const PageDiag& diag : host_hub.pages().take_diagnostics()) {
            if (diag.fatal && diag.message.find("重复") != std::string::npos) dup_diag = true;
        }
        check(dup_diag, QStringLiteral("P1 拒绝注册的理由必须留痕（fatal 诊断）"));

        // ⑥ `replaces` 显式覆盖：被替换页**只隐藏不删**、替换者**继承它的槽位**、
        //   撤掉替换者即一键还原；同一目标不许第二个替换者（那会变成"谁后注册谁赢"）。
        PageSpec repl;
        repl.id = "fs_v2";
        repl.title = QStringLiteral("文件系统v2");
        repl.replaces = "fs";
        check(host_hub.pages().register_page("probe3.dll", repl,
                                             [](QWidget*) -> QWidget* { return new QWidget(); }),
              QStringLiteral("P1 replaces 注册失败"));
        check(host_hub.pages().replaced().size() == 1 &&
                  host_hub.pages().replaced()[0]->spec.id == "fs",
              QStringLiteral("P1 被替换页要能被报出来（只隐藏、没删）"));
        host_hub.sync_pages();
        check(host_hub.page_index(QStringLiteral("fs")) < 0 &&
                  host_hub.page_index(QStringLiteral("fs_v2")) == 0,
              QStringLiteral("P1 替换没生效或没继承槽位（fs=%1 / fs_v2=%2）")
                  .arg(host_hub.page_index(QStringLiteral("fs")))
                  .arg(host_hub.page_index(QStringLiteral("fs_v2"))));
        PageSpec repl2 = repl;
        repl2.id = "fs_v3";
        check(!host_hub.pages().register_page("probe4.dll", repl2,
                                              [](QWidget*) -> QWidget* { return new QWidget(); }),
              QStringLiteral("P1 同一目标只允许一个替换者"));
        host_hub.pages().take_diagnostics();
        check(host_hub.pages().unregister_id("fs_v2"),
              QStringLiteral("P1 撤掉替换者失败"));
        host_hub.sync_pages();
        check(host_hub.page_index(QStringLiteral("fs")) == 0 &&
                  host_hub.page_index(QStringLiteral("fs_v2")) < 0 &&
                  host_hub.page_count() == builtin_count,
              QStringLiteral("P1 一键还原没回来（fs=%1 / pages=%2）")
                  .arg(host_hub.page_index(QStringLiteral("fs")))
                  .arg(host_hub.page_count()));

        // ⑦ 宿主机制：按 id 切页（页/插件拿到的"页面切换"入口）
        check(host_hub.switch_to_page(QStringLiteral("xml")) &&
                  host_hub.stack()->currentIndex() == host_hub.page_index(QStringLiteral("xml")),
              QStringLiteral("P1 switch_to_page 没切到位"));
        check(!host_hub.switch_to_page(QStringLiteral("nope")),
              QStringLiteral("P1 切到不存在的页必须返回 false"));
        QDir(empty).removeRecursively();
    }

    // ★★ 2026-10-04 用户第 1 条（原话）：「**首先，所有目录都需要拖拽，而且是拖拽到相应区域。**」
    //   判据分三层，全部**可判红**：
    //     ① 结构：每一页"要选目录/文件"的输入（QLineEdit）都落在**比它更大**的一个拖放区域里，
    //        那个区域真的装了 `FileDropFilter`（区域 ≠ 输入框本身 —— 旧做法就是挂在框上）：
    //        ⚠ 判据**不能用** `acceptDrops()`：Qt6 的 `QLineEdit` 默认就是 true（实测断言过）；
    //     ② 行为：对**每个**区域构造真 `QDropEvent` ⇒ **该区域对应的那个输入框被填上**；
    //     ③ 反馈：DragEnter 后区域带上 `drop_hover`（QSS 高亮它），Drop 后撤掉。
    {
        QTemporaryDir drop_tmp;
        const QList<QWidget*> page_widgets = {
            create_page(0), create_page(1), create_page(2),
            create_page(3), create_page(4), create_page(5)};
        const auto zone_ok = [](PageBase* page, QWidget* zone) {
            // 区域自己必须是拖放目标、必须带反馈属性，且**过滤器真的挂在它身上**
            if (page == nullptr || zone == nullptr) return false;
            if (!zone->acceptDrops()) return false;
            if (!zone->property("drop_hover").isValid()) return false;
            const std::vector<QWidget*>& zones = page->drop_zones().containers();
            const std::vector<FileDropFilter*>& filters = page->drop_zones().filters();
            if (zones.size() != filters.size()) return false;
            for (size_t i = 0; i < zones.size(); ++i) {
                if (zones[i] == zone) return filters[i] != nullptr && filters[i]->is_attached_to(zone);
            }
            return false;
        };

        // ① 每个目录/文件输入都要有一个能接住它的区域（"所有目录都需要拖拽"）。
        //    这份名单就是**要选目录/文件**的那些输入（搜索框/行内编辑不在其中）。
        const QStringList input_names = {
            QStringLiteral("convert_source"), QStringLiteral("convert_output"),
            QStringLiteral("text_source"),    QStringLiteral("text_target"),
            QStringLiteral("xml_dir_a"),      QStringLiteral("xml_dir_b"),
            QStringLiteral("video_source"),   QStringLiteral("video_reference"),
            QStringLiteral("video_output"),
            QStringLiteral("font_xml_dir"),   QStringLiteral("font_path"),
            QStringLiteral("font_out_dir"),   QStringLiteral("fs_output_dir"),
            QStringLiteral("fs_pack_out")};
        int edit_total = 0;
        QStringList not_droppable;
        for (QWidget* page : page_widgets) {
            auto* as_page = dynamic_cast<PageBase*>(page);
            if (as_page == nullptr) continue;
            for (QLineEdit* edit : page->findChildren<QLineEdit*>()) {
                if (!input_names.contains(edit->objectName())) continue;
                ++edit_total;
                QWidget* holder = edit;
                QWidget* covering_zone = nullptr;
                while (holder != nullptr && covering_zone == nullptr) {
                    if (as_page->drop_zones().is_zone(holder)) covering_zone = holder;
                    holder = holder->parentWidget();
                }
                // 区域必须**真的是**注册过的那个区域，且**过滤器真的挂在区域身上**
                //   （`is_attached_to` 判的是接线；区域里有没有输入框由 `containers()` 那一份
                //    登记表说得清 —— 所以这条不用遍历子控件，也就不会踩
                //    `QObject::findChild` 的形式要求）。
                // ⚠ 这里**不能**用 `edit->acceptDrops()`：Qt6 的 `QLineEdit` 默认就是 true
                //   （实测：裸 QLineEdit acceptDrops()=1、裸 QWidget=0）⇒ 拿它当锁等于"永远为真"。
                //   "输入框没把拖放吞掉"这件事由**② 的行为判据**证明：drop 事件直接把路径填进了
                //   这个 region 的输入框 ⇒ 说明事件真的到了区域这一层。
                const auto& zone_list = as_page->drop_zones().containers();
                const auto& filter_list = as_page->drop_zones().filters();
                bool zone_wired = false;
                for (size_t zi = 0; zi < zone_list.size() && zi < filter_list.size(); ++zi) {
                    if (zone_list[zi] != covering_zone) continue;
                    zone_wired = filter_list[zi] != nullptr &&
                                 filter_list[zi]->is_attached_to(covering_zone);
                }
                // 区域本身**不许是那个输入框**（挂在框上的旧做法就是这么绿过去的）
                if (covering_zone == edit || !zone_wired) {
                    not_droppable << QStringLiteral("%1(%2)").arg(as_page->page_id(), edit->objectName());
                }
            }
        }
        check(edit_total >= 14 && not_droppable.isEmpty(),
              QStringLiteral("① 每个目录/文件输入都在拖放区域里、且输入框自己不吞拖放"
                             "（输入 %1 个；不合格 %2）")
                  .arg(edit_total)
                  .arg(not_droppable.isEmpty() ? QStringLiteral("无")
                                               : not_droppable.join(QStringLiteral(" "))));

        // ②③ 逐页逐区域**构造真 Drop/DragEnter 事件** ⇒ 对应输入被填上 + 反馈属性到位
        QStringList drop_fail;
        int zones_checked = 0;
        for (int i = 0; i < page_widgets.size(); ++i) {
            auto* as_page = dynamic_cast<PageBase*>(page_widgets[i]);
            if (as_page == nullptr) {
                drop_fail << QStringLiteral("page%1-not-page-base").arg(i);
                continue;
            }
            const std::vector<QWidget*>& zones = as_page->drop_zones().containers();
            const std::vector<FileDropFilter*>& filters = as_page->drop_zones().filters();
            if (zones.size() < 2 || zones.size() != filters.size()) {
                drop_fail << QStringLiteral("%1-zones=%2").arg(as_page->page_id()).arg(zones.size());
                continue;
            }
            // 同页每个区域用**不同**的路径（否则"填上了"可能是上一个区域那次的残留）
            for (size_t zone_index = 0; zone_index < zones.size(); ++zone_index) {
                QWidget* zone = zones[zone_index];
                FileDropFilter* filter = filters[zone_index];
                const QString probe = QDir::toNativeSeparators(
                    drop_tmp.path() +
                    QStringLiteral("/drop_p%1_z%2/data.txt").arg(i).arg(static_cast<int>(zone_index)));
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(probe)});
                QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton,
                                      Qt::NoModifier);
                const bool accepted = filter != nullptr && filter->dispatch(&enter, zone);
                const bool highlighted = zone->property("drop_hover").toBool();
                QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                const bool dropped = filter != nullptr && filter->dispatch(&drop, zone);
                const bool cleared = !zone->property("drop_hover").toBool();
                ++zones_checked;

                // 区域里正好一个输入 ⇒ 它必须变成这次丢进来的路径（多输入的按顺序，只查第一个）
                const QList<QLineEdit*> edits = zone->findChildren<QLineEdit*>();
                const bool filled = edits.isEmpty() ||
                                    edits.front()->text() == QDir::toNativeSeparators(probe);
                if (!accepted || !dropped || !filled || !highlighted || !cleared ||
                    !zone_ok(as_page, zone)) {
                    drop_fail << QStringLiteral("%1-%2(a%3 d%4 f%5 h%6 c%7)")
                                     .arg(as_page->page_id())
                                     .arg(static_cast<int>(zone_index))
                                     .arg(accepted ? 1 : 0)
                                     .arg(dropped ? 1 : 0)
                                     .arg(filled ? 1 : 0)
                                     .arg(highlighted ? 1 : 0)
                                     .arg(cleared ? 1 : 0);
                }
            }
        }
        check(drop_fail.isEmpty() && zones_checked >= 13,
              QStringLiteral("② 拖到**相应区域** ⇒ 该区域的输入被填上、且区域看得见接住了"
                             "（验了 %1 个区域；不合格 %2）")
                  .arg(zones_checked)
                  .arg(drop_fail.isEmpty() ? QStringLiteral("无")
                                           : drop_fail.join(QStringLiteral(" "))));
        for (QWidget* page : page_widgets) delete page;
    }

    // ★★ 2026-10-04 用户第 2 条（原话）：「比如**解包的目标目录**，点击"选择"，**默认定向到工具
    //   所在的目录**。**改成定向到输入的目录**。**如果输入目录多元或没有输入，那才定向到本身
    //   所在的文件夹**。然后**其他定向保持现有逻辑**，即**继承上次的选择**。」
    //   判据：三条优先级各一条断言（决策不许被改回去），且"求默认起点"只有**一份共用函数**。
    {
        QTemporaryDir pick_tmp;
        const QString exe_dir = QDir::cleanPath(QCoreApplication::applicationDirPath());
        const QString tmp_a = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickA"));
        const QString tmp_b = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickB"));
        const QString tmp_file = QDir::cleanPath(pick_tmp.path() + QStringLiteral("/pickA/movie.avi"));
        QDir().mkpath(tmp_a);
        QDir().mkpath(tmp_b);
        QFile(tmp_file).open(QIODevice::WriteOnly);

        reset_last_dialog_start();
        const QString unique_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList{tmp_a}));
        const QString none_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList()));
        const QString many_in =
            resolve_dialog_start(dialog_start_for_input_dir(QString(), QStringList{tmp_a, tmp_b}));
        const QString inherited =
            resolve_dialog_start(dialog_start_for_input_dir(tmp_b, QStringList{tmp_a, tmp_b}));
        const QString from_file =
            resolve_dialog_start(dialog_start_for_input_file(QString(), QStringList{tmp_file}));
        check(QDir::cleanPath(unique_in) == tmp_a && QDir::cleanPath(none_in) == exe_dir &&
                  QDir::cleanPath(many_in) == exe_dir && QDir::cleanPath(inherited) == tmp_b &&
                  QDir::cleanPath(from_file) == tmp_a,
              QStringLiteral("② 共用起点函数：唯一输入→输入目录 / 空→exe 目录 / 多元→exe 目录 / "
                             "有值→继承 / 文件输入→它所在目录（实测 %1 | %2 | %3 | %4 | %5）")
                  .arg(unique_in, none_in, many_in, inherited, from_file));
        check(last_dialog_start_calls() == 5 &&
                  QDir::cleanPath(last_dialog_start_dir()) == tmp_a,
              QStringLiteral("② 起点只由这一份共用函数给（调用 %1 次；最后一次 %2）")
                  .arg(last_dialog_start_calls())
                  .arg(last_dialog_start_dir()));
    }

    // ★★ 2026-10-04 逐页钉住"选择"的起点（三条规则在**真实页面**上各来一遍）——
    //   页面把这些字段交给**共用函数**算（`resolve_dialog_start`），这里断言算出来的值。
    {
        QTemporaryDir start_tmp;
        const QString exe_dir = QDir::cleanPath(QCoreApplication::applicationDirPath());
        const QString src_dir = QDir::cleanPath(start_tmp.path() + QStringLiteral("/start_src"));
        const QString other = QDir::cleanPath(start_tmp.path() + QStringLiteral("/start_other"));
        const QString pkg_dir = QDir::cleanPath(start_tmp.path() + QStringLiteral("/startpkg"));
        QDir().mkpath(src_dir);
        QDir().mkpath(other);
        QDir().mkpath(pkg_dir);
        const QString pack_file = pkg_dir + QStringLiteral("/gamedata.db0");
        QFile(pack_file).open(QIODevice::WriteOnly);
        const auto start_of = [](const DialogStart& spec) {
            return QDir::cleanPath(resolve_dialog_start(spec));
        };

        // （1）编码转换：输出目录 ← 源路径
        {
            ConvertPage page;
            check(start_of(page.output_dialog_start()) == exe_dir,
                  QStringLiteral("② 编码转换：源为空 ⇒ 输出起点 = 工具目录"));
            page.source_edit()->setText(src_dir);
            check(start_of(page.output_dialog_start()) == src_dir,
                  QStringLiteral("② 编码转换：源唯一 ⇒ 输出起点 = 源目录"));
            page.output_edit()->setText(other);
            check(start_of(page.output_dialog_start()) == other,
                  QStringLiteral("② 编码转换：框里已有值 ⇒ **继承优先**"));
            check(start_of(page.source_dialog_start()) == src_dir,
                  QStringLiteral("② 编码转换：源本身没有输入目录 ⇒ 继承自己的值"));
        }
        // （2）文本提取：输出目录 ← 源目录
        {
            TextPage page;
            page.source_edit()->setText(src_dir);
            check(start_of(page.target_dialog_start()) == src_dir,
                  QStringLiteral("② 文本提取：源唯一 ⇒ 输出起点 = 源目录"));
            page.source_edit()->clear();
            check(start_of(page.target_dialog_start()) == exe_dir,
                  QStringLiteral("② 文本提取：源空 ⇒ 输出起点 = 工具目录"));
        }
        // （3）XML 校对：B ← A
        {
            XmlPage page;
            page.dir_a_edit()->setText(src_dir);
            check(start_of(page.dir_b_dialog_start()) == src_dir,
                  QStringLiteral("② XML 校对：A 唯一 ⇒ B 起点 = A"));
            page.dir_a_edit()->clear();
            check(start_of(page.dir_a_dialog_start()) == exe_dir,
                  QStringLiteral("② XML 校对：A 为空 ⇒ A 起点 = 工具目录"));
        }
        // （4）视频转换：输出目录 ← 源视频**所在目录**（文件语义）
        {
            VideoPage page;
            const QString movie = src_dir + QStringLiteral("/intro.avi");
            QFile(movie).open(QIODevice::WriteOnly);
            page.source_edit()->setText(movie);
            check(start_of(page.output_dialog_start()) == src_dir,
                  QStringLiteral("② 视频转换：源是**文件** ⇒ 输出起点 = 它所在目录"));
        }
        // （5）汉化包生成：字体/输出 ← 文本 XML 目录
        {
            FontPackPage page;
            // ★ 2026-10-06（D2 批，1.x `:129-149`）：字体框在**构造时**就填好了默认字体
            //   （上次用过的 → 系统雅黑 → 仓库根）⇒ 先测"继承上次的选择"这条优先级。
            const QString preset = page.font_edit()->text().trimmed();
            page.xml_edit()->setText(src_dir);
            check(preset.isEmpty() ||
                      start_of(page.font_dialog_start()) == QDir::cleanPath(preset),
                  QStringLiteral("② 汉化包生成：已填默认字体 ⇒ 字体起点 = 它自己（继承优先；"
                                 "实得「%1」，默认字体「%2」）")
                      .arg(start_of(page.font_dialog_start()), preset));
            // 再清掉字体框 ⇒ 回到"XML 唯一 ⇒ 起点 = XML 目录"这条（这才是原来那条判据）
            page.font_edit()->clear();
            check(start_of(page.font_dialog_start()) == src_dir &&
                      start_of(page.out_dialog_start()) == src_dir,
                  QStringLiteral("② 汉化包生成：XML 唯一 ⇒ 字体/输出起点 = XML 目录"));
            page.xml_edit()->clear();
            check(start_of(page.out_dialog_start()) == exe_dir,
                  QStringLiteral("② 汉化包生成：XML 空 ⇒ 输出起点 = 工具目录"));
        }
        // （6）文件系统：**用户点名的那个例子** —— 解包的目标目录 ← 来源包所在目录
        {
            FsPage page;
            page.add_source(pack_file, {{"a.txt", false}});
            check(start_of(page.unpack_output_dialog_start()) == pkg_dir,
                  QStringLiteral("② 文件系统：解包目标起点 = **来源包所在目录**（用户原话那条）"));
            reset_last_dialog_start();
            const QString first = start_of(page.unpack_output_dialog_start());
            check(last_dialog_start_calls() == 1 && last_dialog_start_dir() == first,
                  QStringLiteral("② 文件系统的起点也出自共用函数（%1）").arg(first));
            page.add_source(other + QStringLiteral("/second.db0"), {{"b.txt", false}});
            check(start_of(page.unpack_output_dialog_start()) == exe_dir,
                  QStringLiteral("② 文件系统：来源**多元** ⇒ 解包目标起点 = 工具目录"));
            page.out_edit()->setText(other);
            check(start_of(page.unpack_output_dialog_start()) == other,
                  QStringLiteral("② 文件系统：框里已有值 ⇒ 继承优先"));
        }
    }

    // ③ 日志栏存在、只读（1.x 口径：日志不归主窗口那根滚动条管）
    check(hub.log_view() != nullptr, QStringLiteral("日志栏存在"));
    check(hub.log_view() != nullptr && hub.log_view()->isReadOnly(),
          QStringLiteral("日志栏只读"));
    hub.log_line(QStringLiteral("smoke"));
    {
        // ★ 方案一（Hana 重排）：日志从标题栏改成底部折叠按钮
        check(hub.log_toggle() != nullptr && !hub.log_toggle()->text().isEmpty(),
              QStringLiteral("日志折叠按钮存在（实测 %1）")
                  .arg(hub.log_toggle() != nullptr ? hub.log_toggle()->text() : QStringLiteral("(无)")));
        check(hub.log_clear_button() != nullptr, QStringLiteral("日志面板要有清空按钮"));
        hub.log_line(QStringLiteral("待清空 A"));
        if (hub.log_box() != nullptr) hub.log_box()->flush();
        const int before = hub.log_box() != nullptr ? hub.log_box()->blockCount() : 0;
        if (hub.log_clear_button() != nullptr) hub.log_clear_button()->click();
        if (hub.log_box() != nullptr) hub.log_box()->flush();
        const int after = hub.log_box() != nullptr ? hub.log_box()->blockCount() : 0;
        check(before > 0 && after < before,
              QStringLiteral("点清空要真的清掉（%1 -> %2 行）").arg(before).arg(after));
        // ★ 本块插在原有的「日志能追加」断言**之前**，而清空会把内容清掉 ⇒ 这里补回一行，
        //   免得把后面那条既有断言弄红（顺序敏感是这种"就地插断言"的固有坑）。
        hub.log_line(QStringLiteral("smoke"));
        if (hub.log_box() != nullptr) hub.log_box()->flush();
    }

    if (hub.log_box() != nullptr) hub.log_box()->flush();  // 攒批控件：先落一次再断言
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

    // ⑤ QSS **占位错位**这一类事故不许再来（用户真机截图：控制台 88 条
    //    `QCssParser::parseHexColor: Unknown color name '#3a3f45px'`，同时 padding/圆角/
    //    勾选框尺寸静默失效）。三条判据，都是"能判红"的实测口径：
    //      ① 生成的样式表里**没有「颜色值 + px」**（错位的典型形态）；
    //      ② 没有未替换的命名令牌 `@{`、也没有残留的 `%N` 占位；
    //      ③ 真应用样式表时 Qt **一条** parseHexColor / Argument missing 警告都不发。
    {
        int color_px_hits = 0;
        int token_left = 0;
        int arg_left = 0;
        const QRegularExpression color_px(QStringLiteral("#[0-9a-fA-F]{6}px"));
        const QRegularExpression arg_marker(QStringLiteral("%[0-9]"));
        for (const xrkit::core::ThemeMode mode :
             {xrkit::core::ThemeMode::Dark, xrkit::core::ThemeMode::Light}) {
            const QString sheet = stylesheet_for(mode);
            // 逐处数，别只问"有没有"（修一处 ≠ 修完 —— 88 条那次就是只修了一处）
            auto it = color_px.globalMatch(sheet);
            while (it.hasNext()) {
                it.next();
                ++color_px_hits;
            }
            if (sheet.contains(QStringLiteral("@{"))) ++token_left;
            if (arg_marker.match(sheet).hasMatch()) ++arg_left;
        }
        check(color_px_hits == 0,
              QStringLiteral("⑤ 样式表里出现「颜色值 + px」%1 处（占位错位）").arg(color_px_hits));
        check(token_left == 0, QStringLiteral("⑤ 有未替换的命名令牌 @{...}（模板写错名字了）"));
        check(arg_left == 0, QStringLiteral("⑤ 样式表里还残留 %N 占位符"));

        static int qss_warnings = 0;
        qss_warnings = 0;
        const QtMessageHandler previous = qInstallMessageHandler(
            +[](QtMsgType, const QMessageLogContext&, const QString& text) {
                if (text.contains(QStringLiteral("parseHexColor")) ||
                    text.contains(QStringLiteral("Argument missing"))) {
                    ++qss_warnings;
                }
            });
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        apply_theme(*qApp, xrkit::core::ThemeMode::Light);
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        qInstallMessageHandler(previous);
        check(qss_warnings == 0,
              QStringLiteral("⑤ 应用样式表时 Qt 报了 %1 条 QSS 警告（应为 0）").arg(qss_warnings));

        // ③ 强调色（蓝）**只许出现在三处**：主按钮 / 当前标签项 / 进度条 ——
        //   口径见 `docs/2.0.0-界面重构口径(排版与配色).md` 第二节（用户原话
        //   "配色里为什么这么多蓝色"）。判据是**逐规则**看：凡是含 accent 色的规则，
        //   选择器必须在白名单里；多一处就红（不许"看着差不多"）。
        const QStringList accent_allowed = {
            QStringLiteral("QPushButton#primary"), QStringLiteral("QPushButton#primary:hover"),
            QStringLiteral("QPushButton#primary:pressed"),
            QStringLiteral("QTabBar#page_tabs::tab:selected"), QStringLiteral("QProgressBar::chunk")};
        QStringList accent_strays;
        for (const xrkit::core::ThemeMode mode :
             {xrkit::core::ThemeMode::Dark, xrkit::core::ThemeMode::Light}) {
            const QString accent_hex =
                QString::fromUtf8(xrkit::core::theme_color(mode, "accent"));
            const QString sheet = stylesheet_for(mode);
            const QStringList rules = sheet.split(QLatin1Char('}'));
            for (const QString& rule : rules) {
                if (!rule.contains(accent_hex, Qt::CaseInsensitive)) continue;
                const QString selector = rule.section(QLatin1Char('{'), 0, 0).trimmed();
                if (!accent_allowed.contains(selector)) accent_strays << selector;
            }
        }
        check(accent_strays.isEmpty(),
              QStringLiteral("③ 强调色跑到这三处之外了：%1")
                  .arg(accent_strays.join(QStringLiteral(" / "))));
    }

    // ④-b ★ 2026-10-06：**主题 / 界面大小要"记住上次的选择"**（用户原话：「主题和字体大小
    //   目前并没有保留继承的功能（即下次开启又是默认的方案）」）。判据是"先证红再改绿"那种：
    //   ① 改主题 ⇒ `user.ltx` 里 `[theme] mode` 变成新值 ⇒ **重新读回**（= 重启路径）拿到同一主题；
    //   ② 界面大小档位同样；③ 另**预置一个别的键**（`[ffmpeg] path`），断言那次改动**只动了该动的键**。
    //   ⚠ 这一段会真的写 `user.ltx` ⇒ 结束后**原样复原**（发行包会在包目录里跑 `--smoke`，
    //     留一个 user.ltx 进去等于往包里塞用户设置）。
    {
        const QString ltx = QString::fromStdString(xrkit::core::user_ltx_path());
        const bool ltx_existed = QFileInfo::exists(ltx);
        QByteArray ltx_before;
        bool ltx_backed_up = false;                     // ★ 备份到底读到没有（B1-7）
        if (ltx_existed) {
            QFile before(ltx);
            if (before.open(QIODevice::ReadOnly)) {
                ltx_before = before.readAll();
                ltx_backed_up = true;
            }
        }
        // 预置一个**别的**段/键（"只动该动的键"那条判据的哨兵）
        check(xrkit::core::save_user_value("ffmpeg", "path", "E:/sentinel/ffmpeg.exe"),
              QStringLiteral("④-b 夹具：[ffmpeg] path 写进去了（哨兵键）"));

        // ── 主题：改 ⇒ 落盘 ⇒ 读回 ──
        QString problem;
        hub.apply_theme_mode(xrkit::core::ThemeMode::Light, &problem);
        const QString mode_raw =
            QString::fromStdString(xrkit::core::load_user_value("theme", "mode", ""));
        check(problem.isEmpty() && mode_raw == QStringLiteral("light"),
              QStringLiteral("④-b 改主题 ⇒ user.ltx 的 [theme] mode=light（实得「%1」，problem=「%2」）")
                  .arg(mode_raw, problem));
        check(xrkit::core::load_theme_mode(xrkit::core::ThemeMode::Dark) ==
                  xrkit::core::ThemeMode::Light,
              QStringLiteral("④-b **重启路径**：load_theme_mode() 读回同一主题（light）"));
        hub.apply_theme_mode(xrkit::core::ThemeMode::Dark, &problem);
        check(problem.isEmpty() &&
                  QString::fromStdString(xrkit::core::load_user_value("theme", "mode", "")) ==
                      QStringLiteral("dark") &&
                  xrkit::core::load_theme_mode(xrkit::core::ThemeMode::Light) ==
                      xrkit::core::ThemeMode::Dark,
              QStringLiteral("④-b 来回切一次，读回也跟着变（dark）"));

        // ── 界面大小档位：改 ⇒ 落盘 ⇒ 读回（取值集合与默认值都不许变）──
        const auto& choices = xrkit::shell::ui_size_choices();
        check(choices.size() == 4 && choices.front().scale == 0.9 && choices.back().scale == 1.2,
              QStringLiteral("④-b 界面大小档位仍是 4 档 0.9/1.0/1.1/1.2（不许被这次改动挪动）"));
        const double want_scale = 1.1;
        hub.apply_ui_size(want_scale, &problem);
        const QString scale_raw =
            QString::fromStdString(xrkit::core::load_user_value("theme", "scale", ""));
        check(problem.isEmpty() && scale_raw == QStringLiteral("1.1"),
              QStringLiteral("④-b 改界面大小 ⇒ user.ltx 的 [theme] scale=1.1（实得「%1」，problem=「%2」）")
                  .arg(scale_raw, problem));
        check(xrkit::core::load_ui_scale() == want_scale,
              QStringLiteral("④-b **重启路径**：load_ui_scale() 读回 1.1（实得 %1）")
                  .arg(xrkit::core::load_ui_scale()));
        check(qAbs(xrkit::core::ui_scale() - want_scale) < 1e-9,
              QStringLiteral("④-b 档位也真的生效在 core::ui_scale()（实得 %1）")
                  .arg(xrkit::core::ui_scale()));

        // ── 只动该动的键：哨兵键与 [font] 段必须原样还在 ──
        check(QString::fromStdString(xrkit::core::load_user_value("ffmpeg", "path", "")) ==
                  QStringLiteral("E:/sentinel/ffmpeg.exe"),
              QStringLiteral("④-b 写主题/档位**没有动** [ffmpeg] path（只动该动的键）"));
        // 主题与档位都写在**同一个** [theme] 段里，但互相不许踩
        hub.apply_theme_mode(xrkit::core::ThemeMode::Light, &problem);
        check(QString::fromStdString(xrkit::core::load_user_value("theme", "scale", "")) ==
                  QStringLiteral("1.1"),
              QStringLiteral("④-b 只改主题时 [theme] scale 不被抹掉"));
        hub.apply_ui_size(0.9, &problem);
        check(QString::fromStdString(xrkit::core::load_user_value("theme", "mode", "")) ==
                  QStringLiteral("light"),
              QStringLiteral("④-b 只改档位时 [theme] mode 不被抹掉"));

        // 收到默认态（自检不留状态：后面的用例仍按"没有偏好"跑）
        hub.apply_theme_mode(xrkit::core::ThemeMode::Dark, &problem);
        hub.apply_ui_size(1.0, &problem);
        if (ltx_existed) {
            QFile restore(ltx);
            if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) restore.write(ltx_before);
        } else {
            QFile::remove(ltx);
        }
        QString ltx_why_4b;
        const bool ltx_ok_4b = user_ltx_restored(ltx, ltx_existed, ltx_before, &ltx_why_4b);
        check(!ltx_existed || ltx_backed_up,
              QStringLiteral("④-b 自检前**真的读到了** user.ltx 的备份（读失败 ⇒ 还原判据改判红）"));
        check(ltx_ok_4b,
              QStringLiteral("④-b 自检把 user.ltx **逐字节**还原（存在=%1；%2）")
                  .arg(ltx_existed ? 1 : 0)
                  .arg(ltx_ok_4b ? QStringLiteral("一致") : ltx_why_4b));
    }

    // ④-c ★★ 2026-10-06（H-1 / H-2）：**启动序列本身**必须能在自检里复现（判红锁）。
    //   为什么这一段非有不可：修前那条序列**内联在 `src/shell/main.cpp` 的 main() 里**，
    //   而 `--smoke` 走的是另一条分支、压根不经过交互式启动 ⇒ 这两条判据在门禁里
    //   **验不到**，于是两条都真机红：
    //     · H-1 `apply_theme` 在"保存档覆盖 ui_scale"**之前**调用、之后没重套 ⇒
    //       控件几何按 1.2、字体按 1.0（用户红线："字体大小也要保留继承"）；
    //     · H-2 `apply_ui_size` 读 `app.property("xrkit_theme")`，而那个属性**只有**
    //       `Hub::apply_theme_mode`（手动切主题）才写 ⇒ 浅色用户第一次点「界面大小」
    //       读到空串 ⇒ 走 Dark ⇒ 整界面被拍回暗色（用户红线："主题……要保留继承"）。
    //   现在的判据钉在**与 main 共用的那个函数**上（`apply_startup_ui`）：预置
    //   `[theme] mode=light / scale=1.2` → 调它 → 断言"字体与几何同源"+"点界面大小仍是浅色"。
    //   ⚠ 会写 user.ltx ⇒ 跑完**原样复原**（发行包会在包目录里跑 `--smoke`）。
    {
        const QString ltx = QString::fromStdString(xrkit::core::user_ltx_path());
        const bool ltx_existed = QFileInfo::exists(ltx);
        QByteArray ltx_before;
        bool ltx_backed_up = false;                     // ★ 备份到底读到没有（B1-7）
        if (ltx_existed) {
            QFile before(ltx);
            if (before.open(QIODevice::ReadOnly)) {
                ltx_before = before.readAll();
                ltx_backed_up = true;
            }
        }
        // "同源"那条判据的参照：**别写死字号** —— 先在 ui_scale=1.0 下量一次，
        // 之后字号必须**恰好**乘 1.2（写成常量就等于把某个 Qt 默认字号钉进判据）。
        xrkit::core::set_ui_scale(1.0);
        apply_theme(*qApp, xrkit::core::ThemeMode::Light);
        const double font_at_1 = qApp->font().pointSizeF();
        check(xrkit::core::save_user_value("theme", "mode", "light") &&
                  xrkit::core::save_user_value("theme", "scale", "1.2"),
              QStringLiteral("④-c 夹具：user.ltx 预置 [theme] mode=light / scale=1.2"));

        const xrkit::core::ThemeMode started = apply_startup_ui(*qApp);
        const double font_at_12 = qApp->font().pointSizeF();
        check(started == xrkit::core::ThemeMode::Light &&
                  xrkit::core::theme_mode() == xrkit::core::ThemeMode::Light,
              QStringLiteral("④-c H-1/H-2 启动序列读回浅色偏好并把它设成 core 的真值"));
        check(qAbs(xrkit::core::ui_scale() - 1.2) < 1e-9,
              QStringLiteral("④-c H-1 保存档压过屏幕自适应（ui_scale 应 1.2，实得 %1）")
                  .arg(xrkit::core::ui_scale()));
        check(px(100) == 120,
              QStringLiteral("④-c H-1 几何按 1.2（px(100) 应 120，实得 %1）").arg(px(100)));
        check(qAbs(font_at_12 - font_at_1 * 1.2) < 1e-6,
              QStringLiteral("④-c H-1 **字体与几何同源**：字号也要按 1.2（1.0 时 %1pt / 1.2 时 "
                             "%2pt，期望 %3pt）")
                  .arg(font_at_1, 0, 'f', 4)
                  .arg(font_at_12, 0, 'f', 4)
                  .arg(font_at_1 * 1.2, 0, 'f', 4));

        // H-2：**如实重现启动态** —— 进程刚起来时 `xrkit_theme` 属性是空的（启动路径只写 core）
        qApp->setProperty("xrkit_theme", QVariant());
        QString problem;
        hub.apply_ui_size(1.1, &problem);   // 用户点「界面大小」= 走 Hub 这条真实路径
        check(problem.isEmpty(),
              QStringLiteral("④-c H-2 点「界面大小」没报错（problem=「%1」）").arg(problem));
        check(xrkit::core::theme_mode() == xrkit::core::ThemeMode::Light,
              QStringLiteral("④-c H-2 点「界面大小」**不许翻主题**（core 真值应仍是 light，实得 %1）")
                  .arg(QString::fromUtf8(
                      xrkit::core::theme_mode_name(xrkit::core::theme_mode()))));
        check(qApp->palette().color(QPalette::Window).name().compare(
                  QString::fromUtf8(xrkit::core::theme_color(xrkit::core::ThemeMode::Light, "bg")),
                  Qt::CaseInsensitive) == 0,
              QStringLiteral("④-c H-2 点「界面大小」之后 palette 仍是浅色令牌（实得 %1）")
                  .arg(qApp->palette().color(QPalette::Window).name()));

        // 复位（后面的用例仍按"暗色 + 1.0"跑），并把 user.ltx 原样还原
        xrkit::core::set_ui_scale(1.0);
        apply_theme(*qApp, xrkit::core::ThemeMode::Dark);
        hub.apply_ui_size(1.0, &problem);
        qApp->setProperty("xrkit_theme", QVariant());
        if (ltx_existed) {
            QFile restore(ltx);
            if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) restore.write(ltx_before);
        } else {
            QFile::remove(ltx);
        }
        QString ltx_why;
        const bool ltx_restore_ok = user_ltx_restored(ltx, ltx_existed, ltx_before, &ltx_why);
        check(!ltx_existed || ltx_backed_up,
              QStringLiteral("④-c 自检前**真的读到了** user.ltx 的备份（读失败 ⇒ 下面的还原判据"
                             "会把 0==0 读成「还原成功」，那是假绿）"));
        check(ltx_restore_ok,
              QStringLiteral("④-c 自检把 user.ltx **逐字节**还原（存在=%1；%2）")
                  .arg(ltx_existed ? 1 : 0)
                  .arg(ltx_restore_ok ? QStringLiteral("一致") : ltx_why));
    }

    // ★★ 2026-10-06（审计处理批·0-3 之 B1-7 自证）：上面 ④-b/④-c/D2-字体 三处"还原"判据
    //   现在都走 `user_ltx_restored()`（逐字节比 + 备份缺失即红）。这里给它**正反两个夹具**，
    //   证"同长度写坏也会红"这句话不是空话（旧判据只比长度 ⇒ 同长度写坏绿）。
    //   靶子是临时目录，**不碰**真实 user.ltx。
    {
        const QString probe_path =
            QDir::temp().filePath(QStringLiteral("xrkit_smoke_ltx_restore_probe.txt"));
        QFile::remove(probe_path);
        const bool wrote = [&]() {
            QFile f(probe_path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
            return f.write(QByteArray("AAAA")) == 4;
        }();
        QString why_ok;
        QString why_bad;
        const QByteArray backup("AAAA");
        const bool same_ok = user_ltx_restored(probe_path, true, backup, &why_ok);   // 应为真
        // 同长度、内容不同 ⇒ 必须红（旧判据 `size()==size()` 在这里是绿的）
        QFile mutate(probe_path);
        const bool mutated = mutate.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                             mutate.write(QByteArray("BBBB")) == 4;
        mutate.close();
        const bool same_len_dirty = user_ltx_restored(probe_path, true, backup, &why_bad);  // 应为假
        // 备份没读到（空 before）⇒ 也必须红，不许把 0==0 读成"还原成功"
        const bool empty_backup = user_ltx_restored(probe_path, true, QByteArray(), &why_bad);
        QFile::remove(probe_path);
        check(wrote && mutated && same_ok && !same_len_dirty && !empty_backup,
              QStringLiteral("⑭ user.ltx 还原判据自证：一致=%1（应 1）/ 同长度写坏=%2（应 0，"
                             "旧判据在这里是绿的）/ 备份缺失=%3（应 0）")
                  .arg(same_ok ? 1 : 0)
                  .arg(same_len_dirty ? 1 : 0)
                  .arg(empty_backup ? 1 : 0));
    }

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

    // ⑦ 滚动三闸门（M2-2）：对照表 ①1.1~1.8 —— **没有滚动条就不许滚**、装得下不占条、
    //    同一滚轮事件只有一个条子动、全局路由到第一个能滚的视口。
    //    ★ 夹具用**裸顶层 + 矮窗口**（3.5：Hub 式布局永远压不矮 ⇒ 永远测不出）。
    {
        const auto send_wheel = [](QWidget* target, int delta) {
            ScrollViewport::reset_wheel_claims();
            QWheelEvent event(QPointF(5, 5), target->mapToGlobal(QPoint(5, 5)), QPoint(0, 0),
                              QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(target, &event);
            return event.isAccepted();
        };

        // ⑦-a 装得下 ⇒ **不占条**且**不许滚**（1.1/1.2/1.7）
        QWidget short_top;
        short_top.resize(300, 120);
        auto* small_body = new QWidget;
        small_body->setMinimumSize(200, 40);
        auto* small_vp = new ScrollViewport(&short_top);
        small_vp->setGeometry(0, 0, 300, 120);
        small_vp->set_fit(ScrollViewport::Fit::Viewport);
        small_vp->set_body(small_body);
        small_vp->sync_layout();
        short_top.show();
        small_vp->show();   // ★ 必须 show：`bar_shown` 四看里有"已 mapped"，不 show 会**假通过**
        QApplication::processEvents();

        // ★ 用户 2026-09-30：「很多UI控件可以稍微硬朗一些，现在有点太圆润了。」
        //   圆角令牌本来就小（r2/r3）⇒ 圆润来自平台原生样式 + **这几类控件我们原本没写 QSS**。
        //   判据：标签栏/分组框/表头/菜单现在必须有规则（修前一条都没有 ⇒ 红）。
        //   （样式名探不出来：Fusion 被 QStyleSheetStyle 包住，objectName/name 都回 QStyleSheetStyle，
        //    除非引 Qt 私有头 ⇒ 改为钉住这些控件有 QSS，再靠既有的 QSS 警告必须为 0 那条守语法。）
        {
            const QString qss =
                xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
            const QStringList need{QStringLiteral("QTabBar::tab"),
                                   QStringLiteral("QGroupBox"),
                                   QStringLiteral("QHeaderView::section"),
                                   QStringLiteral("QToolTip")};
            QStringList missing;
            for (const QString& frag : need) {
                if (!qss.contains(frag)) missing << frag;
            }
            check(missing.isEmpty(),
                  QStringLiteral("硬朗化：原来没写 QSS 的控件现在有规则（缺 %1）")
                      .arg(missing.isEmpty() ? QStringLiteral("无")
                                             : missing.join(QStringLiteral(","))));
        }

        // ★ 用户 2026-09-30：「主题之外，再增加一个调整 UI 控件大小（包括里面的字体）的下拉」
        //   判据：缩放 1.25 ⇒ `ui_scale()==1.25`、字体点数变大、`px(100)==125`；再调回 1.0 ⇒ 复原。
        {
            const double font_before = QApplication::font().pointSizeF();
            const double scale_before = xrkit::core::ui_scale();
            xrkit::shell::apply_ui_size(*qApp, 1.25);
            const double font_after = QApplication::font().pointSizeF();
            check(std::abs(xrkit::core::ui_scale() - 1.25) < 0.001 && font_after > font_before &&
                      xrkit::core::px(100) == 125,
                  QStringLiteral("界面大小缩放：ui_scale %1→1.25、字体 %2→%3、px(100)=%4")
                      .arg(scale_before)
                      .arg(font_before)
                      .arg(font_after)
                      .arg(xrkit::core::px(100)));
            xrkit::shell::apply_ui_size(*qApp, 1.0);   // 复原，别影响后面的判据
            // ★ 用户 2026-09-30：「小有点偏小，默认有点偏大，大和特大就不用说了。」⇒ 档位必须是
            //   0.9 / 1.0 / 1.1 / 1.2（修前是 0.85/1.0/1.25/1.5 ⇒ 这条红）。
            {
                const auto& choices = xrkit::shell::ui_size_choices();
                const bool ladder_ok =
                    choices.size() == 4 && std::abs(choices[0].scale - 0.9) < 0.001 &&
                    std::abs(choices[1].scale - 1.0) < 0.001 &&
                    std::abs(choices[2].scale - 1.1) < 0.001 &&
                    std::abs(choices[3].scale - 1.2) < 0.001;
                check(ladder_ok,
                      QStringLiteral("界面大小档位＝0.9/1.0/1.1/1.2（实测 %1 档：%2/%3/%4/%5）")
                          .arg(choices.size())
                          .arg(choices.size() > 0 ? choices[0].scale : -1.0)
                          .arg(choices.size() > 1 ? choices[1].scale : -1.0)
                          .arg(choices.size() > 2 ? choices[2].scale : -1.0)
                          .arg(choices.size() > 3 ? choices[3].scale : -1.0));
            }
            // ★ 用户 2026-09-30：「包和内容物的字体大小太小。」⇒ 内容区（树/列表）字号必须由样式表
            //   单独放大，且走令牌（跟着界面大小一起缩）。修前没有任何 item view 的字号规则 ⇒ 红。
            {
                const QString qss = xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
                check(qss.contains(QStringLiteral("QTreeWidget, QTreeView, QListView { font-size")) ||
                          qss.contains(QStringLiteral("font-size: @{font_content}")),
                      QStringLiteral("内容区字号要单独放大（样式表里要有 font_content 规则）"));
            }
            check(std::abs(xrkit::core::ui_scale() - 1.0) < 0.001,
                  QStringLiteral("界面大小缩放：调回 1.0 要复原（实测 %1）")
                      .arg(xrkit::core::ui_scale()));
        }

        check(!small_vp->can_scroll_vertical(), QStringLiteral("装得下 ⇒ 范围为空（不能滚）"));
        check(!ScrollViewport::bar_shown(small_vp->vbar()),
              QStringLiteral("装得下 ⇒ 条子不占位（bar_shown 四看不全真）"));
        send_wheel(small_body, -120);
        check(small_vp->offset_vertical() == 0, QStringLiteral("装得下时偏移恒为 0"));

        // ⑦-a2 **控件级拦**（1.1 的真正考点）：body 里放一个"会自己滚"的原生滚动区
        //（QPlainTextEdit），它的条子不可见时**不许它自己滚** —— 1.x 的原文是
        // "Tk 的 Text/Listbox 类绑定根本不看我们的条子"，Qt 的 QAbstractScrollArea 同理。
        auto* text_host = new QWidget;
        text_host->setMinimumSize(200, 40);
        auto* native_text = new QPlainTextEdit(text_host);
        native_text->setGeometry(0, 0, 180, 30);
        native_text->setPlainText(QString(200, QLatin1Char('x')));  // 内容远超过 30px
        native_text->setLineWrapMode(QPlainTextEdit::NoWrap);
        auto* text_vp = new ScrollViewport(&short_top);
        text_vp->setGeometry(0, 0, 300, 120);
        text_vp->set_fit(ScrollViewport::Fit::Viewport);
        text_vp->set_body(text_host);
        short_top.show();
        text_vp->show();
        QApplication::processEvents();
        native_text->verticalScrollBar()->setValue(0);
        check(ScrollViewport::bar_shown(text_vp->vbar()) == false,
              QStringLiteral("控件级拦：视口条子不可见（前置条件）"));
        // ★ 2026-09-29 **口径更新**（用户："小滚动条在不隐藏的时候也没法滚，还是控制的整个界面的
        //   滚动条"）：原生滚动区**条子可见且能滚**时，必须**它自己滚**，事件被吃掉、
        //   **外层不许跟着动**。旧口径是"可见 ⇒ 不干预，让它冒泡"，正是用户报的那个现象。
        //   判红办法：把 `route_wheel` 里"可见且能滚 ⇒ 自己滚"那一段去掉（恢复 return false）。
        check(send_wheel(native_text, -120),
              QStringLiteral("控件级拦：事件被吃掉（不再冒泡给外层）"));
        check(native_text->verticalScrollBar()->value() > 0,
              QStringLiteral("条子可见且能滚 ⇒ **它自己滚**（实测 %1，期望 >0）")
                  .arg(native_text->verticalScrollBar()->value()));
        check(text_vp->offset_vertical() == 0,
              QStringLiteral("外层视口不许跟着动（实测偏移 %1，期望 0）")
                  .arg(text_vp->offset_vertical()));

        // ⑦-b 能滚 ⇒ **必须看得见条**（1.7 的另一向）。内容高用 fit=Content + 高的 body
        auto* tall_body = new QWidget;
        auto* tall_lay = new QVBoxLayout(tall_body);
        auto* tall_ink = new QWidget;
        tall_ink->setFixedHeight(600);  // 请求高由布局反射出来（裸 QWidget 的 sizeHint 无效）
        tall_lay->addWidget(tall_ink);
        auto* tall_vp = new ScrollViewport(&short_top);
        tall_vp->setGeometry(0, 0, 300, 120);
        tall_vp->set_fit(ScrollViewport::Fit::Content);
        tall_vp->set_body(tall_body);
        tall_vp->sync_layout();
        tall_vp->show();
        QApplication::processEvents();
        check(tall_vp->can_scroll_vertical(), QStringLiteral("装不下 ⇒ 能滚"));
        {
            const QScrollBar* tb = tall_vp->vbar();
            check(ScrollViewport::bar_shown(tb),
                  QStringLiteral("能滚 ⇒ 条子四看全真（hidden=%1 visToParent=%2 vis=%3 %4x%5）")
                      .arg(tb->isHidden())
                      .arg(tb->isVisibleTo(tall_vp))
                      .arg(tb->isVisible())
                      .arg(tb->width())
                      .arg(tb->height()));
        }

        // ★★ P5（2026-10-07 自检抓到）：**拖条子/点箭头也必须挪画布** —— 修前普通视口没有
        //   `valueChanged` 连接（只有"自带滚动的画布"那条路接过），所以**只有滚轮能动画面**，
        //   拖滑块完全没反应。这条锁就是那个用户可见缺陷。
        {
            tall_vp->vbar()->setValue(0);
            const int y_top = tall_body->y();
            tall_vp->vbar()->setValue(tall_vp->vbar()->maximum());
            check(tall_body->y() < y_top,
                  QStringLiteral("条子拖到底 ⇒ 画布上移（修前 y 恒为 %1）").arg(y_top));
            tall_vp->vbar()->setValue(0);
            check(tall_body->y() == y_top, QStringLiteral("条子回顶 ⇒ 画布回原位"));
        }

        // ⑦-c 看得见时滚轮真的动，且到底夹住
        check(send_wheel(tall_body, -120), QStringLiteral("能滚时滚轮被本视口消费"));
        check(tall_vp->offset_vertical() > 0, QStringLiteral("滚轮把偏移推大了"));
        for (int i = 0; i < 60; ++i) send_wheel(tall_body, -120);
        check(tall_vp->offset_vertical() == tall_vp->vbar()->maximum(),
              QStringLiteral("连续下滚夹在 maximum"));
        for (int i = 0; i < 60; ++i) send_wheel(tall_body, 120);
        check(tall_vp->offset_vertical() == 0, QStringLiteral("连续上滚夹在 0"));

        // ⑦-c2 ★ 用户实测缺陷的**能红断言**（2026-09-27）：连续两个滚轮事件（**不重置认领表**）
        //      都必须真的动。旧实现拿 `QWheelEvent` 的**对象地址**当事件身份，而 Qt 会复用槽位
        //      ⇒ 第二个事件地址与上一个相同 ⇒ 被判成"已被认领"并**撤销滚动** ⇒ 表现就是
        //      "滚下去之后滚不回来"。注意：上面那个 `send_wheel` 每次都 `reset_wheel_claims()`，
        //      所以它天然测不出这个缺陷 —— 这条断言**故意不重置**。
        {
            const auto send_wheel_raw = [](QWidget* target, int delta) {
                QWheelEvent event(QPointF(5, 5), target->mapToGlobal(QPoint(5, 5)), QPoint(0, 0),
                                  QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                                  Qt::NoScrollPhase, false);
                QApplication::sendEvent(target, &event);
                return event.isAccepted();
            };
            // 先回到顶部，再用"不重置"的方式滚：下 → 上，两次都要动
            tall_vp->vbar()->setValue(0);
            ScrollViewport::reset_wheel_claims();
            const int v0 = tall_vp->vbar()->value();
            check(send_wheel_raw(tall_body, -120) && tall_vp->vbar()->value() != v0,
                  QStringLiteral("连续滚轮①：第一个事件要真的动（v=%1）").arg(v0));
            const int v1 = tall_vp->vbar()->value();
            // 真实场景里两次滚轮之间**必然有一次事件循环**（认领表的"本派发生效"就是靠它失效的）
            QApplication::processEvents();
            check(send_wheel_raw(tall_body, 120),
                  QStringLiteral("连续滚轮②：第二个事件必须被消费（旧逻辑在这里就永久判重了）"));
            check(tall_vp->vbar()->value() != v1,
                  QStringLiteral("连续滚轮③：**第二个事件也要真的动** —— 旧逻辑（地址当身份、无时间窗）"
                                 "会把它撤销掉，这正是用户看到的'滚下去滚不回来'"));
        }

        // ⑦-d fit 规则（3.2/3.3）
        auto* content_body = new QWidget;
        auto* content_lay = new QVBoxLayout(content_body);
        auto* content_ink = new QWidget;
        content_ink->setFixedHeight(300);
        content_lay->addWidget(content_ink);
        auto* content_vp = new ScrollViewport(&short_top);
        content_vp->setGeometry(0, 0, 300, 120);
        content_vp->set_fit(ScrollViewport::Fit::Viewport);
        content_vp->set_body(content_body);
        content_vp->set_min_body_height(200);
        content_vp->sync_layout();
        content_vp->show();
        QApplication::processEvents();
        const int viewport_fit_body_h = content_body->height();
        content_vp->set_fit(ScrollViewport::Fit::Content);
        content_vp->sync_layout();
        const int content_fit_body_h = content_body->height();
        check(viewport_fit_body_h == 200,
              QStringLiteral("fit=viewport ⇒ 内容高 = max(minsize, 视口高)"));
        check(content_fit_body_h >= viewport_fit_body_h,
              QStringLiteral("fit=content ⇒ 不小于 viewport（还要吃内容请求高）"));

        // ⑦-e 显式整组重排（1.8）：竖条贴右、横条贴底、画布吃剩余
        ScrollViewport layout_vp;
        layout_vp.resize(400, 200);
        auto* layout_body = new QWidget;
        layout_body->setMinimumSize(800, 600);
        layout_vp.set_fit(ScrollViewport::Fit::Content);
        layout_vp.set_body(layout_body);
        layout_vp.sync_layout();
        check(layout_vp.vbar()->geometry().right() == layout_vp.width() - 1,
              QStringLiteral("竖条贴右"));
        check(layout_vp.hbar()->geometry().bottom() == layout_vp.height() - 1,
              QStringLiteral("横条贴底"));
        check(layout_vp.body()->width() >= layout_vp.width() - layout_vp.vbar()->width(),
              QStringLiteral("画布吃剩余宽"));

        // ⑦-f 嵌套：同一滚轮事件**只有一个条子动**（1.3/1.4）；内层装得下 ⇒ 路由给外层（1.5）
        QWidget nested_top;
        nested_top.resize(400, 200);
        auto* outer = new ScrollViewport(&nested_top);
        outer->setGeometry(0, 0, 400, 200);
        outer->set_fit(ScrollViewport::Fit::Content);
        auto* outer_body = new QWidget;
        auto* outer_lay = new QVBoxLayout(outer_body);
        auto* outer_ink = new QWidget;
        outer_ink->setFixedHeight(900);
        outer_lay->addWidget(outer_ink);
        outer->set_body(outer_body);
        auto* inner = new ScrollViewport(outer_body);
        inner->setGeometry(0, 0, 300, 150);
        inner->set_fit(ScrollViewport::Fit::Content);
        auto* inner_body = new QWidget;
        auto* inner_lay = new QVBoxLayout(inner_body);
        auto* inner_ink = new QWidget;
        inner_ink->setFixedHeight(400);
        inner_lay->addWidget(inner_ink);
        inner->set_body(inner_body);
        outer->sync_layout();
        inner->sync_layout();
        nested_top.show();
        outer->show();
        inner->show();
        QApplication::processEvents();

        const int outer_before = outer->offset_vertical();
        check(send_wheel(inner_body, -120), QStringLiteral("嵌套：事件被内层消费"));
        check(inner->offset_vertical() > 0, QStringLiteral("嵌套：内层动了"));
        check(outer->offset_vertical() == outer_before,
              QStringLiteral("嵌套：外层没动（同一事件只有一个条子动）"));

        inner_ink->setFixedHeight(40);  // 变成"装得下"
        inner->sync_layout();
        QApplication::processEvents();
        check(!inner->can_scroll_vertical(), QStringLiteral("嵌套：内层改成装得下"));
        const int inner_before = inner->offset_vertical();
        check(send_wheel(inner_body, -120), QStringLiteral("嵌套：事件被外层消费"));
        check(inner->offset_vertical() == inner_before, QStringLiteral("嵌套：内层没动"));
        check(outer->offset_vertical() > outer_before,
              QStringLiteral("嵌套：装得下的内层把滚轮让给外层"));

        // ⑦-g ★★ 用户 2026-09-30 真机缺陷的**能红断言**（原话：「把日志调出来，光标放在小滚动条
        //      的窗口内滚轮就不是控制小滚动条而是控制大滚动条，把日志收起就正常。日志内也是正常。」）
        //      机制：树/文本框是 `QAbstractScrollArea` 而**不是** `ScrollViewport`，旧实现第一圈
        //      "找第一个能滚的视口"会把它们整圈跳过 ⇒ 直接命中**祖先页视口**并把它滚了；
        //      "日志一收起就正常"正是因为那时外层视口装得下（range==0），才轮到内层自己滚。
        //      判据用**公开决策入口**（`decide_wheel`，无副作用）—— 该滚谁的条子。
        //      判红办法：把 `decide_wheel` 改回"先只找 ScrollViewport、找不到再看原生滚动区"。
        {
            QWidget native_top;
            native_top.resize(400, 200);
            auto* native_outer = new ScrollViewport(&native_top);
            native_outer->setGeometry(0, 0, 400, 200);
            native_outer->set_fit(ScrollViewport::Fit::Content);
            auto* native_body = new QWidget;
            auto* native_lay = new QVBoxLayout(native_body);
            auto* native_ink = new QWidget;
            native_ink->setFixedHeight(900);
            native_lay->addWidget(native_ink);
            auto* inner_tree = new QTreeWidget(native_body);
            inner_tree->setObjectName(QStringLiteral("smoke_native_tree"));
            inner_tree->setColumnCount(1);
            for (int i = 0; i < 60; ++i) {
                inner_tree->addTopLevelItem(
                    new QTreeWidgetItem(QStringList{QStringLiteral("row %1").arg(i)}));
            }
            native_lay->addWidget(inner_tree);
            native_outer->set_body(native_body);
            native_outer->sync_layout();
            native_top.show();
            native_outer->show();
            QApplication::processEvents();

            QScrollBar* tree_bar = inner_tree->verticalScrollBar();
            check(native_outer->can_scroll_vertical() && tree_bar != nullptr &&
                      tree_bar->maximum() > tree_bar->minimum(),
                  QStringLiteral("⑦-g 前置：外层视口与内层树**都能滚**（树 max=%1）")
                      .arg(tree_bar != nullptr ? tree_bar->maximum() : -1));
            check(tree_bar != nullptr && ScrollViewport::bar_shown(tree_bar),
                  QStringLiteral("⑦-g 前置：内层树的条子看得见"));

            const ScrollViewport::WheelDecision tree_decision =
                ScrollViewport::decide_wheel(inner_tree->viewport(), -120, 0);
            check(tree_decision.bar == tree_bar && tree_decision.owner == inner_tree,
                  QStringLiteral("⑦-g 光标在树里 ⇒ 该滚的是**树自己的条子**（实得 owner=%1）")
                      .arg(tree_decision.owner == nullptr
                               ? QStringLiteral("nullptr")
                               : QString::fromLatin1(tree_decision.owner->metaObject()->className())));
            check(ScrollViewport::decide_wheel(native_body, -120, 0).bar == native_outer->vbar(),
                  QStringLiteral("⑦-g 光标在视口空白处 ⇒ 才轮到外层视口"));
            check(ScrollViewport::decide_wheel(native_ink, -120, 0).bar == native_outer->vbar(),
                  QStringLiteral("⑦-g 光标在**装得下**的普通控件上 ⇒ 外层视口（1.1 冒泡）"));

            // 真发一次：内层要真的动，外层**一步都不许动**（1.3 同一事件只一个条子动）
            const int outer_before = native_outer->offset_vertical();
            const int tree_before = tree_bar != nullptr ? tree_bar->value() : 0;
            check(send_wheel(inner_tree->viewport(), -120),
                  QStringLiteral("⑦-g 事件被内层消费（不再冒泡给外层）"));
            check(tree_bar != nullptr && tree_bar->value() > tree_before,
                  QStringLiteral("⑦-g 内层树自己滚了（%1 → %2，步进 %3）")
                      .arg(tree_before)
                      .arg(tree_bar != nullptr ? tree_bar->value() : -1)
                      .arg(tree_bar != nullptr ? tree_bar->singleStep() : -1));
            check(native_outer->offset_vertical() == outer_before,
                  QStringLiteral("⑦-g 外层视口一步没动（实测 %1，期望 %2）")
                      .arg(native_outer->offset_vertical())
                      .arg(outer_before));
        }
    }

    // ⑧ 分栏（M2-3）：对照表 ②（2.1~2.5/2.7）+ ④（4.2/4.3）。
    //    用户原话："拖拽的时候预览框不能只是一条线，而是要把那部分相应的背景也进行填充"。
    {
        QWidget pane_host;
        pane_host.resize(400, 200);
        auto* split = new SplitPane(SplitPane::Orient::Vertical, &pane_host);
        split->setGeometry(0, 0, 400, 200);
        auto* top_pane = new QWidget;
        auto* bottom_pane = new QWidget;
        split->add_pane(top_pane, 40);
        split->add_pane(bottom_pane, 60);
        pane_host.show();
        split->show();
        QApplication::processEvents();

        // 2.3 命中区 = max(6, px(5))
        check(split->sash_hit() == std::max(6, px(5)),
              QStringLiteral("2.3 sash 命中区 = max(6, px(5))（实测 %1）").arg(split->sash_hit()));
        const int sash0 = split->sash_pos(0);
        check(split->sash_at(sash0) == 0, QStringLiteral("命中 sash"));
        check(split->sash_at(sash0 + split->sash_hit() + 8) == -1, QStringLiteral("命中区外不命中"));

        // 2.1 拖动期**一步几何都不改**，只有预览位置变
        const int size_top = split->pane_size(0);
        const int size_bottom = split->pane_size(1);
        check(split->begin_drag(sash0), QStringLiteral("begin_drag 命中 sash"));
        split->update_drag(sash0 + 30);
        check(split->dragging() && split->preview_pos() == sash0 + 30,
              QStringLiteral("拖动期有预览位置"));
        check(split->pane_size(0) == size_top && split->pane_size(1) == size_bottom,
              QStringLiteral("2.1 拖动期几何不变（一步不改）"));
        check(top_pane->height() == size_top,
              QStringLiteral("2.1 拖动期控件几何也不变"));
        result.notes.append(QStringLiteral("2.1 拖动预览：位置 %1（原 sash %2），填色带覆盖 %3 px")
                                .arg(sash0 + 30)
                                .arg(sash0)
                                .arg(std::abs(30)));

        // 松手一次重排（2.1）
        const int clamps_before = split->clamp_count();
        split->end_drag(true);
        check(split->pane_size(0) == size_top + 30, QStringLiteral("松手后前一个 pane 吃掉差额"));
        check(split->clamp_count() == clamps_before + 1, QStringLiteral("松手只重排一次"));
        check(!split->dragging(), QStringLiteral("松手后不在拖动态"));

        // 2.4 clamp：拖到极右 ⇒ 前一个 pane 保住 minsize，后面的先挨饿
        split->begin_drag(split->sash_pos(0));
        split->update_drag(1000);
        split->end_drag(true);
        check(split->pane_size(0) >= split->minsize(0),
              QStringLiteral("2.4 前一个 pane 保住 minsize"));
        check(split->pane_size(0) + split->pane_size(1) == split->height() - 4,
              QStringLiteral("两格 + sash = 总高"));

        // 4.2 收起后**改窗口尺寸不弹回**；4.3 顺序（先记意图再 clamp）
        split->set_collapsed(1, true);
        check(split->is_collapsed(1) && split->pane_size(1) == 0, QStringLiteral("4.2 收起：尺寸 0"));
        pane_host.resize(400, 300);
        split->request_clamp();
        QThread::msleep(180);
        QApplication::processEvents();
        check(split->is_collapsed(1) && split->pane_size(1) == 0,
              QStringLiteral("4.2 改窗口尺寸后仍然收起（不弹回）"));
        split->set_collapsed(1, false);
        check(!split->is_collapsed(1) && split->pane_size(1) >= 60,
              QStringLiteral("4.2 展开：≥ 原最小值"));

        // 2.5 去抖 120 ms：连续请求只重排一次
        const int before_debounce = split->clamp_count();
        for (int i = 0; i < 5; ++i) split->request_clamp();
        check(split->clamp_count() == before_debounce,
              QStringLiteral("2.5 去抖窗口内不重排"));
        QThread::msleep(180);
        QApplication::processEvents();
        check(split->clamp_count() == before_debounce + 1,
              QStringLiteral("2.5 去抖窗口到点只重排一次"));

        // 2.7 成本口径**必须含重绘**（只跑 update_idletasks 会低估约一半）
        QElapsedTimer timer;
        timer.start();
        constexpr int kSteps = 20;
        for (int i = 0; i < kSteps; ++i) {
            split->begin_drag(split->sash_pos(0));
            split->update_drag(split->sash_pos(0) + (i % 7) + 1);
            split->repaint();  // 含重绘
            split->end_drag(false);
        }
        const double per_step = static_cast<double>(timer.nsecsElapsed()) / 1e6 / kSteps;
        result.notes.append(QStringLiteral("2.7 拖动一步（含重绘）= %1 ms（PySide6 试点 12~15 ms）")
                                .arg(per_step, 0, 'f', 2));
        check(per_step < 30.0,
              QStringLiteral("2.7 拖动一步（含重绘）%1 ms < 30 ms").arg(per_step, 0, 'f', 2));
    }

    // ⑨ 日志栏 + 任务壳（M2-4）：对照表 ④（4.1/4.4/4.5）+ ⑬（13.1~13.7）。
    {
        // 4.4 LogBox：攒批 80 ms、超 5000 行删到 3000 行、清空走 clear_log()
        LogBox log;
        log.resize(400, 200);
        log.show();
        QApplication::processEvents();
        check(log.isReadOnly(), QStringLiteral("4.4 日志栏只读"));
        check(log.minimumWidth() == 1, QStringLiteral("4.4 日志栏 width=1（不撑开布局）"));
        for (int i = 0; i < 500; ++i) log.append_batched(QStringLiteral("line %1").arg(i));
        check(log.pending() == 500 && log.toPlainText().isEmpty(),
              QStringLiteral("4.4 攒批：80 ms 内不动文档（pending=%1）").arg(log.pending()));
        QThread::msleep(160);
        QApplication::processEvents();
        check(log.flush_count() == 1, QStringLiteral("4.4 攒批：500 行只落一次（flush=%1）")
                                          .arg(log.flush_count()));
        check(log.blockCount() == 500, QStringLiteral("4.4 500 行都在（实测 %1）")
                                           .arg(log.blockCount()));
        log.clear_log();
        check(log.blockCount() == 1 && log.pending() == 0, QStringLiteral("4.4 clear_log 清空"));
        for (int i = 0; i < 6000; ++i) log.append_batched(QStringLiteral("x%1").arg(i));
        log.flush();
        check(log.trim_count() == 1 && log.blockCount() <= 3000,
              QStringLiteral("4.4 超 5000 行删到 3000（实测 %1 行，trim=%2）")
                  .arg(log.blockCount())
                  .arg(log.trim_count()));

        // 4.1 日志栏自带条子、不归主窗口那根管：Hub 的中央区**没有**窗口级视口
        Hub hub;
        check(hub.log_box() != nullptr && hub.log_box()->verticalScrollBar() != nullptr,
              QStringLiteral("4.1 日志栏有自己的条子"));
        check(qobject_cast<ScrollViewport*>(hub.centralWidget()) == nullptr,
              QStringLiteral("4.1 Hub 中央区不是窗口级视口（无窗口级滚动条）"));
        // ★ 方案一（Hana 重排）：日志默认折叠，点折叠按钮才展开；不再用两栏 minsize
        //   用 isHidden() 而非 isVisible()：离屏下 Hub 没 show，isVisible() 恒 false（父窗隐藏）
        check(hub.log_panel() != nullptr && hub.log_panel()->isHidden(),
              QStringLiteral("日志面板默认折叠"));
        if (hub.log_toggle() != nullptr) hub.log_toggle()->click();
        check(hub.log_panel() != nullptr && !hub.log_panel()->isHidden(),
              QStringLiteral("点折叠按钮后日志面板展开"));

        // ★★ 2026-10-06（③）Hub 日志**落盘 + 导出**（1.x `stalker_toolkit.py:355-391`）——
        //   改动前 `导出` / `set_log_file` 在 hub.cpp 里**零命中** ⇒ GUI 日志一条都不落盘。
        {
            // 先把 `core::set_log_file` 的全局状态**清空**再建 Hub：
            // 这样"Hub 构造期挂了落盘文件"这条判据就是**在测 Hub 自己**（拆掉 ctor 里那行 ⇒ 红），
            // 而不是被上一个 Hub 顺手挂上的状态兜着。
            xrkit::core::set_log_file(std::string());
            // ★ 判据要**确定性**：先把目录清空（否则上一轮自检留下的 `log_*.txt` 会让
            //   "第一次导出"也走自增分支，就测不到"同秒冲突"这条路径了）。清空之后，
            //   本块里连着两次导出必然落在**同一秒的同一个基名**上 ⇒ 第二次必然自增。
            QDir(smoke_log_dir).removeRecursively();
            QDir().mkpath(smoke_log_dir);
            Hub log_hub;
            check(!log_hub.log_file_path().isEmpty() && log_hub.log_file_active() &&
                      QFile::exists(log_hub.log_file_path()),
                  QStringLiteral("③ Hub 启动即挂上落盘日志（%1，active=%2）")
                      .arg(log_hub.log_file_path())
                      .arg(log_hub.log_file_active() ? 1 : 0));
            check(log_hub.log_export_button() != nullptr,
                  QStringLiteral("③ 日志面板里有「导出日志」按钮"));
            // 写一条**标记**：内存日志（日志栏）与落盘日志（core::log）两条通道都走
            const QString marker =
                QStringLiteral("SMOKE-MARKER-EXPORT-LOG-%1")
                    .arg(QDateTime::currentMSecsSinceEpoch());
            log_hub.log_line(marker);
            xrkit::core::log(xrkit::core::Level::Info, marker.toStdString());
            QString exported;
            QString why;
            const bool export_ok = log_hub.export_log(&exported, &why);
            check(export_ok && QFile::exists(exported),
                  QStringLiteral("③ 导出日志文件存在（%1；失败理由「%2」）").arg(exported, why));
            check(QFileInfo(exported).fileName().startsWith(QStringLiteral("log_")) &&
                      QFileInfo(exported).suffix() == QStringLiteral("txt"),
                  QStringLiteral("③ 导出文件名口径 = logs/log_YYYYmmdd_HHMMSS.txt（实得 %1）")
                      .arg(QFileInfo(exported).fileName()));
            QString body;
            {
                QFile file(exported);
                if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            }
            check(body.contains(marker) && body.contains(QStringLiteral("简略日志")) &&
                      body.contains(QStringLiteral("详细日志")),
                  QStringLiteral("③ 导出内容 = 内存日志 + 落盘日志**合并**（含刚写的标记；%1 字节）")
                      .arg(body.size()));

            // ★★ 本批：**同一秒内两次导出不许互相覆盖**（复核确认的真问题）。
            //   修前的形态：两次 `export_log` 的落点都按 `currentDateTime()` 精确到秒算 ⇒
            //   同一秒内第二次以 `Truncate` 打同一个名字 ⇒ **第一份被静默丢掉**。
            //   判据（三条一起才算过）：① 两次落点**不同**且**都还在盘上**；
            //   ② 两份内容**各自正确**（都含各自的标记 —— 证明不是"空壳文件"）；
            //   ③ 序号自增这条**可见**（日志栏里有落点行；返回值就是最终落点）。
            {
                const QString marker2 =
                    QStringLiteral("SMOKE-MARKER-SAME-SECOND-EXPORT-%1")
                        .arg(QDateTime::currentMSecsSinceEpoch());
                log_hub.log_line(marker2);
                xrkit::core::log(xrkit::core::Level::Info, marker2.toStdString());
                QString exported2;
                QString why2;
                const bool ok2 = log_hub.export_log(&exported2, &why2);
                check(ok2 && QFile::exists(exported2),
                      QStringLiteral("③ 同秒第二次导出成功（%1；理由「%2」）").arg(exported2, why2));
                check(exported2 != exported,
                      QStringLiteral("③ 同秒两次导出的落点**不同**（第一份 %1 / 第二份 %2）")
                          .arg(QFileInfo(exported).fileName(), QFileInfo(exported2).fileName()));
                check(QFile::exists(exported) && QFile::exists(exported2),
                      QStringLiteral("③ 同秒两次导出的**两份都在**（覆盖 ⇒ 这里必红）"));
                QString body2;
                {
                    QFile file(exported2);
                    if (file.open(QIODevice::ReadOnly)) body2 = QString::fromUtf8(file.readAll());
                }
                // 第二份必须含**它自己那时候**的标记；第一份必须含第一份的标记。
                QString body1_again;
                {
                    QFile file(exported);
                    if (file.open(QIODevice::ReadOnly)) body1_again = QString::fromUtf8(file.readAll());
                }
                check(body2.contains(marker2) && body1_again.contains(marker),
                      QStringLiteral("③ 两份导出**内容各自正确**（第 1 份含标记 1=%1；"
                                     "第 2 份含标记 2=%2）")
                          .arg(body1_again.contains(marker) ? 1 : 0)
                          .arg(body2.contains(marker2) ? 1 : 0));
                // 落点可见：① 点击回调里 `log_line("日志已导出：…")` 打**返回值**（真走点击那条路）；
                //   ② `export_log` 自己在**自增**时打一行说明（本块直调函数 ⇒ 这条必须出现）。
                //   两者合一 ⇒ "最终落点 + 为什么自增" 用户都看得见。
                const QString log_text =
                    log_hub.log_view() != nullptr ? log_hub.log_view()->toPlainText() : QString();
                check(log_text.contains(QFileInfo(exported2).fileName()) ||
                          log_text.contains(QStringLiteral("已存在")),
                      QStringLiteral("③ 自增后的最终落点**可见**（日志栏里至少有自增说明或落点名；"
                                     "查 %1 / 「已存在」）")
                          .arg(QFileInfo(exported2).fileName()));

                // ★★ 本批：**运行日志（构造时定名）也不能同秒互撞**。
                //   第二个 Hub 在同一秒里构造 ⇒ 修前落点与第一个**同名** ⇒ `set_log_file`
                //   把第一个的运行日志顶掉（静默丢一份）。修后必须自增。
                xrkit::core::set_log_file(std::string());
                Hub log_hub2;
                check(!log_hub2.log_file_path().isEmpty() &&
                          log_hub2.log_file_path() != log_hub.log_file_path(),
                      QStringLiteral("③ 同秒构造两个 Hub 的运行日志落点**不同**"
                                     "（第 1 个 %1 / 第 2 个 %2）")
                          .arg(QFileInfo(log_hub.log_file_path()).fileName(),
                               QFileInfo(log_hub2.log_file_path()).fileName()));
                check(QFile::exists(log_hub.log_file_path()),
                      QStringLiteral("③ 第 1 个 Hub 的运行日志**没被顶掉**（%1）")
                          .arg(QFileInfo(log_hub.log_file_path()).fileName()));
                QFile::remove(log_hub2.log_file_path());

                QFile::remove(exported2);
            }

            // 写失败必须**明确报**：把落盘目录指到一个**文件**下面（mkpath 必失败）
            const QString blocker =
                QDir::temp().filePath(QStringLiteral("xrkit_smoke_log_blocker.txt"));
            {
                QFile file(blocker);
                if (file.open(QIODevice::WriteOnly)) file.write("x");
            }
            set_hub_log_dir(blocker + QStringLiteral("/logs"));
            QString bad_path;
            QString bad_why;
            const bool bad_ok = log_hub.export_log(&bad_path, &bad_why);
            check(!bad_ok && !bad_why.isEmpty() && !log_hub.last_export_error().isEmpty(),
                  QStringLiteral("③ 路径不可写 ⇒ 返回 false 且错误文本非空（实得「%1」）").arg(bad_why));
            set_hub_log_dir(smoke_log_dir);
            QFile::remove(blocker);
            QFile::remove(exported);
        }

        // ★ 6.x 排版口径（用户 2026-09-27："排版有很大问题"）—— **几何断言，不看截图**。
        //   为什么用 SplitPane 夹具而不是 Hub：离屏下 **Hub 的几何不落地**（实测子控件高度为 0），
        //   而真正决定比例的机制就是 SplitPane 的分配算法（富余给谁、受不受上限约束）。
        {
            SplitPane pane(SplitPane::Orient::Vertical);
            auto* page_pane = new QWidget;
            auto* log_pane = new QWidget;
            pane.add_pane(page_pane, px(260));
            pane.add_pane(log_pane, px(110));
            pane.set_max_size(1, px(200));   // 与 hub 同口径：日志格上限 200px
            pane.resize(1000, 900);
            pane.show();
            QApplication::processEvents();
            // SplitPane 的重排带 120ms 去抖（2.5）：等它真落地再读尺寸，否则读到的是中间值
            pane.request_clamp();
            QThread::msleep(160);
            QApplication::processEvents();
            if (pane.layout() != nullptr) pane.layout()->activate();
            const int total = pane.height();
            const int log_h = pane.pane_size(1);
            const int page_h = pane.pane_size(0);
            check(log_h > 0 && log_h <= px(200),
                  QStringLiteral("6.x 日志格受 per-pane 上限（实测 %1，上限 %2）")
                      .arg(log_h)
                      .arg(px(200)));
            check(total > 0 && log_h <= total * 30 / 100,
                  QStringLiteral("6.x 日志格 ≤ 窗口高 30%%（实测 %1 / %2 = %3%%）")
                      .arg(log_h).arg(total).arg(total > 0 ? log_h * 100 / total : -1));
            check(total > 0 && page_h >= total * 60 / 100,
                  QStringLiteral("6.x 页面格 ≥ 窗口高 60%%（实测 %1 / %2 = %3%%）")
                      .arg(page_h).arg(total).arg(total > 0 ? page_h * 100 / total : -1));
            pane.hide();
        }

        // 13.x 任务壳
        TaskShell task;
        task.set_idle_status(QStringLiteral("就绪"));
        check(task.run(QStringLiteral("运行中…"), [](const ProgressSink&, const MessageSink&) {
                  QThread::msleep(30);
              }),
              QStringLiteral("13.1 首次 run 被接受"));
        check(task.running(), QStringLiteral("13.1 running 在**线程启动前**就置位"));
        // ★★ 2026-10-06（审计处理批·0-3 之 B1-1）：修前这一条是
        //   `check(!task.run(QStringLiteral("第二次"), nullptr) || true, …)`
        //   —— 两个洞：① `|| true` 让整句**恒真**（无论结果如何都绿）；
        //   ② 传的是 `nullptr` worker，而 `task_shell.cpp` 在 worker 为空时**直接 return false**、
        //      根本没走到"重入拒绝"那条分支 ⇒ 名字声称的"重入被拒"一点都没被测到。
        //   现在喂**真的 worker**，并且把"第二次必须被拒"钉死在返回值上；
        //   真正的牙齿（`rejected_count()==1`）由下一句保留 —— 两句一起才有意义。
        const bool second = task.run(QStringLiteral("第二次"),
                                     [](const ProgressSink&, const MessageSink&) {});
        check(!second, QStringLiteral("13.1 重入被拒（第二次 run 必须返回 false）"));
        check(!second && task.rejected_count() == 1,
              QStringLiteral("13.1 重入返回 false（rejected=%1）").arg(task.rejected_count()));
        // 等第一个任务收尾（队列事件要 processEvents 才会到主线程）
        for (int i = 0; i < 60 && task.running(); ++i) {
            QThread::msleep(10);
            QApplication::processEvents();
        }
        check(!task.running(), QStringLiteral("13.3 收尾回到主线程后 running 归位"));
        check(task.status() == QStringLiteral("就绪"),
              QStringLiteral("13.4 无最终文案 ⇒ 恢复前置状态（实测 %1）").arg(task.status()));

        // 13.7：进度**只发一个 0~100 值**，壳里不做归一化
        task.report_progress(37);
        check(task.last_progress() == 37 && task.progress_bar()->value() == 37,
              QStringLiteral("13.7 进度原样转发（发 37 ⇒ 显示 %1）")
                  .arg(task.progress_bar()->value()));
        // 13.2/13.3：工作线程里的进度/错误都必须**排队回主线程**执行，且错误先于结束
        TaskShell task2;
        const bool started = task2.run(QStringLiteral("跑起来"), [](const ProgressSink& progress,
                                                                   const MessageSink& message) {
            progress(10);
            message("第一步");
            progress(50);
            progress(200);  // 越界：壳里夹到 100
            throw std::runtime_error("炸了");
        });
        check(started, QStringLiteral("13.2 任务启动"));
        for (int i = 0; i < 100 && task2.running(); ++i) {
            QThread::msleep(10);
            QApplication::processEvents();
        }
        const QVector<TaskShell::Event> events = task2.take_events();
        bool all_main = !events.isEmpty();
        QString order;
        for (const TaskShell::Event& e : events) {
            order.append(QLatin1Char(e.kind));
            if (!e.on_main_thread) all_main = false;
        }
        check(all_main, QStringLiteral("13.2 所有 UI 更新都发生在主线程"));
        check(order.contains(QStringLiteral("ef")) &&
                  order.indexOf(QLatin1Char('e')) < order.indexOf(QLatin1Char('f')),
              QStringLiteral("13.3 错误先入队、结束后入队（实得 %1）").arg(order));
        check(task2.last_progress() == 100, QStringLiteral("13.7 越界值夹到 100（实得 %1）")
                                                .arg(task2.last_progress()));
        check(task2.status().startsWith(QStringLiteral("失败：")),
              QStringLiteral("13.4 失败文案回主线程（实测 %1）").arg(task2.status()));
        result.notes.append(QStringLiteral("13.3 事件序列 = %1（p=进度 m=消息 e=错误 f=结束）")
                                .arg(order));

        // ★★ 2026-10-10（P17-4，审计 03 F-08 Low）：**线程表不许只增不减**。
        //   修前事实：`TaskShell::run` 每次 `threads_.emplace_back(...)`，已结束的线程保持
        //   joinable 直到析构（长会话 = 同一页反复跑任务 ⇒ 线程对象无界增长、栈/句柄长期占着）。
        //   修法：每次 run 之前回收**已完成**的（join + 摘除），"跑完了"由 worker 自己置的原子位判。
        //   判据：顺序跑 5 轮、每轮等它收工 ⇒ 槽位数不许长到 5（修前就是 5 ⇒ 红）。
        {
            // ★★ 变量名避开 Qt 的 `slots` 宏！上一版叫 `slots` 直接 C2059/C4091
            //   （Qt 把 `slots` 定义成空宏 ⇒ 声明被吃掉）—— 当场记在这里。
            TaskShell seq_shell;
            for (int round = 0; round < 5; ++round) {
                const bool ok = seq_shell.run(QStringLiteral("槽位 %1").arg(round),
                                              [](const ProgressSink& progress, const MessageSink&) {
                                                  progress(100);
                                              });
                check(ok, QStringLiteral("P17-4 前提：第 %1 轮任务起得来").arg(round));
                for (int i = 0; i < 100 && seq_shell.running(); ++i) {
                    QThread::msleep(5);
                    QApplication::processEvents();
                }
            }
            check(seq_shell.worker_slots() <= 1,
                  QStringLiteral("P17-4 03 F-08：顺序跑 5 轮后线程槽位 %1 个（修前＝只增不减 ⇒ 5 个）")
                      .arg(seq_shell.worker_slots()));
        }

        // ★★ 2026-10-10（P17-5，审计 03 F-07 Medium）：**`report_*` 从 worker 调必须安全**。
        //   修前：这四个公开入口直接 `apply_*`（`bar_->setValue` / `status_->setText`）⇒
        //   从工作线程调就是跨线程碰控件（偶发崩溃）。判据两条：
        //     ① worker 里调完**当场不许改控件**（说明它排队了，不是直落）；
        //     ② 事件泵一次之后控件必须被改，**且那条事件记的线程位是"在主线程"**。
        //   判红办法：把 `report_message` 换回直调 `apply_message` ⇒ ① 立刻红。
        {
            TaskShell report_shell;
            const QString before_text = report_shell.status_label()->text();
            std::thread reporter([&report_shell] {
                report_shell.report_message("来自 worker 的 report_message");
                report_shell.report_progress(66);
            });
            reporter.join();
            const QString right_after = report_shell.status_label()->text();
            check(right_after == before_text,
                  QStringLiteral("P17-5 03 F-07：worker 里调 `report_message` **不当场**改控件"
                                 "（修前直落 ⇒ 当场就变）—— 实得 %1")
                      .arg(right_after == before_text ? QStringLiteral("未变")
                                                      : QStringLiteral("当场变了")));
            QApplication::processEvents();
            check(report_shell.status_label()->text().contains(QStringLiteral("来自 worker")),
                  QStringLiteral("P17-5 03 F-07：排队之后控件被改（实得「%1」）")
                      .arg(report_shell.status_label()->text().left(40)));
            check(report_shell.progress_bar()->value() == 66,
                  QStringLiteral("P17-5 03 F-07：worker 里的 `report_progress` 排队落地（实得 %1）")
                      .arg(report_shell.progress_bar()->value()));
            bool saw_main_message = false;
            for (const TaskShell::Event& e : report_shell.take_events()) {
                if (e.kind == 'm' && e.on_main_thread) saw_main_message = true;
            }
            check(saw_main_message,
                  QStringLiteral("P17-5 03 F-07：那条消息事件确实**在主线程**执行（线程位自证）"));
        }

        // ★★ 2026-10-06（①）取消：**真停 + 留痕 + 理由可见**
        //   事实（改动前）：`task_shell.cpp` 里 `cancel_->setEnabled(...)` 三处、
        //   **一处 `connect` 都没有** ⇒ 六页那颗「取消」是一颗**死按钮**。
        {
            // ⚠ 声明顺序：worker 线程按引用捕获下面这几个原子 ⇒ 它们必须**先于** TaskShell
            //   声明（TaskShell 的析构会 join 线程；后声明的先销毁 ⇒ 反了就是 use-after-free）。
            std::atomic<bool> worker_stopped{false};
            std::atomic<int> steps{0};
            TaskShell cancel_shell;
            cancel_shell.set_idle_status(QStringLiteral("就绪"));
            check(cancel_shell.run(
                      QStringLiteral("长任务"),
                      [&cancel_shell, &worker_stopped, &steps](const ProgressSink& progress,
                                                               const MessageSink& message) {
                          for (int i = 0; i < 400; ++i) {
                              if (cancel_shell.cancel_requested()) {
                                  message("已取消：在循环边界停下（已跑 " + std::to_string(i) +
                                          " 步）");
                                  worker_stopped = true;
                                  return;
                              }
                              steps = i;
                              std::this_thread::sleep_for(std::chrono::milliseconds(5));
                              progress(i * 100 / 400);
                          }
                      }),
                  QStringLiteral("① 取消：长任务起得来（13.1 闸门不变）"));
            QElapsedTimer cancel_clock;
            cancel_clock.start();
            while (steps.load() < 3 && cancel_clock.elapsed() < 5000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            check(cancel_shell.cancel_button() != nullptr &&
                      cancel_shell.cancel_button()->isEnabled(),
                  QStringLiteral("① 取消：跑起来时按钮可点（实得 enabled=%1）")
                      .arg(cancel_shell.cancel_button() != nullptr &&
                                   cancel_shell.cancel_button()->isEnabled()
                               ? 1
                               : 0));
            // 点那颗**以前是死的**按钮
            if (cancel_shell.cancel_button() != nullptr) cancel_shell.cancel_button()->click();
            check(cancel_shell.cancel_requested() && cancel_shell.cancel_requests() == 1,
                  QStringLiteral("① 取消：点按钮真的置上取消位（requests=%1）")
                      .arg(cancel_shell.cancel_requests()));
            cancel_clock.restart();
            while (cancel_shell.running() && cancel_clock.elapsed() < 5000) {
                QApplication::processEvents();
                QThread::msleep(5);
            }
            const qint64 stop_ms = cancel_clock.elapsed();
            check(!cancel_shell.running() && worker_stopped.load() && stop_ms < 2000,
                  QStringLiteral("① 取消：点后 worker 在 %1 ms 内退出（真停；已跑 %2 步）")
                      .arg(stop_ms)
                      .arg(steps.load()));
            bool saw_cancel_event = false;
            for (const TaskShell::Event& e : cancel_shell.take_events()) {
                if (e.kind == 'c') saw_cancel_event = true;
            }
            check(saw_cancel_event, QStringLiteral("① 取消：事件流里留痕（'c'）"));
            check(!cancel_shell.cancel_reason().isEmpty() &&
                      cancel_shell.status().contains(QStringLiteral("已取消")) &&
                      cancel_shell.status() == cancel_shell.cancel_reason(),
                  QStringLiteral("① 取消：理由可见且**优先收尾**（状态栏＝「%1」）")
                      .arg(cancel_shell.status()));
            check(cancel_shell.cancel_button() != nullptr &&
                      !cancel_shell.cancel_button()->isEnabled() &&
                      cancel_shell.cancel_button()->text() == QStringLiteral("取消"),
                  QStringLiteral("① 取消：收尾后按钮复位（disabled + 文案回「取消」）"));
            result.notes.append(
                QStringLiteral("① 取消：点后 %1 ms 内真停（已跑 %2 步；理由＝%3）")
                    .arg(stop_ms)
                    .arg(steps.load())
                    .arg(cancel_shell.cancel_reason()));
            // 反向：**没点取消**的正常任务不许被这位置位
            std::atomic<int> ran{0};
            TaskShell plain_shell;
            plain_shell.run(QStringLiteral("跑完"), [&ran](const ProgressSink&, const MessageSink&) {
                ++ran;
            });
            for (int i = 0; i < 200 && plain_shell.running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            check(ran.load() == 1 && !plain_shell.cancel_requested() &&
                      plain_shell.cancel_requests() == 0,
                  QStringLiteral("① 取消：没点取消时取消位恒为假（反向锁）"));
        }
    }

    // ★★ 2026-10-06（②，用户拍板）**封包与字库的「取消」整块删除**
    //   用户原话：「**封包和字库的取消，在这里本质是完全没有的，故直接删除即可。**」
    //   口径：这两页**连按钮对象都不许存在**（藏起来/置灰/点不动的"取消"仍是假的取消）；
    //        其余三页（编码转换 / 文本提取 / XML 校对）共用任务壳的那颗取消**照旧保留**。
    //   反向锁的形态：删掉 `CancelButton::No` 的参数（＝按钮又回来了）⇒ 立刻红。
    {
        FsPage fs;
        FontPackPage fontpack;
        ConvertPage convert;
        TextPage text;
        XmlPage xml;
        check(fs.task() != nullptr && fs.task()->cancel_button() == nullptr,
              QStringLiteral("② 取消：文件系统页（封包/解包）不许有取消按钮（实得 %1）")
                  .arg(fs.task() != nullptr && fs.task()->cancel_button() != nullptr ? 1 : 0));
        check(fontpack.task() != nullptr && fontpack.task()->cancel_button() == nullptr,
              QStringLiteral("② 取消：汉化包生成页（字库）不许有取消按钮（实得 %1）")
                  .arg(fontpack.task() != nullptr && fontpack.task()->cancel_button() != nullptr
                           ? 1
                           : 0));
        check(convert.task() != nullptr && convert.task()->cancel_button() != nullptr &&
                  convert.task()->cancel_button()->text() == QStringLiteral("取消"),
              QStringLiteral("② 取消：编码转换页的取消**保留**（共用任务壳那颗）"));
        check(text.task() != nullptr && text.task()->cancel_button() != nullptr,
              QStringLiteral("② 取消：文本提取页的取消**保留**"));
        check(xml.task() != nullptr && xml.task()->cancel_button() != nullptr,
              QStringLiteral("② 取消：XML 校对页的取消**保留**"));
    }

    // ★★ 2026-10-07（审计补充 F3「孤儿顶层控件」，效应等同用户可见缺陷）
    //   修前：`xml_page.cpp:166` / `text_page.cpp:111` / `convert_page.cpp:241` 三页都写
    //   `status_ = new QLabel(task_->status());` —— `TaskShell::status()` 返回的是 **QString**
    //   （控件访问器叫 `status_label()`），于是 `QLabel(const QString&, QWidget* = nullptr)`
    //   造出一个**无父**标签 ⇒ 它是个**顶层窗口**、从不进布局（全仓 `addWidget(status_)`
    //   零命中）⇒ 全页消息（空目录 / 导出成功失败 / 复制 id / 还没选源）用户**一个字都看不见**，
    //   而且没人拥有它（泄漏）。
    //   判据＝标签**有父**、在页内（`isAncestorOf`）、不是窗口、页显示后**真的可见且占位**。
    //   （静态面由门禁 `check_page_status_labels.py` 独立成步，这条是"真画出来没有"的行为面。）
    {
        ConvertPage convert;
        TextPage text;
        XmlPage xml;
        struct StatusEntry {
            QWidget* page;
            QLabel* label;
            const char* name;
        };
        const StatusEntry entries[] = {
            {&convert, convert.status(), "编码转换"},
            {&text, text.status(), "文本提取"},
            {&xml, xml.status(), "XML 校对"},
        };
        for (const StatusEntry& entry : entries) {
            const QString name = QString::fromUtf8(entry.name);
            check(entry.label != nullptr,
                  QStringLiteral("F3 %1：页面级消息标签存在（status()）").arg(name));
            if (entry.label == nullptr) continue;
            entry.page->resize(900, page_min_h());
            entry.page->show();
            QApplication::processEvents();
            entry.label->setText(QStringLiteral("F3 可见性判据"));
            QApplication::processEvents();
            check(entry.label->parentWidget() != nullptr && entry.page->isAncestorOf(entry.label) &&
                      !entry.label->isWindow(),
                  QStringLiteral("F3 %1：消息标签**有父、在页内**（不是孤儿顶层控件）").arg(name));
            check(entry.label->isVisible() && entry.label->width() > 0 && entry.label->height() > 0,
                  QStringLiteral("F3 %1：消息标签真的**可见且占位**（可见=%2 %3x%4）")
                      .arg(name)
                      .arg(entry.label->isVisible() ? 1 : 0)
                      .arg(entry.label->width())
                      .arg(entry.label->height()));
        }
    }

    // ⑩ 虚拟化列表（M2-5）：对照表 10.2 的范围规则 + 10.3 的同族原则（只对视口内行取文本）。
    {
        // 10.2 规则逐组对拍：first=top//h、last=min(total,(top+view_h)//h+2)
        struct Case {
            int top;
            int view_h;
            int h;
            int total;
            int first;
            int last;
        };
        const Case cases[] = {
            {0, 196, 20, 10000, 0, 11},      // (0+196)//20+2 = 9+2 = 11
            {180, 196, 20, 10000, 9, 20},    // 9, (376)//20+2 = 18+2 = 20
            {0, 40, 20, 3, 0, 3},            // min(total, ...) 夹到 total
            {0, 400, 20, 5, 0, 5},
            {9999 * 20, 196, 20, 10000, 9999, 10000},
        };
        int rule_bad = 0;
        for (const Case& c : cases) {
            const QPair<int, int> got = VirtualList::visible_range(c.top, c.view_h, c.h, c.total);
            if (got.first != c.first || got.second != c.last) ++rule_bad;
        }
        check(rule_bad == 0, QStringLiteral("10.2 范围规则逐组一致（%1 组里 %2 组不符）")
                                 .arg(sizeof(cases) / sizeof(cases[0]))
                                 .arg(rule_bad));

        VirtualList list;
        list.set_row_height(20);
        list.resize(400, 196);
        list.set_row_count(10000);
        list.set_row_text_provider([](int row) {
            return QStringLiteral("row %1").arg(row);
        });
        list.show();
        QApplication::processEvents();
        const QPair<int, int> range0 = list.visible_range();
        check(range0.first == 0 && range0.second == 11,
              QStringLiteral("10.2 视口范围 = (0,11)（实测 (%1,%2)）")
                  .arg(range0.first)
                  .arg(range0.second));
        check(list.built_count() == 11,
              QStringLiteral("10.2 只为视口内行建控件（建了 %1 个，total=10000）")
                  .arg(list.built_count()));
        check(list.provider_calls() == 11,
              QStringLiteral("10.3 同族：行文本只对视口内行取（调了 %1 次）")
                  .arg(list.provider_calls()));
        // 滚动：范围变、池子**不涨**
        list.set_scroll_top(180);
        check(list.visible_range() == QPair<int, int>(9, 20),
              QStringLiteral("滚动后范围 = (9,20)"));
        check(list.built_count() == 11,
              QStringLiteral("池子不随滚动增长（仍 %1 个）").arg(list.built_count()));
        // total 变化不影响池子大小（没有硬编码上限，也不是"按 total 建"）
        list.set_scroll_top(0);
        list.set_row_count(100);
        const int built_100 = list.built_count();
        list.set_row_count(1000000);
        check(list.built_count() == built_100 && built_100 == 11,
              QStringLiteral("10.2 池子与 total 无关（100 行与 100 万行都是 %1 个）")
                  .arg(list.built_count()));

        // 判据：**换选一行与条数无关**（真实窗口口径是 7~10 ms；这里量离屏下界并报出）
        const auto measure_select = [&list](int total) {
            list.set_row_count(total);
            list.set_scroll_top(0);
            QElapsedTimer timer;
            timer.start();
            constexpr int kRounds = 50;
            for (int i = 0; i < kRounds; ++i) list.set_selected_row(i % 11);
            return static_cast<double>(timer.nsecsElapsed()) / 1e6 / kRounds;
        };
        const double small = measure_select(100);
        const double big = measure_select(1000000);
        result.notes.append(QStringLiteral("10.2 换选一行：total=100 时 %1 ms，total=100 万时 %2 ms")
                                .arg(small, 0, 'f', 4)
                                .arg(big, 0, 'f', 4));
        check(big < 30.0, QStringLiteral("10.2 换选一行 %1 ms < 30 ms 上界（离屏下界口径）")
                              .arg(big, 0, 'f', 4));
        check(small <= 0.0 || big / std::max(0.0001, small) < 5.0,
              QStringLiteral("10.2 换选一行的成本与条数无关（100 vs 100 万 = %1x）")
                  .arg(big / std::max(0.0001, small), 0, 'f', 2));
        check(list.selected_row() == 10 % 11 || list.selected_row() >= 0,
              QStringLiteral("选中行可设（%1）").arg(list.selected_row()));
    }

    // ⑤ 页内滚动 + 标签条（M2-6a）：六页框架的三条口径。
    // 这一组只量**框架**（页自己那层视口 / 贴底操作栏 / 标签条不被包进视口）；
    // 各页的业务判据由 M2-6b..g 各自的片补上。
    {
        // 沿父链找视口 —— 5.1 的正反两面（body 必须在里面、操作栏必须在外面）都用它
        const auto inside_viewport = [](const QWidget* w) {
            for (const QWidget* p = (w == nullptr ? nullptr : w->parentWidget()); p != nullptr;
                 p = p->parentWidget()) {
                if (dynamic_cast<const ScrollViewport*>(p) != nullptr) return true;
            }
            return false;
        };

        // 5.2：`page_min_h()` 必须是**函数** —— 改缩放它就得跟着变。
        // 写成常量的话这三组会全部返回 430，当场露馅（这就是"必须过 px()"的意思）。
        const double saved_scale = xrkit::core::ui_scale();
        const struct {
            double scale;
            int want;
        } scale_cases[] = {{1.0, 430}, {1.5, 645}, {2.0, 860}};
        int scale_bad = 0;
        for (const auto& c : scale_cases) {
            xrkit::core::set_ui_scale(c.scale);
            if (page_min_h() != c.want) ++scale_bad;
        }
        xrkit::core::set_ui_scale(saved_scale);
        check(scale_bad == 0,
              QStringLiteral("5.2 page_min_h() 随缩放变（1.0/1.5/2.0 → 430/645/860，%1 组不符）")
                  .arg(scale_bad));

        // 5.3 的反面证据：**没设过** minsize 的视口默认就是 0 ⇒ "忘了设"必然与
        // `page_min_h()` 不等，所以下面那条判据是有牙的，不是自证。
        ScrollViewport fresh_viewport;
        check(fresh_viewport.min_body_height() == 0 && page_min_h() != 0,
              QStringLiteral("5.3 具名 setter 有牙（未设 = %1，应设 = %2）")
                  .arg(fresh_viewport.min_body_height())
                  .arg(page_min_h()));

        hub.show();
        QApplication::processEvents();

        int frame_bad = 0;
        int order_bad = 0;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr) {
                ++frame_bad;
                ++order_bad;
                continue;
            }
            // 5.3：minsize **真的**设上了（不是"代码里写了"）
            if (!page->minsize_ok()) ++frame_bad;
            // 5.1：页自己那层视口的 fit 必须是 content（静态内容，条子由本视口造）
            if (page->viewport() == nullptr ||
                page->viewport()->fit() != ScrollViewport::Fit::Content) {
                ++frame_bad;
            }
            check(page->page_id() == hub.page_id(i) &&
                      page->page_title() == hub.page_title(i) &&
                      page->header() != nullptr && page->header()->text() == hub.page_title(i),
                  QStringLiteral("第 %1 页框架带着自己的 id/标题（%2 / %3）")
                      .arg(i)
                      .arg(hub.page_id(i))
                      .arg(hub.page_title(i)));

            // 5.1 + 规则 2：页体在视口**里**，操作栏在视口**外**
            if (page->body() == nullptr || page->body()->parentWidget() != page->viewport()) {
                ++order_bad;
            }
            if (page->action_bar() == nullptr || inside_viewport(page->action_bar())) {
                ++order_bad;
            }
            // 操作栏里必须有这个按钮、且**不在视口里**。不能要求它是第 0 项：
            // 真页面在自己构造时就已经加过按钮了（顺序契约由 add_action 保证）。
            auto* probe_button = page->add_action(QStringLiteral("自检占位"));
            bool probe_in_bar = false;
            for (int j = 0; page->action_layout() != nullptr &&
                            j < page->action_layout()->count();
                 ++j) {
                if (page->action_layout()->itemAt(j) != nullptr &&
                    page->action_layout()->itemAt(j)->widget() == probe_button) {
                    probe_in_bar = true;
                }
            }
            if (!probe_in_bar || inside_viewport(probe_button)) {
                ++order_bad;
            }
        }
        check(frame_bad == 0,
              QStringLiteral("5.1/5.3 六页都自带视口且 minsize=page_min_h()（%1 页不符）")
                  .arg(frame_bad));
        check(order_bad == 0,
              QStringLiteral("5.1/规则2 页体在视口内、操作栏在视口外（%1 处不符）")
                  .arg(order_bad));

        // 规则 2 的实测：压到 `page_min_h()` 时操作栏**必须还在**（1.x 的坑是窗口一矮，
        // 贴底那一排被内容挤没，用户找不到"开始"）。
        // 夹具必须是**裸 Toplevel**：Hub 式布局（页列表+页栈+竖分栏）永远压不矮，
        // 挂在 Hub 上量这一条等于量不到（M2-2 踩过同一个坑）。
        int shrink_bad = 0;
        for (int i = 0; i < builtin_page_count(); ++i) {
            PageBase* page = create_page(i);
            if (page == nullptr) {
                ++shrink_bad;
                continue;
            }
            auto* probe_button = page->add_action(QStringLiteral("自检占位"));
            page->resize(640, page_min_h());
            page->show();
            QApplication::processEvents();
            QWidget* bar = page->action_bar();
            const ScrollViewport* viewport = page->viewport();
            if (bar == nullptr || bar->height() <= 0 || probe_button->height() <= 0 ||
                bar->y() + bar->height() > page->height() || bar->y() <= 0 ||
                viewport == nullptr || viewport->height() <= 0 || viewport->y() <= 0) {
                ++shrink_bad;
            }
            page->hide();
            delete page;
        }
        check(shrink_bad == 0,
              QStringLiteral("规则2 裸窗口压到 page_min_h()=%1 时操作栏仍在（%2 页不符）")
                  .arg(page_min_h())
                  .arg(shrink_bad));

        // 5.1 的另一面：**标签条（页列表/页栈）不许被包进任何滚动视口** ——
        // 被包进去的话一滚主导航就滚走了（1.x 明文锁过这一条）。
        check(!inside_viewport(hub.page_tabs()) && !inside_viewport(hub.stack()),
              QStringLiteral("5.1 标签条与页栈不在任何滚动视口里"));
        result.notes.append(QStringLiteral("⑤ 六页框架：page_min_h=%1（缩放 %2），视口 fit=content")
                                .arg(page_min_h())
                                .arg(xrkit::core::ui_scale()));
    }

    // ⑥ 文件系统页的**界面形态**（M2-6b2）。这几条都是 1.x 出过事故的地方：
    // 两套状态混成一套、下拉存显示名、拖放另造一个 zone、解包有两条不同源的路。
    {
        namespace efs = xrkit::engine::fs;
        FsPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 6.13：下拉**取值 = 内部键**，显示名只用于展示
        QComboBox* combo = page.format_combo();
        const std::vector<efs::FsFormatItem>& formats = efs::fs_formats();
        check(combo != nullptr && combo->count() == static_cast<int>(formats.size()),
              QStringLiteral("6.13 格式下拉条数 = 格式表条数（%1）").arg(formats.size()));
        // ★★ 2026-10-06（④）：1.x `fs_app.py:154` 的默认值就是 "auto"、`:193` 的
        //   `fmt_keys` 第一位也是 "auto" ⇒ 2.x 补上它，且**必须在第一项**。
        check(combo != nullptr && combo->itemData(0).toString() == QStringLiteral("auto"),
              QStringLiteral("6.13 auto 是格式下拉第一项（实得 %1）")
                  .arg(combo != nullptr ? combo->itemData(0).toString() : QStringLiteral("<无下拉>")));
        check(efs::format_key_is_auto("auto") && !efs::format_key_to_db("auto").has_value() &&
                  !efs::format_key_is_sqfs("auto"),
              QStringLiteral("6.13 auto ＝**不给格式**（既不是 DB 六格式、也不是 SquashFS）"));
        // ④ 锁②：选 auto ⇒ 交给引擎的 fallback 就是 `nullopt`（与"不传格式"同一条路）
        check(!efs::format_key_to_db(combo->currentData().toString().toStdString()).has_value(),
              QStringLiteral("6.13 选 auto ⇒ 不传格式（fallback=nullopt）"));
        // ② 说明标签：非空、选 auto 时含"自动检测"、换成具体格式后**跟着变**
        {
            const QString auto_hint = page.format_hint_text();
            check(page.format_hint() != nullptr && !auto_hint.isEmpty() &&
                      auto_hint.contains(QStringLiteral("自动检测")),
                  QStringLiteral("6.13 选 auto ⇒ 说明标签非空且说明是「自动检测」（实得 %1）")
                      .arg(auto_hint));
            QString other_hint;
            if (combo != nullptr && combo->count() > 1) {
                combo->setCurrentIndex(1);
                other_hint = page.format_hint_text();
                combo->setCurrentIndex(0);
            }
            check(!other_hint.isEmpty() && other_hint != auto_hint &&
                      page.format_hint() != nullptr &&
                      page.format_hint()->text() == auto_hint,
                  QStringLiteral("6.13 说明标签随所选格式变化（auto=「%1」/ 具体=「%2」）")
                      .arg(auto_hint, other_hint));
        }
        // ④ 封包下拉**不含 auto**（1.x `fs_app.py:196` `[k for k in fmt_keys if k != "auto"]`）
        {
            int auto_items = 0;
            QComboBox* pack_combo = page.pack_format_combo();
            for (int i = 0; pack_combo != nullptr && i < pack_combo->count(); ++i) {
                if (efs::format_key_is_auto(pack_combo->itemData(i).toString().toStdString())) {
                    ++auto_items;
                }
            }
            check(pack_combo != nullptr && auto_items == 0 &&
                      pack_combo->count() == static_cast<int>(formats.size()) - 1,
                  QStringLiteral("6.13 封包下拉 = 非 auto 的全部格式（auto 项 %1、条数 %2 / 期望 %3）")
                      .arg(auto_items)
                      .arg(pack_combo != nullptr ? pack_combo->count() : -1)
                      .arg(static_cast<int>(formats.size()) - 1));
        }
        int key_bad = 0;
        int sqfs_items = 0;
        for (int i = 0; combo != nullptr && i < combo->count(); ++i) {
            const std::string key = combo->itemData(i).toString().toStdString();
            const std::string label = combo->itemText(i).toStdString();
            // ★ 2026-10-06（④）：`auto` 也是一个**引擎认得的内部键**（它＝不给格式），
            //   所以"取值合法性"这条判据要把它算进来（`format_key_to_db` 仍不认它 —— 见上）。
            const bool key_ok = efs::format_key_is_sqfs(key) ||
                                efs::format_key_to_db(key).has_value() ||
                                efs::format_key_is_auto(key);
            // 显示名**不该**能被当成取值解析（这就是那个事故的判据）
            const bool label_resolves =
                efs::format_key_is_sqfs(label) || efs::format_key_to_db(label).has_value();
            if (!key_ok || label_resolves) ++key_bad;
            if (efs::format_key_is_sqfs(key)) ++sqfs_items;
        }
        check(key_bad == 0,
              QStringLiteral("6.13 下拉取值是内部键、显示名解析不出来（%1 项不符）").arg(key_bad));
        check(sqfs_items == 1, QStringLiteral("6.13 SquashFS 在下拉里恰好一条"));

        // 6.4：**没有 drop_zone** —— 拖放挂在既有控件上（1.x 明文写着）
        bool any_drop_zone = false;
        for (QWidget* child : page.findChildren<QWidget*>()) {
            if (child->objectName().contains(QStringLiteral("drop_zone"))) any_drop_zone = true;
        }
        check(!any_drop_zone, QStringLiteral("6.4 页里没有 drop_zone 控件"));
        check(page.package_list() != nullptr && page.package_list()->acceptDrops(),
              QStringLiteral("6.4 拖放直接挂在包列表上（acceptDrops）"));

        // ★★ 2026-10-01 用户第 6 条（原话）：「包列表和文件列表引用的子组件是不是不一样？…
        //   所以，使用同一个小组件来完成。包列表的内容和文件列表的结构实际上是一样的。
        //   统一使用文件列表的那个，因为那个已经打磨过。」
        //   可判红的形式：两者**同一套关键配置**（原来包列表是 `QListWidget`，这几条天然对不上）。
        {
            // 对齐的是**组件模板**那一组属性：列数 / ExtendedSelection / uniformRowHeights /
            // alternatingRowColors / 表头策略（三列 Interactive + 同一个 minimumSectionSize）。
            // **`acceptDrops` 不算"对齐项"**：它是**实例级特性**（包列表是拖放目标，`drop_filter_`
            // 只挂在它身上；条目树就算设了也没有 handler ⇒ 拖上去没反应，反而误导）——
            // 所以这里只单独要求包列表可接收拖放（6.4 的要求），不拿它跟条目树比。
            // ★ 2026-10-02：口径**有意放宽一处**（用户圈着表头打了个「？」之后）。
            //   旧写法要求"两列表三列都 Interactive、拉伸交给条目树末列"，而**包列表要的是"名称列吃
            //   剩余宽度、大小/✕ 靠右钉住"**（否则表头右边 ~400px 是死区 —— 正是用户圈的那块）。
            //   ⇒ 对齐改成"**同一套表头行为**：恰好一列吃剩余宽度（哪一列由各表自己的语义定）＋
            //   其余列一律 Interactive（用户 09-29 要的可拖不许丢）＋ minimumSectionSize 相同"。
            //   放宽的只是"哪一列拉伸"；"同一个组件、同一族行为"没有放宽。
            const auto header_stretch_columns = [](QTreeWidget* tree) {
                if (tree == nullptr || tree->header() == nullptr) return -1;
                int stretched = 0;
                for (int col = 0; col < tree->columnCount(); ++col) {
                    const auto mode = tree->header()->sectionResizeMode(col);
                    if (mode == QHeaderView::Stretch) {
                        ++stretched;
                    } else if (mode != QHeaderView::Interactive) {
                        return -1;   // 出现 Fixed/ResizeToContents 之类 ⇒ 不算同一族
                    }
                }
                if (tree->header()->stretchLastSection()) ++stretched;
                return stretched;
            };
            // ★★「？」的硬判据：**列的总宽必须铺满视口**，否则右半边就是死区（改回去立刻红：
            //   真机 318 < 717，包列表这一条会红，而条目树那边仍然是铺满的 —— 差异看得见）。
            const auto header_fills_viewport = [](QTreeWidget* tree) {
                if (tree == nullptr || tree->header() == nullptr) return false;
                int total = 0;
                for (int col = 0; col < tree->columnCount(); ++col) {
                    total += tree->header()->sectionSize(col);
                }
                return tree->viewport()->width() > 0 &&
                       total >= tree->viewport()->width() - xrkit::core::px(2);
            };
            check(qobject_cast<QTreeWidget*>(page.package_list()) != nullptr &&
                      page.package_list()->columnCount() == 3 &&
                      page.package_list()->selectionMode() ==
                          page.entry_tree()->selectionMode() &&
                      page.package_list()->selectionMode() ==
                          QAbstractItemView::ExtendedSelection &&
                      page.package_list()->uniformRowHeights() ==
                          page.entry_tree()->uniformRowHeights() &&
                      page.package_list()->uniformRowHeights() &&
                      page.package_list()->alternatingRowColors() ==
                          page.entry_tree()->alternatingRowColors() &&
                      header_stretch_columns(page.package_list()) == 1 &&
                      header_stretch_columns(page.entry_tree()) == 1 &&
                      // ★★ 2026-10-02 用户：「为什么包树和文件树的形式不同。文件树的形式已经相当
                      //   成熟，包树直接引用树的统一函数就行啊。」⇒ 装配已收敛成同一个
                      //   `apply_pair_tree_spec`，这里把它**对外可测的那几项**钉住：
                      //   · 两树的"展开语义"必须一致：**单击展开、双击不展开**；
                      //   · 两树都**要有根装饰**（包列表在平表时代留了 `setRootIsDecorated(false)`，
                      //     树化 + 默认收起之后**顶层节点的孩子就再也点不出来了**）——
                      //     把这条去掉立刻红。
                      page.package_list()->expandsOnDoubleClick() == false &&
                      page.entry_tree()->expandsOnDoubleClick() == false &&
                      page.package_list()->rootIsDecorated() &&
                      page.entry_tree()->rootIsDecorated() &&
                      page.package_list()->header()->minimumSectionSize() ==
                          page.entry_tree()->header()->minimumSectionSize() &&
                      page.package_list()->acceptDrops(),
                  QStringLiteral("第6条 包列表与条目树同一套小组件配置（列 %1 / selection %2==%3 / "
                                 "uniform %4==%5 / alt %6==%7 / 表头恰好一列拉伸 %8==%9 / "
                                 "minSection %10==%11 / 包列表可接收拖放 %12）")
                      .arg(page.package_list()->columnCount())
                      .arg(static_cast<int>(page.package_list()->selectionMode()))
                      .arg(static_cast<int>(page.entry_tree()->selectionMode()))
                      .arg(page.package_list()->uniformRowHeights() ? 1 : 0)
                      .arg(page.entry_tree()->uniformRowHeights() ? 1 : 0)
                      .arg(page.package_list()->alternatingRowColors() ? 1 : 0)
                      .arg(page.entry_tree()->alternatingRowColors() ? 1 : 0)
                      .arg(header_stretch_columns(page.package_list()))
                      .arg(header_stretch_columns(page.entry_tree()))
                      .arg(page.package_list()->header()->minimumSectionSize())
                      .arg(page.entry_tree()->header()->minimumSectionSize())
                      .arg(page.package_list()->acceptDrops() ? 1 : 0));
            QApplication::processEvents();
            // ★★ 2026-10-02 用户「？」：**表头/行必须铺满面板**（三列总宽 < 视口宽 ⇒ 右边是死区）。
            check(header_fills_viewport(page.package_list()) &&
                      header_fills_viewport(page.entry_tree()),
                  QStringLiteral("第6条 表头铺满面板无死区（包列表列宽 %1 / 视口 %2，条目树 %3 / 视口 %4）")
                      .arg([&] {
                          int t = 0;
                          for (int c = 0; c < page.package_list()->columnCount(); ++c) {
                              t += page.package_list()->header()->sectionSize(c);
                          }
                          return t;
                      }())
                      .arg(page.package_list()->viewport()->width())
                      .arg([&] {
                          int t = 0;
                          for (int c = 0; c < page.entry_tree()->columnCount(); ++c) {
                              t += page.entry_tree()->header()->sectionSize(c);
                          }
                          return t;
                      }())
                      .arg(page.entry_tree()->viewport()->width()));
        }

        // ★★ 2026-10-01 用户第 3 条（原话）：「并且扫描时有Bug的，选择包模式的时候，是扫不出
        //   金球奖的db的。这是个Bug。不要想着增加后缀的样式（.db0，.db_base等），因为这是
        //   遍历不完的。然后点击"所有文件"才能看见那些包。」
        //   判据：过滤器里必须有 `*.db*` / `*.sq*` 这两段通配，且**不许**再枚举具体后缀
        //   （枚举是"扫不出"的根因：`gamedata.db_textures_1` 不在任何一张名单里）。
        {
            const QString filter = FsPage::package_dialog_filter();
            // ★★ 2026-10-04 补 `*.xdb*`：用户问「`.xdb`，我们的工具能识别吗？」⇒ 旧过滤器要求字面 `.db`
            //   ⇒ "选包"对话框里看不见 `.xdb`。**删掉这条断言立刻红**。
            check(filter.contains(QStringLiteral("*.db*")) &&
                      filter.contains(QStringLiteral("*.sq*")) &&
                      filter.contains(QStringLiteral("*.xdb*")) &&
                      !filter.contains(QStringLiteral("*.db0")) &&
                      !filter.contains(QStringLiteral("*.db_base")),
                  QStringLiteral("第3条 加包过滤器=「%1」（含 *.db* / *.sq* / ★*.xdb*，不枚举具体后缀）")
                      .arg(filter));
        }

        // ★ 2026-09-30 结构重排（用户第四轮 ①「你不认为，堆起来很丑吗？」+ 授权「按自己审美来改」）
        //   判据＝**工具行归位**：工具行与它服务的那个列表**同一父控件**、且行在列表**上面**；
        //   两个面板一起进同一个分隔条。**修前第一条就红**：条目工具行（搜索/全选）的父控件是
        //   解包页、而且落在分隔条**下面**（看着像页级控件）。
        {
            // ★★ ①-a0 **空状态**（一个包都没有）的排版 —— 2026-09-30 用户对着 FS 页发火的真根因：
            //   条目树在空状态被 `setVisible(false)`，而 **Qt 布局不给隐藏控件分配空间** ⇒
            //   那 340 px 全落进 `entry_bar`（普通 QWidget、垂直策略默认 Preferred）⇒ 里面的按钮被
            //   QHBoxLayout **垂直居中** ⇒ 看上去就是"搜索行孤零零飘在两块空白中间"。
            //   修法：工具行钉成 `QSizePolicy::Fixed` + 条目树**永远可见**（空树带表头本身就是
            //   "这里会出现包内条目"的指示，不是解释文字 ⇒ 不违反"界面不要解释段"）。
            //   判红办法：把这两处改回去 ⇒ 下面第一条立刻红（实测会从 42 变成 386）。
            FsPage empty_page;
            empty_page.resize(900, 700);
            empty_page.show();
            QApplication::processEvents();
            check(empty_page.entry_pane() != nullptr && empty_page.entry_pane()->isHidden() &&
                      empty_page.package_list() != nullptr &&
                      empty_page.package_list()->height() > xrkit::core::px(300),
                  QStringLiteral("① 空状态＝条目面板整块收起、包列表填满工作区"
                                 "（包列表实测 %1 > %2）")
                      .arg(empty_page.package_list() != nullptr
                               ? empty_page.package_list()->height()
                               : -1)
                      .arg(xrkit::core::px(300)));
            check(empty_page.entry_tree() != nullptr && empty_page.entry_pane() != nullptr &&
                      empty_page.entry_pane()->isHidden(),
                  QStringLiteral("① 空状态下**整个条目面板收掉**（解包页空状态＝两行 + 一个列表填满，"
                                 "与封包页同形）"));
            // ①-a1 操作栏按钮**贴右**（`add_action` 里 stretch 在 index 0 ⇒ 必须追加，不许前插）
            //
            // ★★ 2026-10-06（审计处理批·0-3 之 B1-6）：修前这句是
            //   `if (action_bar() && unpack_all_button()) { check(贴右) }` ——
            //   条件不成立时**断言静默消失**：`action_bar()` 变 nullptr（正是"操作栏丢了"这种回归）
            //   反而一条失败都不记。现在把条件本身也写成判据（缺件即红），几何检查才在块内做。
            const bool right_ok = empty_page.action_bar() != nullptr &&
                                  empty_page.unpack_all_button() != nullptr;
            check(right_ok,
                  QStringLiteral("① 前提：空状态页的操作栏与「全部解包」按钮都在"
                                 "（action_bar=%1 button=%2）")
                      .arg(empty_page.action_bar() != nullptr ? 1 : 0)
                      .arg(empty_page.unpack_all_button() != nullptr ? 1 : 0));
            if (right_ok) {
                QPushButton* last = empty_page.unpack_all_button();
                const int right_gap =
                    empty_page.action_bar()->width() - (last->x() + last->width());
                check(right_gap >= 0 && right_gap <= xrkit::core::px(12),
                      QStringLiteral("① 操作栏按钮贴右（右侧留白 %1 ≤ %2；修前按钮被顶到左下角）")
                          .arg(right_gap)
                          .arg(xrkit::core::px(12)));
            }
            empty_page.hide();

            // ★★ 2026-10-01 用户三连（都在这一个夹具里判）：
            //   ①「在筛选的时候，文件夹下会多出一个空的行」⇒ 真因＝`make_entry_item` 给"有子节点的
            //     目录"塞的**懒建占位子项**没被摘掉，筛选是一次性建树 ⇒ 它留在第一位当空行。
            //   ②「我不知道搜索字段和结果有什么直接关系」⇒ 旧实现拿**整条路径** contains，
            //     `ai` 会命中 `crouch_down_aim.anm` 这种"凑巧有这两个字母"的；现改成**按名字匹配**
            //     （文件与目录一视同仁）+ 命中项加粗。
            //   ③「DB栏能拖动多选，文件栏不行」⇒ 条目树补 ExtendedSelection。
            {
                FsPage filter_page;
                filter_page.add_source(QStringLiteral("smoke_filter"),
                                       {efs::EntryInfo{std::string("dir"), true, 0},
                                        efs::EntryInfo{std::string("dir/aim.txt"), false, 10},
                                        efs::EntryInfo{std::string("dir/other.bin"), false, 20}});
                filter_page.search_edit()->setText(QStringLiteral("aim"));
                QApplication::processEvents();
                QTreeWidget* tree = filter_page.entry_tree();
                int blanks = 0;
                int rows = 0;
                bool has_other = false;
                bool matched_bold = false;
                std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
                    ++rows;
                    if (item->text(0).isEmpty()) ++blanks;
                    if (item->text(0).contains(QStringLiteral("other"))) has_other = true;
                    if (item->text(0) == QStringLiteral("aim.txt") && item->font(0).bold()) {
                        matched_bold = true;
                    }
                    for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
                };
                for (int i = 0; i < tree->topLevelItemCount(); ++i) walk(tree->topLevelItem(i));
                check(blanks == 0 && rows > 0,
                      QStringLiteral("① 筛选后树里没有空名字的行（空行 %1 行 / 共 %2 行）")
                          .arg(blanks)
                          .arg(rows));
                check(!has_other,
                      QStringLiteral("② 搜索按**名字**匹配：不命中 aim 的 other.bin 不许出现"));
                check(matched_bold, QStringLiteral("② 命中项加粗（一眼看出哪些行是命中的）"));
                check(filter_page.entry_tree()->selectionMode() == QAbstractItemView::ExtendedSelection,
                      QStringLiteral("③ 条目树能框选/拖选多个（ExtendedSelection）"));
                filter_page.hide();
            }

            // ★ 几何必须在**装了东西**的状态下量：空夹具的树是隐藏的（历史现象），不适合量几何。
            //   所以这里用一个**有内容**的独立夹具，不动上面的 `page`。
            FsPage geo_page;
            geo_page.add_source(QStringLiteral("smoke_src"),
                                {efs::EntryInfo{std::string("dir"), true, 0},
                                 efs::EntryInfo{std::string("dir/a.txt"), false, 10}});
            geo_page.show();

            QWidget* pkg_pane = geo_page.package_pane();
            QWidget* entry_pane = geo_page.entry_pane();
            QWidget* pkg_bar = geo_page.package_bar();
            QWidget* entry_bar = geo_page.entry_bar();
            check(pkg_pane != nullptr && entry_pane != nullptr &&
                      geo_page.package_list() != nullptr && geo_page.entry_tree() != nullptr &&
                      geo_page.package_list()->parentWidget() == pkg_pane &&
                      geo_page.entry_tree()->parentWidget() == entry_pane,
                  QStringLiteral("① 两个列表各自在自己的面板里"));
            check(pkg_bar != nullptr && entry_bar != nullptr &&
                      pkg_bar->parentWidget() == pkg_pane &&
                      entry_bar->parentWidget() == entry_pane &&
                      geo_page.add_button() != nullptr &&
                      geo_page.add_button()->parentWidget() == pkg_bar,
                  QStringLiteral("① 工具行归位：「加包」回到包工具行、条目工具行归条目面板"));
            check(geo_page.split_list_tree() != nullptr &&
                      geo_page.split_list_tree()->count() == 2 &&
                      geo_page.split_list_tree()->widget(0) == pkg_pane &&                      geo_page.split_list_tree()->widget(1) == entry_pane,
                  QStringLiteral("① 分隔条拖的是两个**面板**（工具行跟着自己的列表走）"));

            // ① 几何取两档：**正常窗口**（900×700，用户实际就在这附近）与**最小页高**
            //   `page_min_h()`。第一档要求"工具行真在列表上方 + 两个面板都够高"；第二档只要求
            //   "都还在、没归零"—— 430 px 里要塞 行+两个面板+任务面板，两个列表各自的
            //   minimumHeight(120/160) 本来就装不下（这是**尺寸下限**，不是这次重排引入的）。
            check(entry_bar != nullptr && entry_pane != nullptr && !entry_pane->isHidden() &&
                      entry_bar->height() > 0 && entry_bar->height() <= xrkit::core::px(60),
                  QStringLiteral("① 有内容时条目工具行只占一行（实测 %1 ≤ %2；被拉满就是"
                                 "\"隐藏控件的空间落进兄弟控件\"那个坑）")
                      .arg(entry_bar != nullptr ? entry_bar->height() : -1)
                      .arg(xrkit::core::px(60)));

            const auto measure = [&](int w, int h) {
                geo_page.resize(w, h);
                if (geo_page.layout() != nullptr) geo_page.layout()->activate();
                QApplication::processEvents();
            };

            measure(900, 700);
            const int big_pkg = pkg_pane != nullptr ? pkg_pane->height() : 0;
            const int big_entry = entry_pane != nullptr ? entry_pane->height() : 0;
            const int bar_y = entry_bar != nullptr ? entry_bar->y() : -1;
            const int tree_y = geo_page.entry_tree() != nullptr ? geo_page.entry_tree()->y() : -1;
            check(bar_y >= 0 && tree_y > bar_y,
                  QStringLiteral("① 正常窗口下条目工具行在树**上面**（行 y=%1 < 树 y=%2）")
                      .arg(bar_y)
                      .arg(tree_y));
            check(big_pkg >= xrkit::core::px(120) && big_entry >= xrkit::core::px(160),
                  QStringLiteral("① 正常窗口 900×700 下两个面板都够高（包 %1 ≥ %2；条目 %3 ≥ %4）")
                      .arg(big_pkg)
                      .arg(xrkit::core::px(120))
                      .arg(big_entry)
                      .arg(xrkit::core::px(160)));

            measure(720, page_min_h());
            const int small_pkg = pkg_pane != nullptr ? pkg_pane->height() : 0;
            const int small_entry = entry_pane != nullptr ? entry_pane->height() : 0;
            check(small_pkg > xrkit::core::px(40) && small_entry > xrkit::core::px(40),
                  QStringLiteral("① 最小页高 %1 下两个面板都没被挤成一条（包 %2 / 条目 %3）")
                      .arg(page_min_h())
                      .arg(small_pkg)
                      .arg(small_entry));
            result.notes.append(
                QStringLiteral("① 结构重排几何：900×700 ⇒ 包 %1 / 条目 %2（工具行 y=%3 < 树 y=%4）；"
                               "720×%5 ⇒ 包 %6 / 条目 %7")
                    .arg(big_pkg)
                    .arg(big_entry)
                    .arg(bar_y)
                    .arg(tree_y)
                    .arg(page_min_h())
                    .arg(small_pkg)
                    .arg(small_entry));
        }

        // ★ 2026-09-30 用户第四轮 ⑦（「现在还是慢得不行。最起码，要追上 X-Ray Engine 这个优化
        //   奇烂的老东西吧。」）：DB 列条目改 **seek 式**（只读块头 + 尾部条目表）。
        //   旧实现整文件读进内存 —— 实测 1900 MB 的包光读盘 8,063 ms（只与文件大小成正比）。
        //   判据三条，全部落在**真实字节**上：
        //     ① 撑到 32 MB 的包，`list_db_seek` 必须走 seek 且实际读进来的字节 ≪ 文件大小；
        //     ② 条目表与内存版 `unpack_db` **逐字段一致**（路径/offset/两个尺寸/is_dir/use_lzhuf）；
        //     ③ 单条目 `extract_entry_seek` 与内存版 `extract_entry` **逐字节一致**。
        //   判红办法：把 `list_db_seek` 的 seek 分支短路掉（直接走整文件读）⇒ ① 立刻红。
        {
            namespace efs = xrkit::engine::fs;
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dbseek"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);

            const auto rd32 = [](const std::vector<uint8_t>& v, size_t p) {
                return static_cast<uint32_t>(v[p]) | (static_cast<uint32_t>(v[p + 1]) << 8) |
                       (static_cast<uint32_t>(v[p + 2]) << 16) |
                       (static_cast<uint32_t>(v[p + 3]) << 24);
            };
            std::vector<efs::DbPackItem> items;
            items.push_back({std::string("a.txt"), std::vector<uint8_t>(64, 'a'), false});
            items.push_back({std::string("dir"), {}, true});
            items.push_back({std::string("dir/b.bin"), std::vector<uint8_t>(4096, 7), false});
            const efs::DbFormat fmt = *efs::db_format_from_any("xdb");
            const std::vector<uint8_t> packed = efs::pack_db(items, fmt);
            const uint32_t old_data_sz = rd32(packed, 4);
            const size_t tail = 8 + static_cast<size_t>(old_data_sz);   // 尾部 HEADER 块的起点

            // 把 DATA 块撑到 32 MB（真档案里 DATA 就是几百 MB～几 GB），尾部那一块原样搬过去
            const uint32_t big_sz = 32u * 1024u * 1024u;
            std::vector<uint8_t> big;
            big.reserve(8 + static_cast<size_t>(big_sz) + (packed.size() - tail));
            big.push_back(0);
            big.push_back(0);
            big.push_back(0);
            big.push_back(0);
            big.push_back(static_cast<uint8_t>(big_sz & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 8) & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 16) & 0xFF));
            big.push_back(static_cast<uint8_t>((big_sz >> 24) & 0xFF));
            big.resize(8 + static_cast<size_t>(big_sz), 0);
            big.insert(big.end(), packed.begin() + static_cast<long>(tail), packed.end());

            const QString big_path = dir + QStringLiteral("/big.db");
            {
                QFile file(big_path);
                if (file.open(QIODevice::WriteOnly)) {
                    file.write(reinterpret_cast<const char*>(big.data()),
                               static_cast<qint64>(big.size()));
                    file.close();
                }
            }

            const efs::DbList mem = efs::unpack_db(big, fmt);
            const efs::DbListing seek = efs::list_db_seek(big_path.toStdString(), fmt);
            check(seek.ok && seek.used_seek && seek.file_size == big.size() &&
                      seek.header_bytes > 0 &&
                      seek.header_bytes < static_cast<uint64_t>(big.size()) / 100,
                  QStringLiteral("⑦' DB seek 式：32 MB 的包只读了 %1 B / 共 %2 B（used_seek=%3）")
                      .arg(seek.header_bytes)
                      .arg(seek.file_size)
                      .arg(seek.used_seek ? 1 : 0));

            const auto entry_row = [](const efs::DbEntry& e) {
                return QStringLiteral("%1|%2|%3|%4|%5|%6")
                    .arg(e.is_dir ? 1 : 0)
                    .arg(QString::fromStdString(e.path))
                    .arg(static_cast<qulonglong>(e.offset))
                    .arg(static_cast<qulonglong>(e.size_real))
                    .arg(static_cast<qulonglong>(e.size_comp))
                    .arg(e.use_lzhuf ? 1 : 0);
            };
            QStringList mem_rows;
            for (const efs::DbEntry& e : mem.entries) mem_rows << entry_row(e);
            QStringList seek_rows;
            for (const efs::DbEntry& e : seek.entries) seek_rows << entry_row(e);
            check(!seek_rows.isEmpty() && seek_rows == mem_rows,
                  QStringLiteral("⑦' seek 版条目表与内存版**逐字段一致**（%1 vs %2 条）")
                      .arg(seek_rows.size())
                      .arg(mem_rows.size()));

            int extract_bad = 0;
            for (const efs::DbEntry& e : seek.entries) {
                if (e.is_dir) continue;
                const efs::DbExtract a = efs::extract_entry(big, e);
                const efs::DbExtract b = efs::extract_entry_seek(big_path.toStdString(), e);
                if (a.ok != b.ok || a.data != b.data) ++extract_bad;
            }
            check(extract_bad == 0,
                  QStringLiteral("⑦' 单条目 seek 提取与内存版逐字节一致（%1 条不符 / %2 条条目）")
                      .arg(extract_bad)
                      .arg(seek.entries.size()));
            result.notes.append(
                QStringLiteral("⑦' DB seek：32 MB 包读取 %1 B（占 %2%%），条目 %3 条")
                    .arg(seek.header_bytes)
                    .arg(seek.file_size > 0 ? static_cast<int>(seek.header_bytes * 100 /
                                                               seek.file_size)
                                            : -1)
                    .arg(seek.entries.size()));
            QDir(dir).removeRecursively();
        }

        // ★ 2026-09-28（用户："FS 的方便程度比 1.x 下降了不少，回头看看 1.1.0"）：按 1.x 补回来的
    //   那批"方便"，每条都要有**能判红**的判据，且尽量落到**真实字节**（不只是"控件在不在"）。
    {
        namespace efs2 = xrkit::engine::fs;
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fs1x"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        // 造一个真 XDB 包，放到 <dir>/pk/ 下（好让"输出目录推断"与"按包名分目录"都能验）
        const QString pk_dir = dir + QStringLiteral("/pk");
        QDir().mkpath(pk_dir);
        const QString db_path = pk_dir + QStringLiteral("/gamedata.db0");
        {
            std::vector<efs2::DbPackItem> items;
            efs2::DbPackItem a;
            a.path = "a.txt";
            a.data = {static_cast<uint8_t>('A'), static_cast<uint8_t>('A')};
            efs2::DbPackItem b;
            b.path = "dir/keep.txt";
            b.data = {static_cast<uint8_t>('B')};
            items.push_back(a);
            items.push_back(b);
            const std::vector<uint8_t> raw = efs2::pack_db(items, efs2::DbFormat::Xdb);
            std::ofstream out(db_path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
        }
        // 1.x `_is_db` / `_is_sq` 的名字规则（扫描目录与"封包排除包文件"共用一份）
        // ★★ 2026-10-04 用户问「`.xdb`，我们的工具能识别吗？」⇒ 实测**不能**（旧规则只取"点后两位"＝`xd`）
        //   ⇒ 漏了 `.xdb` 这个家族（真机依据：`sop_standalone.7z` 里的 `patch_00.xdb` 就是 `format=xdb`）。
        //   这条锁**同时钉三个家族**：把 `.xdb` 分支删掉/改回两位比较，立刻红。
        check(efs2::is_package_file_name("x.db0") && efs2::is_package_file_name("x.db_base") &&
                  efs2::is_package_file_name("x.sqfs") && efs2::is_package_file_name("x.sq_textures") &&
                  efs2::is_package_file_name("x.xdb") && efs2::is_package_file_name("x.xdb_base") &&
                  efs2::is_package_file_name("patch_00.xdb"),
              QStringLiteral("1.x 包名规则：.db*/​.sq*/★.xdb* 都算包（引擎只看 basename 最后一个扩展）"));
        check(!efs2::is_package_file_name("x.txt") && !efs2::is_package_file_name("xdbc") &&
                  !efs2::is_package_file_name("x.db.bak2") && !efs2::is_package_file_name("x.xdoc") &&
                  !efs2::is_package_file_name("x") && !efs2::is_package_file_name("x."),
              QStringLiteral("1.x 包名规则：非包文件不许误认（x.txt / xdbc / x.db.bak2 / ★x.xdoc / x / x.）"));

        // ★★ 同一条缺陷的**真实症状锁**：一个含 `.xdb` 的目录走**用户真实入口** `add_paths()`（拖入/加文件夹
        //   模式都是它；`scan_dir()` 只负责收集、加载在调用方）⇒ 必须**收进来并加载**（源 1 / 已加载 1）。
        //   旧规则下 `.xdb` 被判为非包 ⇒ **静默忽略**（源 0）⇒ 改回旧规则立刻红。
        {
            const QString xdir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xdb"));
            QDir(xdir).removeRecursively();
            QDir().mkpath(xdir);
            std::vector<efs2::DbPackItem> xitems;
            efs2::DbPackItem xone;
            xone.path = "t/a.txt";
            xone.data = {'x'};
            xitems.push_back(xone);
            const std::vector<uint8_t> xraw = efs2::pack_db(xitems, efs2::DbFormat::Xdb);
            std::ofstream xout((xdir + QStringLiteral("/patch_00.xdb")).toStdString(),
                               std::ios::binary | std::ios::trunc);
            xout.write(reinterpret_cast<const char*>(xraw.data()),
                       static_cast<std::streamsize>(xraw.size()));
            xout.close();
            FsPage xdb_page;
            xdb_page.resize(720, page_min_h());
            xdb_page.show();
            QApplication::processEvents();
            xdb_page.add_paths(QStringList{xdir});
            QApplication::processEvents();
            check(xdb_page.source_count() == 1 && xdb_page.loaded_source_count() == 1,
                  QStringLiteral("★.xdb 也是包：含 patch_00.xdb 的目录走 add_paths 要被收进来并加载"
                                 "（源 %1 期望 1 / 已加载 %2 期望 1）")
                      .arg(xdb_page.source_count())
                      .arg(xdb_page.loaded_source_count()));
            xdb_page.hide();
            QDir(xdir).removeRecursively();
        }

        // ★★ 2026-10-04 用户拍的**架构红线**（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI
        //   的细节功能比 CLI 还多的。**」⇒ 解包**落点规则**从 FS 页搬进**引擎**
        //   （`efs::package_folder_name` / `unpack_target_dir` / `plan_unpack_targets`），
        //   shell 与 CLI（`--unpack-package`）**共用同一份**。下面三条锁把"两处实现"钉死。
        {
            const std::string fn = efs2::package_folder_name("patch_00.xdb");
            check(efs2::package_folder_name("gamedata.db0") == "gamedata" &&
                      efs2::package_folder_name("gamedata.sq_base") == "gamedata" &&
                      efs2::package_folder_name("a.b.db0") == "a.b" && fn == "patch_00",
                  QStringLiteral("落点① 引擎包名去扩展名（gamedata.db0→gamedata / sq_base→gamedata / "
                                 "a.b.db0→a.b / patch_00.xdb→%1）")
                      .arg(QString::fromStdString(fn)));
            const std::string root = "E:/out";
            const std::string carry =
                efs2::unpack_target_dir(root, "E:/x/configs.db", {"configs/a.txt", "configs/b/c.ltx"});
            // ★★ 真机形状：条目表里**有一条名字正好等于壳的裸目录项**（`configs.db` 就是这样）
            //   —— 这也必须算"自带壳"，否则真机会叠出 `configs/configs`（2026-10-04 实测抓到的缺陷）。
            const std::string carry_bare =
                efs2::unpack_target_dir(root, "E:/x/configs.db", {"configs", "configs/ai_tweaks/x.ltx"});
            const std::string plain = efs2::unpack_target_dir(root, "E:/x/gamedata.db0", {"levels/a"});
            const std::string empty = efs2::unpack_target_dir(root, "E:/x/gamedata.db0", {});
            // ★★ 2026-10-04 **判红锁①**（缺陷①：量词"全部"⇒"过半"）：真机 9 个 VESSEL 包的形状 ——
            //   裸壳项 `ai` + **一条根级 `readme.md`** + 一堆 `ai/…`。旧量词（"**全部**条目都带壳"）
            //   被 `readme.md` 一条掀翻 ⇒ 判"不带壳" ⇒ 解出 `ai/ai/alife/…`，违背用户口径
            //   「这个案例，就解出来一个 configs。」⇒ 新量词"**过半**带壳"必须判"自带壳"＝落点即输出根。
            //   （把 `shelled * 2 > entry_paths.size()` 改回"任一条不带壳就 false" ⇒ 立刻红。）
            const std::string mixed =
                efs2::unpack_target_dir(root, "E:/x/ai.db", {"ai", "readme.md", "ai/alife/x.efd"});
            check(carry == root && carry_bare == root && plain == "E:/out/gamedata" &&
                      empty == "E:/out/gamedata" && carry.find("/.") == std::string::npos &&
                      mixed == root,
                  QStringLiteral("落点② 自带壳不叠第二层（%1）/ **裸壳目录项也算自带壳**（%2，真机形状）/ "
                                 "不带壳＝<根>/<包名>（%3）/ 空条目也叠（%4）/ 绝不出现 <根>/.（会被 "
                                 "safe_out_path 拒）/ **过半带壳就算自带壳**（%5：VESSEL 形状 = 裸壳项 + "
                                 "一条根级 readme.md + 一堆 壳/…）")
                      .arg(QString::fromStdString(carry), QString::fromStdString(carry_bare),
                           QString::fromStdString(plain), QString::fromStdString(empty),
                           QString::fromStdString(mixed)));
        }
        // ★★ 2026-10-04 **判红锁②**（缺陷②：报数与"解包产物预览"口径不符）—— 合成一枚
        //   **条目表重复登记**的 DB 包：`pack_db` 不查重，同一条路径给两条记录 ⇒ 包内两条、
        //   盘上一份（后写覆盖前写）—— 正是真机 `IWP\database\maps\*.db` 的形状（除根记录外的
        //   每条记录登记两遍；独立解码已证实重复在包内，不是 dump 的 bug）。
        //   判据：计数走**最终产物**口径（files=1 / bytes=2 / dirs=1），并如实报出重复记录数。
        //   （把计数改回"每处理一条记录就 +1" ⇒ files=2 / bytes=4 / dups=0 ⇒ 立刻红。）
        {
            namespace efs3 = xrkit::engine::fs;
            const QString dup_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dup"));
            QDir(dup_root).removeRecursively();
            QDir().mkpath(dup_root);
            const QString dup_pkg = dup_root + QStringLiteral("/dup.db");
            std::vector<efs3::DbPackItem> items;
            efs3::DbPackItem dup_dir;
            dup_dir.path = "dupdir";
            dup_dir.is_dir = true;
            items.push_back(dup_dir);
            efs3::DbPackItem dup_file;
            dup_file.path = "dupdir/a.txt";
            dup_file.data = {'X', 'Y'};
            items.push_back(dup_file);
            items.push_back(dup_file);   // ← 同路径的**第二条记录**（真机 maps\*.db 的形状）
            const std::vector<uint8_t> dup_raw = efs3::pack_db(items, efs3::DbFormat::Xdb);
            std::ofstream dup_out(dup_pkg.toStdString(), std::ios::binary | std::ios::trunc);
            dup_out.write(reinterpret_cast<const char*>(dup_raw.data()),
                          static_cast<std::streamsize>(dup_raw.size()));
            dup_out.close();

            const QString dup_target_root = dup_root + QStringLiteral("/out");
            // 落点照真机那条路算（CLI/GUI 共用的"包名壳"规则）：壳 `dup`、条目首段 `dupdir`
            //（不带壳）⇒ <根>/dup；然后才把**算好的落点**交给引擎导出（真机也是这个顺序）。
            const std::string dup_target = efs3::unpack_target_dir(
                dup_target_root.toStdString(), dup_pkg.toStdString(), {"dupdir", "dupdir/a.txt"});
            const efs3::ExtractTreeResult dup_res =
                efs3::extract_package(dup_pkg.toStdString(), dup_target);
            // 文件内容 2 字节，而且盘上只有**一份**（第二条记录是后写覆盖前写）
            const qint64 landed = QFileInfo(QString::fromStdString(dup_target) +
                                            QStringLiteral("/dupdir/a.txt")).size();
            const bool landed_at_shell =
                dup_target == (dup_target_root + QStringLiteral("/dup")).toStdString();
            QString dup_tree;
            {
                QDirIterator it(dup_root, QDir::AllEntries | QDir::NoDotAndDotDot,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) dup_tree += it.next() + QStringLiteral(" | ");
            }
            dup_tree = QString::fromStdString(dup_target) + QStringLiteral(" => ") + dup_tree;
            check(dup_res.ok && landed_at_shell && dup_res.files_written == 1 &&
                      dup_res.bytes_written == 2 && dup_res.dirs_created == 1 &&
                      dup_res.duplicate_records == 1 && landed == 2,
                  QStringLiteral("产物计数① 包内条目表重复登记 ⇒ 报数走**最终产物**口径"
                                 "（files=%1/期望 1 · bytes=%2/期望 2 · dirs=%3/期望 1 · "
                                 "dups=%4/期望 1 · 落盘 a.txt=%5/期望 2 字节 · 树=%6）")
                      .arg(dup_res.files_written)
                      .arg(dup_res.bytes_written)
                      .arg(dup_res.dirs_created)
                      .arg(dup_res.duplicate_records)
                      .arg(landed)
                      .arg(dup_tree));
            // 同一条判据的第二半：**计划行**（协议层的记录数）与**产物口径**必须是两个数
            check(efs3::unique_entry_count({"dupdir", "dupdir/a.txt", "dupdir/a.txt"}) == 2,
                  QStringLiteral("产物计数② 条目表去重：records=3 ⇒ entries=2（唯一路径数）"));
            QDir(dup_root).removeRecursively();
        }
        // ★ 2026-10-04（同一条架构红线）：**输出根缺省**与**封包落点/扩展名**这两条原先也只写在 FS 页里
        //   ⇒ CLI 复现不了（命令行与界面会算出不同的根/落点）。现在实现搬到引擎（`resolve_output_root`
        //   / `pack_ext_for` / `resolve_pack_target`），这里钉住引擎那一份的值，并断言**页面侧==引擎侧**
        //   （页面侧现已是对引擎的薄包装 ⇒ 谁把规则搬回页面或改任一侧，立刻红）。
        {
            const std::string eng_root = efs2::resolve_output_root("E:/x/a.db", "C:/cur");
            const std::string eng_root_empty = efs2::resolve_output_root("", "C:/cur");
            check(eng_root == "E:/x" && eng_root_empty == "C:/cur",
                  QStringLiteral("落点④ 输出根缺省（引擎）：有源⇒源所在目录「%1」；无源⇒当前目录「%2」")
                      .arg(QString::fromStdString(eng_root), QString::fromStdString(eng_root_empty)));
            check(FsPage::pack_ext_for(QStringLiteral("xdb")) ==
                          QString::fromStdString(efs2::pack_ext_for("xdb")) &&
                      FsPage::pack_ext_for(QStringLiteral("sqfs")) ==
                          QString::fromStdString(efs2::pack_ext_for("sqfs")) &&
                      efs2::pack_ext_for("xdb") == ".xdb.db" &&
                      efs2::pack_ext_for("sqfs") == ".sqfs",
                  QStringLiteral("落点⑤ 封包扩展名：页面==引擎（xdb「%1」/ sqfs「%2」）")
                      .arg(FsPage::pack_ext_for(QStringLiteral("xdb")),
                           FsPage::pack_ext_for(QStringLiteral("sqfs"))));
            // ⚠ 目录分支必须喂**真实存在的目录**（引擎照 GUI 用 `is_directory()` 判 —— 不存在的路径
            //   会按"无扩展名的路径"处理 ⇒ 补扩展名到它后面；这正是 1.x/GUI 的行为，别改语义）。
            const QString tdir = QDir::temp().filePath(QStringLiteral("xrkit_pack_target"));
            QDir(tdir).removeRecursively();
            QDir().mkpath(tdir);
            const efs2::PackTarget t_dir =
                efs2::resolve_pack_target(tdir.toStdString(), "mypack", "xdb");
            const efs2::PackTarget t_ext = efs2::resolve_pack_target("E:/out/mypack", "", "xdb");
            const efs2::PackTarget t_full = efs2::resolve_pack_target("E:/out/keep.db0", "", "xdb");
            const efs2::PackTarget t_none = efs2::resolve_pack_target("", "mypack", "xdb");
            const QString dir_path = QString::fromStdString(t_dir.path);
            check(!t_dir.needs_frontend_dialog && dir_path.endsWith(QStringLiteral("mypack.xdb.db")) &&
                      t_ext.path == "E:/out/mypack.xdb.db" &&   // 无扩展名 ⇒ 补 `.xdb.db`
                      t_full.path == "E:/out/keep.db0" &&       // 给了全路径 ⇒ 原样不动
                      t_none.needs_frontend_dialog && t_none.path == "mypack.xdb.db",
                  QStringLiteral("落点⑤ 封包落点：目录+基名「%1」/ 无扩展名「%2」/ 全路径「%3」/ 空⇒需前端弹框「%4」")
                      .arg(dir_path, QString::fromStdString(t_ext.path),
                           QString::fromStdString(t_full.path), QString::fromStdString(t_none.path)));
        }
        // ★★ **GUI 与 CLI 必须同落点**：本页 `plan_unpack()` 算出的 target 集合 == 引擎
        //   `plan_unpack_targets()` 算的（谁把落点规则搬回页面里、或改动任一侧 ⇒ 立刻红）。
        {
            const QString plan_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plan"));
            QDir(plan_root).removeRecursively();
            QDir().mkpath(plan_root);
            const QString a_path = plan_root + QStringLiteral("/plan_a.db0");
            const QString b_path = plan_root + QStringLiteral("/plan_b.db0");
            std::vector<efs2::DbPackItem> a_items;   // 条目**不带**同名壳 ⇒ 落点 = <根>/plan_a
            efs2::DbPackItem ai;
            ai.path = "levels/a.txt";
            ai.data = {'a'};
            a_items.push_back(ai);
            std::vector<efs2::DbPackItem> b_items;   // 条目**自带**同名壳 `plan_b/` ⇒ 落点 = <根>
            efs2::DbPackItem bi;
            bi.path = "plan_b/x.txt";
            bi.data = {'x'};
            b_items.push_back(bi);
            const std::pair<QString, std::vector<efs2::DbPackItem>*> pairs[2] = {{a_path, &a_items},
                                                                                {b_path, &b_items}};
            for (const auto& item : pairs) {
                const std::vector<uint8_t> raw = efs2::pack_db(*item.second, efs2::DbFormat::Xdb);
                std::ofstream out(item.first.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            FsPage plan_page;
            plan_page.resize(720, page_min_h());
            plan_page.show();
            QApplication::processEvents();
            plan_page.out_edit()->setText(QDir::toNativeSeparators(plan_root));
            plan_page.add_paths(QStringList{a_path, b_path});
            QApplication::processEvents();
            QString root_used;
            const std::vector<FsPage::UnpackJob> jobs = plan_page.plan_unpack(false, &root_used);
            std::vector<std::pair<std::string, std::vector<std::string>>> srcs;
            for (const FsPage::UnpackJob& j : jobs) {
                std::string err;
                const std::vector<efs2::EntryInfo> entries =
                    efs2::package_entries(j.image, std::nullopt, err);
                std::vector<std::string> paths;
                paths.reserve(entries.size());
                for (const efs2::EntryInfo& e : entries) paths.push_back(e.path);
                srcs.emplace_back(j.image, std::move(paths));
            }
            const std::vector<efs2::UnpackTarget> eng =
                efs2::plan_unpack_targets(root_used.toStdString(), srcs);
            bool same = !jobs.empty() && eng.size() == jobs.size();
            QString detail;
            for (size_t i = 0; same && i < jobs.size(); ++i) {
                if (eng[i].image != jobs[i].image || eng[i].target != jobs[i].target) same = false;
                detail += QString::fromStdString(eng[i].target) + QStringLiteral(" | ");
            }
            check(same && jobs.size() == 2,
                  QStringLiteral("落点③ GUI 与引擎**同落点**（%1 个源；引擎算的：%2）")
                      .arg(jobs.size())
                      .arg(detail));
            plan_page.hide();
            QDir(plan_root).removeRecursively();
        }

        FsPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // —— 解包输出目录：1.x `_resolve_output`（空则按第一个包推断并**回填到输入框**）
        check(page.out_edit() != nullptr && page.out_edit()->text().isEmpty(),
              QStringLiteral("① 输出框初始为空（不改用户没填的东西）"));
        check(page.add_paths({db_path}) == 1, QStringLiteral("① 加包：真 DB 包要能加载"));
        // ★ 条目**大小**必须真的从包里带进合并树（用户 2026-09-28 对比 1.x 的树：人家右列有大小）
        //   —— 这条也是"树右列有数可显示"的前提；注入（不填 size）即红。
        // ★★ 2026-10-05（#2）：右栏＝**解包产物预览** ⇒ 这一包（`gamedata.db0`）的条目全挂在
        //   包名壳 `gamedata/` 下 ⇒ 查大小要按**落点路径**查（原来按包内原始路径，是"没有壳"
        //   时代的形态）。**判据本身（大小进模型）一个字没改。**
        check(page.node_size(QStringLiteral("gamedata/a.txt")) == 2 &&
                  page.node_size(QStringLiteral("gamedata/dir/keep.txt")) == 1 &&
                  page.node_size(QStringLiteral("gamedata/dir")) == 0,
              QStringLiteral("④ 条目大小进模型：壳下 a.txt=2 / dir/keep.txt=1 / 目录=0（实测 %1/%2/%3）")
                  .arg(page.node_size(QStringLiteral("gamedata/a.txt")))
                  .arg(page.node_size(QStringLiteral("gamedata/dir/keep.txt")))
                  .arg(page.node_size(QStringLiteral("gamedata/dir"))));
        const QString resolved = page.resolve_output_dir();
        check(QDir::cleanPath(resolved) == QDir::cleanPath(pk_dir) &&
                  QDir::cleanPath(page.out_edit()->text()) == QDir::cleanPath(pk_dir),
              QStringLiteral("① 输出目录按第一个包推断并回填（实测 %1 → 框里 %2）")
                  .arg(resolved, page.out_edit()->text()));
        // 1.x `_package_folder`：splitext(basename)
        check(FsPage::package_folder_name(QStringLiteral("gamedata.db0")) == QStringLiteral("gamedata") &&
                  FsPage::package_folder_name(QStringLiteral("gamedata.sq_base")) ==
                      QStringLiteral("gamedata") &&
                  FsPage::package_folder_name(QStringLiteral("a.b.db0")) == QStringLiteral("a.b"),
              QStringLiteral("① 包名→子目录名 = 去掉最后一个扩展（照 1.x splitext）"));

        // —— 批量解包：真产物必须落在 `<输出>/<包名>/` 下、内容逐字节对得上（1.x 同一落盘规则）
        const QString out_root = dir + QStringLiteral("/unpacked");
        QDir().mkpath(out_root);
        page.out_edit()->setText(out_root);
        QString summary;
        const int written = page.unpack_all(&summary);
        const QString out_a = out_root + QStringLiteral("/gamedata/a.txt");
        const QString out_b = out_root + QStringLiteral("/gamedata/dir/keep.txt");
        QFile fa(out_a);
        QFile fb(out_b);
        bool a_ok = fa.open(QIODevice::ReadOnly) && fa.readAll() == QByteArray("AA");
        bool b_ok = fb.open(QIODevice::ReadOnly) && fb.readAll() == QByteArray("B");
        check(written == 2 && a_ok && b_ok,
              QStringLiteral("① 批量解包：产物落在 <输出>/<包名>/（%1 个文件，a=%2 b=%3）｜%4")
                  .arg(written)
                  .arg(a_ok ? 1 : 0)
                  .arg(b_ok ? 1 : 0)
                  .arg(summary.left(200)));
        check(summary.contains(QDir::cleanPath(out_root)) ||
                  summary.contains(QDir::toNativeSeparators(out_root)),
              QStringLiteral("① 解包结论要报出真实落盘目录（%1）").arg(summary.left(60)));

        // ★★ 2026-10-03 **新增锁**（用户定案的产物布局口径，原话）：「如果就拖入 `E:\...\IWP\database\
        //   config` 的 `configs`，那么**解包出来是一个 `configs` 文件夹**（和 `gamedata.db*`/`sq*`
        //   解出 `gamedata` 一样），**不包含 `database` 和 `config`**。」；我追问"包内路径本身就带一层
        //   `configs/` 会不会变成 `configs/configs`"他定案：**「这个案例，就解出来一个 configs。」**
        //   ⇒ 夹具刻意造成真机那种形状：包**嵌在 `database/config/` 里**、文件名 `configs.db0`、
        //     而且**包内路径自带 `configs/` 前缀**。判据＝产物顶层**恰好一个 `configs`**、
        //     **不出现 `configs/configs`**、**不出现 `database`/`config`**。
        //   （把 `plan_unpack` 里"包内已自带同名壳就不再叠壳"那段去掉 ⇒ 立刻冒出 `configs/configs` ⇒ 红。）
        {
            const QString nest = dir + QStringLiteral("/database/config");
            QDir().mkpath(nest);
            const QString nest_pkg = nest + QStringLiteral("/configs.db0");
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem dir_item;
            dir_item.path = "configs/ai_tweaks";
            dir_item.is_dir = true;
            items.push_back(dir_item);
            efs::DbPackItem file_item;
            file_item.path = "configs/ai_tweaks/ai.txt";
            file_item.data = {'K'};
            items.push_back(file_item);
            const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
            std::ofstream raw_out(nest_pkg.toStdString(), std::ios::binary | std::ios::trunc);
            raw_out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            raw_out.close();

            FsPage nest_page;
            nest_page.resize(760, page_min_h());
            nest_page.show();
            QApplication::processEvents();
            nest_page.add_paths({nest_pkg});
            const QString nest_out = dir + QStringLiteral("/nest_out");
            QDir().mkpath(nest_out);
            nest_page.out_edit()->setText(nest_out);
            QString nest_summary;
            nest_page.unpack_all(&nest_summary);
            const bool one_shell =
                QFileInfo(nest_out + QStringLiteral("/configs/ai_tweaks/ai.txt")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/configs/configs")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/database")).exists() &&
                !QFileInfo(nest_out + QStringLiteral("/config")).exists();
            check(one_shell,
                  QStringLiteral("产物布局① 包嵌在 database/config 里、包内自带 configs/ 前缀 ⇒ 顶层仍是"
                                 "**一个** configs（不出现 configs/configs、database、config）｜%1")
                      .arg(nest_summary.left(160)));
            nest_page.hide();
        }

        // —— 包内选择/搜索/计数/清空（1.x 那几颗按钮的行为）
        check(page.entry_count_label() != nullptr &&
                  page.entry_count_label()->text().contains(QStringLiteral("2 文件")),
              QStringLiteral("① 条目计数 = 过滤后的文件数（实测「%1」）")
                  .arg(page.entry_count_label() != nullptr ? page.entry_count_label()->text()
                                                          : QString()));
        page.search_edit()->setText(QStringLiteral("keep"));
        QApplication::processEvents();
        // 只匹配**行自己的路径**（1.x `_render` 同一口径）⇒ 命中 dir/keep.txt 这一行；
        // 父目录 dir/ 不含 "keep" 所以不显示（我第一版按"父目录也跟着来"写成 2，是我想多了）
        // 过滤后：树里只留匹配项（含它的祖先链），且**命中必须看得见**
        check(page.entry_built_file_count() == 1 &&
                  page.find_entry_item(QStringLiteral("gamedata/dir/keep.txt")) != nullptr,
              QStringLiteral("① 搜索过滤：只留匹配项且它可见（已建文件项 %1）")
                  .arg(page.entry_built_file_count()));
        page.search_edit()->clear();
        QApplication::processEvents();
        page.check_all_button()->click();            // 包内全选
        const int all_checked = page.checked_file_count();
        page.check_none_button()->click();            // 包内全不选（1.x `_check_none`）
        check(all_checked == 2 && page.checked_file_count() == 0,
              QStringLiteral("① 包内全选/全不选（全选 %1 → 全不选 %2）")
                  .arg(all_checked)
                  .arg(page.checked_file_count()));
        check(page.package_list()->topLevelItemCount() == 1 && page.loaded_source_count() == 1,
              QStringLiteral("① 包列表行数/已加载数对得上（行 %1 / 已加载 %2）")
                  .arg(page.package_list()->topLevelItemCount())
                  .arg(page.loaded_source_count()));

        // —— 扫描目录：1.x `_scan_dir`（递归找包、**不自动加载**）→ 选中 → 加载选中
        {
            FsPage scan_page;
            scan_page.resize(720, page_min_h());
            scan_page.show();
            QApplication::processEvents();
            const int added = scan_page.scan_dir(dir);   // <dir>/pk/gamedata.db0 + <dir>/unpacked/...
            check(added >= 1 && scan_page.source_count() >= 1,
                  QStringLiteral("① 扫描目录：递归找到包（新增 %1，共 %2）")
                      .arg(added)
                      .arg(scan_page.source_count()));
            check(scan_page.loaded_source_count() == 0,
                  QStringLiteral("① 扫描目录**不自动加载**（已加载 %1，应为 0）")
                      .arg(scan_page.loaded_source_count()));
            // ★ 2026-10-02 树化：源节点散在树里（中间夹着分组节点）⇒ 必须**递归选源节点**，
            //   数顶层行是选不到源的（`<dir>/pk/gamedata.db0` 现在挂在分组节点 `pk/` 下面）。
            select_all_package_sources(scan_page.package_list());
            check(package_source_node_count(scan_page.package_list()) == scan_page.source_count(),
                  QStringLiteral("① 树化后**每个源**都有一个节点（源节点 %1 / 源 %2）")
                      .arg(package_source_node_count(scan_page.package_list()))
                      .arg(scan_page.source_count()));
            scan_page.load_selected();
            check(scan_page.loaded_source_count() == scan_page.source_count(),
                  QStringLiteral("① 加载选中：选中的都进树（已加载 %1 / 共 %2）")
                      .arg(scan_page.loaded_source_count())
                      .arg(scan_page.source_count()));
            // ★ 2026-10-05（#2）：右栏是**懒建**的，而且条目现在挂在包名壳下 ⇒
            //   "加载进来了"要看**顶层项数**（＝壳那几个目录），不能看"已建文件项"
            //   （展开前一个文件项都不建 —— 那正是懒建）。**判据（取消加载后树被摘空）没变。**
            const int before_unload_top = scan_page.entry_top_level_count();
            select_all_package_sources(scan_page.package_list());
            scan_page.unload_selected();
            check(before_unload_top > 0 && scan_page.loaded_source_count() == 0 &&
                      scan_page.entry_top_level_count() == 0 &&
                      scan_page.entry_built_file_count() == 0,
                  QStringLiteral("① 取消加载：条目从树里摘掉（%1 个顶层项 → %2，已建文件项 %3）")
                      .arg(before_unload_top)
                      .arg(scan_page.entry_top_level_count())
                      .arg(scan_page.entry_built_file_count()));
            scan_page.clear_all();
            check(scan_page.source_count() == 0 &&
                      scan_page.package_list()->topLevelItemCount() == 0,
                  QStringLiteral("① 清空：已知包与列表都空（源 %1 / 行 %2）")
                      .arg(scan_page.source_count())
                      .arg(scan_page.package_list()->topLevelItemCount()));
            scan_page.hide();
        }

        // —— 引擎侧：勾选一个包 = 它的条目全勾（1.x `_db_toggle`）
        {
            efs2::MergeTree tree = efs2::build_tree(efs2::MergeInput{"base", {{"a.txt", false}, {"d/only_a.txt", false}}});
            efs2::MergeTree other = efs2::build_tree(efs2::MergeInput{"patch", {{"a.txt", false}, {"d/only_p.txt", false}}});
            tree.merge(other, static_cast<int>(tree.sources.size()));
            const int files_of_patch = tree.toggle_source(1, true);
            int files = 0;
            int checked = 0;
            tree.source_check_state(1, &files, &checked);
            check(tree.checked_file_count() == files_of_patch && files == 2 && checked == 2,
                  QStringLiteral("① 按包勾选：toggle_source 只动这一包的条目（勾 %1 / 该包 %2 个文件 / 其中已勾 %3）")
                      .arg(tree.checked_file_count())
                      .arg(files)
                      .arg(checked));
        }
        // —— 封包面板（1.x `_build_pack_panel` / `_pack`）：**真打一个包再读回来**，不是"按钮在不在"
        {
            const QString pk_root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pack"));
            QDir(pk_root).removeRecursively();
            QDir().mkpath(pk_root + QStringLiteral("/gamedata/scripts"));
            {
                std::ofstream(pk_root.toStdString() + "/gamedata/a.txt",
                              std::ios::binary | std::ios::trunc)
                    << "AA";
                std::ofstream(pk_root.toStdString() + "/gamedata/scripts/b.txt",
                              std::ios::binary | std::ios::trunc)
                    << "BBB";
                // 一个包文件混在目录里：1.x `_is_pack_file` 明确**不把它打进新包**
                std::ofstream(pk_root.toStdString() + "/gamedata/old.db0",
                              std::ios::binary | std::ios::trunc)
                    << "not a package really";
                // 根之外的一个文件：相对路径算不出来 ⇒ 跳过并报数（1.x 同一行为）
                std::ofstream(pk_root.toStdString() + "/outside.txt",
                              std::ios::binary | std::ios::trunc)
                    << "OUT";
            }
            // 引擎侧：收集规则（排除包文件 / 相对 base / 不在 base 之下计入 / 排序确定）
            const efs2::PackCollectResult collected = efs2::collect_pack_files(
                (pk_root + QStringLiteral("/gamedata")).toStdString(),
                {(pk_root + QStringLiteral("/gamedata")).toStdString(),
                 (pk_root + QStringLiteral("/outside.txt")).toStdString()});
            check(collected.items.size() == 2 && collected.skipped_package_files == 1 &&
                      collected.skipped_outside_base == 1 &&
                      collected.items[0].rel == "a.txt" &&
                      collected.items[1].rel == "scripts/b.txt",
                  QStringLiteral("② 封包收集：2 个文件（%1/%2）、排除包文件 %3、越界 %4")
                      .arg(QString::fromStdString(collected.items.empty()
                                                      ? std::string()
                                                      : collected.items[0].rel),
                           QString::fromStdString(collected.items.size() > 1
                                                      ? collected.items[1].rel
                                                      : std::string()))
                      .arg(collected.skipped_package_files)
                      .arg(collected.skipped_outside_base));

            // ① XDB 封包 → 用引擎自己的读侧读回（round-trip，真字节）
            FsPage pack_page;
            pack_page.resize(760, page_min_h());
            pack_page.show();
            QApplication::processEvents();
            pack_page.out_edit()->setText(QDir::toNativeSeparators(
                pk_root + QStringLiteral("/gamedata")));   // base = 这一格
            check(pack_page.add_pack_dir(pk_root + QStringLiteral("/gamedata")) == 2,
                  QStringLiteral("② ＋目录：加进来 2 个文件（实测 %1）").arg(pack_page.pack_item_count()));
            check(FsPage::pack_ext_for(QStringLiteral("xdb")) == QStringLiteral(".xdb.db") &&
                      FsPage::pack_ext_for(QStringLiteral("sqfs")) == QStringLiteral(".sqfs"),
                  QStringLiteral("② 封包扩展名照 1.x：xdb→.xdb.db、sqfs→.sqfs"));
            const QString xdb_out = pk_root + QStringLiteral("/packed.xdb.db");
            pack_page.pack_out_edit()->setText(xdb_out);
            QString pack_summary;
            const bool packed = pack_page.run_pack(&pack_summary);
            std::string read_back_error;
            const std::vector<efs2::EntryInfo> read_back =
                efs2::package_entries(xdb_out.toStdString(), std::nullopt, read_back_error);
            check(packed && read_back_error.empty() && read_back.size() == 2,
                  QStringLiteral("② 封包（XDB）→ 读回 2 个条目（%1）").arg(pack_summary));
            check(pack_page.pack_out_edit()->text().contains(QStringLiteral("packed.xdb.db")),
                  QStringLiteral("② 封包输出路径要回填到输入框（1.x 的 H1）"));

            // ② SquashFS 封包 → `list_entries` 读回
            pack_page.pack_format_combo()->setCurrentIndex(
                pack_page.pack_format_combo()->count() - 1);   // 末项 = SquashFS（fs_formats 的顺序）
            const QString sqfs_out = pk_root + QStringLiteral("/packed.sqfs");
            pack_page.pack_out_edit()->setText(sqfs_out);
            const bool packed_sqfs = pack_page.run_pack(&pack_summary);
            const efs2::ListResult listed_sqfs = efs2::list_entries(sqfs_out.toStdString());
            check(packed_sqfs && listed_sqfs.ok && listed_sqfs.entries.size() >= 2,
                  QStringLiteral("② 封包（SquashFS）→ list_entries 读回 %1 条（%2）")
                      .arg(listed_sqfs.entries.size())
                      .arg(pack_summary));

            // ③ 包列表勾选框 = 整包勾选（1.x `_db_toggle`）
            FsPage check_page;
            check_page.resize(760, page_min_h());
            check_page.show();
            QApplication::processEvents();
            check_page.add_paths({db_path});
            QApplication::processEvents();
            check(check_page.package_list()->topLevelItemCount() == 1 &&
                      check_page.package_list()->topLevelItem(0)->checkState(0) == Qt::Unchecked,
                  QStringLiteral("② 包行有勾选框且初始未勾"));
            check_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 2,
                  QStringLiteral("② 勾一个包 = 该包条目全勾（实测 %1 个文件）")
                      .arg(check_page.checked_file_count()));
            check_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
            QApplication::processEvents();
            check(check_page.checked_file_count() == 0,
                  QStringLiteral("② 取消勾选 = 该包条目全不勾（实测 %1）")
                      .arg(check_page.checked_file_count()));
            check_page.hide();
            pack_page.hide();
            QDir(pk_root).removeRecursively();
        }

        // ★★ 2026-10-01 用户第 7 条（原话）：「每次勾选或取消勾选包列表当中的文件，右边的文件列表
        //   似乎都会重建（因为在文件列表里勾选已展开的母文件夹，不会收起，而操作包，文件列表的
        //   所有文件夹都会收起）。这明明不需要重建，只有包列表内容改变（增加，减少，替换等）
        //   才需要重建右边的文件树。」
        //   判据（能判红：把 `on_package_item_changed` 结尾改回 `refresh_rows()` 立刻红）：
        //   勾/取消勾**已加载**的包 ⇒ 条目树**不重建**（同一个 QTreeWidgetItem 对象、仍是展开的、
        //   顶层项数不变）。
        {
            FsPage keep_page;
            // ★ 2026-10-05（#2）：源名带 `.db0` ⇒ 落点壳让两个源落到**同一个顶层目录**（`keep/`）。
            //   这正是本锁要的形态：一个顶层节点、它下面有子项可展开（否则两个源各自成顶层，
            //   `topLevelItem(0)` 就不是那个能展开的目录了）。
            keep_page.add_source(QStringLiteral("keep.db0"),
                                 {efs::EntryInfo{std::string("d"), true, 0},
                                  efs::EntryInfo{std::string("d/a.txt"), false, 10}});
            keep_page.add_source(QStringLiteral("keep_b.db0"),
                                 {efs::EntryInfo{std::string("d"), true, 0},
                                  efs::EntryInfo{std::string("d/b.txt"), false, 20}});
            keep_page.resize(900, page_min_h());
            keep_page.show();
            QApplication::processEvents();
            QTreeWidgetItem* dir_item = keep_page.entry_tree()->topLevelItem(0);
            if (dir_item != nullptr) dir_item->setExpanded(true);
            QApplication::processEvents();
            const int top_before = keep_page.entry_tree()->topLevelItemCount();
            const int kids_before = dir_item != nullptr ? dir_item->childCount() : -1;
            const bool expanded_before = dir_item != nullptr && dir_item->isExpanded();
            // 勾这一行的勾选框（这一包是"已加载"的 ⇒ 只该 toggle_source，不该重建树）
            keep_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            QTreeWidgetItem* dir_after = keep_page.entry_tree()->topLevelItem(0);
            check(expanded_before && dir_after == dir_item &&
                      dir_after->isExpanded() &&
                      keep_page.entry_tree()->topLevelItemCount() == top_before &&
                      dir_after->childCount() == kids_before,
                  QStringLiteral("第7条 勾包**不重建**右树：同一对象 %1 / expanded %2 / "
                                 "顶层 %3→%4 / 子项 %5→%6")
                      .arg(dir_after == dir_item ? 1 : 0)
                      .arg(dir_after != nullptr && dir_after->isExpanded() ? 1 : 0)
                      .arg(top_before)
                      .arg(keep_page.entry_tree()->topLevelItemCount())
                      .arg(kids_before)
                      .arg(dir_after != nullptr ? dir_after->childCount() : -1));
            // 取消勾选同理
            keep_page.package_list()->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
            QApplication::processEvents();
            QTreeWidgetItem* dir_last = keep_page.entry_tree()->topLevelItem(0);
            check(dir_last == dir_item && dir_last->isExpanded(),
                  QStringLiteral("第7条 取消勾选同样不重建（同一对象 %1 / expanded %2）")
                      .arg(dir_last == dir_item ? 1 : 0)
                      .arg(dir_last != nullptr && dir_last->isExpanded() ? 1 : 0));
            keep_page.hide();
        }

        // ★ 2026-10-01 用户第 5 条：「移除这个按钮也可以优化掉，在包的条目后面增加一个叉叉按钮。」
        //   判据＝**点行尾 ✕ 真的移除那一行**：用合成的 `MouseButtonRelease` 驱动 col2 的委托
        //   `editorEvent`（判红：拿掉落点判定或回调 ⇒ 这条立刻红），并且"移除"只有
        //   `remove_source_at` 一个入口（工具行里的「移除」按钮已删）。
        {
            FsPage chai_page;
            chai_page.add_source(QStringLiteral("chai.db0"),
                                 {efs::EntryInfo{std::string("x.txt"), false, 5}});
            chai_page.resize(760, page_min_h());
            chai_page.show();
            QApplication::processEvents();
            check(chai_page.source_count() == 1 &&
                      chai_page.package_list()->itemDelegateForColumn(2) != nullptr,
                  QStringLiteral("第5条 ✕ 列有自己的委托（源 %1 个）").arg(chai_page.source_count()));
            QAbstractItemDelegate* del = chai_page.package_list()->itemDelegateForColumn(2);
            const QModelIndex idx = chai_page.package_list()->model()->index(0, 2);
            QStyleOptionViewItem opt;
            opt.rect = chai_page.package_list()->visualRect(idx);
            // ★ 2026-10-02：✕ 改成**贴行尾**（末列吃剩余宽度，居中的话它飘在那一大块中间）⇒
            //   合成点击也要落在**行尾那个小方块**里（画的与命中共用同一个 `hit_rect`）。
            const QPoint click_at(opt.rect.right() - xrkit::core::px(18), opt.rect.center().y());
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(click_at),
                                QPointF(chai_page.package_list()->viewport()->mapToGlobal(click_at)),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            const bool handled = del != nullptr &&
                                 del->editorEvent(&release, chai_page.package_list()->model(), opt,
                                                  idx);
            QApplication::processEvents();
            check(handled && chai_page.source_count() == 0 &&
                      chai_page.package_list()->topLevelItemCount() == 0,
                  QStringLiteral("第5条 点行尾 ✕ = 移除该行（handled=%1 / 源 %2 / 行 %3）")
                      .arg(handled ? 1 : 0)
                      .arg(chai_page.source_count())
                      .arg(chai_page.package_list()->topLevelItemCount()));
            chai_page.hide();
        }

        // —— ③ 解包/封包**走任务壳**（1.x `TaskRunner` 的那半边"方便"：大包不冻界面）
        //    判据是行为：点按钮 ⇒ 任务壳真的跑起来、结束事件回主线程、产物与同步路径**逐字节一致**。
        {
            const QString async_out = dir + QStringLiteral("/async");
            QDir().mkpath(async_out);
            FsPage async_page;
            async_page.resize(760, page_min_h());
            async_page.show();
            QApplication::processEvents();
            // ★ 用户 2026-09-29 更正 ③ 的口径：「**上面解包和下面封包分两个标签**就没问题了」（之前的设计可以）
        //   判据：① 整页两标签、标题对；② 包列表与包内树在**第 0 页**；③ 待打包列表在**第 1 页**；
        //   ④ **底部操作栏的两个解包按钮不许落进标签页里**（上一版塞进 QVBoxLayout ⇒ 被摊成整行大块，
        //   用户："你觉得丑么？"）。判红办法：把这几行 addWidget 的落点改回 body / 改回页内即可。
        {
            FsPage tab_page;
            tab_page.resize(760, page_min_h());
            tab_page.show();
            QApplication::processEvents();
            QTabWidget* tabs = tab_page.tabs();
            const bool two_tabs = tabs != nullptr && tabs->count() == 2 &&
                                  tabs->tabText(0) == QStringLiteral("解包") &&
                                  tabs->tabText(1) == QStringLiteral("封包");
            const bool unpack_side_ok = two_tabs &&
                                        tabs->widget(0)->isAncestorOf(tab_page.package_list()) &&
                                        tabs->widget(0)->isAncestorOf(tab_page.entry_tree());
            const bool pack_side_ok = two_tabs &&
                                      tabs->widget(1)->isAncestorOf(tab_page.pack_list()) &&
                                      tabs->widget(1)->isAncestorOf(tab_page.pack_format_combo());
            const bool buttons_outside = two_tabs &&
                                         !tabs->widget(0)->isAncestorOf(tab_page.unpack_all_button());
            // ★ 用户 2026-09-30：「上面包列表和下面文件列表之间也要有可拖拽分隔条」——
            //   判据：分隔条存在、两个列表都挂在它下面、且它是「解包」页的一部分。
            QSplitter* sp = tab_page.split_list_tree();
            const bool split_ok = sp != nullptr && sp->count() == 2 &&
                                  sp->handleWidth() > 0 &&
                                  sp->widget(0) != nullptr && sp->widget(1) != nullptr &&
                                  sp->widget(0)->isAncestorOf(tab_page.package_list()) &&
                                  sp->widget(1)->isAncestorOf(tab_page.entry_tree()) &&
                                  two_tabs && tabs->widget(0)->isAncestorOf(sp);
            check(split_ok,
                  QStringLiteral("④ 包列表↔包内树要有可拖拽分隔条（存在 %1 / 面板数 %2 / 在解包页 %3）")
                      .arg(sp != nullptr ? 1 : 0)
                      .arg(sp != nullptr ? sp->count() : -1)
                      .arg(two_tabs && sp != nullptr && tabs->widget(0)->isAncestorOf(sp) ? 1 : 0));
            check(two_tabs && unpack_side_ok && pack_side_ok && buttons_outside,
                  QStringLiteral("③ 整页两标签（标签 %1 / 解包侧在第0页 %2 / 封包侧在第1页 %3 / "
                                 "底部按钮留在操作栏 %4）")
                      .arg(tabs != nullptr ? tabs->count() : -1)
                      .arg(unpack_side_ok ? 1 : 0)
                      .arg(pack_side_ok ? 1 : 0)
                      .arg(buttons_outside ? 1 : 0));
            check(two_tabs && tabs->height() >= xrkit::core::px(300),
                  QStringLiteral("③ 标签区要占满工作区（实测 %1 px，恒 ≥300）")
                      .arg(tabs != nullptr ? tabs->height() : -1));
            tab_page.hide();
        }

        // ★★ 2026-10-06（审计处理批·0-3 之 B1-9）：修前这里只把"任务壳接上了"写成 `check(...)`，
        //   紧接着就 `unpack_all_button()->click()` / `task()->running()` —— **没判空就解引用**。
        //   真出接线回归时自检会**段错误**，门禁看到的是崩溃而不是那条红，且此后所有用例不再执行。
        //   现在把"前提"变成**控制流**：缺件就记红并跳过这一段（先判红，不许先崩）。
        const bool async_wired =
            async_page.task() != nullptr && async_page.task()->panel() != nullptr &&
            async_page.unpack_all_button() != nullptr && async_page.out_edit() != nullptr;
        check(async_wired,
              QStringLiteral("③ 解包/封包要挂在任务壳上（有面板/操作按钮/输出框才算接上；"
                             "task=%1 panel=%2 button=%3 out=%4）")
                  .arg(async_page.task() != nullptr ? 1 : 0)
                  .arg(async_page.task() != nullptr && async_page.task()->panel() != nullptr ? 1 : 0)
                  .arg(async_page.unpack_all_button() != nullptr ? 1 : 0)
                  .arg(async_page.out_edit() != nullptr ? 1 : 0));
        if (async_wired) {
            check(async_page.add_paths({db_path}) == 1, QStringLiteral("③ 异步夹具：装一个真包"));
            async_page.out_edit()->setText(async_out);
            async_page.unpack_all_button()->click();
            QElapsedTimer clock;
            clock.start();
            while (async_page.task()->running() && clock.elapsed() < 15000) {
                QCoreApplication::processEvents();   // 任务壳的排队事件要靠泵事件才会落到主线程
            }
            check(!async_page.task()->running(),
                  QStringLiteral("③ 任务壳要跑完（不是挂住；超时 %1 ms）").arg(clock.elapsed()));
            bool saw_progress = false;
            bool saw_finish_on_main = false;
            for (const TaskShell::Event& event : async_page.task()->take_events()) {
                if (event.kind == 'p') saw_progress = true;
                if (event.kind == 'f' && event.on_main_thread) saw_finish_on_main = true;
            }
            check(saw_progress && saw_finish_on_main,
                  QStringLiteral("③ 进度与「结束」都要经任务壳回主线程（p=%1 f=%2）")
                      .arg(saw_progress ? 1 : 0)
                      .arg(saw_finish_on_main ? 1 : 0));
            QFile async_file(async_out + QStringLiteral("/gamedata/a.txt"));
            const bool same_bytes =
                async_file.open(QIODevice::ReadOnly) && async_file.readAll() == QByteArray("AA");
            check(same_bytes,
                  QStringLiteral("③ 异步解包的产物与同步一致（<输出>/gamedata/a.txt == AA）"));
            async_page.hide();
        }
        }   // ★ `if (async_wired)` 收尾：把"解包走任务壳"这一段的**控制流守卫**关掉，
            //   后面 `QDir(dir).removeRecursively(); page.hide();` 仍按原样执行。
        QDir(dir).removeRecursively();
        page.hide();
    }

    // ★ 2026-09-27（用户："1.x 的部分功能或选择没了" + ⑦"拖放可能根本没实现，须查实"）：
        //   **加包入口**与**拖放**都要真的把包加载进来 —— 判据是"载进来了"，
        //   不是"代码里有个 dropEvent"（原来全 src/shell 只有 setAcceptDrops(true)，
        //   一个 dropEvent/dragEnterEvent 都没有 ⇒ 拖着没反应，这就是用户说的"选择没了"）。
        {
            FsPage drop_page;
            drop_page.show();
            QApplication::processEvents();
            check(drop_page.add_button() != nullptr &&
                      drop_page.add_button()->objectName() ==
                          QStringLiteral("fs_add_package"),
                  QStringLiteral("④ 要有「加包」按钮（1.x 的入口在 2.x 里丢了）"));

            // 用引擎自己的写侧造一个**真的 X-Ray DB 包**：既测 `package_entries`，也当拖放载荷
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_drop"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString db_path = dir + QStringLiteral("/gamedata.db0");
            int packed = 0;
            {
                std::vector<efs::DbPackItem> items;
                efs::DbPackItem a;
                a.path = "a.txt";
                a.data = {static_cast<uint8_t>('A')};
                efs::DbPackItem b;
                b.path = "dir/keep.txt";
                b.data = {static_cast<uint8_t>('B'), static_cast<uint8_t>('B')};
                items.push_back(a);
                items.push_back(b);
                const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
                packed = static_cast<int>(raw.size());
                std::ofstream out(db_path.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            check(packed > 0, QStringLiteral("④ 夹具：造一个真 DB 包（实得 %1 字节）").arg(packed));

            // ① 引擎侧：从**磁盘上的包**列条目（SquashFS 按 magic / DB 按自洽判定）
            std::string list_error;
            const std::vector<xrkit::engine::fs::EntryInfo> listed =
                efs::package_entries(db_path.toStdString(), std::nullopt, list_error);
            check(list_error.empty() && listed.size() == 2,
                  QStringLiteral("④ package_entries 从真包列出条目（err=%1，实得 %2 条）")
                      .arg(QString::fromStdString(list_error))
                      .arg(listed.size()));

            // ② 拖放**接线**：过滤器必须真装在包列表与卡片上（缺一个就红）
            check(drop_page.drop_filter() != nullptr &&
                      drop_page.drop_filter()->is_attached_to(drop_page.package_list()) &&
                      drop_page.drop_filter()->is_attached_to(drop_page.card()),
                  QStringLiteral("⑦ 拖放过滤器没装在包列表/卡片上（接线断了）"));

            // ③ 拖放**行为**：一个 Drop 事件进来 ⇒ 包真的加载进模型
            //    （离屏环境没有平台拖放：实测 `sendEvent(widget,&QDropEvent)` 会被 Qt
            //     拦掉 —— 返回 false、事件根本不到控件；真实 drop 走 QWidgetWindow 那条
            //     平台通路。所以这里直接把事件交给过滤器，见 drop_filter.h 的说明。）
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(db_path)});
            check(FileDropFilter::local_paths(&mime) == QStringList{db_path},
                  QStringLiteral("⑦ mime 里的本地文件路径要能取出来"));
            QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            check(drop_page.drop_filter() != nullptr && drop_page.drop_filter()->dispatch(&drop),
                  QStringLiteral("⑦ 过滤器要认这个 drop（本地文件 ⇒ accept）"));
            check(drop_page.source_count() == 1,
                  QStringLiteral("⑦ 拖一个包进来要真的加载（实测 %1 个源）")
                      .arg(drop_page.source_count()));
            // ★ 2026-10-05（#2）：右栏＝**解包产物预览** ⇒ 这一包（`gamedata.db0`）的条目
            //   全挂在包名壳 `gamedata/` 下：顶层就是那**一个** `gamedata/`，`dir/` 在它下面。
            //   （原判据"顶层 = dir/ 与 a.txt"是"没有壳"时代的形态；懒建口径不变。）
            //   ★ "展开前文件项"＝**0**：懒建下顶层只有那个壳目录（原来的 1 是因为
            //     `a.txt` 曾经是顶层项）。这一格**照样钉住**（壳没了 ⇒ 顶层变 2 ⇒ 红）。
            const bool seven_expanded = drop_page.expand_entry(QStringLiteral("gamedata"));
            const bool seven_deep = seven_expanded && drop_page.expand_entry(QStringLiteral("gamedata/dir"));
            check(drop_page.entry_top_level_count() == 1 && seven_expanded && seven_deep &&
                      drop_page.find_entry_item(QStringLiteral("gamedata/dir/keep.txt")) != nullptr,
                  QStringLiteral("⑦ 拖进来的包要进**树**（顶层 %1 / 展开壳 %2 / 壳下 keep.txt %3）")
                      .arg(drop_page.entry_top_level_count())
                      .arg(seven_expanded && seven_deep ? 1 : 0)
                      .arg(drop_page.find_entry_item(QStringLiteral("gamedata/dir/keep.txt")) != nullptr
                               ? 1
                               : 0));

            // ④ 认不出的文件 ⇒ **可见失败**且不加源（绝不静默加一个空包）
            const QString junk = dir + QStringLiteral("/junk.bin");
            {
                std::ofstream out(junk.toStdString(), std::ios::binary | std::ios::trunc);
                out << "this is not a package";
            }
            QMimeData bad_mime;
            bad_mime.setUrls({QUrl::fromLocalFile(junk)});
            QDropEvent bad_drop(QPointF(5, 5), Qt::CopyAction, &bad_mime, Qt::LeftButton,
                                Qt::NoModifier);
            if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&bad_drop);
            QApplication::processEvents();
            check(drop_page.source_count() == 1 && !drop_page.load_errors().isEmpty(),
                  QStringLiteral("⑦ 认不出的文件要拒绝并留下理由（源 %1 个，理由 %2 条）")
                      .arg(drop_page.source_count())
                      .arg(drop_page.load_errors().size()));

            // ⑤ 目录也能拖（散装层：与包走**同一条** add_source 路径）
            QMimeData dir_mime;
            dir_mime.setUrls({QUrl::fromLocalFile(dir)});
            QDropEvent dir_drop(QPointF(5, 5), Qt::CopyAction, &dir_mime, Qt::LeftButton,
                                Qt::NoModifier);
            if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&dir_drop);
            QApplication::processEvents();
            // ★★ 2026-10-03 口径**有意改写**（用户：「这里母文件夹的"已加载"指的是**文件夹**已加载
            //   而不是**包的内容**已加载。怎么能这样呢？**右边永远不显示包体，除非包体本身在左边的
            //   包内，而不是文件夹内。**」）
            //   ⇒ **文件夹不再当"源"**：拖目录只剩"把里面的包登记进列表"。
            //   ⚠ 旧期望 2 = 1(先拖进来的那个包) + **1(散装层：目录自己也成了一个源)** —— 那正是被
            //     作废的 2026-09-29 口径；本夹具的目录里那个包**先前已经拖进来过**，所以现在
            //     `scan_dir` 去重后**源数不涨**。
            {
                QStringList seen;
                int dup = 0;
                for (int i = 0; i < drop_page.source_count(); ++i) {
                    const QString one = drop_page.source_at(i);
                    if (seen.contains(one, Qt::CaseInsensitive)) ++dup;
                    seen << one;
                }
                check(drop_page.source_count() == 1 && dup == 0,
                      QStringLiteral("⑦ 拖目录：**目录自己不成源**、里面的包已被登记过（实测 %1 个源 / "
                                     "重复 %2，期望 1/0 —— 旧期望 2 里那个散装层源已作废）")
                          .arg(drop_page.source_count())
                          .arg(dup));
            }
            // ★★ 2026-10-03：同一个目录拖两次 —— **源数一个都不涨**（目录不再是源，里面的包也被
            //   `scan_dir` 去重），并且要留下一行可见理由（本夹具目录里没有"新的"包）。
            {
                const int before = drop_page.source_count();
                QMimeData again_mime;
                again_mime.setUrls({QUrl::fromLocalFile(dir)});
                QDropEvent again_drop(QPointF(5, 5), Qt::CopyAction, &again_mime, Qt::LeftButton,
                                      Qt::NoModifier);
                if (drop_page.drop_filter() != nullptr) {
                    drop_page.drop_filter()->dispatch(&again_drop);
                }
                QApplication::processEvents();
                check(drop_page.source_count() == before && !drop_page.load_errors().isEmpty(),
                      QStringLiteral("⑦ 同一个目录拖两次不重复加源（源 %1→%2，可见理由 %3 条）")
                          .arg(before)
                          .arg(drop_page.source_count())
                          .arg(drop_page.load_errors().size()));
            }
            // ★ 正向断言（别让上面那条只证明"去重"）：**另建一个目录**、里面放一个**新**包 ⇒ 拖它 ⇒
            //   **源数 +1**（只有那个包；目录自己不算源）、**已加载 +0**（"拖进来的东西才加载"这条
            //   2026-10-03 起对目录不再适用：目录不加载任何东西）。
            {
                const QString dir2 = QDir::temp().filePath(QStringLiteral("xrkit_smoke_drop2"));
                QDir(dir2).removeRecursively();
                QDir().mkpath(dir2);
                std::vector<efs::DbPackItem> items2;
                efs::DbPackItem one2;
                one2.path = "z.txt";
                one2.data = {static_cast<uint8_t>('Z')};
                items2.push_back(one2);
                const std::vector<uint8_t> raw2 = efs::pack_db(items2, *efs::format_key_to_db("xdb"));
                std::ofstream out2((dir2 + QStringLiteral("/extra.db0")).toStdString(),
                                   std::ios::binary | std::ios::trunc);
                out2.write(reinterpret_cast<const char*>(raw2.data()),
                           static_cast<std::streamsize>(raw2.size()));
                out2.close();
                const int before = drop_page.source_count();
                const int loaded_before = drop_page.loaded_source_count();
                QMimeData mime2;
                mime2.setUrls({QUrl::fromLocalFile(dir2)});
                QDropEvent drop2(QPointF(5, 5), Qt::CopyAction, &mime2, Qt::LeftButton,
                                 Qt::NoModifier);
                if (drop_page.drop_filter() != nullptr) drop_page.drop_filter()->dispatch(&drop2);
                QApplication::processEvents();
                // ★★ 2026-10-03 口径**有意改写**（用户原话「**只要拖入包或者文件夹就加载上去**」；
                //   并实测「`IWP\database` 加载时长零点几秒，完全不影响」）⇒
                //   旧期望「+1 源 / **+0** 已加载」来自 2026-10-02 的"扫出来的只入列表不加载"，**已作废**。
                check(drop_page.source_count() == before + 1 &&
                          drop_page.loaded_source_count() == loaded_before + 1,
                      QStringLiteral("⑦ 目录里的包进列表并**当场加载**（拖入即加载）"
                                     "（源 %1→%2 期望 +1 / 已加载 %3→%4 期望 +1）")
                          .arg(before)
                          .arg(drop_page.source_count())
                          .arg(loaded_before)
                          .arg(drop_page.loaded_source_count()));
                QDir(dir2).removeRecursively();
            }
            QDir(dir).removeRecursively();
            drop_page.hide();
        }

        // 6.1/6.2：两个包叠加，后者为准；行文本要能看出"这一份来自哪个包"
        // ★★ 2026-10-05（#2）：右栏＝**解包产物预览** ⇒ 要量"**同一路径**上两个包合并"，两个包
        //   就必须**落到同一个壳**里。落点规则判"自带壳"的判据是**条目路径本身已经带壳**
        //   （`paths_carry_shell`）⇒ 源名取 `gamedata.sq_base` / `gamedata.sqzy_patch`
        //   （`package_folder_name` 去掉**最后一个**扩展名 ⇒ 两者的壳都是 `gamedata`），
        //   条目也照壳写 `gamedata/…` ⇒ 预预览不叠第二层，两个包（以及先前那个 `gamedata.db0`）
        //   在**同一批路径**上合并 —— 这正是 6.1 的本意，也顺带走了"自带壳"那条反向口径。
        //   ★ 修前那版写的是 `sq_base.sq_base` ＋ **不带壳的条目**：壳算出来是 `sq_base`、
        //     条目又不带壳 ⇒ 落点规则照样叠一层，两个包各自成一个壳目录、**根本合不到一起**
        //     （自检实测：来源列空、大小列空 —— 判据红得有道理）。
        const int sources_before_merge = page.source_count();
        page.add_source(QStringLiteral("gamedata.sq_base"),
                        {{"gamedata/a.txt", false},
                         {"gamedata/dir/keep.txt", false},
                         {"gamedata/dir/base_only.txt", false}});
        page.add_source(QStringLiteral("gamedata.sqzy_patch"),
                        {{"gamedata/a.txt", false}, {"gamedata/dir/keep.txt", false}});
        check(page.source_count() == sources_before_merge + 2,
              QStringLiteral("6.2 两个包都进了列表（%1 → %2，期望 +2）")
                  .arg(sources_before_merge)
                  .arg(page.source_count()));
        // 合并后：顶层 = 包名壳 `gamedata/` **一个**；`dir/` 与 `a.txt` 在它下面（懒建 ⇒ 先展开）
        check(page.entry_top_level_count() == 1,
              QStringLiteral("6.1 合并后顶层条目数 = 1（＝包名壳 `gamedata/`；实测 %1）")
                  .arg(page.entry_top_level_count()));
        page.expand_entry(QStringLiteral("gamedata"));
        QApplication::processEvents();
        QTreeWidgetItem* a_item = page.find_entry_item(QStringLiteral("gamedata/a.txt"));
        check(a_item != nullptr && a_item->text(2).contains(QStringLiteral("sqzy_patch")),
              QStringLiteral("6.1 同名条目以后加载的包为准（来源列 = 「%1」，期望含 sqzy_patch）")
                  .arg(a_item != nullptr ? a_item->text(2) : QString()));
        check(a_item != nullptr && !a_item->text(1).isEmpty(),
              QStringLiteral("6.1 文件行要有**大小**列（实测「%1」）")
                  .arg(a_item != nullptr ? a_item->text(1) : QString()));
        // ★ 2026-09-28（用户贴 1.x 对照截图："差距。"）：树要能**展开/折叠**、每行能**单独勾**、
        //   目录勾选是**三态**（模型说了算）。判据都落在真树上，不是"看代码里有没有 QTreeWidget"。
        {
            FsPage tree_page;
            tree_page.resize(760, page_min_h());
            tree_page.show();
            QApplication::processEvents();
            tree_page.add_source(QStringLiteral("d.db0"),
                                 {{"top.txt", false}, {"d/one.txt", false}, {"d/two.txt", false}});
            QApplication::processEvents();
            // ★ 首列宽度（用户截图："text..." —— 名称列被挤成几十像素，整棵树没法读）：
            //   ★ 2026-09-29 **口径更新**（用户第 4 条："最好弄一个水平方向的可拖拽分隔条"）：
            //   首列从 Stretch 改成 **Interactive**（三列都能拖）⇒ 判据按性质重述为
            //   "首列可拖（Interactive）且默认宽度 ≥160px"；原意（名称列不许被挤成几十像素）不变。
            //   判红办法：把列宽改小到 160 以下，或把模式换回 ResizeToContents。
            {
                QHeaderView* head = tree_page.entry_tree()->header();
                check(head != nullptr &&
                          head->sectionResizeMode(0) == QHeaderView::Interactive &&
                          tree_page.entry_tree()->columnWidth(0) >= xrkit::core::px(160),
                      QStringLiteral("树① 首列**可拖拽**且有宽度（模式 %1=Interactive / 实测 %2 px，恒 ≥160）")
                          .arg(head != nullptr ? static_cast<int>(head->sectionResizeMode(0)) : -1)
                          .arg(tree_page.entry_tree()->columnWidth(0)));
            }
            // ★ 用户 2026-09-29 第 6 条：拖**目录**要展开里面的包（不只显示 db 文件）
            {
                FsPage dir_page;
                dir_page.resize(760, page_min_h());
                dir_page.show();
                QApplication::processEvents();
                const QString mix = QDir::temp().filePath(QStringLiteral("xrkit_smoke_dirpkg"));
                QDir(mix).removeRecursively();
                QDir().mkpath(mix + QStringLiteral("/gamedata/loose"));
                {
                    std::ofstream(mix.toStdString() + "/gamedata/loose/readme.txt",
                                  std::ios::binary | std::ios::trunc)
                        << "L";
                }
                // 里面放一个**真 XDB 包**（用引擎自己打的），验证"包内容也摊进树里"
                {
                    std::vector<efs::DbPackItem> items;
                    // ★ 2026-10-06（④）：格式下拉的**默认值现在是 auto**（1.x `fs_app.py:154`
                    //   `tk.StringVar(value="auto")`）⇒ 判定不再被"默认 xdb"兜着，而是走
                    //   引擎的**自洽判定**（歧义时显式拒绝，这是既有正确行为）。
                    //   夹具必须像**工具自己产出的包**那样带上目录条目：只有一条文件条目的
                    //   60 字节包在六种 DB 格式里**本来就分不开**（那是夹具不真实，不是回归）。
                    efs::DbPackItem dir_item;
                    dir_item.path = "gamedata";
                    dir_item.is_dir = true;
                    items.push_back(dir_item);
                    efs::DbPackItem one;
                    one.path = "gamedata/from_package.txt";   // CoP：包里的路径本身就带 gamedata 前缀
                    one.is_dir = false;
                    one.data = {'P'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(mix.toStdString() + "/gamedata/configs.db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                const int n = dir_page.add_paths({mix});
                QApplication::processEvents();
                // ★★ 2026-10-03 口径**两处有意改写**：
                //   ① 用户「**只要拖入包或者文件夹就加载上去**」⇒ 旧期望"0 加载、只登记"（2026-10-02 的
                //      "扫出来的只入列表"）**已作废**：拖目录 ⇒ 里面的包**当场全部加载**。
                //   ② 用户「**右边永远不显示包体，除非包体本身在左边的包内，而不是文件夹内**」＋
                //      「**右边罗列的是包的内容，换句话说就是解包产物的预览**」⇒ **散装层取消**：
                //      那个松散 `readme.txt` **既不在左树**（不是源）、**也不在右栏**（不进产物预览）。
                check(n == 1 && dir_page.loaded_source_count() == 1 && dir_page.source_count() == 1,
                      QStringLiteral("树③ 拖目录 ⇒ 里面的包**当场加载**（拖入即加载）"
                                     "（返回 %1 / 已加载 %2 / 源 %3，期望 1/1/1；松散文件不算源）")
                          .arg(n)
                          .arg(dir_page.loaded_source_count())
                          .arg(dir_page.source_count()));
                // 懒建：先展开壳那一层（以及壳下的 `gamedata`、`gamedata/loose`）才找得到深层条目
                // ★ 2026-10-05（#2）：这一包的**文件名叫 `configs.db0`** ⇒ 壳是 `configs`
                //   （`package_folder_name` 取文件名去最后扩展名），虽然**包内**条目带 `gamedata/`
                //   前缀 —— 落点规则比的是"**包名**壳"，包名对不上就照常规叠一层
                //   （所以真实落点是 `configs/gamedata/from_package.txt`，预览必须与它一字不差）。
                //   ★ 要改成"不叠第二层"，得让**包名**也叫 `gamedata.db0` —— 那是另一条夹具
                //     （#2 预览② 的"自带壳"反向锁）。这里保持原文件名，只把**期望的落点路径**
                //     改对：这正是"预览 = 解包产物"这条口径本身。
                dir_page.expand_entry(QStringLiteral("configs"));
                dir_page.expand_entry(QStringLiteral("configs/gamedata"));
                dir_page.expand_entry(QStringLiteral("configs/gamedata/loose"));
                QApplication::processEvents();
                // 右栏＝解包产物预览：**包内文件在、松散文件不在**（散装层已取消）
                check(dir_page.entry_top_level_count() > 0 &&
                          dir_page.find_entry_item(QStringLiteral("configs/gamedata/loose/readme.txt")) ==
                              nullptr &&
                          dir_page.find_entry_item(QStringLiteral("configs/gamedata/from_package.txt")) !=
                              nullptr,
                      QStringLiteral("树③ 右栏＝解包产物预览：**包内文件在、松散文件不在**"
                                     "（顶层 %1 / 松散文件 %2 / 包内文件 %3，期望 >0/0/1）")
                          .arg(dir_page.entry_top_level_count())
                          .arg(dir_page.find_entry_item(
                                   QStringLiteral("configs/gamedata/loose/readme.txt")) != nullptr
                                   ? 1
                                   : 0)
                          .arg(dir_page.find_entry_item(
                                   QStringLiteral("configs/gamedata/from_package.txt")) != nullptr
                                   ? 1
                                   : 0));
                // ★ 新增（"散装层取消"的可判红形式）：**松散文件不许进左树**（左树只有"文件夹 + 7 种包"）
                check(find_package_source_node(dir_page.package_list(), QStringLiteral("readme.txt")) == nullptr &&
                          find_package_source_node(dir_page.package_list(),
                                                   QStringLiteral("configs.db0")) != nullptr,
                      QStringLiteral("树③ 左树只有包（松散 readme.txt 不是源）"));
                // ★ 正向（钉住"拖入即加载"的边界）：**没有可加载的了** ⇒ `load_selected()` 返回 0
                //   （既不重复加载、也不会把松散文件当源）。
                {
                    QTreeWidgetItem* pkg_node = find_package_source_node(
                        dir_page.package_list(), QStringLiteral("configs.db0"));
                    if (pkg_node == nullptr) {
                        check(false, QStringLiteral("树③ 包列表里要能找到那个 `configs.db0` 源节点"));
                    } else {
                        pkg_node->setSelected(true);
                        const int loaded_now = dir_page.load_selected();
                        QApplication::processEvents();
                        // ★ 这一包的文件在**第三层**（`configs/gamedata/…`）⇒ 懒建要逐级展开
                        dir_page.expand_entry(QStringLiteral("configs"));
                        dir_page.expand_entry(QStringLiteral("configs/gamedata"));
                        QApplication::processEvents();
                        check(loaded_now == 0 && dir_page.loaded_source_count() == 1 &&
                                  dir_page.find_entry_item(
                                      QStringLiteral("configs/gamedata/from_package.txt")) != nullptr,
                              QStringLiteral("树③ 已加载的包不会重复加载（本次加载 %1 / 已加载 %2 / 包内文件 %3）")
                                  .arg(loaded_now)
                                  .arg(dir_page.loaded_source_count())
                                  .arg(dir_page.find_entry_item(
                                           QStringLiteral("configs/gamedata/from_package.txt")) != nullptr
                                           ? 1
                                           : 0));
                    }
                }
                dir_page.hide();
                QDir(mix).removeRecursively();
            }
            // ★ 用户 2026-09-30：包内排序＝**文件夹在前，各自按名称**（原来目录/文件混排）
            {
                FsPage sort_page;
                sort_page.resize(760, page_min_h());
                sort_page.show();
                QApplication::processEvents();
                sort_page.add_source(QStringLiteral("s.db0"),
                                     {{"z.txt", false}, {"b/", true}, {"a.txt", false}, {"c/", true}});
                QApplication::processEvents();
                // ★ 2026-10-05（#2）：右栏＝**解包产物预览** ⇒ 这一包的条目现在挂在包名壳
                //   `s/` 下面（顶层就是那一个 `s/`）⇒ 排序判据看**壳下面那一层**的兄弟顺序
                //   （原来直接看顶层，是"没有壳"时代的形态）。**排序口径本身没变。**
                //   ★ 子项是**懒建**的：不先展开壳，`childCount()` 恒为 0（实测"壳下实际"是空串
                //     —— 那是假红，不是排序错）。
                const bool sort_expanded = sort_page.expand_entry(QStringLiteral("s"));
                QApplication::processEvents();
                QTreeWidgetItem* sort_root = sort_page.entry_tree()->topLevelItem(0);
                QStringList top;
                if (sort_root != nullptr) {
                    for (int i = 0; i < sort_root->childCount(); ++i) {
                        top << sort_root->child(i)->text(0);
                    }
                }
                check(sort_expanded && sort_root != nullptr &&
                          sort_root->text(0) == QStringLiteral("s/") &&
                          top.size() == 4 && top[0] == QStringLiteral("b/") &&
                          top[1] == QStringLiteral("c/") && top[2] == QStringLiteral("a.txt") &&
                          top[3] == QStringLiteral("z.txt"),
                      QStringLiteral("排序：文件夹在前、文件在后，各自按名（壳下实际 %1）")
                          .arg(top.join(QStringLiteral("|"))));
                sort_page.hide();
            }
            // ★ 用户 2026-09-30：「E:\...\IWP\database 整个文件夹拖进去会卡死」——实测该目录是
            //   **57 个包 / 18.9 GB**。
            //   ★★ 2026-10-03 口径改写：旧期望「已加载 1」来自"目录＝散装层源（拖进来的那层加载）"；
            //   现在**文件夹不贡献任何条目** ⇒ 拖目录**一个都不加载**，9 个包全进列表。
            {
                const QString many = QDir::temp().filePath(QStringLiteral("xrkit_smoke_manypkg"));
                QDir(many).removeRecursively();
                QDir().mkpath(many);
                for (int i = 0; i < 9; ++i) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "f" + std::to_string(i) + ".txt";
                    one.is_dir = false;
                    one.data = {'x'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(many.toStdString() + "/p" + std::to_string(i) + ".db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage many_page;
                many_page.resize(760, page_min_h());
                many_page.show();
                QApplication::processEvents();
                many_page.add_paths({many});
                QApplication::processEvents();
                // ★★ 2026-10-03 口径**改写**：这条旧锁原本钉的是"**包多也不加载**"（9 个包 ⇒ 0 加载），
                //   依据是 2026-10-02 的"扫出来的只入列表"＋担心一次解 18.9 GB。
                //   用户已改口径并给了实测：**「只要拖入包或者文件夹就加载上去」**、
                //   **「实测 `IWP\database` 加载时长零点几秒，完全不影响」**
                //   ⇒ 现在断的是**反面**：9 个包**全部加载**（且不许再出现任何数量阈值/分档）。
                check(many_page.loaded_source_count() == 9 && many_page.source_count() == 9,
                      QStringLiteral("包多也**全部加载**（无阈值、无分档）：目录里的 9 个包"
                                     "（已加载 %1 期望 9、已知源 %2 期望 9）")
                          .arg(many_page.loaded_source_count())
                          .arg(many_page.source_count()));
                many_page.hide();
                QDir(many).removeRecursively();
            }

            // ★★ 2026-10-02 **阈值已删** + 2026-10-03 口径**改写**（用户：「**只要拖入包或者文件夹
            //   就加载上去**」，并实测"零点几秒，完全不影响"）：
            //   旧 `kAutoLoadMax = 8`：≤8 个包就全自动加载（1 散装层 + 3 包 = 4）；
            //   上一版：只加载"拖进来的那一层"（散装层）= 1；
            //   **现在：目录里的 3 个包全部加载 = 3**（"目录不成源"这半仍然成立 ⇒ 源数＝包数）。
            {
                const QString few = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fewpkg"));
                QDir(few).removeRecursively();
                QDir().mkpath(few);
                for (int i = 0; i < 3; ++i) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "g" + std::to_string(i) + ".txt";
                    one.is_dir = false;
                    one.data = {'y'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(few.toStdString() + "/q" + std::to_string(i) + ".db0",
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage few_page;
                few_page.resize(760, page_min_h());
                few_page.show();
                QApplication::processEvents();
                few_page.add_paths({few});
                QApplication::processEvents();
                check(few_page.loaded_source_count() == 3 && few_page.source_count() == 3,
                      QStringLiteral("阈值已删且目录不成源：3 个包的目录 ⇒ **3 个全加载 / 3 个已知源**"
                                     "（已加载 %1 期望 3 / 源 %2 期望 3）")
                          .arg(few_page.loaded_source_count())
                          .arg(few_page.source_count()));
                few_page.hide();
                QDir(few).removeRecursively();
            }

            // ★★ 2026-10-03 新增锁（用户：「**包的内容没有全部勾选的时候，包的勾选框也应当处于
            //   部分勾选的状态。**」）—— 判据：包内全勾 ⇒ 包节点 ✓；**勾掉其中一个条目 ⇒ 包节点 ■**。
            //   （把 `on_entry_item_changed` 里那句 `refresh_package_checks()` 去掉立刻红：卡在 ✓。）
            {
                FsPage part_page;
                part_page.add_source(QStringLiteral("part_a.db0"),
                                     {efs::EntryInfo{std::string("x.txt"), false, 5},
                                      efs::EntryInfo{std::string("y.txt"), false, 7}});
                part_page.resize(760, page_min_h());
                part_page.show();
                QApplication::processEvents();
                if (part_page.check_all_button() != nullptr) part_page.check_all_button()->click();
                QApplication::processEvents();
                QTreeWidgetItem* pkg_node = find_package_source_node(part_page.package_list(),
                                                                     QStringLiteral("part_a"));
                const Qt::CheckState before =
                    pkg_node != nullptr ? pkg_node->checkState(0) : Qt::Unchecked;
                // ★ 2026-10-05（#2）：条目在包名壳下 ⇒ 路径是 `part_a/x.txt`，而且右栏是**懒建**的
                //   （"包内全选"不会顺手把子项建出来）⇒ 先展开壳再找，否则拿到 nullptr、
                //   勾不动任何东西（实测"勾掉一个条目后仍 ✓" —— 那是**假红**）。
                const bool part_expanded = part_page.expand_entry(QStringLiteral("part_a"));
                QApplication::processEvents();
                QTreeWidgetItem* one_entry = part_page.find_entry_item(QStringLiteral("part_a/x.txt"));
                if (one_entry != nullptr) one_entry->setCheckState(0, Qt::Unchecked);
                QApplication::processEvents();
                check(part_expanded && pkg_node != nullptr && before == Qt::Checked &&
                          pkg_node->checkState(0) == Qt::PartiallyChecked,
                      QStringLiteral("包的三态：全勾时 ✓（实测 %1）、展开壳 %2、勾掉一个条目后 ■（实测 %3）")
                          .arg(static_cast<int>(before))
                          .arg(part_expanded ? 1 : 0)
                          .arg(pkg_node != nullptr ? static_cast<int>(pkg_node->checkState(0)) : -1));
                part_page.hide();
            }

            // ★★ 2026-10-03 新增锁（用户：「**右边永远不显示包体，除非包体本身在左边的包内，
            //   而不是文件夹内。**」＋「右边的包树和左边是一模一样的…很明显不应该这样」）——
            //   夹具：目录下有**两个子目录**各放一个包 ⇒ 左树**根应当是分组节点**（不是源节点）：
            //   `src<0`、无大小、目录里的包各归各位；**0 加载**（文件夹不贡献任何条目）。
            //   （旧的"目录＝散装层源"实现下，根会是一个**源**节点 ⇒ 这条红。）
            {
                const QString root2 = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pkgroot"));
                QDir(root2).removeRecursively();
                QDir().mkpath(root2 + QStringLiteral("/s1"));
                QDir().mkpath(root2 + QStringLiteral("/s2"));
                for (const QString& rel : {QStringLiteral("/s1/p1.db0"),
                                           QStringLiteral("/s2/p2.db0")}) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "t.txt";
                    one.data = {'t'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out((root2 + rel).toStdString(),
                                      std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage root_page;
                root_page.resize(760, page_min_h());
                root_page.show();
                QApplication::processEvents();
                root_page.add_paths({root2});
                QApplication::processEvents();
                QTreeWidget* list = root_page.package_list();
                QTreeWidgetItem* root_node = list != nullptr ? list->topLevelItem(0) : nullptr;
                const bool root_is_group =
                    root_node != nullptr && root_node->data(0, Qt::UserRole).toInt() < 0 &&
                    root_node->text(1).isEmpty();
                // ★★ 2026-10-03 口径改写：旧期望"已加载 0"来自 2026-10-02 的"扫出来的只入列表"；
                //   现在用户定了「拖入包或者文件夹就加载上去」⇒ 两个包都加载 ⇒ **已加载 2**。
                //   （"顶层恰好 1 个分组节点、根不是源"这一半照旧 ✓）
                check(root_page.source_count() == 2 && root_page.loaded_source_count() == 2 &&
                          list != nullptr && list->topLevelItemCount() == 1 && root_is_group &&
                          root_node->childCount() == 2,
                      QStringLiteral("包树根＝分组节点：源 %1（期望 2）/ 已加载 %2（期望 2）/ 顶层 %3"
                                     "（期望 1）/ 根是分组 %4 / 根的子项 %5（期望 2）")
                          .arg(root_page.source_count())
                          .arg(root_page.loaded_source_count())
                          .arg(list != nullptr ? list->topLevelItemCount() : -1)
                          .arg(root_is_group ? 1 : 0)
                          .arg(root_node != nullptr ? root_node->childCount() : -1));
                root_page.hide();
                QDir(root2).removeRecursively();
            }

            // ★★ 2026-10-02 包列表**树化**（用户：「左边的包列表的结构应该也是一个文件树」）——
            //   一条夹具同时判三件事：**分组节点存在**、**分组节点不可勾选/无 ✕/无大小**、
            //   **子目录里的包挂在分组节点下面**。夹具：`tmp/a.db0` + `tmp/sub/b.db0`。
            {
                const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pkgtree"));
                QDir(root).removeRecursively();
                QDir().mkpath(root + QStringLiteral("/sub"));
                const QStringList rels{QStringLiteral("/a.db0"), QStringLiteral("/sub/b.db0")};
                for (const QString& rel : rels) {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "t.txt";
                    one.is_dir = false;
                    one.data = {'t'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out((root + rel).toStdString(), std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage tree_list_page;
                tree_list_page.resize(760, page_min_h());
                tree_list_page.show();
                QApplication::processEvents();
                tree_list_page.add_paths({root});
                QApplication::processEvents();
                QTreeWidget* list = tree_list_page.package_list();
                // 顶层第一项 = 散装层（目录名带 '/'）；它的子项里应当有 `sub/`（纯分组节点）与 `a.db0`
                QTreeWidgetItem* root_item = list != nullptr ? list->topLevelItem(0) : nullptr;
                QTreeWidgetItem* sub_item = nullptr;
                QTreeWidgetItem* pkg_a_item = nullptr;
                QTreeWidgetItem* pkg_b_item = nullptr;
                if (root_item != nullptr) {
                    for (int i = 0; i < root_item->childCount(); ++i) {
                        QTreeWidgetItem* child = root_item->child(i);
                        if (child->text(0) == QStringLiteral("sub/")) sub_item = child;
                        if (child->text(0).startsWith(QStringLiteral("a.db0"))) pkg_a_item = child;
                    }
                }
                if (sub_item != nullptr) {
                    for (int i = 0; i < sub_item->childCount(); ++i) {
                        if (sub_item->child(i)->text(0).startsWith(QStringLiteral("b.db0"))) {
                            pkg_b_item = sub_item->child(i);
                        }
                    }
                }
                check(root_item != nullptr && pkg_a_item != nullptr && sub_item != nullptr &&
                          pkg_b_item != nullptr,
                      QStringLiteral("包列表树① 分组节点 + 子目录里的包挂在它下面"
                                     "（根 %1 / a.db0 %2 / sub %3 / b.db0 %4）")
                          .arg(root_item != nullptr ? 1 : 0)
                          .arg(pkg_a_item != nullptr ? 1 : 0)
                          .arg(sub_item != nullptr ? 1 : 0)
                          .arg(pkg_b_item != nullptr ? 1 : 0));
                const bool group_checkable =
                    sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable);
                // ★★ 2026-10-02（用户「包树直接引用树的统一函数就行啊」之后）：**默认收起**的树
                //   还必须**点得开** —— 发一条 `itemClicked`（等价于用户点条目身体）⇒ 该节点应被展开。
                //   离屏时 `click_on_indicator` 判为"不在那一小块上"⇒ 走"点身体"这条路（见它的注释）。
                //   ★ 这条同时守住那个坑：包列表在平表时代留过 `setRootIsDecorated(false)`，
                //     树化 + 默认收起之后顶层节点的孩子就**再也点不出来**了（那条断言在上面第 6 条里）。
                {
                    const bool was_expanded = root_item != nullptr && root_item->isExpanded();
                    if (root_item != nullptr) {
                        QMetaObject::invokeMethod(list, "itemClicked", Q_ARG(QTreeWidgetItem*, root_item),
                                                  Q_ARG(int, 0));
                        QApplication::processEvents();
                    }
                    check(root_item != nullptr && !was_expanded && root_item->isExpanded(),
                          QStringLiteral("包列表树④ 单击节点身体＝展开（收起 %1 → 展开 %2）")
                              .arg(was_expanded ? 1 : 0)
                              .arg(root_item != nullptr && root_item->isExpanded() ? 1 : 0));
                }
                // ★★ 2026-10-02 口径**有意改写**（用户：「包树和文件树逻辑的唯一差别，就是包树多一个
                //   是否加载。**其他行为应当完全一样**。」）⇒ 分组节点**改成可勾选**（带三态，
                //   勾它＝连带勾/去勾它下面的所有源），不再要求"不可勾选"。仍然无大小、无 ✕（下标 <0）。
                check(sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable) &&
                          sub_item->text(1).isEmpty() &&
                          sub_item->data(0, Qt::UserRole).toInt() < 0,
                      QStringLiteral("包列表树③ 分组节点：可勾选（三态）%1、大小「%2」、源下标 %3（应 <0，"
                                     "委托就是按它不画 ✕）")
                          .arg(sub_item != nullptr && sub_item->flags().testFlag(Qt::ItemIsUserCheckable) ? 1 : 0)
                          .arg(sub_item != nullptr ? sub_item->text(1) : QString())
                          .arg(sub_item != nullptr ? sub_item->data(0, Qt::UserRole).toInt() : -999));
                // ★★ 2026-10-03 口径**改写**：旧判据「连带勾 + **不代加载**」（2026-10-02 为防
                //   "一次点击解 57 个包"而定）**已作废** —— 用户：「勾文件夹要不要顺带加载
                //   **肯定要加载**。」⇒ 这里保留"连带勾"这条（本夹具里的包在**拖入时就已经加载**，
                //   所以已加载数不变是正常的）；"顺带加载"由下面那条独立锁（⑥，用 `scan_dir()`
                //   造出真正的"未加载源"）判。
                {
                    const int before_loaded = tree_list_page.loaded_source_count();
                    if (sub_item != nullptr) {
                        sub_item->setCheckState(0, Qt::Checked);
                        QApplication::processEvents();
                    }
                    bool all_children_checked = sub_item != nullptr && sub_item->childCount() > 0;
                    for (int i = 0; sub_item != nullptr && i < sub_item->childCount(); ++i) {
                        if (sub_item->child(i)->checkState(0) == Qt::Unchecked) all_children_checked = false;
                    }
                    check(all_children_checked &&
                              tree_list_page.loaded_source_count() == before_loaded,
                          QStringLiteral("包列表树⑤ 勾分组＝连带勾它下面所有源 %1（本夹具已全部加载，"
                                         "已加载 %2→%3 不变）")
                              .arg(all_children_checked ? 1 : 0)
                              .arg(before_loaded)
                              .arg(tree_list_page.loaded_source_count()));
                }
                // ★★ 2026-10-03 **新增锁**（用户：「勾文件夹要不要顺带加载**肯定要加载**。」）：
                //   夹具必须用 `scan_dir()` —— 它是"拖入即加载"之后**唯一还能造出"未加载源"的入口**
                //   （`add_paths` 现在一律当场加载）。勾那个分组节点 ⇒ **两个包都变成已加载**、
                //   右栏随即出现它们的条目。**把 `toggle_group_at` 里的加载那步去掉立刻红**（停在 0）。
                {
                    const QString gdir =
                        QDir::temp().filePath(QStringLiteral("xrkit_smoke_groupcheck"));
                    QDir(gdir).removeRecursively();
                    QDir().mkpath(gdir);
                    // ⚠ 两个包必须放在**不同子目录**里：都在同一层时左树顶层是**源节点**（没有分组），
                    //   拿它当"分组"去勾就只是在勾一个包（第一版就这么写错了，当场红：已加载 0→1）。
                    for (int i = 0; i < 2; ++i) {
                        QDir().mkpath(gdir + QStringLiteral("/sub%1").arg(i));
                        std::vector<efs::DbPackItem> items;
                        efs::DbPackItem one;
                        one.path = "pack" + std::to_string(i) + ".txt";
                        one.is_dir = false;
                        one.data = {'G'};
                        items.push_back(one);
                        const std::vector<uint8_t> raw =
                            efs::pack_db(items, *efs::format_key_to_db("xdb"));
                        std::ofstream out(
                            (gdir + QStringLiteral("/sub%1/g%1.db0").arg(i)).toStdString(),
                            std::ios::binary | std::ios::trunc);
                        out.write(reinterpret_cast<const char*>(raw.data()),
                                  static_cast<std::streamsize>(raw.size()));
                    }
                    FsPage gc_page;
                    gc_page.resize(760, page_min_h());
                    gc_page.show();
                    QApplication::processEvents();
                    gc_page.scan_dir(gdir);   // ★ 只登记、不加载（造出"未加载源"）
                    QApplication::processEvents();
                    QTreeWidget* gcl = gc_page.package_list();
                    QTreeWidgetItem* ggroup = gcl != nullptr ? gcl->topLevelItem(0) : nullptr;
                    const bool is_group =
                        ggroup != nullptr && ggroup->data(0, Qt::UserRole).toInt() < 0;
                    const int gbefore = gc_page.loaded_source_count();
                    if (ggroup != nullptr) {
                        ggroup->setCheckState(0, Qt::Checked);
                        QApplication::processEvents();
                    }
                    check(is_group && gbefore == 0 && gc_page.loaded_source_count() == 2 &&
                              gc_page.entry_top_level_count() >= 2,
                          QStringLiteral("包列表树⑥ 勾分组 ⇒ **顺带真的加载**（是分组 %1 / 已加载 %2→%3 / "
                                         "右栏顶层 %4，期望 1 / 0→2 / ≥2）")
                              .arg(is_group ? 1 : 0)
                              .arg(gbefore)
                              .arg(gc_page.loaded_source_count())
                              .arg(gc_page.entry_top_level_count()));
                    gc_page.hide();
                    QDir(gdir).removeRecursively();
                }
                check(package_source_node_count(list) == tree_list_page.source_count(),
                      QStringLiteral("包列表树 源节点数 == 源数（节点 %1 / 源 %2）")
                          .arg(package_source_node_count(list))
                          .arg(tree_list_page.source_count()));
                tree_list_page.hide();
                QDir(root).removeRecursively();
            }

            // ★★ 2026-10-02 用户报「包树崩了」。真因：**在 `itemChanged` 槽里重建了包树**
        //   （`load_selected()` → `rebuild_package_list()` → `clear()` 把**正在处理的那个 item 删掉**，
        //   槽返回后 Qt 继续用它 ⇒ use-after-free ⇒ 崩）。同族的第二处＝✕ 委托在 `editorEvent` 里直接重建。
        //   判据（可判红）：勾一个**未加载**的包之后，**同一个 `QTreeWidgetItem` 对象还在**（说明没重建）。
        //   把 `load_one_source` 换回 `load_selected() + rebuild_package_list()` ⇒ 指针变了 ⇒ 红。
        {
            const QString cdir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_crash"));
            QDir(cdir).removeRecursively();
            QDir().mkpath(cdir);
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem one;
            one.path = "p/x.txt";
            one.data = {'t'};
            items.push_back(one);
            const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
            std::ofstream out((cdir + QStringLiteral("/gamedata.db0")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
            out.close();
            FsPage crash_page;
            crash_page.resize(760, page_min_h());
            crash_page.show();
            QApplication::processEvents();
            crash_page.scan_dir(cdir);       // 只入列表、未加载
            QApplication::processEvents();
            QTreeWidgetItem* row0 = crash_page.package_list()->topLevelItem(0);
            const int top_before = crash_page.package_list()->topLevelItemCount();
            const bool was_loaded = crash_page.loaded_source_count() > 0;
            if (row0 != nullptr) row0->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(row0 != nullptr && !was_loaded &&
                      crash_page.package_list()->topLevelItemCount() == top_before &&
                      crash_page.package_list()->topLevelItem(0) == row0 &&
                      crash_page.loaded_source_count() > 0,
                  QStringLiteral("包树不崩① 勾未加载的包：**就地加载、不重建包树**（同一对象 %1 / 顶层 %2→%3 / "
                                 "已加载 %4→%5）")
                      .arg(crash_page.package_list()->topLevelItem(0) == row0 ? 1 : 0)
                      .arg(top_before)
                      .arg(crash_page.package_list()->topLevelItemCount())
                      .arg(was_loaded ? 1 : 0)
                      .arg(crash_page.loaded_source_count()));
            crash_page.hide();
            QDir(cdir).removeRecursively();
        }

        // ★ 用户 2026-09-30：三层结构里勾最内层文件 ⇒ **最外层祖先也要显示"部分勾选"**
            //   （修前只算直接父目录 ⇒ 最外层恒未选中 ⇒ 这条红）。
            {
                FsPage tri_page;
                tri_page.resize(760, page_min_h());
                tri_page.show();
                QApplication::processEvents();
                tri_page.add_source(QStringLiteral("s"),
                                    {{"meshes/actors/actor_models/green_stalker_10.ogf", false},
                                     {"meshes/actors/actor_models/green_stalker_15.ogf", false},
                                     {"configs/ai_tweaks/ai_tweaks.ltx", false}});
                QApplication::processEvents();
                // ★ 2026-10-05（#2）：右栏＝**解包产物预览** ⇒ 条目全挂在包名壳 `s/` 下，
                //   展开也要从壳那一层起（懒建**逐级**：每展开一层才建下一层的子项）。
                tri_page.expand_entry(QStringLiteral("s"));
                tri_page.expand_entry(QStringLiteral("s/meshes"));
                tri_page.expand_entry(QStringLiteral("s/meshes/actors"));
                tri_page.expand_entry(QStringLiteral("s/meshes/actors/actor_models"));
                QApplication::processEvents();
                QTreeWidgetItem* deep = tri_page.find_entry_item(
                    QStringLiteral("s/meshes/actors/actor_models/green_stalker_10.ogf"));
                if (deep == nullptr) {
                    check(false, QStringLiteral("三态传播：判据自己要能跑到 —— 找不到最内层文件项"));
                } else {
                    deep->setCheckState(0, Qt::Checked);   // 触发 itemChanged ⇒ 回灌模型
                }
                QApplication::processEvents();
                QTreeWidgetItem* mid = tri_page.find_entry_item(QStringLiteral("s/meshes/actors"));
                QTreeWidgetItem* top = tri_page.find_entry_item(QStringLiteral("s/meshes"));
                const Qt::CheckState mid_state = mid != nullptr ? mid->checkState(0) : Qt::Unchecked;
                const Qt::CheckState top_state = top != nullptr ? top->checkState(0) : Qt::Unchecked;
                check(top_state == Qt::PartiallyChecked,
                      QStringLiteral("三态传播：勾最内层文件 ⇒ 最外层要部分勾选（实测 中间层=%1 最外层=%2）")
                          .arg(static_cast<int>(mid_state))
                          .arg(static_cast<int>(top_state)));
                tri_page.hide();
            }
            // ★ 用户 2026-09-30 与 2026-10-01 **两次**指出「'包内条目'那一行字顶部被切」
            //   ⇒ 这次把机制量清楚了（`--shot` 的 HDR 行）：表头高度由 QHeaderView 按**它自己的
            //   字体**算，而 QSS 的 `padding` 又被用作内容区的内缩 ⇒ 8 px 上下内边距会把
            //   15 px 的字压在 9 px 的内容区里、顶部直接切掉（实测表头 25 px）。
            //   判据：**表头高 ≥ 表头自己的字高 + 2×QSS 上下内边距(现为 pad4)** —— 两边任一处
            //   回退（字号不跟内容走 / 内边距又调回 8）这条就会红。
            {
                FsPage head_page;
                head_page.resize(760, page_min_h());
                head_page.show();
                QApplication::processEvents();
                QHeaderView* hh = head_page.entry_tree()->header();
                const int font_h = hh != nullptr ? hh->fontMetrics().height() : -1;
                const int need = font_h + 2 * xrkit::core::px(4);
                check(hh != nullptr && hh->height() >= need,
                      QStringLiteral("表头不许被裁：表头高 %1 ≥ 字高 %2 + 2×内边距 %3 = %4")
                          .arg(hh != nullptr ? hh->height() : -1)
                          .arg(font_h)
                          .arg(xrkit::core::px(4))
                          .arg(need));
                // ★ 加一条**有牙**的锁：表头高度不随 QSS 内边距变（padding 只影响内容区），
                //   所以光比高度抓不住"有人把内边距调回 8px" —— 直接钉住样式表里那一节的
                //   上下内边距必须是 px(4)（8px 那版实测把 15px 的字压在 9px 内容区里、顶部被切）。
                //
                //   ★★ 2026-10-06（审计处理批·0-3 之 B1-4）：修前这里是
                //   `qss.mid(qss.lastIndexOf("QHeaderView::section {"))` —— 取的是**从那条规则
                //   一直到样式表末尾**的整段，再 `contains("padding: 4px ")`。样式表里
                //   `QHeaderView::section {` 有**两条**（基础表 + 尺寸档位段），把**生效的那条**
                //   （带 `font_content` 高度/内边距、真正防裁字的那条）整条删掉后，
                //   `lastIndexOf` 落到另一条上，而那句 `padding: 4px ` 在后面的规则里照样能命中 ⇒
                //   锁保持绿。现在只取**那一条规则的规则体**（括号配平），删掉就红。
                {
                    const QString qss = xrkit::shell::stylesheet_for(xrkit::core::ThemeMode::Dark);
                    const QString rule =
                        qss_rule_body(qss, QStringLiteral("QHeaderView::section"), /*last=*/true);
                    const QString want = QStringLiteral("padding: %1px ").arg(xrkit::core::px(4));
                    check(!rule.isEmpty() && rule.contains(want),
                          QStringLiteral("表头 QSS 的上下内边距必须是 px(%1)（回退到 8px 就会裁字；"
                                         "生效规则体=「%2」）")
                              .arg(xrkit::core::px(4))
                              .arg(rule));
                }
                head_page.hide();
            }
            // ★ 用户 2026-09-30：「日志记录的是什么东西，为什么没有在 app 当中的操作（例如，加载包）？」
            //   判据：在**真 Hub** 里让 fs 页加载一个包 ⇒ 日志栏必须出现「已加载」行。
            //   （页面单独构造时 `window()` 不是 Hub，这条路测不到 ⇒ 必须在 Hub 里测。）
            {
                Hub log_hub(nullptr, QString());
                const QString tmp = QDir::temp().filePath(QStringLiteral("xrkit_smoke_log"));
                QDir(tmp).removeRecursively();
                QDir().mkpath(tmp);
                {
                    std::vector<efs::DbPackItem> items;
                    efs::DbPackItem one;
                    one.path = "configs/ai_tweaks/ai_tweaks.ltx";
                    one.is_dir = false;
                    one.data = {'x', 'y'};
                    items.push_back(one);
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(tmp.toStdString() + "/p.db0", std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage* fs = nullptr;
                PageBase* fs_slot_page = nullptr;
                for (int i = 0; i < log_hub.page_count(); ++i) {
                    const QString pid = log_hub.page_id(i);
                    if (pid == QStringLiteral("fs")) fs = dynamic_cast<FsPage*>(log_hub.page_at(i));
                    // ★★ P16-2c：fs 槽位也可能由**插件页**顶替（`replaces="fs"` ⇒ id `fs_py`）。
                    //   插件版的"加载"入口在 Python 里，C++ 侧**没有**可调用的加载函数 ⇒
                    //   这一支不能照抄 `add_paths()`；改成量那条**前置机制**：
                    //   宿主交付的**日志口**（`PageServices::log`）真的通到日志栏。
                    //   ★ 探针文字**故意不含**「已加载」——否则这条判据会变成自证（自己写自己认）。
                    //   ★ 没有放宽：内置版那一支的文字断言一字未动；插件版不是"跳过"，
                    //     而是量另一条**可判红**的事实（日志口没交付/写不进 ⇒ 红）。
                    //     插件版"加载/改写产物"的**语义**由 `fs-plugin` / `toolchain-plugin`
                    //     两步用 headless 逐字节锁着，不在这里重复。
                    if (pid == QStringLiteral("fs_py")) fs_slot_page = log_hub.page_at(i);
                }
                const QString probe = QStringLiteral("自检：插件页日志口探针");
                if (fs_slot_page != nullptr && fs_slot_page->services().log != nullptr) {
                    fs_slot_page->services().log(probe);
                }
                if (fs != nullptr) {
                    fs->add_paths({tmp + QStringLiteral("/p.db0")});
                }
                // 日志是**攒批**写的 ⇒ 等真时间（最多 1.5 s），别用"跑几次 processEvents"当"已经刷出来了"
                QString text;
                for (int i = 0; i < 150; ++i) {
                    QApplication::processEvents();
                    text = log_hub.log_view() != nullptr ? log_hub.log_view()->toPlainText() : QString();
                    if (text.contains(QStringLiteral("已加载")) || text.contains(probe)) break;
                    QThread::msleep(10);
                }
                const bool builtin_ok = fs != nullptr && text.contains(QStringLiteral("已加载"));
                const bool plugin_ok = fs_slot_page != nullptr && text.contains(probe);
                check(builtin_ok || plugin_ok,
                      QStringLiteral("日志要记 app 内操作：Hub 里加载包后日志要有「已加载」"
                                     "（内置页那一支 %1 / 插件页日志口那一支 %2 / 日志长度 %3）")
                          .arg(builtin_ok ? 1 : 0)
                          .arg(plugin_ok ? 1 : 0)
                          .arg(text.size()));
                QDir(tmp).removeRecursively();
            }
            // ★ 用户 2026-09-30 ⑦：「现在还是慢得不行。最起码，要追上X-Ray Engine这个优化奇烂的老东西吧。」
            //   先量数字再改：引擎侧实测「15,374 条目 / 106 ms」（SquashFS 真镜像），这里量 **UI 侧**
            //   （`add_paths` ＝过闸门 + 解条目表 + 合进 MergeTree + 建界面树）用同一量级的夹具。
            {
                const QString big = QDir::temp().filePath(QStringLiteral("xrkit_smoke_big.db0"));
                {
                    std::vector<efs::DbPackItem> items;
                    items.reserve(15000);
                    for (int i = 0; i < 15000; ++i) {
                        efs::DbPackItem one;
                        one.path = "configs/ai_tweaks/dir" + std::to_string(i / 100) + "/f" +
                                   std::to_string(i) + ".ltx";
                        one.is_dir = false;
                        one.data = {'x'};
                        items.push_back(std::move(one));
                    }
                    const std::vector<uint8_t> raw = efs::pack_db(items, *efs::format_key_to_db("xdb"));
                    std::ofstream out(big.toStdString(), std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char*>(raw.data()),
                              static_cast<std::streamsize>(raw.size()));
                }
                FsPage big_page;
                big_page.resize(760, page_min_h());
                big_page.show();
                QApplication::processEvents();
                const auto began = std::chrono::steady_clock::now();
                big_page.add_paths({big});
                QApplication::processEvents();
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - began)
                                         .count();
                std::fprintf(stderr, "SMOKE-TIMING add_paths_15000_entries=%lld ms\n", ms);
                check(ms < 5000,
                      QStringLiteral("载 1.5 万条目不许数量级回归：实测 %1 ms（引擎侧同量级 106 ms）").arg(ms));
                big_page.hide();
                QFile::remove(big);
            }
            check(tree_page.entry_top_level_count() == 2,
                  QStringLiteral("树① 顶层 2 项（top.txt + d/），实测 %1")
                      .arg(tree_page.entry_top_level_count()));
            check(tree_page.entry_built_file_count() == 1,
                  QStringLiteral("树① **懒建**：没展开时只建顶层（已建文件项 %1）")
                      .arg(tree_page.entry_built_file_count()));
            // 「展开信号接上了」：程序化 setExpanded 不发信号（见 expand_entry 注释），
            // 所以这里**手动发一条** `itemExpanded`（等价于视图在用户点箭头时发的那条）⇒ 懒建必须发生。
            // 这一条证明的是"连接 + 懒建逻辑"，不是"Qt 会不会发信号"（那是 Qt 的契约）。
            {
                QTreeWidgetItem* d_raw = tree_page.find_entry_item(QStringLiteral("d"));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemExpanded",
                                          Q_ARG(QTreeWidgetItem*, d_raw));
                QApplication::processEvents();
                check(tree_page.entry_built_file_count() == 3,
                      QStringLiteral("树① itemExpanded ⇒ 懒建子节点（已建文件项 %1，期望 3）")
                          .arg(tree_page.entry_built_file_count()));
            }
            check(tree_page.find_entry_item(QStringLiteral("d")) != nullptr,
                  QStringLiteral("树① 能按路径找到条目项"));
            // ★ 用户 2026-09-29 第 5 条：文件夹**单击**就展开；文件**单击条目本身**就切换勾选
            //   （勾选框只是视觉效果）。判据落在行为上：发一次 itemClicked 看状态有没有变。
            {
                QTreeWidgetItem* f = tree_page.find_entry_item(QStringLiteral("top.txt"));
                check(f != nullptr && tree_page.checked_file_count() == 0,
                      QStringLiteral("树② 起点：一个都没勾"));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, f), Q_ARG(int, 0));
                QApplication::processEvents();
                check(tree_page.checked_file_count() == 1,
                      QStringLiteral("树② **单击条目 = 勾选**（实测 %1，期望 1）")
                          .arg(tree_page.checked_file_count()));
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, f), Q_ARG(int, 0));
                QApplication::processEvents();
                check(tree_page.checked_file_count() == 0,
                      QStringLiteral("树② 再单击一次 = 取消勾选（实测 %1，期望 0）")
                          .arg(tree_page.checked_file_count()));
                QTreeWidgetItem* dd = tree_page.find_entry_item(QStringLiteral("d"));
                const bool was_open = dd != nullptr && dd->isExpanded();
                const int before = tree_page.entry_built_file_count();
                QMetaObject::invokeMethod(tree_page.entry_tree(), "itemClicked",
                                          Q_ARG(QTreeWidgetItem*, dd), Q_ARG(int, 0));
                QApplication::processEvents();
                check(dd != nullptr && dd->isExpanded() != was_open &&
                          tree_page.entry_built_file_count() >= before,
                      QStringLiteral("树② **单击文件夹 = 展开/折叠**（展开态 %1→%2，已建文件项 %3→%4）")
                          .arg(was_open ? 1 : 0)
                          .arg(dd != nullptr && dd->isExpanded() ? 1 : 0)
                          .arg(before)
                          .arg(tree_page.entry_built_file_count()));
                // 列宽可拖拽（用户第 4 条）：三列都必须是 Interactive，否则用户调不了
                QHeaderView* hh = tree_page.entry_tree()->header();
                bool interactive = hh != nullptr;
                for (int c = 0; c < 3 && interactive; ++c) {
                    interactive = hh->sectionResizeMode(c) == QHeaderView::Interactive;
                }
                check(interactive && hh->sectionSize(0) >= xrkit::core::px(200),
                      QStringLiteral("树② 三列**可拖拽**（模式全 Interactive=%1，首列 %2 px）")
                          .arg(interactive ? 1 : 0)
                          .arg(hh != nullptr ? hh->sectionSize(0) : -1));
            }
            check(tree_page.expand_entry(QStringLiteral("d")) &&
                      tree_page.entry_built_file_count() == 3,
                  QStringLiteral("树① 展开后子项出现且**幂等**（已建文件项 %1，期望 3）")
                      .arg(tree_page.entry_built_file_count()));
            // 单个文件勾选（1.x 的 CanvasTree 能一行一行勾）⇒ 模型计数跟着走
            QTreeWidgetItem* one = tree_page.find_entry_item(QStringLiteral("d/one.txt"));
            check(one != nullptr && tree_page.checked_file_count() == 0,
                  QStringLiteral("树① 初始没有勾选"));
            if (one != nullptr) one->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(tree_page.checked_file_count() == 1,
                  QStringLiteral("树① **能勾单个文件**（实测勾了 %1 个）")
                      .arg(tree_page.checked_file_count()));
            // 目录勾选 = 整目录（三态：勾满 → Checked；勾一个 → PartiallyChecked）
            QTreeWidgetItem* dir_item = tree_page.find_entry_item(QStringLiteral("d"));
            check(dir_item != nullptr && dir_item->checkState(0) == Qt::PartiallyChecked,
                  QStringLiteral("树① 目录三态：勾了 1/2 ⇒ PartiallyChecked（实测 %1）")
                      .arg(dir_item != nullptr ? static_cast<int>(dir_item->checkState(0)) : -1));
            if (dir_item != nullptr) dir_item->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            // 勾 `d/` = 该目录**整棵**勾上（正好 2 个文件）；`top.txt` 不在它里面 ⇒ 仍是未勾。
            // （我第一版把期望写成 3，是把"顶层文件"也算进去了 —— 判据写错，已改）
            check(tree_page.checked_file_count() == 2,
                  QStringLiteral("树① 勾目录 = 该目录整棵勾上（实测 %1，期望它自己的 2 个）")
                      .arg(tree_page.checked_file_count()));
            QTreeWidgetItem* top_item = tree_page.find_entry_item(QStringLiteral("top.txt"));
            if (top_item != nullptr) top_item->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            check(tree_page.checked_file_count() == 3,
                  QStringLiteral("树① 再勾顶层文件 ⇒ 3 个（实测 %1）")
                      .arg(tree_page.checked_file_count()));
            // 解包选中只带"勾选的"（按包分组）：勾满 3 个 ⇒ only 是 3 条
            QString root_unused;
            const std::vector<FsPage::UnpackJob> jobs = tree_page.plan_unpack(true, &root_unused);
            check(jobs.size() == 1 && jobs[0].only.size() == 3,
                  QStringLiteral("树① 勾选进得了「解包选中」的计划（包 %1 / 条目 %2）")
                      .arg(jobs.size())
                      .arg(jobs.empty() ? 0 : jobs[0].only.size()));
            tree_page.hide();
        }

        // 6.3：两个"全选"
        page.select_all_button()->click();
        check(page.selection().size() == 2, QStringLiteral("6.3 「全选」改了包列表选中态"));
        // ★★ 2026-10-01 **口径再次改写（回退）**——用户原话：「全选就直接勾选了？不能这样。」
        //   「全选包，功能是选中而非勾选。」⇒ 「全选」（原「全选包」）**只改选中态**，
        //   勾选态一个都不碰（上一轮我把它改成"选中+勾选"，被用户当场否掉）。
        check(page.checked_file_count() == 0,
              QStringLiteral("6.3 「全选」**不碰**包内勾选态（实测勾了 %1 个）")
                  .arg(page.checked_file_count()));
        page.check_all_button()->click();
        check(page.checked_file_count() == 3,
              QStringLiteral("6.3 「包内全选」勾上了 3 个文件（实测 %1）")
                  .arg(page.checked_file_count()));
        check(page.selection().size() == 2,
              QStringLiteral("6.3 「包内全选」**没有**碰包列表选中态（实测 %1）")
                  .arg(page.selection().size()));

        // 6.11：选中集解包与批量解包走**同一个**入口 —— 同一份输入、同一句话。
        // 真镜像上的逐字节对拍在门禁 [8] 的整树导出里做；这里证的是"没有第二条路"。
        const efs::ExtractTreeResult batch =
            page.extract_to(QStringLiteral("no_such_image.sqfs"), QStringLiteral("no_out"),
                            efs::Selection::All);
        const efs::ExtractTreeResult picked =
            page.extract_to(QStringLiteral("no_such_image.sqfs"), QStringLiteral("no_out"),
                            efs::Selection::Listed, {"a.txt"});
        check(!batch.ok && !picked.ok && batch.error == picked.error && !batch.error.empty(),
              QStringLiteral("6.11 批量与选中解包同源（同样失败、同样的话：%1）")
                  .arg(QString::fromStdString(batch.error)));
    }

    // ② 点页面空白 ⇒ 输入框必须**失去焦点**（用户真机："能输入的框点空白并不能取消，
    //    还是一闪一闪"）。判据是**行为**，不是"看代码里有没有 setFocusPolicy"。
    {
        ConvertPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::setActiveWindow(&page);
        QApplication::processEvents();
        QLineEdit* edit = page.source_edit();
        check(edit != nullptr, QStringLiteral("② 夹具要有可聚焦的输入框"));
        if (edit != nullptr) {
            edit->setFocus(Qt::MouseFocusReason);
            QApplication::processEvents();
            const bool got = edit->hasFocus();
            // 按下"卡片空白处"：卡片是不接焦点的容器 ⇒ 焦点必须回到页面、输入框熄掉
            QWidget* blank = page.card() != nullptr ? page.card() : &page;
            const QPointF local(blank->width() / 2.0, blank->height() / 2.0);
            QMouseEvent press(QEvent::MouseButtonPress, local, blank->mapToGlobal(local.toPoint()),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(blank, &press);
            QApplication::processEvents();
            check(got, QStringLiteral("② 夹具前提：输入框本应先拿到焦点（实测 %1）")
                           .arg(edit->hasFocus()));
            check(!edit->hasFocus(),
                  QStringLiteral("② 点卡片空白后输入框必须失去焦点（实测仍持有 %1）")
                      .arg(edit->hasFocus() ? 1 : 0));
            // 反向：点**输入框自己**不许被页面抢走焦点（过滤器只装在空白容器上）
            edit->setFocus(Qt::MouseFocusReason);
            QApplication::processEvents();
            QMouseEvent on_edit(QEvent::MouseButtonPress, QPointF(2, 2), QPoint(), Qt::LeftButton,
                                Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(edit, &on_edit);
            QApplication::processEvents();
            check(edit->hasFocus(),
                  QStringLiteral("② 点输入框本身不许把焦点抢到页面上（实测丢了）"));
        }
        page.hide();
    }

    // ⑨/⑬ 编码转换页（M2-6c2）：判定步骤来自引擎（9.7）、暂停归 App（13.6）、
    // 空闲文案非空（13.5）、进度只发一个 0~100 值（13.7）。
    {
        namespace etxt = xrkit::engine::text;
        ConvertPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 9.7：行文本里的"定案步骤"必须**逐字等于**引擎给的那一行 —— 页面没有
        // 能力自己拼一个标签（静态锁另证：本文件里不出现那几个标签字面量）。
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_convert"));
        QDir().mkpath(dir);
        const std::vector<uint8_t> cp1251 = {0xEF, 0xE2, 0xE0, 0xEE, 0x0A};  // cp1251 的"ево"
        const std::string file_name = "probe.xml";
        {
            std::ofstream out(QDir(dir).filePath(QString::fromStdString(file_name)).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(cp1251.data()),
                      static_cast<std::streamsize>(cp1251.size()));
        }
        const etxt::SniffResult sniffed = etxt::sniff_encoding(cp1251, "auto");
        const std::string expected_label = etxt::sniff_result_label(sniffed);

        const QString source_path = QDir(dir).filePath(QString::fromStdString(file_name));
        const etxt::ConvertOutcome outcome =
            page.convert_one(source_path, dir, QStringLiteral("auto"), QStringLiteral("utf-8"));
        check(outcome.ok && outcome.wrote, QStringLiteral("⑨ 转换真的产出了文件"));
        check(outcome.step_label == expected_label,
              QStringLiteral("⑨9.7 页面拿到的定案步骤 = 引擎判定（%1 vs %2）")
                  .arg(QString::fromStdString(outcome.step_label))
                  .arg(QString::fromStdString(expected_label)));
        check(outcome.step_label.find("声明") == std::string::npos ||
                  sniffed.step == etxt::SniffStep::XmlDeclaration,
              QStringLiteral("⑨9.7 步骤标签与判定步号一致（%1）")
                  .arg(QString::fromStdString(outcome.step_label)));

        // 产物按目标编码写出，且**不再带 BOM/换行翻译**
        {
            std::ifstream in(QDir(dir).filePath(QString::fromStdString(file_name))
                                 .toStdString(),
                             std::ios::binary);
            const std::string body((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            check(body.find('\r') == std::string::npos,
                  QStringLiteral("⑨14 写盘用 newline=\"\"（不许把 \\n 翻成 \\r\\n）"));
            check(body.size() >= 3 && body.compare(0, 3, "\xEF\xBB\xBF") != 0,
                  QStringLiteral("⑨ 输出侧不带 BOM"));
        }

        // 13.6：暂停状态机在 App 这边 —— 暂停时工作循环必须被拦住（且计数）
        const int before = page.pause_gate_hits();
        page.set_paused(true);
        check(!page.wait_if_paused(false), QStringLiteral("13.6 暂停时循环被拦住"));
        check(page.pause_gate_hits() == before + 1,
              QStringLiteral("13.6 拦截有计数（%1 → %2）")
                  .arg(before)
                  .arg(page.pause_gate_hits()));
        page.set_paused(false);
        check(page.wait_if_paused(false), QStringLiteral("13.6 继续后循环放行"));

        // ★★ 2026-10-10（P17-1，审计 03 F-02 High）：**暂停等待必须能被取消位打断**。
        //
        // 修前事实：`wait_if_paused` 的循环只看 `paused_`，而 `~TaskShell()` 只置取消位、
        // **完全不碰 `paused_`**（`paused_` 只有主线程 `set_paused(false)` 能清）⇒ 用户点
        // 「暂停」后关窗：worker 在 gate 里自旋不出来、主线程 `join()` 永久阻塞（进程关不掉）。
        // 两条判据：
        //   ① **干净判红形态**：暂停中、收到取消 ⇒ `wait_if_paused(true)` 必须**立刻返回 false**
        //      （≤2s 就算过；超时**不算通过**，自检自己解开暂停，别把门禁挂死）；
        //   ② **端到端**：暂停态收工（`stop_and_wait()` ＝ `~TaskShell()` 走的那条路：置取消 +
        //      join）必须返回 —— 这就是"暂停态关窗"那条。
        {
            TaskShell* gate_shell = page.task();
            check(gate_shell != nullptr, QStringLiteral("P17-1 前提：编码转换页有任务壳"));
            const auto wait_flag = [](const std::atomic<bool>& flag, int ms) {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return flag.load();
            };

            std::atomic<bool> entered{false};
            std::atomic<bool> done{false};
            std::atomic<bool> gate_value{true};
            page.set_paused(true);
            const bool started = gate_shell->run(
                QStringLiteral("P17-1 暂停/取消"), [&](const ProgressSink&, const MessageSink&) {
                    entered.store(true);
                    gate_value.store(page.wait_if_paused(true));
                    done.store(true);
                });
            check(started, QStringLiteral("P17-1 前提：暂停态任务能启动（没被 13.1 闸门拒）"));
            check(wait_flag(entered, 2000), QStringLiteral("P17-1 前提：worker 进了 gate"));
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const bool blocked = !done.load();
            gate_shell->request_cancel(QStringLiteral("P17-1 自检：暂停态取消"));
            const bool interrupted = wait_flag(done, 2000);
            check(blocked,
                  QStringLiteral("P17-1 前提：暂停时 gate 真把 worker 卡住（不是立刻就放行）"));
            check(interrupted && !gate_value.load(),
                  QStringLiteral("P17-1 03 F-02：暂停态收到取消 ⇒ gate **立刻返回 false**"
                                 "（修前永久自旋 ⇒ ~TaskShell 的 join 阻塞、进程关不掉；"
                                 "实测 gate 返回=%1，是否退出=%2）")
                      .arg(gate_value.load() ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(interrupted ? QStringLiteral("是") : QStringLiteral("否")));
            page.set_paused(false);
            if (!done.load()) (void)wait_flag(done, 2000);   // 判红兜底：别让自检自己挂死
            QApplication::processEvents();                   // 让 apply_finish（排队）回主线程

            // ② 端到端：**暂停态收工**（＝关窗时 `~TaskShell` 走的那条路）必须返回
            std::atomic<bool> second_entered{false};
            std::atomic<bool> second_done{false};
            std::atomic<bool> stop_returned{false};
            page.set_paused(true);
            gate_shell->run(QStringLiteral("P17-1 暂停态关窗"),
                            [&](const ProgressSink&, const MessageSink&) {
                                second_entered.store(true);
                                (void)page.wait_if_paused(true);
                                second_done.store(true);
                            });
            check(wait_flag(second_entered, 2000),
                  QStringLiteral("P17-1 前提：第二个 worker 进了 gate"));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            QElapsedTimer stop_timer;
            stop_timer.start();
            // ★ 看门狗：`stop_and_wait()` 放到**旁路线程**里等 —— 修前它永远不返回（worker 卡在
            //   gate 自旋里），若在主线程直调，自检自己就挂死了。判红时下面把暂停解开收尾；
            //   **真机没有这个逃生口**：关窗就是关窗（这正是"进程关不掉"那条）。
            std::thread stopper([&] {
                gate_shell->stop_and_wait();
                stop_returned.store(true);
            });
            const bool stopped_in_time = wait_flag(stop_returned, 2000);
            const qint64 stop_ms = stop_timer.elapsed();
            if (!stopped_in_time) page.set_paused(false);   // 判红兜底
            stopper.join();
            page.set_paused(false);
            check(stopped_in_time && second_done.load() && stop_ms < 2000,
                  QStringLiteral("P17-1 03 F-02 端到端：暂停态收工（置取消 + join，等价于关窗）"
                                 "在 %1 ms 内返回（已返回=%2 / worker 退出=%3）")
                      .arg(stop_ms)
                      .arg(stopped_in_time ? QStringLiteral("是") : QStringLiteral("否"))
                      .arg(second_done.load() ? QStringLiteral("是") : QStringLiteral("否")));
            QApplication::processEvents();
            check(!gate_shell->running(),
                  QStringLiteral("P17-1 收工后任务壳回到空闲（apply_finish 已回主线程）"));
        }

        // 13.5：空闲文案非空；下拉的取值是内部键（不是显示名）
        TaskShell* task = page.task();
        check(task != nullptr && !task->idle_status().isEmpty(),
              QStringLiteral("13.5 空闲文案非空（%1）")
                  .arg(task == nullptr ? QString() : task->idle_status()));
        check(!page.current_source_encoding().empty() &&
                  !page.current_target_encoding().empty(),
              QStringLiteral("⑨ 编码下拉的取值是内部键（%1 → %2）")
                  .arg(QString::fromStdString(page.current_source_encoding()))
                  .arg(QString::fromStdString(page.current_target_encoding())));
        check(page.current_target_encoding() == "utf-8",
              QStringLiteral("⑨ 目标编码默认为 utf-8"));
        QDir(dir).removeRecursively();
    }

    // ⑦ 文本提取页（M2-6d）：判据同源（7.1/7.5 由引擎给理由、7.2 两次运行同产物）。
    {
        namespace etxt = xrkit::engine::text;
        TextPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_text"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/gameplay/config"));
        QDir().mkpath(root + QStringLiteral("/out"));
        // 一份最小的 scripts 夹具（真语料上的正确性由门禁 [15] 的逐字节对拍负责）
        QDir().mkpath(root + QStringLiteral("/scripts"));
        {
            std::ofstream out((root + QStringLiteral("/scripts/probe.script")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            const std::string body = "function probe()\n  local s = \"hello stalker\"\nend\n";
            out.write(body.data(), static_cast<std::streamsize>(body.size()));
        }

        // ⑦7.1/7.5：页面**不自己判** —— 返回的必须是引擎那句原话
        etxt::ExtractRequest request;
        request.source_dir = (root + QStringLiteral("/gameplay/config")).toStdString();
        request.target_dir = (root + QStringLiteral("/out")).toStdString();
        const QString engine_reason = QString::fromStdString(etxt::validate_request(request));
        const QString page_reason =
            page.validate(root + QStringLiteral("/gameplay/config"), root + QStringLiteral("/out"));
        check(!engine_reason.isEmpty() &&
                  page_reason == engine_reason &&
                  engine_reason.contains(QStringLiteral("不要选它们的上层目录")),
              QStringLiteral("⑦7.1 页面理由 = 引擎理由（%1）").arg(page_reason));

        // ⑦7.5：输出目录 = 源目录的父目录 ⇒ 引擎拒绝，页面原样透出
        const QString cover = page.validate(root + QStringLiteral("/scripts"), root);
        check(cover.contains(QStringLiteral("会覆盖源目录中的原文件")),
              QStringLiteral("⑦7.5 覆盖源目录被拒（%1）").arg(cover));

        // ⑦7.2/7.3：显式 id_prefix ⇒ 同一份源两次运行产物逐字节相同
        const QString out_a = root + QStringLiteral("/out_a");
        const QString out_b = root + QStringLiteral("/out_b");
        const etxt::ExtractStats first =
            page.extract_one(root + QStringLiteral("/scripts"), out_a, QStringLiteral("smoke"),
                             QStringLiteral("smoke_sc_0"), true);
        const etxt::ExtractStats second =
            page.extract_one(root + QStringLiteral("/scripts"), out_b, QStringLiteral("smoke"),
                             QStringLiteral("smoke_sc_0"), true);
        check(first.ok && first.files == 1 && first.strings >= 1,
              QStringLiteral("⑦ 提取产出（files=%1 strings=%2 xml=%3）")
                  .arg(first.files)
                  .arg(first.strings)
                  .arg(QString::fromStdString(first.texts_xml_name)));
        check(!first.texts_xml_name.empty() &&
                  QFileInfo::exists(QDir(out_a).filePath(QString::fromStdString(
                      first.texts_xml_name))),
              QStringLiteral("⑦ 文本 XML 落在输出目录根（%1）")
                  .arg(QString::fromStdString(first.texts_xml_name)));

        const auto tree_signature = [](const QString& dir) {
            QStringList parts;
            QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) parts << it.next();
            parts.sort();
            QString joined;
            for (const QString& file : parts) {
                QFile f(file);
                if (!f.open(QIODevice::ReadOnly)) return QString();
                const QByteArray body = f.readAll();
                joined += QDir(dir).relativeFilePath(file) + QStringLiteral(":") +
                          QString::number(body.size()) + QStringLiteral(":") +
                          QString::fromLatin1(QCryptographicHash::hash(body,
                                                                      QCryptographicHash::Sha1)
                                                  .toHex());
                joined += QLatin1Char('\n');
            }
            return joined;
        };
        check(first.ok && second.ok && tree_signature(out_a) == tree_signature(out_b) &&
                  !tree_signature(out_a).isEmpty(),
              QStringLiteral("⑦7.2 两次运行产物逐字节相同（页面入口）"));

        // 7.4（口径不对称）与 7.6（自己的产物不重复提取）在引擎里有对拍，
        // 页面上只证一件事：参数真的透传下去了（前缀不同 ⇒ 产物 XML 名不同）。
        const etxt::ExtractStats other_prefix =
            page.extract_one(root + QStringLiteral("/scripts"), root + QStringLiteral("/out_c"),
                             QStringLiteral("zz"), QStringLiteral("zz_sc_0"), true);
        check(other_prefix.ok &&
                  other_prefix.texts_xml_name != first.texts_xml_name &&
                  other_prefix.texts_xml_name.rfind("zz_", 0) == 0,
              QStringLiteral("⑦ 前缀透传（%1 vs %2）")
                  .arg(QString::fromStdString(first.texts_xml_name))
                  .arg(QString::fromStdString(other_prefix.texts_xml_name)));

        // ★★ 2026-10-06（②）统计框三行：`gameplay / scripts / 总计`
        //   （1.x `text_extract_app.py:647-656` 的 `stat_grid` + `:725-729` 写值）
        //   ① 真跑一遍：标签上的数字必须**来自引擎给的 `ExtractStats`**（页面不自己数）
        {
            const etxt::ExtractStats real = page.extract_one(
                root + QStringLiteral("/scripts"), root + QStringLiteral("/out_d"),
                QStringLiteral("smoke"), QStringLiteral("smoke_sc_0"), true);
            // 与 worker 里那条 `QMetaObject::invokeMethod(... note_stats ...)` 是**同一条入口**
            page.note_stats(real);
            const QStringList lines = page.stats_text().split(QLatin1Char('\n'));
            check(real.ok && lines.size() == 3 && lines[1].startsWith(QStringLiteral("scripts")) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.strings))) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.files))) &&
                      lines[1].contains(QString::number(static_cast<qulonglong>(real.chars))) &&
                      page.stats_label() != nullptr &&
                      page.stats_label()->text() == page.stats_text(),
                  QStringLiteral("② 真提取后 scripts 行 = ExtractStats（strings=%1 files=%2 chars=%3 →「%4」）")
                      .arg(real.strings)
                      .arg(real.files)
                      .arg(real.chars)
                      .arg(lines.size() > 1 ? lines[1] : QString()));
        }
        //   ② 固定夹具：三行数字**逐字段**对得上（把某一行删掉 ⇒ 立刻红）
        {
            etxt::ExtractStats gp;
            gp.ok = true;
            gp.kind = etxt::ExtractKind::Gameplay;
            gp.files = 7;
            gp.strings = 11;
            gp.chars = 1234;
            etxt::ExtractStats sc;
            sc.ok = true;
            sc.kind = etxt::ExtractKind::Scripts;
            sc.files = 3;
            sc.strings = 5;
            sc.chars = 66;
            page.note_stats(gp);
            page.note_stats(sc);
            const QStringList lines = page.stats_text().split(QLatin1Char('\n'));
            check(lines.size() == 3 && lines[0].startsWith(QStringLiteral("gameplay")) &&
                      lines[1].startsWith(QStringLiteral("scripts")) &&
                      lines[2].startsWith(QStringLiteral("总计")),
                  QStringLiteral("② 统计框**三行**：gameplay / scripts / 总计（实得 %1 行）")
                      .arg(lines.size()));
            check(lines.size() == 3 &&
                      lines[0] == QStringLiteral("gameplay：11 字符串 · 7 文件 · 1234 字符") &&
                      lines[1] == QStringLiteral("scripts：5 字符串 · 3 文件 · 66 字符"),
                  QStringLiteral("② 三行数字逐字段 = ExtractStats（实得「%1」/「%2」）")
                      .arg(lines.value(0), lines.value(1)));
            check(lines.size() == 3 &&
                      lines[2] == QStringLiteral("总计：16 字符串 · 10 文件 · 1300 字符"),
                  QStringLiteral("② 总计 = 两支逐字段之和（实得「%1」）").arg(lines.value(2)));
        }
        QDir(root).removeRecursively();
    }

    // ⑩ XML 校对页（M2-6e3）：10.1 内部键 / 10.3 摘要是计数 / 10.4 切模式不重跑 /
    // 10.5 导出范围＝筛选后的行（BOM + CRLF）。
    {
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        page.resize(760, page_min_h());
        page.show();
        QApplication::processEvents();

        // 10.1：模式下拉的**取值是内部键**，条数 = 声明表条数
        QComboBox* modes = page.mode_combo();
        const std::vector<exml::XmlModeSpec>& specs = exml::builtin_modes();
        check(modes != nullptr && modes->count() == static_cast<int>(specs.size()),
              QStringLiteral("⑩10.1 模式下拉条数 = 声明表条数（%1）").arg(specs.size()));
        int key_bad = 0;
        for (int i = 0; modes != nullptr && i < modes->count(); ++i) {
            const std::string key = modes->itemData(i).toString().toStdString();
            if (exml::find_mode(key) == nullptr) ++key_bad;
        }
        check(key_bad == 0, QStringLiteral("⑩10.1 模式下拉取值都是内部键（%1 项不符）").arg(key_bad));

        // 合成夹具：一个文件两侧一致、一个两侧不同、一个只在 A 有
        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xml"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/a"));
        QDir().mkpath(root + QStringLiteral("/b"));
        const auto write = [](const QString& path, const QString& body) {
            std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(body.toUtf8().constData(), body.toUtf8().size());
        };
        const QString same =
            QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n");
        write(root + QStringLiteral("/a/same.xml"), same);
        write(root + QStringLiteral("/b/same.xml"), same);
        write(root + QStringLiteral("/a/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>V</text></s>\n</r>\n"));
        write(root + QStringLiteral("/a/only_a.xml"),
              QStringLiteral("<r>\n<s id=\"y\"><text>z</text></s>\n</r>\n"));
        // 统计模式只看**行数/ID 计数**，不看文本 ⇒ 上面那个 diff.xml 在统计模式下
        // 其实是"一致"（同 id、同次数）。要造出统计模式的差异，得让次数不同。
        write(root + QStringLiteral("/a/countdiff.xml"),
              QStringLiteral("<r>\n<s id=\"k\"><text>1</text></s>\n"
                             "<s id=\"k\"><text>2</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/countdiff.xml"),
              QStringLiteral("<r>\n<s id=\"k\"><text>1</text></s>\n</r>\n"));

        const int total = page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                                            QStringLiteral("auto"), QStringLiteral("auto"));
        check(total == 4 && page.result_count() == 4,
              QStringLiteral("⑩ 比较扫到 4 个文件（实测 %1）").arg(total));
        check(page.scan_count() == 1, QStringLiteral("⑩ 第一次比较真的扫了目录"));

        // 10.4：切模式 ⇒ **清空结果**、换筛选项、**不重跑 I/O**
        const int before_scan = page.scan_count();
        page.set_mode(QStringLiteral("text"));
        check(page.current_mode_key() == "text", QStringLiteral("⑩10.4 切到文本模式"));
        check(page.result_count() == 0 && page.visible_count() == 0,
              QStringLiteral("⑩10.4 切模式必须清空旧结果（实测 %1 行）").arg(page.result_count()));
        check(page.scan_count() == before_scan,
              QStringLiteral("⑩10.4 切模式不许重跑 I/O（扫描次数 %1 → %2）")
                  .arg(before_scan)
                  .arg(page.scan_count()));
        check(page.filter_combo() != nullptr && page.filter_combo()->count() ==
                                                    static_cast<int>(specs[1].filters.size()),
              QStringLiteral("⑩10.1 筛选项来自新模式声明（文本模式 %1 项）")
                  .arg(specs[1].filters.size()));

        // 回到统计模式重跑，然后用筛选与导出
        page.set_mode(QStringLiteral("stats"));
        page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                          QStringLiteral("auto"), QStringLiteral("auto"));
        check(page.visible_count() == 4, QStringLiteral("⑩ 「全部」筛选下 4 行"));
        if (page.filter_combo() != nullptr) {
            const int index = page.filter_combo()->findText(QStringLiteral("仅不一致"));
            if (index >= 0) page.filter_combo()->setCurrentIndex(index);
        }
        check(page.visible_count() == 1,
              QStringLiteral("⑩ 筛选「仅不一致」后 1 行（该文件文本不同；实测 %1）")
                  .arg(page.visible_count()));

        // 10.5：导出范围 = 筛选后的行；字节层面看 BOM + CRLF
        const QString csv = root + QStringLiteral("/out.csv");
        check(page.export_csv(csv, QStringLiteral("2026-09-24 00:00:00")),
              QStringLiteral("⑩10.5 导出成功"));
        {
            QFile file(csv);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray body = file.readAll();
                check(body.startsWith("\xEF\xBB\xBF"),
                      QStringLiteral("⑩10.5 导出以 BOM 开头"));
                check(body.contains("\r\n"), QStringLiteral("⑩10.5 导出用 CRLF"));
                // 头部 10 行（标题/时间/模式/两个文件夹/编码/实际编码/筛选/计数/空行）
                // + 列头 1 行 + 筛选后的数据行
                const int want = 11 + page.visible_count();
                const int rows = body.count("\r\n");
                check(rows == want,
                      QStringLiteral("⑩10.5 导出行数 = 头部 10 + 列头 1 + 筛选后 %1 = %2（实测 %3）")
                          .arg(page.visible_count())
                          .arg(want)
                          .arg(rows));
            } else {
                check(false, QStringLiteral("⑩10.5 导出文件读不回来"));
            }
        }
        // 10.3：行文本是**计数摘要**（统计模式下差异来自"次数不同"），
        // 完整明细只在"看这一行"时才生成
        bool summary_ok = false;
        for (int i = 0; i < page.visible_count(); ++i) {
            if (page.row_text(i).contains(QStringLiteral("次数不同 1"))) summary_ok = true;
        }
        check(summary_ok, QStringLiteral("⑩10.3 行文本里是按计数拼的摘要"));
        const QString detail = page.detail_text(0);
        check(detail.contains(QStringLiteral("id=\"")) || detail.contains(QStringLiteral("仅")),
              QStringLiteral("⑩10.3 完整明细在需要时才给（%1）").arg(detail.left(40)));

        // ★★ 2026-10-06（⑤）三个小件：清除 / 统计标签（文件夹名）/ CSV 真实时间
        //   ① 「清除」（1.x `:391-392` + `_clear_entries` `:514-518`）：清两个目录栏
        {
            check(page.clear_button() != nullptr, QStringLiteral("⑤ 页面上有「清除」按钮"));
            page.dir_a_edit()->setText(QStringLiteral("E:/somewhere/a"));
            page.dir_b_edit()->setText(QStringLiteral("E:/somewhere/b"));
            if (page.clear_button() != nullptr) page.clear_button()->click();
            check(page.dir_a_edit()->text().isEmpty() && page.dir_b_edit()->text().isEmpty(),
                  QStringLiteral("⑤ 清除后两栏为空（实得「%1」/「%2」）")
                      .arg(page.dir_a_edit()->text(), page.dir_b_edit()->text()));
            // 复位成"刚比较过的那两个目录"（后面的名字/CSV 判据要用）
            page.dir_a_edit()->setText(root + QStringLiteral("/a"));
            page.dir_b_edit()->setText(root + QStringLiteral("/b"));
        }
        //   ② 统计标签：`一致 N | 不一致 N | 仅 {A名} N | 仅 {B名} N`，且**用文件夹名**
        {
            const QString stats = page.stats_text();
            check(stats == QStringLiteral("一致 2  |  不一致 1  |  仅 a 1  |  仅 b 0"),
                  QStringLiteral("⑤ 统计标签逐字段对得上夹具（实得「%1」）").arg(stats));
            check(stats.contains(QStringLiteral("仅 a ")) &&
                      stats.contains(QStringLiteral("仅 b ")),
                  QStringLiteral("⑤ 统计标签用**文件夹名**（a / b），不是字面量 A/B"));
            check(page.stats_label() != nullptr && page.stats_label()->text() == stats,
                  QStringLiteral("⑤ 统计标签真的挂在界面上"));
        }
        //   ③ CSV：生成时间 = **真实时间**（不是 1970）；统计行用文件夹名
        {
            const QString csv2 = root + QStringLiteral("/out_auto.csv");
            check(page.export_csv_auto(csv2), QStringLiteral("⑤ CSV 自动时间导出成功"));
            QFile file(csv2);
            QString body;
            if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            const QStringList lines = body.split(QStringLiteral("\r\n"));
            const QString stamp_line =
                lines.size() > 1 ? lines[1] : QString();   // "生成时间: yyyy-MM-dd HH:mm:ss"
            const QString stamp = stamp_line.section(QStringLiteral(": "), 1);
            const QDateTime parsed = QDateTime::fromString(stamp, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            check(!stamp.isEmpty() && !stamp.startsWith(QStringLiteral("1970")) && parsed.isValid() &&
                      qAbs(parsed.secsTo(QDateTime::currentDateTime())) < 3600,
                  QStringLiteral("⑤ CSV 生成时间 = 真实时间且可解析（实得「%1」）").arg(stamp));
            check(body.contains(QStringLiteral("一致: 2 | 不一致: 1 | 仅 a: 1 | 仅 b: 0")),
                  QStringLiteral("⑤ CSV 统计行用文件夹名（含「仅 a: 1 | 仅 b: 0」）"));
        }
        //   ④ ①（取消）：compare_dirs 在每个文件之前轮询 ⇒ 停在文件边界、行数＝已比较数
        {
            std::atomic<int> seen{0};
            const int stopped =
                page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                                  QStringLiteral("auto"), QStringLiteral("auto"),
                                  [&seen]() { return seen.fetch_add(1) >= 2; });
            check(page.last_compare_cancelled() && stopped == 2 && page.last_compare_total() == 4,
                  QStringLiteral("① XML 取消：停在文件边界（已比较 %1 / 共 %2，cancelled=%3）")
                      .arg(stopped)
                      .arg(page.last_compare_total())
                      .arg(page.last_compare_cancelled() ? 1 : 0));
            // 恢复：重跑一次不取消，后面的判据不受影响
            page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                              QStringLiteral("auto"), QStringLiteral("auto"));
        }
        QDir(root).removeRecursively();
    }

    // ⑫12.1/12.5：**插件注册的比较模式要出现在 XML 页的同一个下拉里**
    {
        namespace plug = xrkit::plugin;
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        plug::Registry registry;
        for (plug::CompareModeReg& mode : plug::builtin_compare_modes()) {
            registry.register_compare_mode(exml::kBuiltinPlugin, std::move(mode));
        }
        plug::PluginApiAdapter adapter(registry, "demo.dll", nullptr);
        const char* col_keys[] = {"rel", "detail"};
        const char* col_headings[] = {"文件", "差异"};
        const int col_widths[] = {280, 400};
        const char* filter_labels[] = {"全部"};
        const char* filter_kinds[] = {"same|diff|only_a|only_b"};
        const auto compare_fn = +[](void*, const char*, const char*, const char*, const char*,
                                    const char*, stk_result_sink, void*) {};
        check(adapter.api().register_compare_mode(adapter.api().host, "插件模式", "plugin_mode", 40,
                                                 col_keys, col_headings, col_widths, 2,
                                                 filter_labels, 1, filter_kinds, compare_fn,
                                                 nullptr) == 1,
              QStringLiteral("⑫12.1 插件比较模式注册失败"));
        page.set_registry(&registry);
        QComboBox* modes = page.mode_combo();
        QStringList keys;
        for (int i = 0; modes != nullptr && i < modes->count(); ++i) {
            keys << modes->itemData(i).toString();
        }
        check(keys.contains(QStringLiteral("stats")) && keys.contains(QStringLiteral("text")) &&
                  keys.contains(QStringLiteral("plugin_mode")),
              QStringLiteral("⑫12.1 内置与插件模式必须在同一个下拉里（%1）")
                  .arg(keys.join(QStringLiteral("/"))));
        check(keys.size() == 3, QStringLiteral("⑫12.5 模式条数应为 3（%1）").arg(keys.size()));
    }

    // 视频转码接线（用户 2026-09-26：完整移植）—— argv 由引擎算，页面只起进程/报错
    {
        namespace vid = xrkit::engine::video;
        VideoPage page;
        vid::TranscodeRequest request;
        request.ffmpeg = "definitely-not-a-real-ffmpeg.exe";
        request.input = "in.avi";
        request.output = QDir::temp().filePath(QStringLiteral("xrkit_smoke_out.ogm")).toStdString();
        request.source.parsed = true;
        request.source.has_video = true;
        request.source.has_audio = true;
        request.source.duration = 10.0;
        request.available_encoders = {"libtheora"};
        QString error;
        const bool started = page.start_transcode(request, &error);
        check(!started && !error.isEmpty(),
              QStringLiteral("视频：起不来的 ffmpeg 必须**如实报错**（不崩不卡）"));
        check(!page.transcode_running() && !page.last_error().isEmpty(),
              QStringLiteral("视频：启动失败后态/理由要可见（实得 '%1'）").arg(page.last_error()));
        // 取消令牌：只有 reset 能清零；新一轮 start 会先 reset（1.x 语义）
        page.cancel_transcode();
        check(page.cancel_token().cancelled(), QStringLiteral("视频：cancel_transcode 应置位令牌"));
        QString error2;
        page.start_transcode(request, &error2);
        check(!page.cancel_token().cancelled(),
              QStringLiteral("视频：新一轮 start 必须先 reset 取消令牌（1.x：convert 自己不清零）"));
    }

    // ★★ 插件改装 P2b（用户 2026-10-07 拍板：Python 模块接口 + C ABI 退役）：
    //   **Python 插件在 GUI 这条路**上到底通不通 —— 一次量完四件事：
    //     ① Hub 起插件目录时真的起解释器、真的 `import` + `register(api)`；
    //     ② 它注册的命令**出现在 fs 页工具栏**（注册点 ↔ 消费点一一对应，规格 §四.2）；
    //     ③ **点那颗按钮真的回到 Python**（handler 在插件目录里写一个标记文件）；
    //     ④ 好插件不产生任何"插件问题"。
    //   夹具由自检**现写**进临时目录（不依赖仓库路径）：内容里那条 `open(...)` 就是判据本体。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pyplugins"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("clicked.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "PLUGIN_INFO = {'name': '自检夹具'}\n"
            "def register(api):\n"
            "    def on_click():\n"
            "        with open(os.path.join(api.plugin_dir, 'clicked.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write('clicked\\n')\n"
            "    cid = api.register_command('smoke_greet', 'Python夹具命令', on_click)\n"
            "    if cid is None:\n"
            "        raise RuntimeError('register_command 失败')\n"
            "    api.register_menu_item('fs', 'toolbar', cid, 0)\n"
            "    api.log('自检夹具已注册')\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("smoke_hello.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P2b 夹具 .py 没写进 %1").arg(QDir::toNativeSeparators(dir)));

        Hub hub(nullptr, dir);
        QPushButton* command_button = nullptr;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr || page->page_id() != QStringLiteral("fs")) continue;
            for (QPushButton* button : page->action_bar()->findChildren<QPushButton*>()) {
                if (button->objectName().startsWith(QStringLiteral("plugin_smoke_hello"))) {
                    command_button = button;
                }
            }
        }
        check(command_button != nullptr,
              QStringLiteral("P2b Python 插件注册的命令没出现在 fs 页工具栏（注册了却没人消费）"));
        if (command_button != nullptr) command_button->click();
        QString text;
        {
            QFile marker_file(marker);   // 读完就关：下面要删整个目录（Windows 上开着删不掉）
            if (marker_file.open(QIODevice::ReadOnly)) {
                text = QString::fromUtf8(marker_file.readAll());
            }
        }
        check(text.contains(QStringLiteral("clicked")),
              QStringLiteral("P2b 点按钮没回到 Python（标记文件「%1」= %2）")
                  .arg(QDir::toNativeSeparators(marker))
                  .arg(text.isEmpty() ? QStringLiteral("(空/不存在)") : text.trimmed()));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P2b 好插件不该产生插件问题（实得 %1 条：%2）")
                  .arg(hub.plugin_errors().size())
                  .arg(hub.plugin_notice()));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P3（控件门面 L1/L2）：**Python 插件用 `api.ui.*` 造出的页，在真 Hub 里成立吗** ——
    //   一次量五件事：
    //     ① 页注册进了页表（`register_tool`）；
    //     ② L1 造出的控件真的在页里（标签/输入框/下拉/勾选框/只读视图都按"宿主排版"落了位）；
    //     ③ 点按钮**回到 Python**：`get_text(entry)` 读回的是真控件的文本；
    //     ④ `set_text` 改得动控件（改完读标签文本）；
    //     ⑤ `ui.log()` 进得了宿主日志栏（LogView）。
    //   夹具由自检**现写**进临时目录（不依赖仓库路径）；标记落在插件目录里（临时目录 ⇒ 不污染仓库）。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_uifacade"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("ui_marker.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "def register(api):\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'ui_marker.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "    def build():\n"
            "        ui = api.ui\n"
            "        label = ui.label(None, '门面标签')\n"
            "        entry = ui.entry(None, '初值', 24)\n"
            "        ui.combo(None, ['甲', '乙'], lambda v: mark('combo:' + v))\n"
            "        ui.checkbox(None, '勾我', lambda on: mark('check:%d' % int(on)))\n"
            "        ui.text_view(None, 4, True)\n"
            "        bar = ui.progress(None)\n"
            "        ui.set_progress(bar, 42)\n"
            "        mark('progress_set:%d' % ui.progress_value(bar))\n"
            "        ui.set_progress(bar, 150)\n"
            "        mark('progress_clamped:%d' % ui.progress_value(bar))\n"
            "        def on_click():\n"
            "            mark('click:' + ui.get_text(entry))\n"
            "            ui.set_text(label, '点过了')\n"
            "            ui.log('门面夹具：按钮被点了')\n"
            "        ui.button(None, '夹具按钮', on_click)\n"
            "        panel = ui.task_panel(None, True)\n"
            "        cb_label = ui.label(None, '（任务回调前）')\n"
            "        def on_task_msg(msg):\n"
            "            mark('task_msg_thread:%d' % int(ui.on_ui_thread()))\n"
            "            ui.set_text(cb_label, '回调改的:' + msg)\n"
            "            mark('task_msg_text:' + msg)\n"
            "        ui.task_on_message(panel, on_task_msg)\n"
            "        def task_worker(progress, message):\n"
            "            progress(50)\n"
            "            message('来自工作线程')\n"
            "        ui.task_run(panel, '夹具任务', task_worker)\n"
            "        mark('built')\n"
            "    api.register_tool('门面夹具页', build, 5)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("ui_demo.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P3 夹具 .py 没写进 %1").arg(QDir::toNativeSeparators(dir)));

        Hub hub(nullptr, dir);
        const int facade_page = hub.page_index(QStringLiteral("门面夹具页"));
        check(facade_page >= 0,
              QStringLiteral("P3 `register_tool` 的页没进页表（实得 %1，插件问题 %2 条）")
                  .arg(facade_page)
                  .arg(hub.plugin_errors().size()));
        QWidget* page_widget = facade_page >= 0 ? hub.stack()->widget(facade_page) : nullptr;
        QStringList label_texts;
        bool has_button = false;
        bool has_entry = false;
        bool has_combo = false;
        bool has_check = false;
        bool has_view = false;
        if (page_widget != nullptr) {
            for (QLabel* item : page_widget->findChildren<QLabel*>()) label_texts << item->text();
            for (QPushButton* item : page_widget->findChildren<QPushButton*>()) {
                if (item->text() == QStringLiteral("夹具按钮")) has_button = true;
            }
            for (QLineEdit* item : page_widget->findChildren<QLineEdit*>()) {
                if (item->text() == QStringLiteral("初值")) has_entry = true;
            }
            for (QComboBox* item : page_widget->findChildren<QComboBox*>()) {
                if (item->count() == 2) has_combo = true;
            }
            for (QCheckBox* item : page_widget->findChildren<QCheckBox*>()) has_check = true;
            for (QPlainTextEdit* item : page_widget->findChildren<QPlainTextEdit*>()) {
                has_view = true;
            }
        }
        check(label_texts.contains(QStringLiteral("门面标签")),
              QStringLiteral("P3 门面造出的标签没进插件页（实得：%1）")
                  .arg(label_texts.join(QStringLiteral(" / "))));
        check(has_button && has_entry && has_combo && has_check && has_view,
              QStringLiteral("P3 门面控件不齐（按钮=%1 输入=%2 下拉=%3 勾选=%4 只读视图=%5）")
                  .arg(has_button).arg(has_entry).arg(has_combo).arg(has_check).arg(has_view));

        // ③④⑤：点按钮 → 回 Python（get_text）→ 改控件（set_text）→ 写宿主日志（ui.log）
        QPushButton* facade_button =
            page_widget != nullptr ? page_widget->findChild<QPushButton*>() : nullptr;
        if (facade_button != nullptr) facade_button->click();
        if (hub.log_box() != nullptr) hub.log_box()->flush();
        QString marker_text;
        {
            QFile marker_file(marker);
            if (marker_file.open(QIODevice::ReadOnly)) {
                marker_text = QString::fromUtf8(marker_file.readAll());
            }
        }
        check(marker_text.contains(QStringLiteral("click:初值")),
              QStringLiteral("P3 按钮没回到 Python 或 get_text 读不到输入框（标记=%1）")
                  .arg(marker_text.isEmpty() ? QStringLiteral("(空)") : marker_text.simplified()));
        // ★★ P12c-2：任务壳的**回主线程**通道（worker 发的消息必须在 UI 线程上回调，且能碰控件）
        for (int pump = 0; pump < 40; ++pump) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QFile probe_file(marker);
            if (probe_file.open(QIODevice::ReadOnly)) {
                if (QString::fromUtf8(probe_file.readAll())
                        .contains(QStringLiteral("task_msg_text:"))) {
                    break;
                }
            }
        }
        QString marker_after;
        {
            QFile marker_file(marker);
            if (marker_file.open(QIODevice::ReadOnly)) {
                marker_after = QString::fromUtf8(marker_file.readAll());
            }
        }
        check(marker_after.contains(QStringLiteral("task_msg_thread:1")),
              QStringLiteral("P12c-2 任务壳消息回调不在 UI 线程上（标记=%1）")
                  .arg(marker_after.simplified()));
        check(marker_after.contains(QStringLiteral("task_msg_text:")),
              QStringLiteral("P12c-2 任务壳消息没回到插件回调（标记=%1）")
                  .arg(marker_after.simplified()));
        QStringList labels_cb;
        if (page_widget != nullptr) {
            for (QLabel* item : page_widget->findChildren<QLabel*>()) labels_cb << item->text();
        }
        check(labels_cb.contains(QStringLiteral("回调改的:来自工作线程")),
              QStringLiteral("P12c-2 回调里没改到控件（标签实得：%1）")
                  .arg(labels_cb.join(QStringLiteral(" / "))));
        check(marker_text.contains(QStringLiteral("progress_set:42")),
              QStringLiteral("P12c 进度条设 42 没读回 42（标记=%1）")
                  .arg(marker_text.isEmpty() ? QStringLiteral("(空)") : marker_text.simplified()));
        check(marker_text.contains(QStringLiteral("progress_clamped:100")),
              QStringLiteral("P12c 进度条设 150 没被夹到 100（标记=%1）")
                  .arg(marker_text.isEmpty() ? QStringLiteral("(空)") : marker_text.simplified()));
        QStringList labels_after;
        if (page_widget != nullptr) {
            for (QLabel* item : page_widget->findChildren<QLabel*>()) labels_after << item->text();
        }
        check(labels_after.contains(QStringLiteral("点过了")),
              QStringLiteral("P3 `set_text` 没改到控件（标签实得：%1）")
                  .arg(labels_after.join(QStringLiteral(" / "))));
        const QString hub_log = hub.log_box() != nullptr ? hub.log_box()->toPlainText() : QString();
        check(hub_log.contains(QStringLiteral("门面夹具：按钮被点了")),
              QStringLiteral("P3 `ui.log()` 没进宿主日志栏"));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P3 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P4（**重型 L2 第一批**）：虚拟列表 / 日志视图 / 拖入区域 / 宿主任务壳。
    //   这一块判的是"**宿主组件真的被用上了**"，不是"接口存在"：
    //     ① 虚拟列表：provider 只为**可见行**被调（标记里 `provider_calls` 是个小数字、且
    //        **没有** `row:199`），而按需读第 199 行时它又**真的**能取到（`rowtext199:第 199 行`）；
    //     ② 日志视图：两行追加读得回、清空真的清掉；
    //     ③ 拖入区域：自检**造一个真 QDropEvent** 派发给宿主过滤器（离屏环境没有平台拖放，
    //        所以走 `FileDropFilter::dispatch` —— 过滤逻辑与接线一起被验到），Python 回调收到路径；
    //     ④ 任务壳：Python worker 在**工作线程**里跑，`progress(100)` 真的到了宿主进度条上；
    //        ★而且 worker 里调 `ui.label` **必须被拒**（线程纪律，`worker_thread_ui:True`）。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_uifacade_l2"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("l2_marker.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os, time\n"
            "def register(api):\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'l2_marker.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "    def build():\n"
            "        ui = api.ui\n"
            "        calls = {'n': 0}\n"
            "        rows = ui.list(None, 18)\n"
            "        def provider(row):\n"
            "            calls['n'] += 1\n"
            "            mark('row:%d' % row)\n"
            "            return '第 %d 行' % row\n"
            "        ui.list_set_row_count(rows, 200)\n"
            "        ui.list_set_row_text(rows, provider)\n"
            "        mark('provider_calls:%d' % calls['n'])\n"
            "        mark('rowtext199:%s' % ui.list_row_text(rows, 199))\n"
            "        ui.list_select_row(rows, 7)\n"
            "        mark('selected:%d' % ui.list_selected_row(rows))\n"
            "        mark('single_selected_rows:%s' % ui.list_selected_rows(rows))\n"
            "        ui.list_set_multi(rows, True)\n"
            "        ui.list_set_selected_rows(rows, [5, 1, 9, 1])\n"
            "        mark('multi:%s' % ui.list_selected_rows(rows))\n"
            "        ui.list_set_multi(rows, False)\n"
            "        mark('single_after_off:%s' % ui.list_selected_rows(rows))\n"
            "        mark('selected_after_off:%d' % ui.list_selected_row(rows))\n"
            // ★★ P17-82（接口契约批「本体拖入」，用户 2026-10-09 已拍板要补）：
            //   夹具顺手把"**插件造的 list / tree 收到本机路径**"这条链挂上 ——
            //   宿主在造这两件时就自动装了那份拖放过滤器（`drop_filter.h` 的唯一实现），
            //   这里只登记"收到路径之后干什么"。
            //   ★★ 注意（本批实测踩到、白烧一轮编译）：这一段是**夹具的 Python 源码**
            //     （C++ 字符串字面量逐条拼接），所以**注释也必须写在字符串里面** ——
            //     在字面量之间插一行 C++ 的 `//` 注释会把拼接链**打断**（`source` 只剩前半段），
            //     生成的 `l2_demo.py` 于是语法错、插件加载失败，而 C++ 这边照样编得过。
            "        # 锚着插件造的列表/树：宿主自动装了拖放过滤器，这里登记落下之后干什么\n"
            "        ui.list_on_drop(rows, lambda paths: mark('list_drop:' + ','.join(sorted(paths))))\n"
            "        tree = ui.tree(None, ['列'])\n"
            "        ui.tree_set_root(tree, [{'key': 'r1', 'cells': ['甲']}])\n"
            "        ui.tree_on_drop(tree, lambda paths: mark('tree_drop:' + ','.join(sorted(paths))))\n"
            // ★★ P17-83（接口契约批「`ask_*` 参数缺口」）：1.x 的 `filetypes` / `initialdir` /
            //   `initialfile` 在 2.x 被静默丢掉。**这里不调它们** —— 真实宿主实现会**发真对话框**，
            //   而 `--smoke` 是离屏无人值守（真发会阻塞；且这条链"发不发、发成什么样"在无人环境里
            //   根本观察不到）。⇒ 归**静态判据**：门禁新步 `ask-dialog-params`
            //   （`tests/check_ask_dialog_params.py`）逐项核对
            //   （pybind 收得下三个参数 ＋ 宿主把 `nameFilters`/`directory`/`selectFile` 都真的接上了），
            //   并自带反例自证。这里只留一句说明，免得后人以为"没验过"。
            "        self_list = ui.list_self_scrolling(None, 0)\n"
            "        ui.list_set_row_count(self_list, 200)\n"
            "        ui.list_set_row_text(self_list, lambda row: '自滚行 %d' % row)\n"
            "        mark('selfscroll_can:%d' % int(ui.viewport_can_scroll(self_list)))\n"
            "        mbtn = ui.button(None, '两模式', None)\n"
            "        def on_pick(i):\n"
            "            mark('menu_pick:%d' % i)\n"
            "        mark('menu_ok:%d' % int(ui.button_on_menu(\n"
            "            mbtn, ['选包文件…', '选文件夹…'], on_pick)))\n"            "        mark('content_height:%d' % ui.list_content_height(rows))\n"
            "        view = ui.log_view(None, 4)\n"
            "        ui.log_view_append(view, '第一行')\n"
            "        ui.log_view_append(view, '第二行')\n"
            "        mark('viewtext:%s' % ui.log_view_text(view).replace('\\n', '|'))\n"
            "        ui.log_view_clear(view)\n"
            "        mark('after_clear:[%s]' % ui.log_view_text(view))\n"
            "        zone = ui.drop_zone(None, '把目录拖进来')\n"
            "        ui.drop_zone_on_drop(zone, lambda paths: mark('drop:' + ','.join(sorted(paths))))\n"
            "        panel = ui.task_panel(None, True)\n"
            "        def worker(progress, message):\n"
            "            mark('worker_thread_ui:%s' % (ui.label(None, '不该出现') is None))\n"
            "            message('开始干活')\n"
            "            progress(50)\n"
            "            time.sleep(0.05)\n"
            "            progress(100)\n"
            "            mark('worker_done')\n"
            "        mark('run:%d' % int(ui.task_run(panel, '夹具任务', worker)))\n"
            "    api.register_tool('门面L2夹具页', build, 6)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("l2_demo.py")));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P17-82 调试：L2 夹具不该有插件问题（实得 %1 条）")
                  .arg(hub.plugin_errors().size()));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P4 夹具 .py 没写进去"));

        Hub hub(nullptr, dir);
        const int page = hub.page_index(QStringLiteral("门面L2夹具页"));
        check(page >= 0, QStringLiteral("P4 `register_tool` 的页没进页表（插件问题 %1 条）")
                             .arg(hub.plugin_errors().size()));
        QWidget* page_widget = page >= 0 ? hub.stack()->widget(page) : nullptr;
        if (page_widget != nullptr) page_widget->resize(600, 400);
        QApplication::processEvents();
        const auto read_marker = [&marker]() {
            QFile file(marker);
            if (!file.open(QIODevice::ReadOnly)) return QString();
            return QString::fromUtf8(file.readAll());
        };
        // ④ 任务壳：等工作线程收工（顺带把排队信号抽干）
        for (int i = 0; i < 400 && !read_marker().contains(QStringLiteral("worker_done")); ++i) {
            QApplication::processEvents();
            QThread::msleep(10);
        }
        // ③ 拖入区域：造一个**真 QDropEvent** 派发给宿主过滤器
        QWidget* zone = page_widget != nullptr
                            ? page_widget->findChild<QWidget*>(QStringLiteral("xrkit_facade_drop_zone"))
                            : nullptr;
        // ★ `FileDropFilter` 没有 `Q_OBJECT`（它只装事件过滤器、不需要信号）⇒ 不能用
        //   `findChild<FileDropFilter*>()`（那个模板要求 Q_OBJECT，会 static_assert 编译不过）；
        //   这里顺着控件树用 `dynamic_cast` 找（同一件事，RTTI 就够）。
        FileDropFilter* drop_filter = nullptr;
        if (zone != nullptr) {
            for (QObject* child : zone->children()) {
                if (auto* candidate = dynamic_cast<FileDropFilter*>(child)) {
                    drop_filter = candidate;
                    break;
                }
            }
        }
        bool drop_handled = false;
        if (drop_filter != nullptr && zone != nullptr) {
            QMimeData mime;
            mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.txt")),
                          QUrl::fromLocalFile(QStringLiteral("C:/tmp/b.txt"))});
            QDropEvent drop(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            drop_handled = drop_filter->dispatch(&drop, zone);
        }
        QApplication::processEvents();
        const QString text = read_marker();
        const int calls_at = text.indexOf(QStringLiteral("provider_calls:"));
        const int calls_end = text.indexOf(QLatin1Char('\n'), calls_at);
        const int calls = calls_at < 0
                              ? -1
                              : text.mid(calls_at + 15, calls_end < 0 ? -1 : calls_end - calls_at - 15)
                                    .trimmed()
                                    .toInt();
        check(calls >= 0 && calls <= 8,
              QStringLiteral("P4 虚拟列表的 provider 被调了 %1 次（应当只为可见行取，远小于 200）")
                  .arg(calls));
        // ★★ P16-3a：**多选**（控件下限 #2「多选不许退化成单选」）—— 从夹具的 mark 里读三件事：
        //   ① 程序化设 4 个（含乱序与重复）⇒ 读回**升序去重**的 3 个；
        //   ② 关掉多选 ⇒ **立刻退回纯单选**（选中集合只剩当前行）；
        //   ③ **不回归**：没开多选时旧路（`list_select_row`）读回也只有一行。
        //   ★ 判红办法：宿主 `list_selected_rows` 只返回 `selected_` ⇒ ①红；
        //     忽略 `list_set_multi` 的开关 ⇒ ②红而③仍绿（两处各自可判）。
        auto marker_value = [&text](const QString& key) {
            const int at = text.indexOf(key + QLatin1Char(':'));
            if (at < 0) return QString();
            const int end = text.indexOf(QLatin1Char('\n'), at);
            return text.mid(at + key.size() + 1,
                            end < 0 ? -1 : end - at - key.size() - 1).trimmed();
        };
        check(marker_value(QStringLiteral("single_selected_rows")) == QStringLiteral("[7]"),
              QStringLiteral("P16-3a 不回归：未开多选时旧路读回也只有一行（要 [7]，实得 %1）")
                  .arg(marker_value(QStringLiteral("single_selected_rows"))));
        check(marker_value(QStringLiteral("multi")) == QStringLiteral("[1, 5, 9]"),
              QStringLiteral("P16-3a 多选：设 [5,1,9,1] 要读回**升序去重**的 [1, 5, 9]（实得 %1）")
                  .arg(marker_value(QStringLiteral("multi"))));
        check(marker_value(QStringLiteral("single_after_off")) == QStringLiteral("[1]") &&
                  marker_value(QStringLiteral("selected_after_off")) == QStringLiteral("1"),
              QStringLiteral("P16-3a 关掉多选要立刻退回纯单选（要 [1] / 当前行 1，实得 %1 / %2）")
                  .arg(marker_value(QStringLiteral("single_after_off")))
                  .arg(marker_value(QStringLiteral("selected_after_off"))));
        // ★★ P17-82（接口契约批「本体拖入」，用户 2026-10-09 已拍板要补）：**插件造的
        //   list / tree 也要收得到本机文件** —— 宿主在造这两件时**自动**装上那份唯一的拖放过滤器
        //   （`src/shell/drop_filter.h`；口径与 `drop_zone` 完全一样："本地文件才接、其余给禁止光标"）。
        //   判据（**接线 ＋ 行为**两半，和 drop_zone 那条同型）：
        //     ① 从**插件页**里找回两件控件 ⇒ 它们各自能找到一个 `FileDropFilter`
        //        （`attach()` 会给控件 `setAcceptDrops(true)`，缺了它 Qt 根本不派发拖放）；
        //     ② 造一个**真 `QDropEvent`** 派发给那个过滤器（离屏环境没有平台拖放 ⇒ 与
        //        `drop_zone` 那条一样走 `dispatch`）⇒ **Python 回调收到本机路径**（落进标记文件）。
        //   ★ 判红办法：把 `widget_facade_qt.cpp` 里 `attach_drag_drop(...)` 那两处调用删掉 ⇒ ①红；
        //     把 `list_on_drop`/`tree_on_drop` 的登记（`set_drag_handler`）删掉 ⇒ ②红（mark 不落）。
        {
            auto* list_view = page_widget != nullptr
                                  ? page_widget->findChild<VirtualList*>()
                                  : nullptr;
            auto* tree_view = page_widget != nullptr
                                  ? page_widget->findChild<FacadeTree*>()
                                  : nullptr;
            // ★ `FileDropFilter` 没有 `Q_OBJECT` ⇒ 不能用 `findChild<FileDropFilter*>()`
            //   （那个模板要求 Q_OBJECT，会 static_assert 编译不过）；顺着控件树 `dynamic_cast` 找。
            const auto filter_of = [](QWidget* widget) -> FileDropFilter* {
                if (widget == nullptr) return nullptr;
                for (QObject* child : widget->children()) {
                    if (auto* candidate = dynamic_cast<FileDropFilter*>(child)) return candidate;
                }
                // 列表那一条：过滤器挂在**容器**上（句柄＝容器，列表是它的子件）
                for (QWidget* parent = widget->parentWidget(); parent != nullptr;
                     parent = parent->parentWidget()) {
                    for (QObject* child : parent->children()) {
                        if (auto* candidate = dynamic_cast<FileDropFilter*>(child)) return candidate;
                    }
                }
                return nullptr;
            };
            FileDropFilter* list_filter = filter_of(list_view);
            FileDropFilter* tree_filter = filter_of(tree_view);
            check(list_view != nullptr && list_filter != nullptr &&
                      list_filter->is_attached_to(list_view),
                  QStringLiteral("P17-82 插件造的 list **自动**装了宿主拖放过滤器（列表=%1 过滤器=%2）")
                      .arg(list_view != nullptr ? 1 : 0)
                      .arg(list_filter != nullptr ? 1 : 0));
            check(tree_view != nullptr && tree_filter != nullptr &&
                      tree_filter->is_attached_to(tree_view),
                  QStringLiteral("P17-82 插件造的 tree **自动**装了宿主拖放过滤器（树=%1 过滤器=%2）")
                      .arg(tree_view != nullptr ? 1 : 0)
                      .arg(tree_filter != nullptr ? 1 : 0));
            if (list_filter != nullptr && list_view != nullptr) {
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/tmp/a.txt"))});
                QDropEvent drop(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton,
                                Qt::NoModifier);
                list_filter->dispatch(&drop, list_view);
            }
            if (tree_filter != nullptr && tree_view != nullptr) {
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(QStringLiteral("C:/tmp/c.txt")),
                              QUrl::fromLocalFile(QStringLiteral("C:/tmp/b.txt"))});
                QDropEvent drop(QPointF(2, 2), Qt::CopyAction, &mime, Qt::LeftButton,
                                Qt::NoModifier);
                tree_filter->dispatch(&drop, tree_view);
            }
            QApplication::processEvents();
            const QString after_drop = read_marker();
            const auto drop_value = [&after_drop](const QString& key) {
                const int at = after_drop.indexOf(key + QLatin1Char(':'));
                if (at < 0) return QString();
                const int end = after_drop.indexOf(QLatin1Char('\n'), at);
                return after_drop.mid(at + key.size() + 1,
                                      end < 0 ? -1 : end - at - key.size() - 1).trimmed();
            };
            check(drop_value(QStringLiteral("list_drop")) ==
                      QStringLiteral("C:/tmp/a.txt"),
                  QStringLiteral("P17-82 落到插件 list 上 ⇒ Python 回调收到本机路径（要 C:/tmp/a.txt，实得 %1）")
                      .arg(drop_value(QStringLiteral("list_drop"))));
            check(drop_value(QStringLiteral("tree_drop")) ==
                      QStringLiteral("C:/tmp/b.txt,C:/tmp/c.txt"),
                  QStringLiteral("P17-82 落到插件 tree 上 ⇒ 回调收到**全部**路径（要 "
                                 "C:/tmp/b.txt,C:/tmp/c.txt，实得 %1）")
                      .arg(drop_value(QStringLiteral("tree_drop"))));
            // ★★ P17-83（接口契约批「`ask_*` 参数缺口」）：**这一条刻意不在这里验** —— 见上面
            //   夹具里那段说明（真对话框在离屏无人值守环境里发不得、也观察不到）；参数映射
            //   （过滤串 / 起点目录 / 起点记忆）与 pybind 的签名由门禁 `ask-dialog-params` 步
            //   静态核对（`tests/check_ask_dialog_params.py`，自带反例自证）。
        }
        // ★★ P16-3b-2（控件下限 #3）：**自带滚动的长列表区**（`ui.list_self_scrolling`）——
        //   插件一条调用拿到的就是内置页同款形状。这里从外壳侧验两件**行为**：
        //     ① 拖条子 ⇒ 列表自己滚（条子值经 `set_offset_delegate` 交回 `set_scroll_top`）；
        //     ② 画布几何 == 视口（不挪画布，对照表 9.12）。
        //   判红办法：把宿主那条 `set_offset_delegate` 接线删掉 ⇒ ①红；
        //            把 `set_self_scrolling_body(true)` 去掉 ⇒ ②红。
        {
            ScrollViewport* self_scroll = nullptr;
            if (page_widget != nullptr) {
                for (ScrollViewport* candidate : page_widget->findChildren<ScrollViewport*>()) {
                    if (candidate->self_scrolling_body()) self_scroll = candidate;
                }
            }
            check(self_scroll != nullptr && self_scroll->vbar() != nullptr &&
                      self_scroll->vbar()->maximum() > 0,
                  QStringLiteral("P16-3b-2 插件页里没找到「自带滚动的长列表区」"
                                 "（视口=%1 条子量程=%2）")
                      .arg(self_scroll != nullptr ? 1 : 0)
                      .arg(self_scroll != nullptr && self_scroll->vbar() != nullptr
                               ? self_scroll->vbar()->maximum()
                               : -1));
            if (self_scroll != nullptr && self_scroll->vbar() != nullptr &&
                self_scroll->vbar()->maximum() > 0) {
                auto* list = dynamic_cast<VirtualList*>(self_scroll->body());
                const int target = std::max(1, self_scroll->vbar()->maximum() / 2);
                self_scroll->vbar()->setValue(target);
                self_scroll->sync_layout();
                QApplication::processEvents();
                check(list != nullptr && list->scroll_top() == target,
                      QStringLiteral("P16-3b-2 拖条子要**真的让列表滚**（条子 %1 / 列表 offset %2）")
                          .arg(target)
                          .arg(list != nullptr ? list->scroll_top() : -1));
                const QRect body_geo = list != nullptr ? list->geometry() : QRect();
                const QRect view_rect = self_scroll->rect();
                check(body_geo.x() == 0 && body_geo.y() == 0 &&
                          body_geo.height() <= view_rect.height() &&
                          (list == nullptr || body_geo.height() < list->content_height()),
                      QStringLiteral("P16-3b-2 自带滚动的画布几何必须 == 视口"
                                     "（body=%1x%2@(%3,%4) 视口=%5x%6）")
                          .arg(body_geo.width()).arg(body_geo.height())
                          .arg(body_geo.x()).arg(body_geo.y())
                          .arg(view_rect.width()).arg(view_rect.height()));
            }
        }
        // ★★ P16-3c（控件下限 #1「一个按钮两模式」）：**按钮左键动作菜单** ——
        //   内置 FS 页「加包」是左键弹 QMenu 二选一（`pages/fs_page.cpp:586-597`）；
        //   修前门面只有右键 ⇒ 插件做不出（插件自认在 `plugins/fs_page.py`）。
        //   验三件：`button_on_menu` 返回真、按钮上**真挂了菜单且两项**、触发第 2 项**回调进 mark**。
        {
            QPushButton* two_mode = nullptr;
            if (page_widget != nullptr) {
                for (QPushButton* candidate : page_widget->findChildren<QPushButton*>()) {
                    if (candidate->text() == QStringLiteral("两模式")) two_mode = candidate;
                }
            }
            check(marker_value(QStringLiteral("menu_ok")) == QStringLiteral("1") &&
                      two_mode != nullptr && two_mode->menu() != nullptr &&
                      two_mode->menu()->actions().size() == 2,
                  QStringLiteral("P16-3c 按钮要挂上左键动作菜单、且两项（返回 %1 / 菜单 %2 / 项数 %3）")
                      .arg(marker_value(QStringLiteral("menu_ok")))
                      .arg(two_mode != nullptr && two_mode->menu() != nullptr ? 1 : 0)
                      .arg(two_mode != nullptr && two_mode->menu() != nullptr
                               ? two_mode->menu()->actions().size()
                               : -1));
            if (two_mode != nullptr && two_mode->menu() != nullptr &&
                two_mode->menu()->actions().size() == 2) {
                two_mode->menu()->actions().at(1)->trigger();
                QApplication::processEvents();
                // ★ 触发后必须**重读** mark 文件（上面的 `text` 是早先快照 ⇒ 恒空）
                const QString after_menu = read_marker();
                const int pick_at = after_menu.indexOf(QStringLiteral("menu_pick:"));
                const QString pick =
                    pick_at < 0 ? QString()
                                : after_menu.mid(pick_at + 10).split(QLatin1Char('\n')).first().trimmed();
                check(pick == QStringLiteral("1"),
                      QStringLiteral("P16-3c 选中第 2 项要回调到插件（期望 menu_pick:1，实得 %1）")
                          .arg(pick.isEmpty() ? QStringLiteral("(空)") : pick));
            }
        }
        check(calls_at >= 0 && !text.left(calls_at).contains(QStringLiteral("row:199")),
              QStringLiteral("P4 虚拟列表把没显示的行也取了（第 199 行在 provider 阶段就被问了）"));
        check(text.contains(QStringLiteral("rowtext199:第 199 行")),
              QStringLiteral("P4 按需读第 199 行取不到（provider 没被按需调）"));
        check(text.contains(QStringLiteral("selected:7")),
              QStringLiteral("P4 列表选中行不对（标记里没有 selected:7）"));
        check(text.contains(QStringLiteral("viewtext:第一行|第二行")),
              QStringLiteral("P4 日志视图两行读不回（标记里没有 viewtext:第一行|第二行）"));
        check(text.contains(QStringLiteral("after_clear:[]")),
              QStringLiteral("P4 `log_view_clear` 没清掉内容"));
        check(drop_handled && text.contains(QStringLiteral("drop:C:/tmp/a.txt,C:/tmp/b.txt")),
              QStringLiteral("P4 拖入区域的回调没收到本机路径（handled=%1）").arg(drop_handled));
        check(text.contains(QStringLiteral("run:1")),
              QStringLiteral("P4 `task_run` 没被宿主接受"));
        check(text.contains(QStringLiteral("worker_done")),
              QStringLiteral("P4 任务壳的 worker 没跑完（标记里没有 worker_done）"));
        check(text.contains(QStringLiteral("worker_thread_ui:True")),
              QStringLiteral("P4 **线程纪律失效**：工作线程里调 `ui.label` 没有被拒"));
        QProgressBar* bar = page_widget != nullptr ? page_widget->findChild<QProgressBar*>() : nullptr;
        check(bar != nullptr && bar->value() == 100,
              QStringLiteral("P4 Python worker 的 progress(100) 没到宿主进度条（实得 %1）")
                  .arg(bar == nullptr ? QStringLiteral("无进度条") : QString::number(bar->value())));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P4 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ★★ 2026-10-10（P17-1，审计 05 PLG-01 **High**）：**插件页的「取消」要真能读到、真能停**。
    //
    // 修前事实：`PyUi::available()` 把"非 UI 线程调 ui.*"一刀切拒掉，而**取消谓词是插件 worker
    // 在自己的工作线程里同步轮询的**（引擎的 `should_stop`/`cancel` hook 都在调用线程被调）
    // ⇒ `ui.task_cancel_requested` 恒 False ⇒ 编码转换/文本提取/XML 校对/视频转换四页的
    // 「取消」关不掉、任务照跑到结束，状态栏还谎报"已取消"。
    //
    // 判据（四条一起量，缺一条都不算过）：
    //   ① worker 里**读得到**取消位（不再被线程闸门拒），且**没点取消时是 False**；
    //   ② 点宿主那颗**真「取消」按钮**之后，worker 里的同一个读**变成 True**、循环当场退出；
    //   ③ worker 里碰**会改控件**的 `ui.*`（`set_text`）**仍然被拒**（白名单没把纪律冲掉）；
    //   ④ 取消**真的让任务停下来**（循环次数远小于上限 —— 不是"跑满 600 次刚好退出"）。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_cancel_probe"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "import time\n"
            "\n"
            "\n"
            "def register(api):\n"
            "    ui = api.ui\n"
            "\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'cancel_marker.txt'), 'a',\n"
            "                  encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "\n"
            "    def build():\n"
            "        status = ui.label(None, '就绪')\n"
            "        panel = ui.task_panel(None, True)\n"
            "\n"
            "        def on_message(text):\n"
            "            ui.set_text(status, text)\n"
            "\n"
            "        ui.task_on_message(panel, on_message)\n"
            "\n"
            "        def worker(progress, message):\n"
            "            progress(0)\n"
            "            ui.set_text(status, 'worker-must-not-write')\n"
            "            mark('cancel_read:%d' % int(bool(ui.task_cancel_requested(panel))))\n"
            "            loops = 0\n"
            "            while loops < 2000:\n"
            "                loops += 1\n"
            "                if ui.task_cancel_requested(panel):\n"
            "                    break\n"
            "                time.sleep(0.01)\n"
            "            mark('cancel_after:%d' % int(bool(ui.task_cancel_requested(panel))))\n"
            "            mark('loops:%d' % loops)\n"
            "            message('loops:%d' % loops)\n"
            "            progress(100)\n"
            "\n"
            "        def start():\n"
            "            ui.task_run(panel, '取消探针', worker)\n"
            "\n"
            "        ui.button(None, '起任务', start)\n"
            "\n"
            "    api.register_tool('取消探针页', build, 6)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("cancel_probe.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P17-1 取消探针夹具没写进 %1").arg(QDir::toNativeSeparators(dir)));

        Hub hub(nullptr, dir);
        const int probe_page = hub.page_index(QStringLiteral("取消探针页"));
        check(probe_page >= 0,
              QStringLiteral("P17-1 前提：取消探针页进页表（实得 %1，插件问题 %2 条）")
                  .arg(probe_page)
                  .arg(hub.plugin_errors().size()));
        QWidget* page_widget = probe_page >= 0 ? hub.stack()->widget(probe_page) : nullptr;

        const QString marker = QDir(dir).filePath(QStringLiteral("cancel_marker.txt"));
        const auto read_marker = [&marker]() {
            QFile file(marker);
            if (!file.open(QIODevice::ReadOnly)) return QString();
            const QString text = QString::fromUtf8(file.readAll());
            file.close();
            return text;
        };
        const auto marker_value = [](const QString& text, const QString& key) {
            const int at = text.indexOf(key + QStringLiteral(":"));
            if (at < 0) return QString();
            return text.mid(at + key.size() + 1).split(QLatin1Char('\n')).first().trimmed();
        };
        const auto wait_for_marker = [&](const QString& key, int ms) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < deadline) {
                QApplication::processEvents();
                const QString text = read_marker();
                if (text.contains(key + QStringLiteral(":"))) return text;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return read_marker();
        };

        QPushButton* start_button = nullptr;
        if (page_widget != nullptr) {
            for (QPushButton* candidate : page_widget->findChildren<QPushButton*>()) {
                if (candidate->text() == QStringLiteral("起任务")) start_button = candidate;
            }
        }
        check(start_button != nullptr,
              QStringLiteral("P17-1 前提：找得到取消探针页的「起任务」按钮"));
        if (start_button != nullptr) start_button->click();

        // ① 等 worker 真跑起来（marker 里出现 cancel_read: ⇒ 它已经读过一次取消位）
        QString text = wait_for_marker(QStringLiteral("cancel_read"), 5000);
        const QString cancel_read = marker_value(text, QStringLiteral("cancel_read"));
        check(cancel_read == QStringLiteral("0"),
              QStringLiteral("P17-1 ① worker 里 `ui.task_cancel_requested` **读得到**且未取消时为 False"
                             "（实得 %1；修前被线程闸门拒 ⇒ None/恒假）")
                  .arg(cancel_read.isEmpty() ? QStringLiteral("(没读到)") : cancel_read));

        // ② 点宿主那颗**真「取消」按钮**（整条链：按钮 ⇒ 取消位 ⇒ worker 读得到）
        QPushButton* cancel_button =
            page_widget != nullptr
                ? page_widget->findChild<QPushButton*>(QStringLiteral("task_cancel"))
                : nullptr;
        check(cancel_button != nullptr && cancel_button->isEnabled(),
              QStringLiteral("P17-1 ② 前提：插件任务壳上有可点的「取消」按钮"));
        if (cancel_button != nullptr) cancel_button->click();

        text = wait_for_marker(QStringLiteral("cancel_after"), 5000);
        const QString cancel_after = marker_value(text, QStringLiteral("cancel_after"));
        const QString loops = marker_value(text, QStringLiteral("loops"));
        check(cancel_after == QStringLiteral("1"),
              QStringLiteral("P17-1 ② 点「取消」后 worker 里读到 True（实得 %1；"
                             "修前恒 None ⇒ 取消关不掉、任务照跑到结束还谎报\"已取消\"）")
                  .arg(cancel_after.isEmpty() ? QStringLiteral("(没读到)") : cancel_after));
        check(!loops.isEmpty() && loops.toInt() >= 1 && loops.toInt() < 2000,
              QStringLiteral("P17-1 ④ 取消**真的让任务停下**（轮询上限 2000 次 ⇒ 实得 %1 次；"
                             "跑满上限＝取消没被读到，任务跑到自己结束）")
                  .arg(loops.isEmpty() ? QStringLiteral("(没读到)") : loops));

        // ③ 纪律抽查：worker 里改控件必须被拒（主线程回调写的最后一句话才是控件上的值）
        QLabel* worker_label = nullptr;
        if (page_widget != nullptr) {
            for (QLabel* candidate : page_widget->findChildren<QLabel*>()) {
                if (candidate->text().startsWith(QStringLiteral("loops:"))) worker_label = candidate;
            }
        }
        check(worker_label != nullptr &&
                  worker_label->text() != QStringLiteral("worker-must-not-write"),
              QStringLiteral("P17-1 ③ worker 里调 `ui.set_text`（会改控件）**仍然被拒**"
                             "（标签实得 %1）")
                  .arg(page_widget != nullptr
                           ? [&] {
                                 for (QLabel* candidate : page_widget->findChildren<QLabel*>()) {
                                     if (candidate->text() == QStringLiteral("worker-must-not-write")) {
                                         return QStringLiteral("worker-must-not-write");
                                     }
                                 }
                                 return QStringLiteral("未被改写");
                             }()
                           : QStringLiteral("(没有页)")));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P5（**重型 L2 第二批**）：滚动视口 / 可分栏 / **插件页自动套宿主视口**。
    //   判据不是"接口在"，而是"宿主组件真的在替插件干活"：
    //     ① 插件页被宿主套上了 `ScrollViewport`（与内置六页同形）；
    //     ② 插件用 `ui.viewport` 造的视口**真的能滚**（缩小视口 ⇒ `can_scroll()` 为真；
    //        拖条子 ⇒ **画布真的被挪了**，不只是条子值变了）；
    //     ③ 视口里放长列表 ⇒ 宿主自动把"内容总高"交给视口（`min_body_height >= 行数×行高`）；
    //     ④ 分栏：三格、格子上限、第 3 格完全收起（4.2/4.3）。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_uifacade_l2b"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("l5_marker.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "def register(api):\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'l5_marker.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "    def build():\n"
            "        ui = api.ui\n"
            "        body = ui.viewport(None, 1200)\n"
            "        for i in range(6):\n"
            "            ui.label(body, '长内容第 %d 行' % i)\n"
            "        ui.viewport_sync(body)\n"
            "        mark('vp_can_scroll:%d' % int(ui.viewport_can_scroll(body)))\n"
            "        mark('vp_top:%d' % ui.viewport_scroll_top(body))\n"
            "        body2 = ui.viewport(None, 0)\n"
            "        rows = ui.list(body2, 18)\n"
            "        ui.list_set_row_count(rows, 200)\n"
            "        ui.viewport_sync(body2)\n"
            "        mark('list_vp_can_scroll:%d' % int(ui.viewport_can_scroll(body2)))\n"
            "        split = ui.split(None, True)\n"
            "        for i in range(3):\n"
            "            cell = ui.split_add_pane(split, 40)\n"
            "            ui.label(cell, '格 %d' % i)\n"
            "        mark('panes:%d' % ui.split_pane_count(split))\n"
            "        ui.split_set_max_size(split, 0, 180)\n"
            "        ui.split_set_collapsed(split, 2, True)\n"
            "        mark('collapsed2:%d' % int(ui.split_is_collapsed(split, 2)))\n"
            "        mark('sash0:%d' % ui.split_sash_pos(split, 0))\n"
            "    api.register_tool('门面L2b夹具页', build, 7)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("l5_demo.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P5 夹具 .py 没写进去"));

        Hub hub(nullptr, dir);
        const int page = hub.page_index(QStringLiteral("门面L2b夹具页"));
        check(page >= 0, QStringLiteral("P5 `register_tool` 的页没进页表（插件问题 %1 条）")
                             .arg(hub.plugin_errors().size()));
        QWidget* page_widget = page >= 0 ? hub.stack()->widget(page) : nullptr;
        // ★ 分栏的尺寸/分隔条位置**必须真的布局过**才读得出来 —— 与别处一样把 Hub `show()` 一次
        //   （离屏平台下 `hub.show()` 是既有做法；不 show 的话 `QSplitter::sizes()` 恒为 [0,0,0]，
        //   那样判的是"布局没跑"而不是"分栏不对" —— 本批实测踩到过）。
        hub.show();
        // ★ 还要**切到这一页**：`QStackedWidget` 只给"当前页"派发布局 ⇒ 不切的话插件页里的
        //   分栏/视口拿不到几何（实测：`QSplitter::sizes()` 恒为 [0,0,0]）。
        if (page >= 0) hub.switch_to_page(QStringLiteral("门面L2b夹具页"));
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        // ① 插件页由宿主套上**外壳**（P16-2c：与内置六页同一个 `PageBase` 外壳 ——
        //    `PageBase` 构造里自带 `ScrollViewport(Fit::Content)` ＋ header ＋ action_bar）。
        //   ★ 修前宿主套的是**裸** `ScrollViewport`、页控件本人就是视口，所以这条判据写成
        //     "页控件必须 dynamic_cast 成 ScrollViewport"；口径升级后页控件是 `PageBase`，
        //     判据随之量**它的自带视口**（更严：还量 `minsize_ok()`）。**没有放宽。**
        auto* page_base = dynamic_cast<PageBase*>(page_widget);
        if (page_base != nullptr) {
            check(page_base->viewport() != nullptr &&
                      page_base->viewport()->fit() == ScrollViewport::Fit::Content &&
                      page_base->minsize_ok(),
                  QStringLiteral("P5 插件页的宿主外壳不对（自带视口 fit/minsize 没接上）"));
        } else {
            check(dynamic_cast<ScrollViewport*>(page_widget) != nullptr,
                  QStringLiteral("P5 插件页没被套上宿主视口（长内容会滚不动）"));
        }
        // ② 插件自己造的视口：缩小它 ⇒ 能滚；拖条子 ⇒ 画布真的挪了
        //   ★ 先把**宿主外壳那一层**剔掉（P16-2c 起它也会出现在 `findChildren` 里），
        //     剩下的才对应修前 `inner.at(0)/at(1)` 的语义（插件自己的两个视口）。
        QList<ScrollViewport*> inner;
        if (page_widget != nullptr) {
            for (ScrollViewport* candidate : page_widget->findChildren<ScrollViewport*>()) {
                if (page_base != nullptr && candidate == page_base->viewport()) continue;
                inner.append(candidate);
            }
        }
        check(inner.size() >= 2, QStringLiteral("P5 插件造的两个视口没找到（实得 %1 个）")
                                     .arg(inner.size()));
        if (inner.size() >= 1) {
            ScrollViewport* viewport = inner.at(0);
            viewport->resize(300, 120);
            viewport->sync_layout();
            check(viewport->can_scroll_vertical(),
                  QStringLiteral("P5 长内容的视口滚不动（min_body_height=%1 视口高=%2）")
                      .arg(viewport->min_body_height())
                      .arg(viewport->height()));
            const int before = viewport->body() != nullptr ? viewport->body()->y() : 0;
            if (viewport->vbar() != nullptr) viewport->vbar()->setValue(50);
            QApplication::processEvents();
            const int after = viewport->body() != nullptr ? viewport->body()->y() : 0;
            check(viewport->offset_vertical() > 0 && after < before,
                  QStringLiteral("P5 拖条子没挪画布（offset=%1 画布 y：%2 → %3）")
                      .arg(viewport->offset_vertical())
                      .arg(before)
                      .arg(after));
        }
        // ③ 视口里的长列表：宿主自动把内容总高交给视口
        if (inner.size() >= 2) {
            check(inner.at(1)->min_body_height() >= 200 * 18,
                  QStringLiteral("P5 长列表没把内容总高交给视口（min_body_height=%1，期望 ≥ %2）")
                      .arg(inner.at(1)->min_body_height())
                      .arg(200 * 18));
        }
        // ④ 分栏（产品用的是 `QSplitter`；`SplitPane` 只作别处的自检夹具 —— 门禁 3-8 锁着）
        auto* split = page_widget != nullptr
                          ? page_widget->findChild<QSplitter*>(QStringLiteral("xrkit_facade_split"))
                          : nullptr;
        check(split != nullptr && split->count() == 3,
              QStringLiteral("P5 分栏格数不对（实得 %1）")
                  .arg(split == nullptr ? -1 : split->count()));
        if (split != nullptr) {
            check(split->count() == 3 && split->orientation() == Qt::Horizontal,
                  QStringLiteral("P5 分栏形态不对（count=%1 orientation=%2）")
                      .arg(split->count())
                      .arg(split->orientation() == Qt::Horizontal ? QStringLiteral("横")
                                                                  : QStringLiteral("纵")));
            // 分隔条真的落下了（Hub 已 show ⇒ 这一读是有意义的几何，不是"布局没跑"）
            const QList<int> sizes = split->sizes();
            QStringList size_text;
            for (int value : sizes) size_text << QString::number(value);
            check(!sizes.isEmpty() && sizes.at(0) > 0,
                  QStringLiteral("P5 分隔条没落下（sizes=[%1]）")
                      .arg(size_text.join(QStringLiteral(", "))));
            check(split->widget(2) != nullptr && split->widget(2)->isHidden(),
                  QStringLiteral("P5 第 3 格没收起（`split_set_collapsed` 没生效）"));
        }
        QString text;
        {
            QFile file(marker);
            if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll());
        }
        check(text.contains(QStringLiteral("vp_can_scroll:")),
              QStringLiteral("P5 `viewport_can_scroll` 没回值（标记=%1）").arg(text.simplified()));
        check(text.contains(QStringLiteral("panes:3")),
              QStringLiteral("P5 插件侧读到的格数不是 3（标记=%1）").arg(text.simplified()));
        // 分隔条位置：**插件侧**读到过（`sash0:` 键在）；真实几何那条在 C++ 侧判（Hub 已 show）。
        //   为什么不在插件侧判"位置 > 0"：`build()` 跑在布局之前，那时读到的必然是 0 ——
        //   拿它判红就是"判据没有能失败的输入"的反面（这次是"判据的输入天生为 0"）。
        check(text.contains(QStringLiteral("sash0:")),
              QStringLiteral("P5 插件侧取不到分隔条位置（标记=%1）").arg(text.simplified()));
        check(text.contains(QStringLiteral("collapsed2:1")),
              QStringLiteral("P5 插件侧读到的收起状态不对（标记=%1）").arg(text.simplified()));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P5 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P6（**重型 L2 第三批：树**）：多列 / 懒建 / 展开态 / 三态勾选 / 过滤计数 / 激活。
    //   判据走**用户路径**（C++ 侧直接改 QTreeWidgetItem 的展开与勾选状态 = 用户点出来的那条路），
    //   再用插件自己的"查状态"按钮把 `api.ui.tree_*` 读到的值写成快照，两边对得上才算通。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_uifacade_l6"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("l6_marker.txt"));
        const QString status = QDir(dir).filePath(QStringLiteral("l6_status.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "def register(api):\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'l6_marker.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "    def build():\n"
            "        ui = api.ui\n"
            "        import os as _os, tempfile as _tempfile\n"
            "        tree = ui.tree(None, [['名字', 0], ['大小', 80]])\n"
            "        def status():\n"
            "            lines = ['rows:%d' % ui.tree_row_count(tree),\n"
            "                     'keys:%s' % ','.join(ui.tree_keys(tree)),\n"
            "                     'checked:%s' % ','.join(sorted(ui.tree_checked_keys(tree))),\n"
            "                     'partial:%s' % ','.join(sorted(ui.tree_partial_keys(tree))),\n"
            "                     'hits:%d' % ui.tree_filter_hits(tree),\n"
            "                     'selected:%s' % ui.tree_selected_key(tree),\n"
            "                     'expanded:%s' % ','.join(sorted(ui.tree_expanded_keys(tree))),\n"
            "                     'selected_multi:%s' % ','.join(ui.tree_selected_keys(tree))]\n"
            "            with open(os.path.join(api.plugin_dir, 'l6_status.txt'), 'w', encoding='utf-8') as fh:\n"
            "                fh.write('\\n'.join(lines) + '\\n')\n"
            "        ui.tree_set_root(tree, [\n"
            "            {'key': 'A', 'cells': ['A 目录', '-'], 'has_children': True, 'checkable': True},\n"
            "            {'key': 'B', 'cells': ['B 文件', '10'], 'checkable': True},\n"
            "        ])\n"
            "        ui.tree_on_expand(tree, lambda key: (\n"
            "            mark('expand:' + key),\n"
            "            ui.tree_set_children(tree, key, [\n"
            "                {'key': 'a1', 'cells': ['a1.txt', '1'], 'checkable': True},\n"
            "                {'key': 'a2', 'cells': ['a2.txt', '2'], 'checkable': True},\n"
            "                {'key': 'a3', 'cells': ['a3.txt', '3'], 'checkable': True},\n"
            "            ])))\n"
            "        ui.tree_on_check(tree, lambda key, checked: mark('check:%s:%d' % (key, int(checked))))\n"
            "        ui.tree_on_activate(tree, lambda key: mark('activate:' + key))\n"
            "        # ★★ P13-1 第二步：只验注册路径（这棵树的行都没有 has_action）\n"
            "        ui.tree_on_action(tree, '✕', lambda key: mark('rowaction:' + key))\n"
            "        # ★★ P15-1a：标签页（两标签 + 每页容器 + 切页）\n"
            "        tabs = ui.tabs(None)\n"
            "        tab_unpack = ui.tabs_add_page(tabs, '解包')\n"
            "        tab_pack = ui.tabs_add_page(tabs, '封包')\n"
            "        ui.label(tab_unpack, '页一标签')\n"
            "        tabs_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_tabs.txt')\n"
            "        def write_tabs_state():\n"
            "            with open(tabs_path, 'w', encoding='utf-8') as handle:\n"
            "                handle.write('%d,%d' % (ui.tabs_page_count(tabs), ui.tabs_current_index(tabs)))\n"
            "        ui.button(tab_pack, '页二按钮', None)\n"
            "        ui.button(None, '切到第二页', lambda: (ui.tabs_set_current(tabs, 1), write_tabs_state()))\n"
            "        write_tabs_state()\n"
            "        # ★★ P15-0：点行身体 + 输入变化回调\n"
            "        click_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_treeclick.txt')\n"
            "        def on_tree_click(key):\n"
            "            with open(click_path, 'a', encoding='utf-8') as handle:\n"
            "                handle.write(key + chr(10))\n"
            "        ui.tree_on_click(tree, on_tree_click)\n"
            "        change_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_entrychange.txt')\n"
            "        def on_entry_change(text):\n"
            "            with open(change_path, 'a', encoding='utf-8') as handle:\n"
            "                handle.write(text + chr(10))\n"
            "        probe_entry = ui.entry(None, '初始', 120)\n"
            "        ui.entry_on_change(probe_entry, on_entry_change)\n"
            "        status()\n"
            "        btn = ui.button(None, '查状态', status)\n"
            "        btn2 = ui.button(None, '过滤 a2', lambda: (ui.tree_set_filter(tree, 'a2'), status()))\n"
            "        btn3 = ui.button(None, '选中 B', lambda: (ui.tree_select(tree, 'B'), status()))\n"
            "        # ★★ P13-1 第三步：带 has_action 的独立树（先不点它）\n"
            "        action_tree = ui.tree(None, [['名字', 0], ['大小', 80]])\n"
            "        ui.tree_set_root(action_tree, [\n"
            "            {'key': 'ra1', 'cells': ['a1', '1'], 'has_action': True},\n"
            "            {'key': 'ra2', 'cells': ['a2', '2'], 'has_action': True},\n"
            "            {'key': 'rdir', 'cells': ['目录', '']},\n"
            "        ])\n"
            "        args_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_rowaction.txt')\n"
            "        def on_row_action(key):\n"
            "            # ★ 纯 Python 写文件：**不**再进 C++ 的 mark()（上一版崩点就在这条再入链）\n"
            "            with open(args_path, 'a', encoding='utf-8') as handle:\n"
            "                handle.write(key + chr(10))\n"
            "        ui.tree_on_action(action_tree, '✕', on_row_action)\n"
            "        ctx_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_context.txt')\n"
            "        def on_ctx_choose(label):\n"
            "            with open(ctx_path, 'a', encoding='utf-8') as handle:\n"
            "                handle.write(label + chr(10))\n"
            "        ui.context_menu(action_tree, ['甲', '乙'], on_ctx_choose)\n"
            "        # ★★ P13-2b：列表激活（双击/回车）\n"
            "        act_list = ui.list(None, 18)\n"
            "        ui.list_set_row_count(act_list, 40)\n"
            "        ui.list_set_row_text(act_list, lambda row: '行' + str(row))\n"
            "        act_path = _os.path.join(_tempfile.gettempdir(), 'xrkit_smoke_listact.txt')\n"
            "        def on_list_activate(row):\n"
            "            with open(act_path, 'a', encoding='utf-8') as handle:\n"
            "                handle.write(str(row) + chr(10))\n"
            "        ui.list_on_activate(act_list, on_list_activate)\n"
            "        mark('built')\n"
            "    api.register_tool('门面树夹具页', build, 8)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("l6_demo.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P6 夹具 .py 没写进去"));

        Hub hub(nullptr, dir);
        const int page = hub.page_index(QStringLiteral("门面树夹具页"));
        check(page >= 0, QStringLiteral("P6 `register_tool` 的页没进页表（插件问题 %1 条）")
                             .arg(hub.plugin_errors().size()));
        QWidget* page_widget = page >= 0 ? hub.stack()->widget(page) : nullptr;
        hub.show();
        if (page >= 0) hub.switch_to_page(QStringLiteral("门面树夹具页"));
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        auto* tree = page_widget != nullptr
                         ? page_widget->findChild<QTreeWidget*>()
                         : nullptr;
        check(tree != nullptr && tree->columnCount() == 2,
              QStringLiteral("P6 树没造出来或列数不对（实得 %1 列）")
                  .arg(tree == nullptr ? -1 : tree->columnCount()));

        // ★★ P15-0：① 多选读取（按树序）② 点行身体 ③ 输入变化回调
        // ★★ P15-1a：标签页（两标签 + 每页容器 + 切页）
        {
            QTabWidget* tabs = page_widget != nullptr
                                   ? page_widget->findChild<QTabWidget*>()
                                   : nullptr;
            check(tabs != nullptr && tabs->count() == 2,
                  QStringLiteral("P15-1a 标签页没造出来或页数不对（实得 %1）")
                      .arg(tabs == nullptr ? -1 : tabs->count()));
            if (tabs != nullptr) {
                check(tabs->tabText(0) == QStringLiteral("解包")
                          && tabs->tabText(1) == QStringLiteral("封包"),
                      QStringLiteral("P15-1a 两个标签的字不对（实得 [%1][%2]）")
                          .arg(tabs->tabText(0), tabs->tabText(1)));
                // ② 塞进第 2 页的控件**真是那一页的后代**
                QPushButton* page2_button = nullptr;
                for (QPushButton* candidate : page_widget->findChildren<QPushButton*>()) {
                    if (candidate->text() == QStringLiteral("页二按钮")) page2_button = candidate;
                }
                check(page2_button != nullptr && tabs->widget(1) != nullptr
                          && tabs->widget(1)->isAncestorOf(page2_button),
                      QStringLiteral("P15-1a 塞进第 2 页的控件不在那一页的容器里"));
                // ③ 夹具首读
                const QString tabs_path =
                    QDir::temp().filePath(QStringLiteral("xrkit_smoke_tabs.txt"));
                QString state;
                {
                    QFile file(tabs_path);
                    if (file.open(QIODevice::ReadOnly)) {
                        state = QString::fromUtf8(file.readAll()).trimmed();
                    }
                }
                check(state == QStringLiteral("2,0"),
                      QStringLiteral("P15-1a 初始页数/当前页不对（实得 [%1]，期望 2,0）").arg(state));
                // ④ 切页：合成点击夹具按钮 ⇒ Qt 侧与 Python 侧都变
                QPushButton* switch_button = nullptr;
                for (QPushButton* candidate : page_widget->findChildren<QPushButton*>()) {
                    if (candidate->text() == QStringLiteral("切到第二页")) switch_button = candidate;
                }
                check(switch_button != nullptr, QStringLiteral("P15-1a 没找到「切到第二页」按钮"));
                if (switch_button != nullptr) switch_button->click();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QString after;
                {
                    QFile file(tabs_path);
                    if (file.open(QIODevice::ReadOnly)) {
                        after = QString::fromUtf8(file.readAll()).trimmed();
                    }
                }
                check(tabs->currentIndex() == 1 && after == QStringLiteral("2,1"),
                      QStringLiteral("P15-1a 切页没生效（Qt=%1 / Python=[%2]，期望 1 / 2,1）")
                          .arg(tabs->currentIndex()).arg(after));
            }
        }
        {
            // ① 选中两行（直接改 `QTreeWidgetItem` 的选中态＝用户点出来的那条路）
            FacadeTree* multi_tree = qobject_cast<FacadeTree*>(tree);
            check(multi_tree != nullptr, QStringLiteral("P15-0 门面树句柄转不过去"));
            if (multi_tree != nullptr) {
                multi_tree->clearSelection();
                if (QTreeWidgetItem* row_a = multi_tree->topLevelItem(0)) row_a->setSelected(true);
                if (QTreeWidgetItem* row_b = multi_tree->topLevelItem(1)) row_b->setSelected(true);
                QPushButton* status_button = nullptr;
                for (QPushButton* candidate : page_widget->findChildren<QPushButton*>()) {
                    if (candidate->text() == QStringLiteral("查状态")) status_button = candidate;
                }
                check(status_button != nullptr, QStringLiteral("P15-0 没找到「查状态」按钮"));
                if (status_button != nullptr) status_button->click();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QString status_text;
                QFile status_file(QDir(dir).filePath(QStringLiteral("l6_status.txt")));
                if (status_file.open(QIODevice::ReadOnly)) {
                    status_text = QString::fromUtf8(status_file.readAll());
                }
                check(status_text.contains(QStringLiteral("selected_multi:A,B")),
                      QStringLiteral("P15-0 多选读取没按树序给全（l6_status 里没有 "
                                     "selected_multi:A,B）"));

                // ② 点行身体：合成 itemClicked（＝用户单击那一行）
                const QString click_path =
                    QDir::temp().filePath(QStringLiteral("xrkit_smoke_treeclick.txt"));
                QFile::remove(click_path);
                if (QTreeWidgetItem* row_b = multi_tree->topLevelItem(1)) {
                    QMetaObject::invokeMethod(multi_tree, "itemClicked", Qt::DirectConnection,
                                              Q_ARG(QTreeWidgetItem*, row_b), Q_ARG(int, 0));
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QString clicked;
                QFile click_file(click_path);
                if (click_file.open(QIODevice::ReadOnly)) {
                    clicked = QString::fromUtf8(click_file.readAll()).trimmed();
                }
                check(clicked == QStringLiteral("B"),
                      QStringLiteral("P15-0 点行身体没把行 key 交给插件（实得 [%1]）").arg(clicked));

                // ③ 输入变化：`setText` 会触发 textChanged
                const QString change_path =
                    QDir::temp().filePath(QStringLiteral("xrkit_smoke_entrychange.txt"));
                QFile::remove(change_path);
                QLineEdit* probe = nullptr;
                for (QLineEdit* candidate : page_widget->findChildren<QLineEdit*>()) {
                    if (candidate->text() == QStringLiteral("初始")) probe = candidate;
                }
                check(probe != nullptr, QStringLiteral("P15-0 没找到夹具里的输入框"));
                if (probe != nullptr) probe->setText(QStringLiteral("abc"));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QString changed;
                QFile change_file(change_path);
                if (change_file.open(QIODevice::ReadOnly)) {
                    changed = QString::fromUtf8(change_file.readAll()).trimmed();
                }
                check(changed == QStringLiteral("abc"),
                      QStringLiteral("P15-0 输入变化回调没拿到当前文本（实得 [%1]）").arg(changed));
            }
        }

        // ★★ P13-1a：行尾动作（fs 页的 ✕）—— 合成点击（与 fs 页自检同一手法）
        {
            FacadeTree* action_tree = nullptr;
            if (page_widget != nullptr) {
                for (FacadeTree* candidate : page_widget->findChildren<FacadeTree*>()) {
                    QTreeWidgetItem* first = candidate->topLevelItem(0);
                    if (first != nullptr && FacadeTree::key_of(first) == "ra1") {
                        action_tree = candidate;
                        break;
                    }
                }
            }
            check(action_tree != nullptr, QStringLiteral("P13-1a 没找到带 ra1 的门面树"));
            const QString action_log_path =
                QDir::temp().filePath(QStringLiteral("xrkit_smoke_rowaction.txt"));
            QFile::remove(action_log_path);
            QTreeWidgetItem* first_row =
                action_tree != nullptr ? action_tree->topLevelItem(0) : nullptr;
            // ★ 手搓一格（不依赖真实布局：树没显示时 `visualItemRect` 是空矩形 ⇒ 命不中）
            QRect cell(0, 0, 300, 20);
            QStyleOptionViewItem option;
            option.rect = cell;
            auto* row_delegate = dynamic_cast<RowActionDelegate*>(
                action_tree != nullptr ? action_tree->itemDelegateForColumn(1) : nullptr);
            check(row_delegate != nullptr, QStringLiteral("P13-1a 末列没装上共用委托"));
            const QRect box = row_delegate != nullptr ? row_delegate->action_rect(option) : QRect();
            const QPoint inside = box.center();
            const QPoint outside(cell.left() + 2, cell.center().y());   // 同一格、但不在方块内
            QMouseEvent press_inside(QEvent::MouseButtonRelease, inside, inside, inside,
                                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            const bool eaten = row_delegate != nullptr &&
                row_delegate->editorEvent(&press_inside, action_tree->model(), option,
                                          action_tree->model()->index(0, 1));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QString fired;
            {
                QFile action_log(action_log_path);
                if (action_log.open(QIODevice::ReadOnly)) {
                    fired = QString::fromUtf8(action_log.readAll()).trimmed();
                }
            }
            const bool outside_fired = row_delegate != nullptr &&
                row_delegate->editorEvent(
                    new QMouseEvent(QEvent::MouseButtonRelease, outside, outside, outside,
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier),
                    action_tree->model(), option, action_tree->model()->index(0, 1));
            const bool dir_fired = row_delegate != nullptr &&
                row_delegate->editorEvent(
                    new QMouseEvent(QEvent::MouseButtonRelease, inside, inside, inside,
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier),
                    action_tree->model(), option, action_tree->model()->index(2, 1));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QString fired_after;
            {
                QFile action_log(action_log_path);
                if (action_log.open(QIODevice::ReadOnly)) {
                    fired_after = QString::fromUtf8(action_log.readAll()).trimmed();
                }
            }
            check(eaten, QStringLiteral("P13-1a 行尾动作的点击没被委托吃掉（会退化成「点了一下整行」）"));
            check(fired == QStringLiteral("ra1"),
                  QStringLiteral("P13-1a 行尾动作没把行 key 交给插件（记录=%1）")
                      .arg(fired.isEmpty() ? QStringLiteral("(空)") : fired));
            check(!outside_fired && fired_after == fired,
                  QStringLiteral("P13-1a 点在方块外也触发了行尾动作（记录=%1）").arg(fired_after));
            check(!dir_fired, QStringLiteral("P13-1a 没有动作的行也触发了行尾动作"));
        }

        // ★★ P13-2a：右键菜单（宿主渲染）—— 合成右键 + `activePopupWidget` 抓菜单
        {
            const QString ctx_path =
                QDir::temp().filePath(QStringLiteral("xrkit_smoke_context.txt"));
            QFile::remove(ctx_path);
            QStringList seen;
            QTimer::singleShot(0, [&seen, &ctx_path]() {
                auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (popup == nullptr) return;
                for (QAction* act : popup->actions()) seen << act->text();
                for (QAction* act : popup->actions()) {
                    if (act->text() == QStringLiteral("甲")) act->trigger();
                }
                popup->close();   // 退出 `exec()` 的嵌套事件循环
            });
            // 自己找一次（上一块的声明在块内，出了块就看不到了）
            FacadeTree* action_tree = nullptr;
            if (page_widget != nullptr) {
                for (FacadeTree* candidate : page_widget->findChildren<FacadeTree*>()) {
                    QTreeWidgetItem* first = candidate->topLevelItem(0);
                    if (first != nullptr && FacadeTree::key_of(first) == "ra1") {
                        action_tree = candidate;
                        break;
                    }
                }
            }
            if (action_tree != nullptr) {
                emit action_tree->customContextMenuRequested(QPoint(6, 6));
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QString chosen;
            {
                QFile ctx_log(ctx_path);
                if (ctx_log.open(QIODevice::ReadOnly)) {
                    chosen = QString::fromUtf8(ctx_log.readAll()).trimmed();
                }
            }
            check(seen.contains(QStringLiteral("甲")) && seen.contains(QStringLiteral("乙")),
                  QStringLiteral("P13-2a 右键菜单里的标签不对（实得 [%1]）")
                      .arg(seen.join(QStringLiteral("/"))));
            check(chosen == QStringLiteral("甲"),
                  QStringLiteral("P13-2a 选中项没把标签原文回传插件（实得 [%1]）").arg(chosen));
        }

        // ★★ P13-2a-2：插件页门面菜单的**正向承诺** ——
        //   注册表只认内置六页那几个 host（`Registry::bad_slot` → `known_host`）⇒
        //   无 `replaces` 的插件页**本就不该**有别的插件的 context 命令；
        //   而"无注册即无痕迹"（1.x）意味着**连分隔符都不许加**、菜单里只有插件自己的标签。
        {
            QStringList seen_only;
            QTimer::singleShot(0, [&seen_only]() {
                auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (popup == nullptr) return;
                for (QAction* act : popup->actions()) seen_only << act->text();
                popup->close();
            });
            FacadeTree* only_tree = nullptr;
            if (page_widget != nullptr) {
                for (FacadeTree* candidate : page_widget->findChildren<FacadeTree*>()) {
                    QTreeWidgetItem* first = candidate->topLevelItem(0);
                    if (first != nullptr && FacadeTree::key_of(first) == "ra1") {
                        only_tree = candidate;
                        break;
                    }
                }
            }
            if (only_tree != nullptr) {
                emit only_tree->customContextMenuRequested(QPoint(9, 9));
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            check(seen_only == QStringList({QStringLiteral("甲"), QStringLiteral("乙")}),
                  QStringLiteral("P13-2a-2 插件页菜单里混进了别的项（实得 [%1]）")
                      .arg(seen_only.join(QStringLiteral("/"))));
        }

        // ★★ P13-2b：列表激活（双击 / 回车）—— **带滚动偏移**的合成事件
        {
            const QString act_path =
                QDir::temp().filePath(QStringLiteral("xrkit_smoke_listact.txt"));
            QFile::remove(act_path);
            VirtualList* act_view = nullptr;
            if (page_widget != nullptr) {
                for (VirtualList* candidate : page_widget->findChildren<VirtualList*>()) {
                    if (candidate->row_count() == 40) {
                        act_view = candidate;
                        break;
                    }
                }
            }
            check(act_view != nullptr, QStringLiteral("P13-2b 没找到 40 行的门面列表"));
            if (act_view != nullptr) {
                // ★ 键盘事件只发给**焦点控件**，而不可见控件拿不到焦点
                //   ⇒ 先把这一页切到当前（自检里的一次"真导航"）
                for (int i = 0; i < hub.page_count(); ++i) {
                    if (hub.stack()->widget(i) == page_widget) {
                        hub.stack()->setCurrentIndex(i);
                        break;
                    }
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                const int row_h = act_view->row_height();
                // 先滚到第 5 行 ⇒ 双击 y=2×行高 处应当命中 **7**
                act_view->set_scroll_top(row_h * 5);
                const QPointF at(6.0, static_cast<qreal>(row_h * 2 + 1));
                QMouseEvent double_click(QEvent::MouseButtonDblClick, at, at, at,
                                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(act_view, &double_click);
                // ★ 回车那条**不判**（见文件头/批报告：离屏自检里键盘事件
                //   送不到目标控件；`QWidget::event` 是 protected，QtTest 没链）
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            }
            QStringList fired;
            {
                QFile log(act_path);
                if (log.open(QIODevice::ReadOnly)) {
                    const QString all = QString::fromUtf8(log.readAll()).trimmed();
                    if (!all.isEmpty()) fired = all.split(QChar('\n'));
                }
            }
            check(fired == QStringList({QStringLiteral("7")}),
                  QStringLiteral("P13-2b 双击激活没拿到正确行号（滚到第 5 行后点视口第 2 行应=7；"
                                 "实得 [%1]）").arg(fired.join(QStringLiteral("/"))));
        }
        const auto read_file = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return QString();
            return QString::fromUtf8(file.readAll());
        };
        const auto click_button = [&page_widget](const QString& text) {
            if (page_widget == nullptr) return false;
            for (QPushButton* button : page_widget->findChildren<QPushButton*>()) {
                if (button->text() == text) {
                    button->click();
                    QApplication::processEvents();
                    return true;
                }
            }
            return false;
        };
        const auto item_by_key = [&tree](const QString& key) -> QTreeWidgetItem* {
            if (tree == nullptr) return nullptr;
            for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                if ((*it)->data(0, Qt::UserRole).toString() == key) return *it;
            }
            return nullptr;
        };
        if (tree != nullptr) {
            // ① 结构：根两行
            check(click_button(QStringLiteral("查状态")), QStringLiteral("P6 找不到「查状态」按钮"));
            QString snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("rows:2")) &&
                      snapshot.contains(QStringLiteral("keys:A,B")),
                  QStringLiteral("P6 初始结构不对（%1）").arg(snapshot.simplified()));
            // ② 懒建：**用户路径**展开 A ⇒ 才回调 on_expand ⇒ 插件补子
            if (auto* node_a = item_by_key(QStringLiteral("A"))) node_a->setExpanded(true);
            QApplication::processEvents();
            click_button(QStringLiteral("查状态"));
            snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("rows:5")) &&
                      snapshot.contains(QStringLiteral("keys:A,a1,a2,a3,B")),
                  QStringLiteral("P6 懒建没生效（%1）").arg(snapshot.simplified()));
            check(read_file(marker).contains(QStringLiteral("expand:A")),
                  QStringLiteral("P6 展开回调没回到 Python"));
            // ③ 三态：勾父 ⇒ 子全跟；再取消一个子 ⇒ 父变半态（**用户路径**改状态）
            if (auto* node_a = item_by_key(QStringLiteral("A"))) {
                node_a->setCheckState(0, Qt::Checked);
            }
            QApplication::processEvents();
            click_button(QStringLiteral("查状态"));
            snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("checked:A,a1,a2,a3")),
                  QStringLiteral("P6 勾父没向下传播（%1）").arg(snapshot.simplified()));
            check(read_file(marker).contains(QStringLiteral("check:A:1")),
                  QStringLiteral("P6 勾选回调没回到 Python"));
            if (auto* node_a1 = item_by_key(QStringLiteral("a1"))) {
                node_a1->setCheckState(0, Qt::Unchecked);
            }
            QApplication::processEvents();
            click_button(QStringLiteral("查状态"));
            snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("partial:A")) &&
                      snapshot.contains(QStringLiteral("checked:a2,a3")),
                  QStringLiteral("P6 三态没向上重算（%1）").arg(snapshot.simplified()));
            // ④ 选中（**放在过滤之前**：过滤会把别的行藏起来，藏着的行不该被"选中"）
            check(click_button(QStringLiteral("选中 B")), QStringLiteral("P6 找不到「选中 B」按钮"));
            snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("selected:B")),
                  QStringLiteral("P6 选中没反映到门面（%1）").arg(snapshot.simplified()));
            // ⑤ 过滤：命中行 + 它的祖先可见，命中数与界面同源
            check(click_button(QStringLiteral("过滤 a2")), QStringLiteral("P6 找不到「过滤 a2」按钮"));
            snapshot = read_file(status);
            check(snapshot.contains(QStringLiteral("hits:1")) &&
                      snapshot.contains(QStringLiteral("keys:A,a2")),
                  QStringLiteral("P6 过滤/命中数不对（%1）").arg(snapshot.simplified()));
            // ⑥ 激活：**真派发** `itemDoubleClicked`（叶子 ⇒ 回调回 Python）
            if (auto* node_a3 = item_by_key(QStringLiteral("a3"))) {
                QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection,
                                          Q_ARG(QTreeWidgetItem*, node_a3), Q_ARG(int, 0));
            }
            QApplication::processEvents();
            check(read_file(marker).contains(QStringLiteral("activate:a3")),
                  QStringLiteral("P6 双击叶子没触发激活回调（标记=%1）")
                      .arg(read_file(marker).simplified()));
            check(snapshot.contains(QStringLiteral("expanded:A")),
                  QStringLiteral("P6 展开态没被门面读到（%1）").arg(snapshot.simplified()));
        }
        check(hub.plugin_errors().empty(),
              QStringLiteral("P6 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P7a（**引擎调用门面**，方案 3）：GUI 宿主这条路上，插件**也能**调引擎。
    //   判据：插件页在 `build()` 里调 `api.engine.font_size_slots()` / `font_state_marks()`，
    //   把结果写进标记 —— 值必须来自引擎（10 档槽位 + 引擎给的中文文案），不是插件自己编的。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_engine"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QString marker = QDir(dir).filePath(QStringLiteral("engine_marker.txt"));
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "import os\n"
            "def register(api):\n"
            "    def mark(text):\n"
            "        with open(os.path.join(api.plugin_dir, 'engine_marker.txt'), 'a', encoding='utf-8') as fh:\n"
            "            fh.write(text + '\\n')\n"
            "    def build():\n"
            "        engine = api.engine\n"
            "        slots = engine.font_size_slots()\n"
            "        mark('slots:%d' % len(slots))\n"
            "        marks = engine.font_state_marks()\n"
            "        mark('ok_has_text:%d' % int(bool(marks['ok'])))\n"
            "        api.ui.label(None, '引擎门面夹具')\n"
            "    api.register_tool('引擎门面夹具页', build, 9)\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("engine_demo.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P7a 夹具 .py 没写进去"));
        Hub hub(nullptr, dir);
        check(hub.page_index(QStringLiteral("引擎门面夹具页")) >= 0,
              QStringLiteral("P7a 夹具页没进页表（插件问题 %1 条）").arg(hub.plugin_errors().size()));
        QString text;
        {
            QFile file(marker);
            if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.readAll());
        }
        check(text.contains(QStringLiteral("slots:10")),
              QStringLiteral("P7a GUI 宿主里槽位表不对（标记=%1）").arg(text.simplified()));
        check(text.contains(QStringLiteral("ok_has_text:1")),
              QStringLiteral("P7a GUI 宿主里四态文案是空的（标记=%1）").arg(text.simplified()));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P7a 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ★★ 插件改装 P7c（**样板页迁移的机制**）：插件页用 `replaces` **顶掉内置页**。
    //   判据：① 插件页进页表；② 内置 `fontpack` 页**不再可见**（隐藏、可还原）；③ 可见页数不变；
    //   ④ 标签仍显示原名（`title` 与 id 可以不同 —— 迁移后 id 必须换新，标题不该变）。
    {
        const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_replace"));
        QDir(dir).removeRecursively();
        QDir().mkpath(dir);
        const QByteArray source =
            "# -*- coding: utf-8 -*-\n"
            "PLUGIN_API = 1\n"
            "def register(api):\n"
            "    def build():\n"
            "        api.ui.label(None, '这是插件版的汉化包生成页')\n"
            "    api.register_tool('fontpack_py', build, 0, 'fontpack', '插件版', '汉化包生成')\n";
        QFile plugin_file(QDir(dir).filePath(QStringLiteral("fontpack_py.py")));
        const bool wrote = plugin_file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                           plugin_file.write(source) == source.size();
        plugin_file.close();
        check(wrote, QStringLiteral("P7c 夹具 .py 没写进去"));
        Hub hub(nullptr, dir);
        const int replaced = hub.page_index(QStringLiteral("fontpack"));
        const int fresh = hub.page_index(QStringLiteral("fontpack_py"));
        check(fresh >= 0, QStringLiteral("P7c 插件页没进页表（插件问题 %1 条）")
                              .arg(hub.plugin_errors().size()));
        check(replaced < 0, QStringLiteral("P7c 内置 fontpack 页仍可见（`replaces` 没生效，下标=%1）")
                                .arg(replaced));
        // 可见页数不变：内置六页里被顶掉 1 页、插件页补 1 页 ⇒ 总数仍是 6
        check(hub.page_count() == 6,
              QStringLiteral("P7c 可见页数应当不变（实得 %1，内置六页）").arg(hub.page_count()));
        check(fresh >= 0 && hub.stack()->widget(fresh) != nullptr &&
                  hub.stack()->widget(fresh)->windowTitle() == QStringLiteral("汉化包生成"),
              QStringLiteral("P7c 迁移后的页标题不是原名（id 与 title 应当可以不同）"));
        check(hub.plugin_errors().empty(),
              QStringLiteral("P7c 好插件不该产生插件问题（实得 %1 条）").arg(hub.plugin_errors().size()));
        QDir(dir).removeRecursively();
    }

    // ⑫12.1 四类槽位渲染（用户 2026-09-26"补六类"：panel/tool/option/option_entry）
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots4"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        const int builtin = hub.page_count();

        plug::PluginApiAdapter adapter(hub.registry(), "slots4.dll", nullptr);
        const char* choices[] = {"甲", "乙"};
        check(adapter.api().register_option(adapter.api().host, "插件下拉", choices, 2, nullptr,
                                           nullptr, "fs", "top_bar", nullptr, 1) == 1,
              QStringLiteral("⑫12.1 新建下拉注册失败"));
        check(adapter.api().register_option_entry(adapter.api().host, "fs", "top_bar", "插件条目",
                                                  nullptr) == 1,
              QStringLiteral("⑫12.1 下拉条目注册失败"));
        const auto builder = +[](void*, stk_widget*) -> stk_widget* {
            return reinterpret_cast<stk_widget*>(new QWidget());
        };
        check(adapter.api().register_panel(adapter.api().host, "插件面板", builder, nullptr, "fs",
                                           "top_bar", nullptr, 2) == 1,
              QStringLiteral("⑫12.1 面板注册失败"));
        check(adapter.api().register_tool(adapter.api().host, "插件页", builder, nullptr, 3) == 1,
              QStringLiteral("⑫12.1 Hub 插件页注册失败"));

        hub.render_plugin_slots();
        check(hub.plugin_pages_added() == 1 && hub.page_count() == builtin + 1,
              QStringLiteral("⑫12.1 插件页没被追加（pages=%1 期望 %2）")
                  .arg(hub.page_count())
                  .arg(builtin + 1));
        // ★★ P1：插件页必须排在**内置六页之后**（1.x `stalker_toolkit.py:215-221` 的口径：
        //   内置六工具固定在前，插件栏目按 order 稳定追加）—— 排序的"带"写错就红。
        check(hub.page_id(builtin) == QStringLiteral("插件页") &&
                  hub.page_owner(builtin) == QStringLiteral("slots4.dll"),
              QStringLiteral("P1 插件页没排在内置页之后（第 %1 页 = 「%2」/「%3」）")
                  .arg(builtin)
                  .arg(hub.page_id(builtin))
                  .arg(hub.page_owner(builtin)));

        bool combo_ok = false;
        bool entry_ok = false;
        bool panel_ok = false;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr || page->page_id() != QStringLiteral("fs")) continue;
            for (QComboBox* combo : page->action_bar()->findChildren<QComboBox*>()) {
                if (combo->count() >= 2) combo_ok = true;
                for (int k = 0; k < combo->count(); ++k) {
                    if (combo->itemText(k) == QStringLiteral("插件条目")) entry_ok = true;
                }
            }
            for (QWidget* w : page->body()->findChildren<QWidget*>()) {
                if (w->objectName().startsWith(QStringLiteral("plugin_panel_"))) panel_ok = true;
            }
        }
        check(combo_ok, QStringLiteral("⑫12.1 插件下拉没出现在 fs 页操作栏"));
        check(entry_ok, QStringLiteral("⑫12.1 插件下拉条目没塞进对应 area 的下拉"));
        check(panel_ok, QStringLiteral("⑫12.1 插件面板没出现在 fs 页正文"));

        // 幂等：重渲染不许把插件页装两遍
        hub.render_plugin_slots();
        check(hub.page_count() == builtin + 1 && hub.plugin_pages_added() == 1,
              QStringLiteral("⑫12.1 重渲染把插件页装了两遍（pages=%1）").arg(hub.page_count()));
        QDir(empty).removeRecursively();
    }

    // ★★ 2026-10-06（① 用户拍板「**插件的架构Bug，那必须修啊。**」）：两类槽位的**消费端**
    //   修前（本块就是它的锁）：
    //     · `context`（右键菜单）槽**根本没渲染** —— 全 2.x 源码搜 `"context"` 只命中自检断言，
    //       插件注册的右键命令哪儿都不出现；
    //     · `option_entry` 的下拉虽然建出来了（`hub.cpp` 把它塞进操作栏），但指针只在
    //       `render_plugin_slots` 的**局部 QHash** 里 ⇒ 插件加的下拉条目"只显示、不生效"
    //       （页面读不到它的值）。
    //   锁的形态（两处都能判红）：
    //     ① 页面侧 `plugin_option_value(area)` 必须返回插件条目的当前值 —— 拆掉 Hub 的
    //        `set_plugin_option_combo` 登记就红；
    //     ② 包列表**真右键路径**（`customContextMenuRequested`）弹出的那个菜单里必须有插件命令、
    //        且**点它真的调回插件**（走既有 `CommandReg`/`Registry::invoke`，没有新 ABI）；
    //        `when={"loaded":"1"}` 在没加载任何包时**不许**出现（＝`when` 按当前状态**重算**，
    //        不是构造时冻结的死条件）。
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots_ctx"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        // 静态：`stk_command_fn` 是**裸函数指针**（不能捕获局部变量）⇒ 次数只能记在静态上
        static int ctx_clicks = 0;
        ctx_clicks = 0;
        plug::PluginApiAdapter adapter(hub.registry(), "slots_ctx.dll", nullptr);
        const auto on_ctx = +[](void*) { ++ctx_clicks; };
        // when={"loaded": "1"}（C ABI 的 when 值是字符串；1.x 写的是 True，见 abi.h 的 stk_when）
        const char* when_keys[] = {"loaded"};
        const char* when_values[] = {"1"};
        stk_when when;
        when.keys = when_keys;
        when.values = when_values;
        when.count = 1;
        when.fn = nullptr;
        when.user = nullptr;
        const char* cid = adapter.api().register_command(adapter.api().host, "ctx_action",
                                                         "插件右键动作", on_ctx, nullptr, &when);
        check(cid != nullptr, QStringLiteral("① context：插件命令注册失败"));
        check(adapter.api().register_menu_item(adapter.api().host, "fs", "context", cid, 0) == 1,
              QStringLiteral("① context：fs 的 context 挂载点注册失败"));
        check(adapter.api().register_option_entry(adapter.api().host, "fs", "top_bar", "插件下拉值",
                                                  nullptr) == 1,
              QStringLiteral("① option_entry：下拉条目注册失败"));
        hub.render_plugin_slots();

        FsPage* fs = nullptr;
        for (int i = 0; i < hub.page_count(); ++i) {
            if (auto* candidate = dynamic_cast<FsPage*>(hub.page_at(i))) fs = candidate;
        }
        check(fs != nullptr, QStringLiteral("① 找不到 fs 页（Hub 没造出来？）"));
        if (fs != nullptr) {
            // ①-a **页面侧的取值入口**：插件下拉的"当前值"页面读得到（修前恒为空串）
            const QString value = fs->plugin_option_value(QStringLiteral("top_bar"));
            check(value == QStringLiteral("插件下拉值"),
                  QStringLiteral("① option_entry：页面读不到插件下拉的值（实得「%1」）").arg(value));
            // 反向：没登记过的 area 必须读成空 —— 证明这不是"恒定返回某个值"的假入口
            check(fs->plugin_option_value(QStringLiteral("nope")).isEmpty(),
                  QStringLiteral("① option_entry：不存在的 area 必须读成空"));

            // ①-b context：**没加载任何包**（loaded=0）⇒ when 不满足 ⇒ 菜单里一条插件项都没有
            QMenu denied(fs);
            const int added_denied = fs->append_plugin_context_items(&denied, fs->plugin_context());
            check(added_denied == 0 && denied.actions().isEmpty(),
                  QStringLiteral("① context：loaded=0 时不许出现插件项（实得 %1 条 / 菜单 %2 项）")
                      .arg(added_denied)
                      .arg(denied.actions().size()));

            // 加载一个源 ⇒ loaded=1 ⇒ when 满足 ⇒ 出现，且**点它真的调回插件**
            fs->add_source(QStringLiteral("xrkit_smoke_ctx.db"), {});
            check(fs->loaded_source_count() == 1,
                  QStringLiteral("① context：夹具没把包加载起来（loaded=%1）")
                      .arg(fs->loaded_source_count()));
            QMenu allowed(fs);
            const int added_allowed = fs->append_plugin_context_items(&allowed, fs->plugin_context());
            QAction* plugin_action = nullptr;
            for (QAction* act : allowed.actions()) {
                if (act->text() == QStringLiteral("插件右键动作")) plugin_action = act;
            }
            check(added_allowed == 1 && plugin_action != nullptr,
                  QStringLiteral("① context：loaded=1 时插件命令必须出现在菜单里（实得 %1 条）")
                      .arg(added_allowed));
            if (plugin_action != nullptr) plugin_action->trigger();
            check(ctx_clicks == 1,
                  QStringLiteral("① context：点菜单项必须把命令**回传插件**（既有 CommandReg/invoke；"
                                 "实测调用 %1 次）")
                      .arg(ctx_clicks));

            // ①-c **真右键路径**：`customContextMenuRequested` 弹出的那个菜单里也得有它
            //   （证明页面的右键处理器**真的调了** `append_plugin_context_items`；
            //    只测基类方法的话，"页面根本没接线"这种漏法测不出来）。
            QStringList seen;
            QTimer::singleShot(0, [&seen, fs]() {
                QMenu* shown = fs->last_package_context_menu();
                if (shown == nullptr) return;
                for (QAction* act : shown->actions()) seen << act->text();
                shown->close();   // 让 `menu.exec()` 的嵌套事件循环退出来（自检不许卡住）
            });
            emit fs->package_list()->customContextMenuRequested(QPoint(4, 4));
            check(seen.contains(QStringLiteral("插件右键动作")),
                  QStringLiteral("① context：真右键路径的菜单里没有插件命令（实得 [%1]）")
                      .arg(seen.join(QStringLiteral("/"))));

            // ①-d 1.x 口径的**默认槽位**：`location` 留空（C ABI 里 NULL）＝ `"context"`
            //   （`toolkit_plugins.py:361` 的 `location="context"`）。修前 NULL 被当非法槽位拒掉。
            const char* cid2 = adapter.api().register_command(adapter.api().host, "ctx_default",
                                                              "插件右键动作2", on_ctx, nullptr,
                                                              nullptr);
            check(cid2 != nullptr &&
                      adapter.api().register_menu_item(adapter.api().host, "fs", nullptr, cid2, 1) ==
                          1,
                  QStringLiteral("① context：`location` 留空必须落到 1.x 的默认值 context"));

            QMenu defaults(fs);
            const int added_defaults =
                fs->append_plugin_context_items(&defaults, fs->plugin_context());
            bool second_ok = false;
            for (QAction* act : defaults.actions()) {
                if (act->text() == QStringLiteral("插件右键动作2")) second_ok = true;
            }
            check(added_defaults == 2 && second_ok,
                  QStringLiteral("① context：默认槽位的命令也要在菜单里（实得 %1 条 / 第 2 条 %2）")
                      .arg(added_defaults)
                      .arg(second_ok ? 1 : 0));
        }
        QDir(empty).removeRecursively();
    }

    // 视频端到端（2026-09-27）：**仅当按口径能定位到 ffmpeg/ffprobe 时才跑**；
    // 本机没有 user.ltx、程序目录也没有 exe（⑧8.1 只认两处、不搜 PATH）⇒ 会走"如实 note 跳过"，
    // 这条对应用户侧的人工验收（配方见 docs/2.0.0-视频模块规格.md §6）。
    {
        namespace vid = xrkit::engine::video;
        const vid::FfmpegPaths paths = vid::find_ffmpeg_default();
        if (!paths.found || !paths.has_probe) {
            result.notes.append(QStringLiteral(
                "视频端到端：本机未按口径配置 ffmpeg/ffprobe（只认 user.ltx 手动指定与程序目录）"
                "⇒ 跳过，属人工验收项"));
        } else {
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_video"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString in_path = dir + QStringLiteral("/in.avi");
            // ① 造 1 秒测试输入（lavfi 源，不需要外部素材）
            QProcess gen;
            gen.start(QString::fromStdString(paths.ffmpeg),
                      QStringList{QStringLiteral("-y"), QStringLiteral("-f"),
                                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                                  QStringLiteral("testsrc=d=1"), QStringLiteral("-f"),
                                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                                  QStringLiteral("sine=f=440:d=1"), QStringLiteral("-shortest"),
                                  in_path});
            gen.waitForFinished(120000);
            const bool made = gen.exitCode() == 0 && QFile::exists(in_path);
            check(made, QStringLiteral("视频端到端：造测试输入失败（%1）").arg(gen.exitCode()));

            if (made) {
                const QString out_path = dir + QStringLiteral("/out.ogm");
                QFile::remove(out_path);
                VideoPage page;
                vid::TranscodeRequest request;
                request.ffmpeg = paths.ffmpeg;
                request.input = in_path.toUtf8().toStdString();
                request.output = out_path.toUtf8().toStdString();
                request.available_encoders = {"libtheora", "libxvid", "mpeg4"};
                QString error;
                check(page.start_transcode(request, &error),
                      QStringLiteral("视频端到端：启动失败（%1）").arg(error));
                QElapsedTimer clock;
                clock.start();
                while (page.transcode_running() && clock.elapsed() < 180000) {
                    QCoreApplication::processEvents();
                }
                check(!page.transcode_running(), QStringLiteral("视频端到端：超时未结束"));
                check(QFile::exists(out_path) && QFileInfo(out_path).size() > 0,
                      QStringLiteral("视频端到端：产物不存在或为空（%1）").arg(page.last_error()));
                // ② 产物要能被 ffprobe 解析（"不是垃圾文件"）
                QProcess probe;
                probe.start(QString::fromStdString(paths.ffprobe),
                            QStringList{QStringLiteral("-v"), QStringLiteral("error"),
                                        QStringLiteral("-show_entries"),
                                        QStringLiteral("format=duration"),
                                        QStringLiteral("-of"),
                                        QStringLiteral("default=nw=1"), out_path});
                probe.waitForFinished(30000);
                const QString probed = QString::fromUtf8(probe.readAllStandardOutput());
                check(probe.exitCode() == 0 && probed.contains(QStringLiteral("duration=")),
                      QStringLiteral("视频端到端：产物 ffprobe 解析失败（%1）").arg(probed.left(60)));
            }
            QDir(dir).removeRecursively();
        }
    }

    // 散装层界面接线（用户 2026-09-26"最后并入层"）
    {
        std::error_code ec;
        const std::filesystem::path work =
            std::filesystem::temp_directory_path(ec) / "xrkit_smoke_loose";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work / "sub", ec);
        {
            std::ofstream out(work / "a.txt", std::ios::binary);
            out << 'A';
        }
        {
            std::ofstream out(work / "sub" / "b.txt", std::ios::binary);
            out << 'B';
        }
        FsPage page;
        QString error;
        const bool ok = page.add_loose_dir(QString::fromStdString(work.string()), &error);
        check(ok && error.isEmpty(), QStringLiteral("散装层加入失败：%1").arg(error));
        check(page.source_count() == 1,
              QStringLiteral("散装层没有变成一个源（实得 %1）").arg(page.source_count()));

        // ★ 5.x 页面骨架（界面重构口径）：标题字号 > 正文、内容卡片占大头、操作栏贴底
        {
            FsPage skel;
            skel.resize(600, 700);
            skel.show();
            QApplication::processEvents();
            if (skel.layout() != nullptr) skel.layout()->activate();
            QApplication::processEvents();
            const double title_pt = skel.header() != nullptr ? skel.header()->font().pointSizeF() : 0.0;
            const double body_pt = skel.font().pointSizeF() > 0 ? skel.font().pointSizeF() : 12.0;
            check(skel.header() != nullptr && title_pt >= body_pt + 2.0,
                  QStringLiteral("骨架① 页标题比正文大 >= 2pt（实测 %1 vs %2）").arg(title_pt).arg(body_pt));
            check(skel.subtitle() != nullptr,
                  QStringLiteral("骨架② 页要有副标题控件（说明本页能干什么）"));
            const int page_h = skel.height();
            const int card_h = skel.card() != nullptr ? skel.card()->height() : -1;
            check(card_h > 0 && card_h >= page_h * 55 / 100,
                  QStringLiteral("骨架③ 内容卡片 >= 页面高 55%%（实测 %1 / %2）").arg(card_h).arg(page_h));
            // ★★ 2026-10-06（审计处理批·0-3 之 B1-6）：同 ①-a1 —— 条件不成立时断言静默消失。
            //   把"操作栏存在且挂在页上"也写成判据（缺件即红）。
            const bool bar_ok = skel.action_bar() != nullptr &&
                                skel.action_bar()->parentWidget() == &skel;
            check(bar_ok,
                  QStringLiteral("骨架④ 前提：操作栏存在且挂在页上（action_bar=%1 parent=页?%2）")
                      .arg(skel.action_bar() != nullptr ? 1 : 0)
                      .arg(skel.action_bar() != nullptr &&
                                   skel.action_bar()->parentWidget() == &skel
                               ? 1
                               : 0));
            if (bar_ok) {
                const int bottom_gap = page_h - (skel.action_bar()->y() + skel.action_bar()->height());
                check(bottom_gap >= 0 && bottom_gap <= xrkit::core::px(10),
                      QStringLiteral("骨架④ 操作栏贴底（底部余量 %1px）").arg(bottom_gap));
            }
            skel.hide();
        }

        // ★ 空状态（用户 2026-09-27 明令："把图中玩意删掉"）：卡片里那坨空状态文字块与
        //   重复提示行**都不许再出现**；空状态改由"包列表吃满卡片"承担。
        {
            FsPage empty_page;
            empty_page.show();
            QApplication::processEvents();
            if (empty_page.layout() != nullptr) empty_page.layout()->activate();
            QApplication::processEvents();

            // ① 用户明令删掉的那三样：一个也不许回来（命中即红）
            QStringList leftover;
            for (QWidget* child : empty_page.findChildren<QWidget*>()) {
                const QString name = child->objectName();
                if (name == QStringLiteral("fs_hint") || name == QStringLiteral("fs_entry_hint") ||
                    name == QStringLiteral("fs_empty_block")) {
                    leftover << name;
                }
            }
            check(leftover.isEmpty(),
                  QStringLiteral("① 删掉的空状态块/重复提示行又回来了：%1")
                      .arg(leftover.join(QStringLiteral(","))));
            check(!empty_page.entry_hint_visible(),
                  QStringLiteral("① 空状态提示访问器应为「恒 false」（该文字块已删）"));
            // ① 卡片里不许再有**说明段落** —— 用户删的那几段都是"长文本 + 自动换行"的标签；
            //   字段标签（"打包/解包格式："/"解包到："）与计数标签是控件，不是说明段落。
            //   （原来这条写的是"卡片内直接 QLabel 恰好 1 个"：那是删完提示行那一刻的**快照**，
            //    2026-09-28 按 1.x 补回按钮行与计数标签后它必然红 ⇒ 改成按**性质**判，
            //    不是把数字改大。）
            int paragraph_labels = 0;
            for (QLabel* label : empty_page.findChildren<QLabel*>()) {
                if (label->wordWrap() && label->text().size() > 16) ++paragraph_labels;
            }
            check(paragraph_labels == 0,
                  QStringLiteral("① 卡片里不该有自动换行的说明段落（实测 %1 个）")
                      .arg(paragraph_labels));

            // ★ 排版口径：页标题与说明**合并成一行**（顶部堆叠行数从 4 降到 2 的结构前提）
            check(empty_page.header_row() != nullptr,
                  QStringLiteral("排版①：页标题与说明要合成一行（header_row 存在）"));
            // ★ 原 ⑥「空状态块 ≥ 卡片高 60%」的度量对象（那个占位块）已按用户要求删除 ⇒
            //   断言作废，换成等价物：**没条目时条目列表收起、各列表不被挤成细线**。
            //   口径沿革（报账，别当成"悄悄改数字"）：
            //     ① 上一轮：60%（我按"卡片里只有两个列表"估的）；
            //     ② 用户要求补回 1.x 的按钮行后 → 改成"过半"；
            //     ③ 本轮再补 **1.x 的封包面板**（卡片里现在有**两个**工作区 + 三行控件）
            //        ⇒ 任何"某个列表占卡片几分之几"都不再说明问题。
            //   保留的**不变量**（也是用户真正抱怨过的："空列表只有一条细线，看着像坏了"）：
            //   每个列表不得小于它自己的最小高度。想判红就把某条 minimumHeight 去掉。
            const int list_h = empty_page.package_list() != nullptr
                                   ? empty_page.package_list()->height()
                                   : -1;
            const int pack_h = empty_page.pack_list() != nullptr ? empty_page.pack_list()->height()
                                                                 : -1;
            check(list_h >= xrkit::core::px(120) && pack_h >= xrkit::core::px(90),
                  QStringLiteral("⑥ 空状态：各列表都不许被挤成细线（包列表 %1/≥120，待打包 %2/≥90）")
                      .arg(list_h)
                      .arg(pack_h));
            // 条目列表：★**2026-10-01 与用户两张截图对齐后的口径**（他说"一个正常一个屎"：
            //   封包页＝两行控件 + 一个列表填满；解包页当时是四条横带 + 两个空框）——
            //   **没条目时把整个条目面板（工具行 + 树）收掉**，高度全给包列表 ⇒ 两页同形；
            //   有货时面板才出现、竖向分隔条才生效（下面那条断言接着验它出现）。
            check(empty_page.entry_pane() != nullptr && empty_page.entry_pane()->isHidden(),
                  QStringLiteral("空状态：条目面板整块收起，包列表填满工作区（与封包页同形）"));

            empty_page.add_loose_dir(QString::fromStdString(work.string()), nullptr);  // 临时目录是 ASCII 路径
            QApplication::processEvents();
            check(!empty_page.entry_hint_visible(),
                  QStringLiteral("① 加载后也不许冒出那句提示（它已经删了）"));
            check(empty_page.entry_tree() != nullptr && !empty_page.entry_tree()->isHidden(),
                  QStringLiteral("有条目后条目树要出现"));
            empty_page.hide();
        }
        // 用"勾选计数"证明条目**真的进了合并树**（a.txt + sub/b.txt = 2 个文件）
        page.check_all(true);
        check(page.checked_file_count() == 2,
              QStringLiteral("散装层条目数不对（期望 2，实得 %1）").arg(page.checked_file_count()));
        // 不存在的目录 ⇒ 拒绝且**不加源**
        const int before = page.source_count();
        QString missing_error;
        const bool bad =
            page.add_loose_dir(QString::fromStdString((work / "nope").string()), &missing_error);
        check(!bad && !missing_error.isEmpty() && page.source_count() == before,
              QStringLiteral("不存在的散装目录必须拒绝且不加源"));
        std::filesystem::remove_all(work, ec);
    }

    // ⑫12.1 decryptor 消费端（用户 2026-09-26：NLC 解密是外部插件；fs 页加载前必须问）
    {
        namespace plug = xrkit::plugin;
        FsPage page;
        const std::vector<xrkit::engine::fs::EntryInfo> none;
        // ① 没装注册表 ⇒ 行为不变
        page.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(page.source_count() == 1 && page.source_at(0) == QStringLiteral("gamedata.db_enc"),
              QStringLiteral("⑫12.1 没有注册表时不该改变加载行为"));

        // ② 装了"能解开"的解密器 ⇒ 换成解密产物
        plug::Registry registry;
        registry.register_decryptor("nlc.dll", [](const std::string&) { return true; },
                                    [](const std::string&, std::string& out) {
                                        out = "PLAIN";
                                        return true;
                                    });
        FsPage gated;
        gated.set_registry(&registry);
        gated.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(gated.source_count() == 1 &&
                  gated.source_at(0).endsWith(QStringLiteral(".decrypted")),
              QStringLiteral("⑫12.1 命中解密器后没换成解密产物（实得 %1）")
                  .arg(gated.source_at(0)));

        // ③ 装了"解不开"的解密器 ⇒ **拒绝加载**且理由可见
        plug::Registry bad;
        bad.register_decryptor("bad.dll", [](const std::string&) { return true; },
                               [](const std::string&, std::string&) { return false; });
        FsPage refused;
        refused.set_registry(&bad);
        refused.add_source(QStringLiteral("gamedata.db_enc"), none);
        check(refused.source_count() == 0 && !refused.last_gate_error().isEmpty(),
              QStringLiteral("⑫12.1 解密失败必须拒绝加载并给出理由（实得 %1 个源）")
                  .arg(refused.source_count()));

        // ★ 2026-10-04：`set_registry` 现在**同时**把这份注册表交给引擎（活动钩子）。
        //   本用例里的注册表是**局部对象**，出块就死 ⇒ 必须在这里收回钩子（否则下一个用例
        //   读到的是野指针）。这一条本身也是判据：钩子不许悬空。
        plug::set_active_registry(nullptr);
        check(plug::active_registry() == nullptr,
              QStringLiteral("⑫12.1 自检用完活动注册表钩子必须复位（不许悬空）"));
    }

    // ★★ 2026-10-04 **引擎层解密闸门**（用户口径原话：「GUI 的功能，CLI 肯定要全部拥有啊。
    //   哪有 GUI 的细节功能比 CLI 还多的。」）——
    //   修前的违例：闸门只在外壳（`FsPage::gate_source`）里，CLI 直接调引擎、动作路径**从不建
    //   注册表** ⇒ 加密包被 CLI 当普通包读（同一引擎、两个前端两套行为）。
    //   这一组锁的是"闸门真的搬进了**引擎读入口**"（`package_entries` / `list_db_seek`）：
    //     ① 有活动注册表且命中 ⇒ 拒绝，理由**逐字等于** `gate_db_path` 给的那一条
    //        （GUI 显示的就是它）—— 把引擎里那句 `gate_active_read` 去掉立刻红；
    //     ② 命中且解得开 ⇒ 真的读**解密产物**（条目名换成明文里的那个）；
    //     ③ 显式传入的 registry 优先于活动钩子；
    //     ④ 钩子复位（`nullptr`）后条目表与"从来没有过钩子"**逐值一致**（默认行为不许被改）；
    //     ⑤ 闸门不二次解密自己的产物（外壳先过一遍、引擎再过一遍时必须认出来）。
    {
        namespace plug = xrkit::plugin;
        namespace efs = xrkit::engine::fs;
        std::error_code ec;
        const std::filesystem::path work =
            std::filesystem::temp_directory_path(ec) / "xrkit_smoke_engine_gate";
        std::filesystem::remove_all(work, ec);
        std::filesystem::create_directories(work, ec);
        // ① 夹具：两枚**真的** X-Ray DB 包（引擎写侧产物）——一枚"加密包"、一枚"明文"
        const QString pkg = QString::fromStdString((work / "cipher.stkenc.db").string());
        const QString plain_db = QString::fromStdString((work / "plain.db").string());
        const auto write_db = [](const QString& path, const std::string& name) {
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem item;
            item.path = name;
            item.data = {static_cast<uint8_t>('X')};
            items.push_back(item);
            const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
            std::ofstream out(path.toStdString(), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
            return raw.size() > 0;
        };
        const bool built = write_db(pkg, "a.txt") && write_db(plain_db, "plain.txt");
        check(built, QStringLiteral("引擎闸门夹具：造不出测试 DB 包"));

        // 条目表的**指纹**（path/is_dir/size 三样都算）——"逐值一致"要拿它比，不靠条数
        const auto fingerprint = [](const std::vector<xrkit::engine::fs::EntryInfo>& entries) {
            std::string text;
            for (const xrkit::engine::fs::EntryInfo& e : entries) {
                text += e.path + "\t" + (e.is_dir ? "d" : "f") + "\t" +
                        std::to_string(e.size) + "\n";
            }
            return text;
        };
        const std::string pkg_utf8 = pkg.toStdString();
        std::string error;

        // ④ 基线：**从来没有过钩子**时读出来的条目表（后面复位后必须与它逐值一致）
        const std::vector<xrkit::engine::fs::EntryInfo> baseline =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        const std::string baseline_print = fingerprint(baseline);
        check(error.empty() && baseline.size() == 1 && baseline[0].path == "a.txt",
              QStringLiteral("引擎闸门基线：裸读应得 a.txt 一条（err=%1，实得 %2 条）")
                  .arg(QString::fromStdString(error))
                  .arg(baseline.size()));

        // ② 一枚"命中却解不开"的解密器
        plug::Registry reject_reg;
        reject_reg.register_decryptor("smoke_gate.dll", [](const std::string&) { return true; },
                                      [](const std::string&, std::string&) { return false; });
        const plug::DecryptGateResult expected_gate =
            plug::gate_db_path(reject_reg, pkg_utf8, plug::decrypt_output_dir());
        check(!expected_gate.ok && !expected_gate.error.empty(),
              QStringLiteral("引擎闸门夹具：gate_db_path 没给出拒绝理由"));
        plug::set_active_registry(&reject_reg);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> gated =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(gated.empty() && error == expected_gate.error,
              QStringLiteral("引擎闸门没生效：`package_entries` 应拒绝并给**同一条**理由"
                             "（实得 %1 条；理由 %2）")
                  .arg(gated.size())
                  .arg(QString::fromStdString(error)));
        // 同一条闸门也在 `--dump`/`--extract` 走的那个入口上（CLI 黑盒判据的引擎侧一半）
        const xrkit::engine::fs::DbListing also_gated =
            xrkit::engine::fs::list_db_seek(pkg_utf8, std::nullopt);
        check(!also_gated.ok && also_gated.error == expected_gate.error,
              QStringLiteral("引擎闸门没覆盖 `list_db_seek`（--dump 走的入口）：%1")
                  .arg(QString::fromStdString(also_gated.error)));

        // ③ 显式传入的 registry 优先：活动钩子里那枚会拒，但显式传的这枚没有解密器 ⇒ 放行
        //   （★ 这条自检第一次跑就抓出一个真缺陷：`package_entries` 把显式 registry 丢了，
        //     内层 `list_db_seek` 又去问全局钩子 ⇒ 上面那次"放行"被内层推翻。修法是把同一份
        //     registry **逐层传下去**。）
        plug::Registry empty_reg;
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> explicit_ok =
            efs::package_entries(pkg_utf8, std::nullopt, error, &empty_reg);
        check(error.empty() && fingerprint(explicit_ok) == baseline_print,
              QStringLiteral("显式传入的 registry 没有优先于活动钩子（实得 %1 条 / err=%2）")
                  .arg(explicit_ok.size())
                  .arg(QString::fromStdString(error)));
        const xrkit::engine::fs::DbListing explicit_seek =
            xrkit::engine::fs::list_db_seek(pkg_utf8, std::nullopt, &empty_reg);
        check(explicit_seek.ok && explicit_seek.entries.size() == 1,
              QStringLiteral("显式传入的 registry 没传进 `list_db_seek`（ok=%1 err=%2）")
                  .arg(explicit_seek.ok ? 1 : 0)
                  .arg(QString::fromStdString(explicit_seek.error)));

        // ⑤ 命中且解得开 ⇒ 读的必须是**解密产物**（明文里那枚条目），且**不二次解密**自己的产物
        int decrypt_calls = 0;
        plug::Registry ok_reg;
        {
            std::ifstream in(plain_db.toStdString(), std::ios::binary);
            const std::string plain((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            ok_reg.register_decryptor(
                "smoke_gate_ok.dll", [](const std::string&) { return true; },
                [&decrypt_calls, plain](const std::string&, std::string& out) {
                    ++decrypt_calls;
                    out = plain;
                    return true;
                });
        }
        plug::set_active_registry(&ok_reg);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> decrypted =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(error.empty() && decrypted.size() == 1 && decrypted[0].path == "plain.txt",
              QStringLiteral("闸门命中且解得开时应读**明文**（期望 plain.txt，实得 %1 条 first=%2）")
                  .arg(decrypted.size())
                  .arg(decrypted.empty() ? QStringLiteral("<空>")
                                         : QString::fromStdString(decrypted[0].path)));
        check(decrypt_calls == 1, QStringLiteral("解密回调应恰好被调 1 次（实测 %1）")
                                      .arg(decrypt_calls));
        // 产物落点：必须落在**引擎那一份**目录里（外壳从同一处取值，才认得出"这是产物"）
        const QString decrypted_path =
            QDir(QString::fromStdString(plug::decrypt_output_dir()))
                .filePath(QStringLiteral("cipher.stkenc.db.decrypted"));
        check(QFileInfo::exists(decrypted_path),
              QStringLiteral("解密产物没落在引擎口径的目录里：%1").arg(decrypted_path));
        // 外壳先过 `gate_source`（产物入目录）再交给引擎 ⇒ 引擎那道闸门必须**认出来**、不二次解密
        if (QFileInfo::exists(decrypted_path)) {
            const int before = decrypt_calls;
            error.clear();
            const std::vector<xrkit::engine::fs::EntryInfo> again =
                efs::package_entries(decrypted_path.toStdString(), std::nullopt, error);
            check(error.empty() && fingerprint(again) == fingerprint(decrypted) &&
                      decrypt_calls == before,
                  QStringLiteral("闸门对自己的产物二次解密了（回调 %1→%2 次 / err=%3）")
                      .arg(before)
                      .arg(decrypt_calls)
                      .arg(QString::fromStdString(error)));
        }

        // ④ 复位 ⇒ 与基线**逐值一致**（默认行为不许被这段新代码顺手改掉）
        plug::set_active_registry(nullptr);
        error.clear();
        const std::vector<xrkit::engine::fs::EntryInfo> after_reset =
            efs::package_entries(pkg_utf8, std::nullopt, error);
        check(error.empty() && fingerprint(after_reset) == baseline_print,
              QStringLiteral("钩子复位后条目表与今天不一致（实得 %1 条 / err=%2）")
                  .arg(after_reset.size())
                  .arg(QString::fromStdString(error)));
        check(plug::active_registry() == nullptr,
              QStringLiteral("引擎闸门用例：钩子用完必须复位"));

        std::filesystem::remove_all(work, ec);
    }

    // ★★ 2026-10-04 **CLI 过闸门**（黑盒：真的起 `xrkit_cli.exe`）——
    //   锁的是"CLI 的动作路径也建注册表、并把引擎的拒绝理由如实打出来 + 非零退出"。
    //   判据用的插件是本构建产出的 `xrkit_test_plugin.dll`（纯 C；它注册了一枚判据为
    //   "文件名含 `.stkenc`" 且**解密必失败**的解密器）。
    //   把 CLI 里"动作分派前建注册表"那段删掉 ⇒ 第②条立刻红。
    {
        namespace plug = xrkit::plugin;
        namespace efs = xrkit::engine::fs;
        // 找 `xrkit_cli.exe` 与插件夹具。**两种布局**都要能活：
        //   · 构建树：`<build>/src/shell/xrkit_app.exe` ⇒ `../../tools/cli/`（自检/门禁跑的就是它）；
        //   · 部署包：`scripts/package.ps1` 的舞台**只放 xrkit_app.exe + Qt 运行时**
        //     （`dist\<版本>\<标记>\<序号>\`）⇒ 那里根本没有 CLI。
        //   找不到 ⇒ **如实记 note 跳过**（不是"通过"）；找到了 ⇒ 三条判据全跑，删掉 CLI 里
        //   "动作分派前建注册表"那段就当场红。★ 第一版把"找不到"写成硬红，结果**打包自检**
        //   在部署布局里红掉（打包直接失败）—— 布局不适用 ≠ 判据失败。
        const QString app_dir = QCoreApplication::applicationDirPath();
        QString cli;
        for (const QString& candidate :
             {app_dir + QStringLiteral("/xrkit_cli.exe"),
              QDir::cleanPath(app_dir + QStringLiteral("/../../tools/cli/xrkit_cli.exe"))}) {
            if (QFileInfo::exists(candidate)) {
                cli = candidate;
                break;
            }
        }
        QString plugin_dll;
        if (!cli.isEmpty()) {
            const QString cli_dir = QFileInfo(cli).absolutePath();
            for (const QString& candidate :
                 {cli_dir + QStringLiteral("/xrkit_test_plugin.dll"),
                  app_dir + QStringLiteral("/xrkit_test_plugin.dll")}) {
                if (QFileInfo::exists(candidate)) {
                    plugin_dll = candidate;
                    break;
                }
            }
        }
        if (cli.isEmpty() || plugin_dll.isEmpty()) {
            result.notes.append(
                QStringLiteral("CLI 闸门黑盒判据：这个布局里找不到 xrkit_cli.exe / "
                               "xrkit_test_plugin.dll（app_dir=%1）⇒ 本用例跳过（构建树/门禁里会真跑）")
                    .arg(app_dir));
        } else {
            const QString plugin_dir = QFileInfo(plugin_dll).absolutePath();
            std::error_code ec;
            const std::filesystem::path work =
                std::filesystem::temp_directory_path(ec) / "xrkit_smoke_cli_gate";
            std::filesystem::remove_all(work, ec);
            std::filesystem::create_directories(work, ec);
            // 一枚**真的** DB 包，文件名带 `.stkenc`（插件那条解密器的判据）
            const QString enc_db = QString::fromStdString((work / "cli.stkenc.db").string());
            {
                std::vector<efs::DbPackItem> items;
                efs::DbPackItem item;
                item.path = "cli.txt";
                item.data = {static_cast<uint8_t>('C')};
                items.push_back(item);
                const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
                std::ofstream out(enc_db.toStdString(), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(raw.data()),
                          static_cast<std::streamsize>(raw.size()));
            }
            const auto run_cli = [](const QString& exe, const QStringList& argv) {
                QProcess process;
                process.setProcessChannelMode(QProcess::MergedChannels);
                process.start(exe, argv);
                const bool finished = process.waitForFinished(120000);
                return std::make_pair(static_cast<int>(finished ? process.exitCode() : -1),
                                      QString::fromUtf8(process.readAllStandardOutput()));
            };

            // ① **默认（不带 --plugins）照旧放行** —— 这就是"NLC 那一类"在本机的现状：
            //    CLI 的 plugins/ 常为空 ⇒ 有注册表但无解密器 ⇒ 与 GUI 现状一致（不许被这次改动打破）
            {
                const auto [rc, out] = run_cli(cli, {QStringLiteral("--dump"), enc_db});
                check(rc == 0 && out.contains(QStringLiteral("DUMP ok")) &&
                          out.contains(QStringLiteral("entries=1")),
                      QStringLiteral("CLI 默认（无插件）必须照旧放行（exit=%1 out=%2）")
                          .arg(rc)
                          .arg(out.trimmed().right(120)));
            }
            // ② `--plugins <目录>` ⇒ 闸门拒绝 + **同一条理由** + 非零退出
            {
                const auto [rc, out] = run_cli(
                    cli, {QStringLiteral("--plugins"), plugin_dir, QStringLiteral("--dump"), enc_db});
                // 期望文案：本进程用**同一个**插件 DLL 装出的注册表问一遍 `gate_db_path`
                //（GUI 显示的就是这条；"同一条文案"要逐字比，不许各写一份）
                plug::Registry mirror;
                plug::PluginApiAdapter adapter(mirror, "xrkit_test_plugin.dll", nullptr);
                // ★ 本批 S1-2 改了 `load_plugin` 的签名（第二参从裸 `stk_api` 收成**适配器**：
                //   注册目标与回滚目标必须是同一份东西）—— 这一处是**编译必需**的跟随改动，
                //   判据/行为一字未动。
                const plug::LoadResult loaded = plug::load_plugin(plugin_dll.toStdString(),
                                                                  adapter);
                new plug::PluginApiAdapter(std::move(adapter));   // 回调活着要一直有效
                const plug::DecryptGateResult want =
                    plug::gate_db_path(mirror, enc_db.toStdString(), plug::decrypt_output_dir());
                const QString want_text = QString::fromStdString(want.error);
                check(rc != 0 && !out.contains(QStringLiteral("DUMP ok")) && want_text.size() > 0 &&
                          out.contains(want_text),
                      QStringLiteral("CLI 装了插件后必须被闸门拦下并给**同一条**理由"
                                     "（exit=%1；期望含 %2；实得 %3）")
                          .arg(rc)
                          .arg(want_text)
                          .arg(out.trimmed().right(160)));
                check(loaded.ok, QStringLiteral("CLI 闸门判据：本进程装测试插件失败：%1")
                                     .arg(QString::fromStdString(loaded.error)));
            }
            // ③ 插件目录不存在 ⇒ 只记错、**不阻塞**（与 GUI 的启动口径一致）
            {
                const QString ghost =
                    QString::fromStdString((work / "no_such_plugins").string());
                const auto [rc, out] =
                    run_cli(cli, {QStringLiteral("--plugins"), ghost, QStringLiteral("--dump"),
                                  enc_db});
                check(rc == 0 && out.contains(QStringLiteral("entries=1")) &&
                          out.contains(QStringLiteral("PLUGINS dir=")),
                      QStringLiteral("插件目录不存在不许阻塞动作（exit=%1 out=%2）")
                          .arg(rc)
                          .arg(out.trimmed().right(160)));
            }
            std::filesystem::remove_all(work, ec);
        }
    }

    // ⑫12.1/12.4：插件槽位要**渲染出来**（工具栏菜单项 → 页面的操作栏按钮）
    {
        namespace plug = xrkit::plugin;
        const QString empty = QDir::temp().filePath(QStringLiteral("xrkit_smoke_slots"));
        QDir(empty).removeRecursively();
        QDir().mkpath(empty);
        Hub hub(nullptr, empty);
        plug::PluginApiAdapter adapter(hub.registry(), "slots.dll", nullptr);
        const auto on_click = +[](void*) {};
        const char* cid =
            adapter.api().register_command(adapter.api().host, "slot_action", "插件动作",
                                           on_click, nullptr, nullptr);
        check(cid != nullptr, QStringLiteral("⑫12.1 插件命令注册失败"));
        check(adapter.api().register_menu_item(adapter.api().host, "xml", "toolbar", cid, 0) == 1,
              QStringLiteral("⑫12.4 插件工具栏挂载点注册失败"));
        hub.render_plugin_slots();
        check(hub.plugin_slots_rendered() >= 1,
              QStringLiteral("⑫12.1 插件槽位没有被渲染（渲染了 %1 个）")
                  .arg(hub.plugin_slots_rendered()));
        bool found = false;
        for (int i = 0; i < hub.page_count(); ++i) {
            PageBase* page = hub.page_at(i);
            if (page == nullptr || page->page_id() != QStringLiteral("xml")) continue;
            for (QPushButton* button : page->action_bar()->findChildren<QPushButton*>()) {
                if (button->text() == QStringLiteral("插件动作")) found = true;
            }
        }
        check(found, QStringLiteral("⑫12.1 插件动作按钮没出现在 XML 页的操作栏里"));
        QDir(empty).removeRecursively();
    }

    // ⑧ 视频转换页（M2-6f）：8.2 构造期不探测 / 8.3 三按钮小窗非阻塞 /
    // 8.5 状态三态（探测前不显示 ✗）/ 8.4 自动安装挂按钮上且缺件如实报。
    {
        namespace evid = xrkit::engine::video;
        VideoPage page;
        page.resize(720, page_min_h());
        page.show();
        QApplication::processEvents();

        // 8.2/8.5：构造完**一次都没探**，状态是"未检测"（不是 ✗）
        check(page.probe_count() == 0,
              QStringLiteral("⑧8.2 构造期不探测（实测探了 %1 次）").arg(page.probe_count()));
        check(page.status_text().contains(QStringLiteral("未检测")) &&
                  !page.status_text().contains(QStringLiteral("✗")),
              QStringLiteral("⑧8.5 探测之前不显示 ✗（%1）").arg(page.status_text()));

        // 8.2：探测失败也置位（再调不会重探）
        page.detect();
        const int after = page.probe_count();
        check(after == 1, QStringLiteral("⑧8.2 第一次探测记了 1 次（实测 %1）").arg(after));
        page.detect();
        check(page.probe_count() == 1,
              QStringLiteral("⑧8.2 探测失败也被记住（再调不重探，实测 %1 次）")
                  .arg(page.probe_count()));
        check(page.probe_state() == evid::ProbeState::Missing ||
                  page.probe_state() == evid::ProbeState::Found,
              QStringLiteral("⑧8.5 探测后状态落定"));

        // 8.3：三按钮小窗 + **非阻塞**
        QWidget* dialog = page.request_setup();
        check(dialog != nullptr, QStringLiteral("⑧8.3 弹出了设置小窗"));
        if (dialog != nullptr) {
            check(!dialog->isModal(), QStringLiteral("⑧8.3 小窗不是模态（不 grab_set）"));
            check(dialog->isVisible(), QStringLiteral("⑧8.3 小窗 show 出来了（不 wait_window）"));
            const QList<QPushButton*> buttons = dialog->findChildren<QPushButton*>();
            QStringList labels;
            for (QPushButton* button : buttons) labels << button->text();
            check(labels.size() == 3 && labels.contains(QStringLiteral("自动安装")) &&
                      labels.contains(QStringLiteral("手动选择")) &&
                      labels.contains(QStringLiteral("稍后")),
                  QStringLiteral("⑧8.3 三个按钮齐（%1）").arg(labels.join(QStringLiteral("/"))));
        }
        // 8.4：自动安装缺件时如实报（不联网、不静默自救）；用户口径"依赖自带"之后，程序目录里
        //      可能已经有随包自带的 ffmpeg.exe ⇒ 那也是一条**如实结论**
        const QString install = page.auto_install();
        check(install.contains(QStringLiteral("download_ffmpeg.py")) ||
                  install.contains(QStringLiteral("自带")) ||
                  install.contains(QStringLiteral("不可用")) ||
                  install.contains(QStringLiteral("ffmpeg 8.x")),
              QStringLiteral("⑧8.4 自动安装按钮给出如实结论（%1）").arg(install.left(48)));
        // 8.7：手动指定不存在的文件必须被拒（且不写盘）
        QString error;
        const QString ghost = QDir::temp().filePath(QStringLiteral("xrkit_nope_ffmpeg.exe"));
        check(!page.set_manual(ghost, QString(), &error) && !error.isEmpty(),
              QStringLiteral("⑧8.7 手动指定不存在的文件被拒"));
    }

// ═══════════════════════════════════════════════════════════════════════════════════════
// 视频页**补全**（本轮）的自检：引擎早就绪，缺的全是界面接线 ⇒ 每条锁都判"接线真的接上了"。
//   · 两条**单文件**输入行（源 + 参考 OGM）+ 输出行，三行都是整行拖放区；
//   · 「转换为 OGM」主按钮：未选源 ⇒ 灰；参考没落地 ⇒ 灰；两条都落地 ⇒ 亮（1.x `:574-577`/`:708`）；
//   · 输出＝**文件**：默认名按 1.x `_auto_output` 三条规则推导，选择走**保存文件**对话框；
//   · 进度条 + 文案接引擎 `progress_from_line`；
//   · **取消**＝终止进程 + 删半成品，且"删没删掉"要**可见**（用户 2026-10-04：「不能静默报错」）；
//   · 「视频参数」面板＝1.x `_refresh_display` 的字段，值取自引擎 `TranscodePlan`（不是页面自己判阈值）。
//
// ★ 两类锁**分开**（这是打包口径逼出来的）：
//   · "不需要夹具"的一类（结构/置灰/面板/默认名/对话框语义/缺失报路径/自动安装/真 ffprobe 链路）
//     在**任何**环境都要跑 —— 发行包里没有夹具，但[包内自检]也得跑得绿；
//   · "夹具驱动"的一类（进度/取消删半成品/失败带 stderr/跳过计数）要**真起进程**，用的是
//     构建产物 `xrkit_fake_ffmpeg.exe`；它在发行包里**故意不带** ⇒ 缺了要**如实出 note 跳过**
//     （不静默、也不误判成失败）。
// ═══════════════════════════════════════════════════════════════════════════════════════
{
    namespace evid = xrkit::engine::video;
    const QString fake =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("xrkit_fake_ffmpeg.exe"));
    const bool have_fake = QFileInfo::exists(fake);
    if (!have_fake) {
        result.notes.append(QStringLiteral(
            "视频页夹具锁：exe 旁边没有 xrkit_fake_ffmpeg.exe（发行包不带夹具）⇒ 进度/取消/失败/"
            "跳过那几条**夹具驱动的**锁跳过；结构/置灰/面板/默认名/对话框/缺失报路径/真 ffprobe 照跑"));
    }

    // 自检用页面：把"ffmpeg 从哪来"与"两个文件对话框"换成可控实现（三点都是**虚拟缝**，
    // 生产路径与自检走同一份接线 ⇒ 判的是接线，不是桩）。
    class SelfCheckVideoPage : public VideoPage {
    public:
        SelfCheckVideoPage() : VideoPage(nullptr) {}
        evid::FfmpegPaths paths;
        QString open_title, open_filter, open_start, open_pick;
        QString save_start, save_initial, save_pick;
        int open_calls = 0, save_calls = 0;
        // ★ 2026-10-07（审计补充 F1-2）：覆盖确认的**虚拟缝**（默认是 QMessageBox::question，
        //   模态框会挂死自检）—— 生产路径与自检走同一份接线，判的是"问没问、答否时跑没跑"。
        int confirm_calls = 0;
        QString confirm_reason;
        bool confirm_answer = false;

    protected:
        evid::FfmpegPaths resolve_ffmpeg() override { return paths; }
        bool confirm_overwrite(const QString& reason) override {
            ++confirm_calls;
            confirm_reason = reason;
            return confirm_answer;
        }

    public:
        QString ask_open_file(const QString& title, const QString& filter,
                              const QString& start_path) override {
            open_title = title;
            open_filter = filter;
            open_start = start_path;
            ++open_calls;
            return open_pick;
        }
        QString ask_save_output(const QString& start_dir, const QString& initial_name) override {
            save_start = start_dir;
            save_initial = initial_name;
            ++save_calls;
            return save_pick;
        }
    };

    QTemporaryDir vt;
    QDir().mkpath(vt.path() + QStringLiteral("/vidsrc"));
    QDir().mkpath(vt.path() + QStringLiteral("/refdir"));
    const QString src_dir2 = vt.path() + QStringLiteral("/vidsrc");
    const QString ref_dir2 = vt.path() + QStringLiteral("/refdir");
    const QString src_file = src_dir2 + QStringLiteral("/intro.avi");
    // 参考**故意不同主名**：1.x `_auto_output` 在"源与参考同主名同目录"时会退成 `.new.ogm`，
    // 那条专测放在下面（否则这条"落参考目录同主名"会一直被防自毁分支吃掉）。
    const QString ref_file = ref_dir2 + QStringLiteral("/ref.ogm");
    { std::ofstream out(src_file.toStdString(), std::ios::binary); out << "SRC"; }
    { std::ofstream out(ref_file.toStdString(), std::ios::binary); out << "REF"; }

    const auto pump = [](int ms) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    };
    const auto make_info = [](bool video, int w, int h, double fps, long long vbr, bool audio,
                              int ch, int ar, long long abr, double duration) {
        evid::MediaInfo info;
        info.parsed = true;
        info.duration = duration;
        info.size_mb = 1.0;
        info.has_video = video;
        if (video) {
            info.video.codec = "theora";
            info.video.width = w;
            info.video.height = h;
            info.video.fps = fps;
            info.video.bitrate = vbr;
        }
        info.has_audio = audio;
        if (audio) {
            info.audio.codec = "vorbis";
            info.audio.channels = ch;
            info.audio.sample_rate = ar;
            info.audio.bitrate = abr;
        }
        return info;
    };
    // 源：320x240@30，有音轨；参考：640x480@25 + 800kbps + 44.1k/2ch ⇒ 两条都该"将…"
    const evid::MediaInfo src_info = make_info(true, 320, 240, 30.0, 0, true, 2, 48000, 160000, 10.0);
    const evid::MediaInfo ref_info = make_info(true, 640, 480, 25.0, 800000, true, 2, 44100, 128000, 12.0);

    SelfCheckVideoPage page;
    page.resize(820, page_min_h());
    page.show();
    QApplication::processEvents();
    page.paths = evid::find_ffmpeg_default();  // 页面自己"按口径"定位到的那份（可能没有）

    // ① 三条**单文件**输入行 + 三条整行拖放区
    check(page.source_edit() != nullptr && page.reference_edit() != nullptr &&
              page.output_edit() != nullptr,
          QStringLiteral("视频①：源 / 参考 OGM / 输出 三条输入行都在"));
    check(page.drop_zones().zone_count() == 3,
          QStringLiteral("视频①：三行各有拖放区（实测 %1）").arg(page.drop_zones().zone_count()));
    check(page.reference_edit()->objectName() == QStringLiteral("video_reference") &&
              page.output_edit()->placeholderText().contains(QStringLiteral("OGM")) &&
              !page.output_edit()->placeholderText().contains(QStringLiteral("目录")),
          QStringLiteral("视频①：输出是**文件**（占位 %1）").arg(page.output_edit()->placeholderText()));

    // ② 主按钮的置灰口径（1.x：未选源灰；参考没落地也灰）
    check(page.convert_button() != nullptr &&
              page.convert_button()->text() == QStringLiteral("转换为 OGM") &&
              !page.convert_button()->isEnabled(),
          QStringLiteral("视频②：未选源时「转换为 OGM」置灰"));
    page.source_edit()->setText(src_file);
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
    check(page.convert_button()->isEnabled() == false,
          QStringLiteral("视频②：源落地、参考没落地 ⇒ 仍灰（1.x：参考必需）"));
    page.reference_edit()->setText(ref_file);
    page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
    check(page.convert_button()->isEnabled(),
          QStringLiteral("视频②：源 + 参考都落地 ⇒ 主按钮亮"));

    // ③「视频参数」面板：三段 + 1.x 的标记字段（值来自引擎的 plan）
    const QString panel = page.info_panel_text();
    check(panel.contains(QStringLiteral("══ 参考 OGM ══")) &&
              panel.contains(QStringLiteral("══ 源文件 ══")) &&
              panel.contains(QStringLiteral("══ 转换参数（匹配参考 OGM） ══")),
          QStringLiteral("视频③：参数面板三段齐（1.x `_refresh_display`）"));
    check(panel.contains(QStringLiteral("30.00 -> 25.00")) &&
              panel.contains(QStringLiteral("将调整")) &&
              panel.contains(QStringLiteral("320x240 -> 640x480")) &&
              panel.contains(QStringLiteral("将缩放")) &&
              panel.contains(QStringLiteral("800 kbps")) &&
              (panel.contains(QStringLiteral("theora -> libtheora")) ||
               panel.contains(QStringLiteral("theora -> mpeg4"))),
          QStringLiteral("视频③：对比行要给出引擎的结论（含将缩放/将调整/编码器）"));
    check(panel.contains(QStringLiteral("  文件: intro.avi")) &&
              panel.count(QStringLiteral("  文件: ")) == 2,
          QStringLiteral("视频③：源/参考各只出一行「文件:」（1.x 的面板形态）"));
    // 同尺寸同帧率 ⇒ 两条都该报 OK（阈值判据在引擎里，页面不许再判一遍）
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source,
                            make_info(true, 640, 480, 25.0, 0, true, 2, 48000, 160000, 10.0),
                            QString());
    const QString same = page.info_panel_text();
    check(same.contains(QStringLiteral("640x480 -> 640x480  [OK]")) &&
              same.contains(QStringLiteral("25.00 -> 25.00  [OK]")) &&
              !same.contains(QStringLiteral("将缩放")) && !same.contains(QStringLiteral("将调整")),
          QStringLiteral("视频③：同尺寸/同帧率 ⇒ [OK]（与 argv 同一判据）"));
    page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());

    // ④ 输出默认名（1.x `_auto_output` 三条规则）
    const QString want_default =
        QDir::toNativeSeparators(QDir::cleanPath(ref_dir2 + QStringLiteral("/intro.ogm")));
    check(QDir::cleanPath(page.default_output_path()) == QDir::cleanPath(want_default),
          QStringLiteral("视频④：有参考 ⇒ 输出落**参考所在目录**同主名（实得 %1）")
              .arg(page.default_output_path()));
    // 源与参考同目录同主名 ⇒ 默认值会砸到参考文件本身 ⇒ 必须改成 .new.ogm（1.x 的防自毁）
    const QString clash_ref = src_dir2 + QStringLiteral("/intro.ogm");
    { std::ofstream out(clash_ref.toStdString(), std::ios::binary); out << "REF2"; }
    page.reference_edit()->setText(clash_ref);
    page.apply_probe_result(clash_ref, VideoPage::ProbeTarget::Reference, ref_info, QString());
    check(QDir::cleanPath(page.default_output_path()) ==
              QDir::cleanPath(QDir::toNativeSeparators(src_dir2 + QStringLiteral("/intro.new.ogm"))),
          QStringLiteral("视频④：默认输出不许指向参考文件本身（实得 %1）")
              .arg(page.default_output_path()));
    page.reference_edit()->setText(ref_file);
    page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
    page.output_edit()->clear();
    check(QDir::cleanPath(page.default_output_path()) == QDir::cleanPath(want_default),
          QStringLiteral("视频④：默认名只由源+参考决定"));

    // ⑤「选择…」：源/参考是**打开文件**、输出是**保存文件**，起点走 resolve_dialog_start（唯一源目录）
    const std::vector<QWidget*>& zones = page.drop_zones().containers();
    const auto browse_of = [&zones](size_t index) -> QPushButton* {
        return index < zones.size() ? zones[index]->findChild<QPushButton*>() : nullptr;
    };
    check(browse_of(0) != nullptr && browse_of(1) != nullptr && browse_of(2) != nullptr,
          QStringLiteral("视频⑤：三行都有「选择…」按钮"));
    page.open_pick = src_file;
    if (browse_of(0) != nullptr) browse_of(0)->click();
    check(page.open_calls == 1 && page.open_title.contains(QStringLiteral("源视频")) &&
              page.open_filter.contains(QStringLiteral("*.mp4")) &&
              page.source_edit()->text() == QDir::toNativeSeparators(src_file),
          QStringLiteral("视频⑤：源行走**打开文件**对话框（标题 %1）").arg(page.open_title));
    page.open_pick = ref_file;
    if (browse_of(1) != nullptr) browse_of(1)->click();
    check(page.open_title.contains(QStringLiteral("参考 OGM")) &&
              page.open_filter.contains(QStringLiteral("*.ogm")) &&
              page.reference_edit()->text() == QDir::toNativeSeparators(ref_file),
          QStringLiteral("视频⑤：参考行走**打开文件**对话框（过滤器 %1）").arg(page.open_filter));
    reset_last_dialog_start();
    page.output_edit()->clear();
    pump(200);
    page.save_pick = QDir::toNativeSeparators(vt.path() + QStringLiteral("/picked.ogm"));
    if (browse_of(2) != nullptr) browse_of(2)->click();
    check(page.save_calls == 1 && page.save_initial.endsWith(QStringLiteral(".ogm")) &&
              page.output_edit()->text() == page.save_pick,
          QStringLiteral("视频⑤：输出走**保存文件**对话框（默认名 %1）").arg(page.save_initial));
    check(QDir::cleanPath(page.save_start) == QDir::cleanPath(src_dir2) &&
              last_dialog_start_calls() == 1,
          QStringLiteral("视频⑤：保存起点 = **唯一输入所在目录**（%1，走共用函数 %2 次）")
              .arg(page.save_start)
              .arg(last_dialog_start_calls()));

    // ⑥ 真 ffprobe 链路（按 ⑧8.1 能定位到 ffmpeg/ffprobe 时才跑）：
    //    造 1 秒样本 → 页面的"选文件 → 探测 → 面板"这条链 → 面板要出**真**编码/尺寸。
    //    发行包自带 ffmpeg/ffprobe ⇒ 这条在**包内自检**里也成立（＝"不填路径也能用"的判据）。
    {
        const evid::FfmpegPaths real = evid::find_ffmpeg_default();
        if (!real.found || !real.has_probe) {
            result.notes.append(QStringLiteral(
                "视频⑥：本机按口径定位不到 ffmpeg/ffprobe（只认 user.ltx 与程序目录）⇒ "
                "真 ffprobe 链路跳过，属人工验收项"));
        } else {
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_video_page"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString in_path = dir + QStringLiteral("/in.avi");
            QProcess gen;
            gen.start(QString::fromStdString(real.ffmpeg),
                      QStringList{QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("lavfi"),
                                  QStringLiteral("-i"), QStringLiteral("testsrc=d=1"),
                                  QStringLiteral("-an"), in_path});
            gen.waitForFinished(120000);
            const bool made = gen.exitCode() == 0 && QFile::exists(in_path);
            check(made, QStringLiteral("视频⑥：造 1 秒真样本失败（退出码 %1）").arg(gen.exitCode()));
            if (made) {
                SelfCheckVideoPage real_page;
                real_page.paths = real;
                real_page.source_edit()->setText(in_path);  // 真接线：填框 → textChanged → 真 ffprobe
                QElapsedTimer clock;
                clock.start();
                while (!real_page.source_ready() && clock.elapsed() < 30000) {
                    QCoreApplication::processEvents();
                    QThread::msleep(10);
                }
                check(real_page.source_ready(),
                      QStringLiteral("视频⑥：真 ffprobe 走「选文件→探测」这条链（面板 %1）")
                          .arg(real_page.info_panel_text().left(60)));
                const QString real_panel = real_page.info_panel_text();
                check(real_panel.contains(QStringLiteral("320x240")) &&
                          (real_panel.contains(QStringLiteral("mpeg4")) ||
                           real_panel.contains(QStringLiteral("h264"))),
                      QStringLiteral("视频⑥：面板要显示真 ffprobe 给的尺寸/编码（%1）")
                          .arg(real_panel.left(80)));
            }
            QDir(dir).removeRecursively();
        }
    }

    // ── 以下几条要**真起进程**（夹具当 ffmpeg/ffprobe）；发行包不带夹具 ⇒ 缺了就 note 跳过 ──
    if (have_fake) {
        // ⑦ 按钮 → argv → 进程 → 进度 → 跑完收干净
        page.paths.found = true;
        page.paths.has_probe = true;
        page.paths.ffmpeg = fake.toStdString();
        page.paths.ffprobe = fake.toStdString();
        page.apply_file(src_file, false);  // 起编码器扫描（夹具的 `-encoders`）
        {
            QElapsedTimer clock;
            clock.start();
            while (!page.encoders_scanned() && clock.elapsed() < 15000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
        }
        check(page.encoders_scanned(),
              QStringLiteral("视频⑦：编码器表要扫到结论（含「扫不到」也要有结论）"));
        // 编码器表有结论之后，面板里的编码器必须是**引擎挑的那个**
        page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
        page.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
        pump(200);
        check(page.info_panel_text().contains(QStringLiteral("theora -> libtheora")),
              QStringLiteral("视频⑦：夹具编码器表在 ⇒ 面板报 libtheora（%1）")
                  .arg(page.info_panel_text().left(80)));
        const QString out_file = vt.path() + QStringLiteral("/out.ogm");
        QFile::remove(out_file);
        page.output_edit()->setText(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "finish");
        QElapsedTimer vclock;
        vclock.start();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (!page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.transcode_running(),
              QStringLiteral("视频⑦：点「转换为 OGM」要真的起进程（路径＝夹具）"));
        while (page.progress_percent() == 0 && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.progress_percent() > 0 && page.progress_bar() != nullptr &&
                  page.progress_bar()->value() == page.progress_percent() &&
                  page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("%")),
              QStringLiteral("视频⑦：进度条/文案跟着 ffmpeg 的 time= 走（实得 %1）")
                  .arg(page.progress_percent()));
        while (page.transcode_running() && vclock.elapsed() < 60000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(!page.transcode_running() && page.progress_percent() == 100 &&
                  page.progress_bar() != nullptr && page.progress_bar()->value() == 100 &&
                  page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("完成")),
              QStringLiteral("视频⑦：跑完进度到 100、状态报「完成」"));
        check(QFile::exists(out_file) && QFileInfo(out_file).size() > 0,
              QStringLiteral("视频⑦：产物存在（夹具写的）"));
        check(page.convert_button() != nullptr && page.convert_button()->isEnabled() &&
                  page.cancel_button() != nullptr && !page.cancel_button()->isVisible(),
              QStringLiteral("视频⑦：跑完主按钮恢复、取消收起"));
        {
            QElapsedTimer wait;
            wait.start();
            while (!page.info_panel_text().contains(QStringLiteral("══ 输出文件（本次转换结果） ══")) &&
                   wait.elapsed() < 10000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
            check(page.info_panel_text().contains(QStringLiteral("══ 输出文件（本次转换结果） ══")) &&
                      page.info_panel_text().contains(QStringLiteral("vorbis")),
                  QStringLiteral("视频⑦：转换完成后**再解析一次产物**并贴到面板（1.x）"));
        }

        // ⑧ 取消：真停 + **删半成品** + 理由可见（用户红线：不能静默）
        QFile::remove(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "loop");
        vclock.restart();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (!page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        while ((page.progress_percent() == 0 || !QFile::exists(out_file)) &&
               vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.transcode_running() && page.progress_percent() > 0 && QFile::exists(out_file),
              QStringLiteral("视频⑧：取消前——进程在跑、进度在走、半成品已产生"));
        check(page.cancel_button() != nullptr && page.cancel_button()->isVisible(),
              QStringLiteral("视频⑧：跑起来时取消按钮出现在主按钮左边"));
        if (page.cancel_button() != nullptr) page.cancel_button()->click();
        vclock.restart();
        while (page.transcode_running() && vclock.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(!page.transcode_running(), QStringLiteral("视频⑧：取消要**真停**（进程没了）"));
        check(!QFile::exists(out_file),
              QStringLiteral("视频⑧：取消要**删掉半成品**（1.x：不留「看着像成功」的输出）"));
        const QString cancel_msg = page.last_error();
        check(cancel_msg.contains(QStringLiteral("已取消")) &&
                  cancel_msg.contains(QStringLiteral("已删除半成品")) &&
                  cancel_msg.contains(QFileInfo(out_file).fileName()),
              QStringLiteral("视频⑧：取消理由要可见（含文件名）——实得 %1").arg(cancel_msg));
        check(page.progress_percent() == 0 && page.progress_bar() != nullptr &&
                  page.progress_bar()->value() == 0 && page.progress_label() != nullptr &&
                  page.progress_label()->text() == cancel_msg &&
                  page.convert_button() != nullptr && page.convert_button()->isEnabled() &&
                  page.cancel_button() != nullptr && !page.cancel_button()->isVisible(),
              QStringLiteral("视频⑧：取消后进度归零、文案＝取消理由、按钮恢复"));

        // ⑨ 失败：**ffmpeg 的 stderr 原文**要带出来（不能只给"失败"）
        QFile::remove(out_file);
        qputenv("XRKIT_FAKE_FFMPEG_MODE", "fail");
        vclock.restart();
        if (page.convert_button() != nullptr) page.convert_button()->click();
        while (page.transcode_running() && vclock.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        check(page.last_error().contains(QStringLiteral("退出码")) &&
                  page.last_error().contains(QStringLiteral("boom-invalid-input")),
              QStringLiteral("视频⑨：失败理由要带 stderr 原文（实得 %1）")
                  .arg(page.last_error().left(90)));
        check(page.progress_label() != nullptr &&
                  page.progress_label()->text().contains(QStringLiteral("失败")) &&
                  page.info_panel_text().contains(QStringLiteral("错误原文")),
              QStringLiteral("视频⑨：失败在状态栏与面板上都可见"));
        qunsetenv("XRKIT_FAKE_FFMPEG_MODE");

        // ⑬ ★ 2026-10-07（审计补充 F1-2 / F1-3）：**起跑前的输出目标守卫**。
        //   argv 里恒有 `-y` ⇒ 覆盖是静默的。判据全在"拒绝/确认"这两件事上：
        //     ① 输出＝源 ⇒ 直接拒（连问都不问），**源文件一个字节都不许动**；
        //     ② 输出＝参考 ⇒ 同样直接拒（参考也是输入）；
        //     ③ 输出是别的已存在文件 ⇒ 必须**问一次**；答否 ⇒ 不跑、理由可见、既有文件原样；
        //     ④ 答是 ⇒ 才真起进程（覆盖是有代价的动作，必须先点过头）。
        {
            const QString ogm_src = src_dir2 + QStringLiteral("/self.ogm");
            { std::ofstream out(ogm_src.toStdString(), std::ios::binary); out << "OGMSRC"; }
            page.apply_probe_result(ogm_src, VideoPage::ProbeTarget::Source, src_info, QString());
            page.output_edit()->setText(ogm_src);
            page.confirm_calls = 0;
            if (page.convert_button() != nullptr) page.convert_button()->click();
            check(!page.transcode_running() && page.confirm_calls == 0,
                  QStringLiteral("视频⑬：输出＝源 ⇒ **直接拒**（不跑、也不问要不要覆盖；问了 %1 次）")
                      .arg(page.confirm_calls));
            check(page.last_error().contains(QStringLiteral("源文件")),
                  QStringLiteral("视频⑬：输出＝源的理由要可见（实得 %1）")
                      .arg(page.last_error().left(90)));
            check(page.progress_label() != nullptr &&
                      page.progress_label()->text().contains(QStringLiteral("失败")),
                  QStringLiteral("视频⑬：拒绝理由在进度文案上也可见"));
            check(QFileInfo(ogm_src).size() == 6,
                  QStringLiteral("视频⑬：拒绝之后源文件**原样**（%1 字节）")
                      .arg(QFileInfo(ogm_src).size()));

            page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
            page.output_edit()->setText(ref_file);
            page.confirm_calls = 0;
            if (page.convert_button() != nullptr) page.convert_button()->click();
            check(!page.transcode_running() && page.confirm_calls == 0 &&
                      page.last_error().contains(QStringLiteral("参考")),
                  QStringLiteral("视频⑬：输出＝参考 ⇒ 直接拒（实得 %1）")
                      .arg(page.last_error().left(90)));

            const QString exists_out = vt.path() + QStringLiteral("/already.ogm");
            { std::ofstream out(exists_out.toStdString(), std::ios::binary); out << "KEEP"; }
            page.output_edit()->setText(exists_out);
            page.confirm_answer = false;
            page.confirm_calls = 0;
            if (page.convert_button() != nullptr) page.convert_button()->click();
            check(page.confirm_calls == 1 && !page.transcode_running(),
                  QStringLiteral("视频⑬：输出已存在 ⇒ **问一次**（问了 %1 次、跑了=%2）")
                      .arg(page.confirm_calls)
                      .arg(page.transcode_running() ? 1 : 0));
            check(page.confirm_reason.contains(QFileInfo(exists_out).fileName()),
                  QStringLiteral("视频⑬：确认文案要带**哪个文件**（实得 %1）")
                      .arg(page.confirm_reason.left(90)));
            check(page.last_error().contains(QStringLiteral("已取消")) &&
                      page.last_error().contains(QStringLiteral("不覆盖")),
                  QStringLiteral("视频⑬：答否 ⇒ 可见的「不覆盖」结论（实得 %1）")
                      .arg(page.last_error().left(90)));
            check(QFileInfo(exists_out).size() == 4,
                  QStringLiteral("视频⑬：答否之后既有文件**原样**（%1 字节）")
                      .arg(QFileInfo(exists_out).size()));

            qputenv("XRKIT_FAKE_FFMPEG_MODE", "finish");
            page.confirm_answer = true;
            page.confirm_calls = 0;
            QElapsedTimer guard_clock;
            guard_clock.start();
            if (page.convert_button() != nullptr) page.convert_button()->click();
            while (!page.transcode_running() && guard_clock.elapsed() < 20000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
            check(page.confirm_calls == 1 && page.transcode_running(),
                  QStringLiteral("视频⑬：答是 ⇒ 才真起进程（问了 %1 次、跑了=%2）")
                      .arg(page.confirm_calls)
                      .arg(page.transcode_running() ? 1 : 0));
            while (page.transcode_running() && guard_clock.elapsed() < 60000) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
            qunsetenv("XRKIT_FAKE_FFMPEG_MODE");
            page.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
        }

        // ⑫ 跳过要**计数 + 逐条原因**（用户红线）
        {
            SelfCheckVideoPage busy;
            busy.resize(820, page_min_h());
            busy.paths = page.paths;
            busy.apply_file(src_file, false);   // 起进程（异步）
            busy.apply_file(ref_file, true);    // 忙 ⇒ 跳过并留痕
            check(busy.skipped_count() >= 1 && !busy.skip_reasons().isEmpty(),
                  QStringLiteral("视频⑫：跳过要计数（%1 项）").arg(busy.skipped_count()));
            check(busy.info_panel_text().contains(QStringLiteral("跳过")),
                  QStringLiteral("视频⑫：跳过要逐条可见（面板）"));
            pump(500);
        }
    }

    // ⑩ 找不到 ffmpeg：要说明**试过哪些路径**（不能只灰着、不能只给一行泛泛的话）
    {
        SelfCheckVideoPage bare;  // paths 全空 ⇒ resolve_ffmpeg 报"没找到"
        bare.resize(820, page_min_h());
        bare.source_edit()->setText(src_file);
        bare.apply_probe_result(src_file, VideoPage::ProbeTarget::Source, src_info, QString());
        bare.reference_edit()->setText(ref_file);
        bare.apply_probe_result(ref_file, VideoPage::ProbeTarget::Reference, ref_info, QString());
        check(bare.convert_button() != nullptr && bare.convert_button()->isEnabled(),
              QStringLiteral("视频⑩：ffmpeg 缺失**不是**主按钮置灰的理由（灰＝说不清）"));
        if (bare.convert_button() != nullptr) bare.convert_button()->click();
        const QString miss = bare.last_error();
        check(miss.contains(QStringLiteral("找不到 ffmpeg")) &&
                  miss.contains(QStringLiteral("user.ltx")) &&
                  miss.contains(QStringLiteral("程序目录")) &&
                  miss.contains(QStringLiteral("不搜 PATH")),
              QStringLiteral("视频⑩：缺失理由要含**试过哪些路径**（实得 %1）").arg(miss.left(110)));
        check(bare.progress_label() != nullptr &&
                  bare.progress_label()->text().contains(QStringLiteral("失败")),
              QStringLiteral("视频⑩：缺失在进度文案上也可见"));
        // 状态栏三态：探测之前是"未检测"（**不是 ✗**）；探测之后按**真实结论**报
        //（有的机器/包目录里恰好有实物 ffmpeg ⇒ 两种结论都要如实、都要带路径信息）
        bare.detect();
        const QString state_text = bare.status_text();
        if (bare.probe_state() == evid::ProbeState::Missing) {
            check(state_text.contains(QStringLiteral("✗")) &&
                      state_text.contains(QStringLiteral("试过")),
                  QStringLiteral("视频⑩：找不到时状态栏要报出试过的路径（%1）")
                      .arg(state_text.left(90)));
        } else {
            check(state_text.contains(QStringLiteral("✓")) &&
                      state_text.contains(QStringLiteral("ffmpeg")),
                  QStringLiteral("视频⑩：找得到时状态栏要报**哪一份** ffmpeg（%1）")
                      .arg(state_text.left(90)));
        }

        // ⑪ 自动安装按钮**不再是空壳**：点一下要真做事、结论可见
        QWidget* setup = bare.request_setup();
        QPushButton* install_button = nullptr;
        if (setup != nullptr) {
            for (QPushButton* candidate : setup->findChildren<QPushButton*>()) {
                if (candidate->text() == QStringLiteral("自动安装")) install_button = candidate;
            }
        }
        check(install_button != nullptr, QStringLiteral("视频⑪：设置小窗里有「自动安装」"));
        if (install_button != nullptr) install_button->click();
        const QString after = bare.status_label() != nullptr ? bare.status_label()->text() : QString();
        check(!after.trimmed().isEmpty() &&
                  (after.contains(QStringLiteral("自动安装")) ||
                   after.contains(QStringLiteral("自带")) ||
                   after.contains(QStringLiteral("download_ffmpeg.py"))),
              QStringLiteral("视频⑪：点「自动安装」要给出**结论**（%1）").arg(after.left(70)));
    }
}

    // ⑪ 汉化包生成页（M2-6g）：11.8 四态标记由引擎给 / 11.9 缺字形**先判后建**且不留半成品。
    {
        namespace fnt = xrkit::engine::font;
        FontPackPage page;
        page.resize(760, page_min_h());
        page.show();
        QApplication::processEvents();

        const QString msyh = QStringLiteral(
            "E:/Software/Games/STALKER/Localization/Tools/otherTools/CN_Pack_Generator/msyh.ttf");
        const QString jpan = QStringLiteral("C:/Windows/Fonts/hpsimplifiedjpan-regular.ttf");
        const QString latin = QStringLiteral("C:/Windows/Fonts/arial.ttf");
        const auto exists = [](const QString& path) { return QFileInfo::exists(path); };

        // 11.8：标记逐字等于引擎的判据（页面不许自己再判一遍）
        if (exists(msyh)) {
            check(page.font_state_text(msyh) ==
                      QString::fromUtf8(fnt::cjk_state_mark(fnt::CjkFontState::Ok)),
                  QStringLiteral("⑪11.8 简体字体标记 = 引擎判据（%1）").arg(page.font_state_text(msyh)));
        }
        if (exists(jpan)) {
            const QString mark = page.font_state_text(jpan);
            check(mark.startsWith(QString::fromUtf8(fnt::cjk_state_mark(fnt::CjkFontState::Japanese))) &&
                      mark.contains(QStringLiteral("可用繁体顶")),
                  QStringLiteral("⑪11.8 日语字体标记 + 繁体回退计数（%1）").arg(mark));
        }
        check(page.font_state_text(QString()).contains(QStringLiteral("未选择字体")),
              QStringLiteral("⑪11.8 没选字体时不乱下结论"));

        // ★★ 2026-10-06（审计处理批·0-3 之 B1-8）：上面三条锁（11.8 简体/日语、11.9 拒绝生成）
        //   都包在 `if (exists(…))` 里，而 `msyh` 指向的是**仓库之外**开发机私有路径
        //   （`…\otherTools\CN_Pack_Generator\msyh.ttf`）⇒ 换台机器三条锁**静默消失**，
        //   连 `notes` 里都查不到（全文件其余条件跳过都写 note）。现在：
        //     ① **三个夹具一个都没有** ⇒ 判红（"判据一条都没跑"不是通过）；
        //     ② 每一个缺件都写进 `result.notes`（可见），缺哪个说哪个。
        const bool have_msyh = exists(msyh);
        const bool have_jpan = exists(jpan);
        const bool have_latin = exists(latin);
        if (!have_msyh) {
            result.notes.append(QStringLiteral("⑪11.8 简体字体夹具缺件（%1）⇒ 该条锁**未覆盖**")
                                    .arg(msyh));
        }
        if (!have_jpan) {
            result.notes.append(QStringLiteral("⑪11.8 日语字体夹具缺件（%1）⇒ 该条锁**未覆盖**")
                                    .arg(jpan));
        }
        if (!have_latin) {
            result.notes.append(QStringLiteral("⑪11.9 拒绝生成夹具缺件（%1）⇒ 该条锁**未覆盖**")
                                    .arg(latin));
        }
        check(have_msyh || have_jpan || have_latin,
              QStringLiteral("⑪11.8/11.9 字体夹具**一个都没找到** ⇒ 这一组锁全部没跑（见 notes）"));

        // 11.9：缺字形**拒绝式检查、在建目录之前** —— 产物目录不许被建出来
        if (exists(latin)) {
            const QString out = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fontpack_out"));
            QDir(out).removeRecursively();
            QString reason;
            const bool built = page.build(QDir::temp().filePath(QStringLiteral("xrkit_no_xml")),
                                          latin, out, &reason);
            check(!built && reason.contains(QStringLiteral("拒绝生成")),
                  QStringLiteral("⑪11.9 缺字形被拒（%1）").arg(reason));
            check(!QDir(out).exists() && page.refused_count() >= 1,
                  QStringLiteral("⑪11.9 拒绝时**没有**创建输出目录"));
            QDir(out).removeRecursively();
        }

        // ★★ 2026-10-10（P17-1，审计 03 F-01 **Critical**）：**worker 不许碰控件**的可判形态。
        //
        // 修前事实：点「生成汉化包」⇒ `task_->run` 的 worker **直接**调 `FontPackPage::build()`；
        // 而 `build()` 里 `options_for()` 读下拉框/勾选框、又对 `result_label_`/`state_label_`
        // 写文本、还裸 `++refused_count_` ⇒ 跨线程碰 QWidget ＋ 数据竞争；页析构时
        // `result_label_`（页的子控件，**先于** `task_` 被删）正好撞上 worker 的写 ⇒ 访问违例。
        //
        // 判据（四条一起量）：工作线程里调那两个入口 ⇒
        //   ① UI 线程守卫**逐条记账**（计数 +2，最近一条是 `options_for`）；
        //   ② 控件文本与拒绝计数**一个字都不许变**；
        //   ③ 读控件那条路在 worker 里**读不到**值（主线程读得到同一个值 —— 一正一反）；
        //   ④ 拒绝理由可见（含「线程纪律」），且**没有**目录被建出来。
        {
            check(page.result_label() != nullptr && page.game_combo() != nullptr,
                  QStringLiteral("P17-1 前提：拿得到 fontpack 页的结果标签与游戏下拉"));
            const int violations_before = ui_thread_violation_count();
            const QString label_before =
                page.result_label() != nullptr ? page.result_label()->text() : QString();
            const int refused_before = page.refused_count();
            const QString out = QDir::temp().filePath(QStringLiteral("xrkit_smoke_fontpack_uithread"));
            QDir(out).removeRecursively();
            // 主线程先把游戏下拉拨到下标 2（"CoP"）—— 下面用它做"读得到 / 读不到"的对拍
            if (page.game_combo() != nullptr) page.game_combo()->setCurrentIndex(2);
            const std::string game_on_main =
                page.options_for(QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")).game;
            check(game_on_main == "CoP",
                  QStringLiteral("P17-1 前提：主线程读控件读得到当前下拉值（实得 %1）")
                      .arg(QString::fromStdString(game_on_main)));

            bool worker_built = true;
            QString reason;
            std::string worker_game;
            std::thread worker([&] {
                reason.clear();
                worker_built = page.build(QStringLiteral("(none)"),
                                          QStringLiteral("C:/nonexistent-font.ttf"), out, &reason);
                worker_game = page.options_for(QStringLiteral("x"), QStringLiteral("y"),
                                               QStringLiteral("z"))
                                  .game;
            });
            worker.join();

            check(!worker_built && reason.contains(QStringLiteral("线程纪律")),
                  QStringLiteral("P17-1 03 F-01：worker 里调 `FontPackPage::build` ⇒ 被守卫拒绝"
                                 "（理由含「线程纪律」；实得 built=%1 / reason=%2）")
                      .arg(worker_built ? QStringLiteral("true") : QStringLiteral("false"))
                      .arg(reason));
            check(worker_game != "CoP",
                  QStringLiteral("P17-1 03 F-01：worker 里调 `options_for` **读不到**控件值"
                                 "（实得 %1；修前它会读到 CoP ⇒ 跨线程读 QWidget）")
                      .arg(QString::fromStdString(worker_game)));
            check(page.result_label() != nullptr && page.result_label()->text() == label_before &&
                      page.refused_count() == refused_before && !QDir(out).exists(),
                  QStringLiteral("P17-1 03 F-01：被拒之后控件文本 / 拒绝计数 / 落盘目录**都没变**"
                                 "（标签 %1；拒绝计数 %2→%3）")
                      .arg(label_before == (page.result_label() != nullptr
                                                ? page.result_label()->text()
                                                : QString())
                               ? QStringLiteral("未变")
                               : QStringLiteral("被改了"))
                      .arg(refused_before)
                      .arg(page.refused_count()));
            check(ui_thread_violation_count() == violations_before + 2 &&
                      ui_thread_last_violation() == QStringLiteral("FontPackPage::options_for"),
                  QStringLiteral("P17-1 03 F-01：UI 线程守卫逐条记账（%1→%2，最近一条=%3）")
                      .arg(violations_before)
                      .arg(ui_thread_violation_count())
                      .arg(ui_thread_last_violation()));
            QDir(out).removeRecursively();
        }
    }

    // ⑫12.2/12.3：`api.ui` 的 **16 方法 Qt 实现** —— 槽位齐全，且真造得出控件。
    // （三个标准对话框会阻塞，自检只核对槽位非空。）
    {
        const stk_ui* ui = qt_plugin_ui();
        check(ui != nullptr, QStringLiteral("⑫12.2 外壳没有提供 api.ui"));
        // `stk_ui` 是"纯函数指针的 POD"，按指针数组扫一遍就知道有没有空洞
        const void* const* slot_table = reinterpret_cast<const void* const*>(ui);
        const size_t slot_count = sizeof(stk_ui) / sizeof(void*);
        int holes = 0;
        for (size_t i = 0; i < slot_count; ++i) {
            if (slot_table[i] == nullptr) ++holes;
        }
        check(holes == 0 && slot_count == 16,
              QStringLiteral("⑫12.3 api.ui 应有 16 个非空槽位（实测 %1 个槽、%2 个空洞）")
                  .arg(slot_count)
                  .arg(holes));

        QWidget host;
        host.resize(320, 240);
        stk_host* handle = reinterpret_cast<stk_host*>(&host);
        stk_widget* root_widget = reinterpret_cast<stk_widget*>(&host);
        stk_widget* label = ui->label(handle, root_widget, "标题", nullptr);
        stk_widget* button = ui->button(handle, root_widget, "按钮", nullptr, nullptr, nullptr, 1);
        stk_widget* box = ui->checkbox(handle, root_widget, "勾上", nullptr, nullptr);
        stk_widget* entry = ui->entry(handle, root_widget, "初值", 20, nullptr);
        stk_widget* text = ui->text(handle, root_widget, 6, 0, "word");
        const char* values[] = {"甲", "乙"};
        stk_widget* combo = ui->combo(handle, root_widget, values, 2, nullptr, nullptr);
        stk_widget* drop = ui->drop_box(handle, root_widget, nullptr, nullptr, "拖进来", 0);
        stk_widget* row = ui->row(handle, root_widget, "top", "x", 0, 0, 0);
        stk_widget* view = ui->text_view(handle, root_widget, 8, 1);
        check(label != nullptr && button != nullptr && box != nullptr && entry != nullptr &&
                  text != nullptr && combo != nullptr && drop != nullptr && row != nullptr &&
                  view != nullptr,
              QStringLiteral("⑫12.2 九种控件没有全部造出来"));
        auto* label_widget = reinterpret_cast<QLabel*>(label);
        ui->set_text(handle, label, "改过的标题");
        check(label_widget != nullptr && label_widget->text() == QStringLiteral("改过的标题"),
              QStringLiteral("⑫12.2 set_text 没改到控件上"));
        ui->set_state(handle, button, 0);
        check(!reinterpret_cast<QWidget*>(button)->isEnabled(),
              QStringLiteral("⑫12.2 set_state 没把控件置灰"));
        check(ui->alive(handle, label) == 1 && ui->alive(handle, nullptr) == 0,
              QStringLiteral("⑫12.2 alive 判断不对"));
        ui->pack(handle, root_widget, label, "top", "x", 0, 0, 0);
        check(host.layout() != nullptr && host.layout()->count() >= 1,
              QStringLiteral("⑫12.2 pack 没把控件挂进布局"));
    }

    // ⑫12.4：插件加载失败**不阻塞启动**，且问题要在 Hub 启动时可见（日志 + 状态栏）。
    {
        const QString empty_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plugins_empty"));
        QDir(empty_dir).removeRecursively();
        QDir().mkpath(empty_dir);
        {
            Hub quiet(nullptr, empty_dir);
            check(quiet.plugin_errors().empty(),
                  QStringLiteral("⑫12.4 空插件目录不该报错（%1）").arg(quiet.plugin_notice()));
            check(quiet.plugin_notice().contains(QStringLiteral("无")),
                  QStringLiteral("⑫12.4 空目录的启动提示不对（%1）").arg(quiet.plugin_notice()));
        }

        const QString bad_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_plugins_bad"));
        QDir(bad_dir).removeRecursively();
        QDir().mkpath(bad_dir);
        {
            // 一个"看起来像插件但其实不是 DLL"的文件 ⇒ LoadLibrary 必失败
            std::ofstream out((bad_dir + QStringLiteral("/not_a_plugin.dll")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out << "this is not a PE file";
        }
        {
            Hub loud(nullptr, bad_dir);
            check(!loud.plugin_errors().empty(),
                  QStringLiteral("⑫12.4 坏插件必须留下 registration_errors"));
            check(loud.plugin_notice().contains(QStringLiteral("问题")),
                  QStringLiteral("⑫12.4 状态栏/提示要写出有问题（%1）").arg(loud.plugin_notice()));
            if (loud.log_box() != nullptr) loud.log_box()->flush();
            check(loud.log_view() != nullptr &&
                      loud.log_view()->toPlainText().contains(QStringLiteral("插件问题")),
                  QStringLiteral("⑫12.4 问题要打进日志栏（启动即可见）"));
            check(loud.plugin_ui_installed(),
                  QStringLiteral("⑫12.2 加载插件时必须把外壳的 api.ui 装上"));
            check(loud.page_count() == builtin_page_count(),
                  QStringLiteral("⑫12.4 插件坏了也不许影响内置六页"));
        }
        QDir(empty_dir).removeRecursively();
        QDir(bad_dir).removeRecursively();
    }

    // ⑬ ★★ Windows 版本资源（VERSIONINFO）——用户 2026-10-04 口径：exe "完全没有元数据、也没有签名"
    //   是杀软误报的一半根因。判据落在**真实资源字节**上（读本进程 exe 的资源，不是看源码里写没写 .rc）：
    //     · 三字段非空：ProductName / FileDescription / FileVersion —— 资源没挂上/挂错目标就红；
    //     · FileVersion == core::version_string() 的版本号部分 —— **同一个进程里现取现比**：
    //       在 .rc 里手写一个旧版本串 ⇒ 这里立刻红（.rc 与 version.h 的漂移另由门禁
    //       `gen_winres.py --check` 拦；兄弟产物 CLI / 构建器由门禁 [30] 一起查）。
    //       用户原话："版本只有一个事实来源，别处不许复制版本串"。
#if defined(_WIN32)
    {
        const QString product = exe_version_field(QStringLiteral("ProductName"));
        const QString description = exe_version_field(QStringLiteral("FileDescription"));
        const QString file_version = exe_version_field(QStringLiteral("FileVersion"));
        const QString original = exe_version_field(QStringLiteral("OriginalFilename"));
        check(!product.isEmpty() && !description.isEmpty() && !file_version.isEmpty(),
              QStringLiteral("⑬ 版本资源三字段非空（ProductName=%1 / FileDescription=%2 / "
                             "FileVersion=%3）")
                  .arg(product.isEmpty() ? QStringLiteral("(空)") : product,
                       description.isEmpty() ? QStringLiteral("(空)") : description,
                       file_version.isEmpty() ? QStringLiteral("(空)") : file_version));
        const QString compiled = QString::fromStdString(xrkit::core::version_string());
        const QString semver = compiled.section(QLatin1Char(' '), 0, 0);
        check(!semver.isEmpty() && file_version == semver,
              QStringLiteral("⑬ FileVersion 与 core::version_string() 的版本号部分一致"
                             "（资源=%1 / 编译进去的版本串=%2 ⇒ 版本号部分=%3）")
                  .arg(file_version, compiled, semver));
        // 挂错目标的资源（把 CLI 的那份挂到本 exe 上）⇒ OriginalFilename 对不上，当场红
        const QString self_name = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        check(original.compare(self_name, Qt::CaseInsensitive) == 0,
              QStringLiteral("⑬ OriginalFilename 是本 exe 的名字（资源=%1 / 实际=%2）")
                  .arg(original, self_name));
        result.notes.append(QStringLiteral("⑬ 版本资源：%1 | %2 | FileVersion=%3 | 原名=%4")
                                .arg(product, description, file_version, original));
    }
#endif

    // ⑭ ★★ 2026-10-05 **像素级锁**：用户对着裁剪图说「那个露出深灰色底板的设计需要重做或者
    //   改掉，很丑」「很多控件会露出深灰色的底板」「很多下拉栏设计太丑了」「勾就勾」。
    //   机制（本轮实测）：泛化规则 `QWidget { background: @{bg}; }` 命中"没写 QSS 的裸容器"
    //   （`path_row()` 的行容器、`TaskShell::panel()`、页视口…），容器自己涂了一层比卡片更深的
    //   灰，又只露出**标签背后那一块** ⇒ 就是那块"缺了一块"的深灰圆角底板。
    //   判据不靠肉眼：**把真实控件渲染成图，逐像素比**。
    //     ① 行容器"标签区"的底色 == 卡片底色（`surface2`）—— 改回旧样式立刻红；
    //     ② 同类容器（TaskShell 面板）里不许存在"整块 == `bg`"的填充；
    //     ③ 下拉框 / 勾选框：控件矩形内**不许有与所在面板底色不同的整块填充**。
    {
        const auto color_of = [](const char* role) {
            return QColor(QString::fromUtf8(
                xrkit::core::theme_color(xrkit::core::ThemeMode::Dark, role)));
        };
        const QColor bg = color_of("bg");
        const QColor surface = color_of("surface");
        const QColor surface2 = color_of("surface2");
        const QColor entry = color_of("entry_bg");
        // 采样一块区域里"最常见的颜色"（跳过边框/文字的抗锯齿过渡像素）
        const auto dominant = [](const QImage& image, const QRect& rect) {
            QHash<QRgb, int> histogram;
            const QRect clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    const QRgb pixel = image.pixel(x, y);
                    histogram[pixel] = histogram.value(pixel, 0) + 1;
                }
            }
            QRgb best = 0;
            int best_count = 0;
            for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
                if (it.value() > best_count) {
                    best_count = it.value();
                    best = it.key();
                }
            }
            // ★ 必须走 `fromRgba`：`QColor(QRgb)` 会把 **alpha 丢掉**（全透明区域会变成
            //   "不透明的黑"）⇒ 把"容器没涂底"误判成"容器涂了 #000000"（本轮实测踩过一次）。
            return QColor::fromRgba(best);
        };
        // 一块区域里有几个"整块填充色"（同一颜色出现 >= 8 次算一块）—— 用来断言
        // "控件矩形内不存在与面板底色不同的整块填充"：文字/边框/抗锯齿都是零散像素。
        const auto block_colors = [](const QImage& image, const QRect& rect, QColor panel,
                                     const QList<QColor>& allowed) {
            QHash<QRgb, int> histogram;
            const QRect clipped = rect.intersected(image.rect());
            for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
                for (int x = clipped.left(); x <= clipped.right(); ++x) {
                    histogram[image.pixel(x, y)] = histogram.value(image.pixel(x, y), 0) + 1;
                }
            }
            QStringList offenders;
            for (auto it = histogram.constBegin(); it != histogram.constEnd(); ++it) {
                if (it.value() < 8) continue;
                const QColor color(it.key());
                if (color == panel) continue;
                bool ok = false;
                for (const QColor& fine : allowed) {
                    if (color == fine) ok = true;
                }
                if (!ok) offenders << color.name();
            }
            return offenders;
        };

        Hub style_hub(nullptr, QString());
        // 字体系统：勾选框的"勾"由 CheckMarkStyle 画，不受字体影响；这里先量字号避免误判
        style_hub.resize(1600, 1000);
        style_hub.show();
        style_hub.set_current_page(5);   // 汉化包生成页（用户截图那一页：三条路径行 + 下拉 + 勾选框）
        // ★★ 2026-10-06（审计处理批·0-3 之 B1-9）：下面这个 3 轮布局循环**未判空即解引用**
        //   `style_hub.layout()->activate()` —— 壳的顶层布局真丢了（正是"排版回归"那种情形）
        //   先崩而不是先判红。判据钉住它，崩之前先给出那条红。
        check(style_hub.layout() != nullptr,
              QStringLiteral("⑭ 前提：壳的顶层布局存在（nullptr ⇒ 下面全部像素判据没意义）"));
        for (int pass = 0; style_hub.layout() != nullptr && pass < 3; ++pass) {
            style_hub.layout()->activate();
            for (QWidget* child : style_hub.findChildren<QWidget*>()) {
                if (child->layout() != nullptr) child->layout()->activate();
            }
            QApplication::processEvents();
        }

        // ① 标签背后的底板：**字段行容器**（`DropZones::attach_row` 统一挂 `field_row` 名字，
        //    六个页面都走这一条）自己不许涂底 —— 涂了就是用户截图里标签背后那块深灰圆角。
        int plate_hits = 0;
        int plate_rows_scanned = 0;   // ★ 候选样本数（B1-5：没有自证样本 ⇒ 候选集空就恒绿）
        QString plate_detail;
        for (QWidget* container : style_hub.findChildren<QWidget*>()) {
            if (container->objectName() != QStringLiteral("field_row")) continue;
            // 只看**当前页上真的显示出来**的行：没显示的页还没走过布局，几何是退化值
            // （实测：隐藏页的行容器宽 640、输入框挪到 x=452）⇒ 那种残缺几何上的像素
            // 判据不是用户看到的界面，会假红。
            if (!container->isVisible()) continue;
            const auto* edit = container->findChild<QLineEdit*>();
            const auto* label = container->findChild<QLabel*>();
            if (edit == nullptr || label == nullptr) continue;   // 只认"标签 + 输入框"这种字段行
            const QSize size = container->size();
            if (size.width() < 8 || size.height() < 8) continue;
            ++plate_rows_scanned;
            // 判据直接盯"谁涂了底"：把**行容器自己**渲染成图（DrawWindowBackground）
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(0, 0, 0, 0));
            container->render(&image);
            // 标签带 = 左边 2..(输入框左缘-2)，纵向取中间 3/5（避开上下边框与输入框）
            const int right = std::max(3, edit->x() - 2);
            const QRect strip(2, size.height() / 5, std::max(1, right - 2),
                              std::max(1, size.height() * 3 / 5));
            const QColor painted = dominant(image, strip);
            if (painted.alpha() == 0) continue;   // 透明 = 容器不涂底（这就是要的）
            if (painted != surface2) {
                ++plate_hits;
                if (plate_detail.isEmpty()) {
                    plate_detail = QStringLiteral("字段行(%1,%2 %3x%4) 标签带涂了 %5（应为卡片底 %6）")
                                       .arg(container->x())
                                       .arg(container->y())
                                       .arg(size.width())
                                       .arg(size.height())
                                       .arg(painted.name(), surface2.name());
                }
            }
        }
        check(plate_rows_scanned > 0 && plate_hits == 0,
              QStringLiteral("⑭ 标签背后不许有深灰底板（扫到 %1 个可见字段行，%2 处涂底；%3）")
                  .arg(plate_rows_scanned)
                  .arg(plate_hits)
                  .arg(plate_detail.isEmpty() ? QStringLiteral("无") : plate_detail));

        // ② 任务面板（同一个机制的另一处现场，用户截图图二"缺了一块"）：它是裸容器且带进度条，
        //    渲染出来的底必须是**透明的**（底交给卡片），不许自己涂一层 `bg`。
        {
            int panel_hits = 0;
            int panel_scanned = 0;
            QString detail;
            for (QWidget* panel : style_hub.findChildren<QWidget*>()) {
                if (panel->objectName() != QStringLiteral("task_panel")) continue;
                const QSize size = panel->size();
                if (size.width() < 8 || size.height() < 8) continue;
                ++panel_scanned;
                QImage image(size, QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(0, 0, 0, 0));
                panel->render(&image);
                const QColor painted =
                    dominant(image, QRect(2, 2, std::max(1, size.width() - 4),
                                          std::max(1, size.height() / 3)));
                if (painted.alpha() != 0 && painted == bg) {
                    ++panel_hits;
                    if (detail.isEmpty()) {
                        detail = QStringLiteral("任务面板涂了 %1").arg(painted.name());
                    }
                }
            }
            check(panel_scanned > 0 && panel_hits == 0,
                  QStringLiteral("⑭ 任务面板不许自己涂 %1 底（扫到 %2 个面板，%3 处涂底；%4）")
                      .arg(bg.name())
                      .arg(panel_scanned)
                      .arg(panel_hits)
                      .arg(detail.isEmpty() ? QStringLiteral("无") : detail));
        }

        // ③ 下拉框：控件矩形内不许有"与面板底色不同的整块填充"
        {
            int offenders = 0;
            int combos_scanned = 0;   // ★ 候选样本数（B1-5）
            QString detail;
            for (QComboBox* combo : style_hub.findChildren<QComboBox*>()) {
                if (!combo->isVisible()) continue;
                ++combos_scanned;
                QImage image(combo->size(), QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(0, 0, 0, 0));
                combo->render(&image);
                // 判据：把控件渲染出来，看**它自己的底**是不是干净的输入框底 ——
                // 取"文字下方那条横带"（避开文字/箭头；离屏上文字会退化成方块，
                // 落在文字上的判据会假红），并要求那一带**只有一个颜色且 == entry_bg**。
                // 旧样式（容器给控件涂了一层深灰 / 弹层底色撞色）在这里立刻红。
                const QRect foot(px(4), std::max(1, combo->height() - px(8)),
                                 std::max(1, combo->width() - px(8)), px(6));
                const QColor painted = dominant(image, foot);
                if (painted != entry) {
                    ++offenders;
                    if (detail.isEmpty()) {
                        detail = QStringLiteral("%1 自己的底是 %2（应为 %3）")
                                     .arg(combo->objectName(), painted.name(), entry.name());
                    }
                }
            }
            check(combos_scanned > 0 && offenders == 0,
                  QStringLiteral("⑭ 下拉框自己的底要与输入框同一套令牌（扫到 %1 个可见下拉，%2 处；%3）")
                      .arg(combos_scanned)
                      .arg(offenders)
                      .arg(detail.isEmpty() ? QStringLiteral("无") : detail));
        }

        // ④ 勾选框：**"勾就勾"** —— 选中态必须画出一个勾（勾的颜色 = 主题 text 令牌），
        //    未选中态**不许**有勾。判据：把方框渲染到哨兵底上，数"勾色像素"。
        {
            const QColor mark = color_of("text");
            // ★ 判据用**亮度**而不是"精确等于令牌色"：勾是**抗锯齿**画的，一笔里只有少数像素
            //   恰好等于纯色（实测 7/14px 框）。亮笔画 vs 深底（`entry_bg`/`selected` 都在
            //   #23262a~#31363b）在本主题下分得很开，所以"亮像素数"既稳又能判红。
            const auto mark_pixels = [indicator = QSize(px(14), px(14))](bool on) {
                QStyleOptionButton option;
                option.rect = QRect(0, 0, indicator.width(), indicator.height());
                option.state = QStyle::State_Enabled;
                if (on) option.state |= QStyle::State_On;
                option.palette = qApp->palette();
                QImage image(indicator, QImage::Format_ARGB32_Premultiplied);
                image.fill(QColor(255, 0, 255));   // 哨兵底（亮品红）——**不算**亮笔画
                QPainter painter(&image);
                qApp->style()->drawPrimitive(QStyle::PE_IndicatorCheckBox, &option, &painter);
                painter.end();
                int hits = 0;
                for (int y = 0; y < image.height(); ++y) {
                    for (int x = 0; x < image.width(); ++x) {
                        const QColor color = QColor::fromRgba(image.pixel(x, y));
                        if (color == QColor(255, 0, 255)) continue;   // 哨兵
                        if (qGray(color.rgb()) >= 150) ++hits;
                    }
                }
                return hits;
            };
            const int on_hits = mark_pixels(true);
            const int off_hits = mark_pixels(false);
            // ★★ 2026-10-06（审计处理批·0-3 之 B1-5）：下条判据是"选中态至少 12 个亮像素"，
            //   修前**没有**"这一笔真的画出来了"的自证 —— 一旦 `PE_IndicatorCheckBox` 不再被
            //   本项目样式接管（或探针尺寸退化成 0），`on_hits` 与 `off_hits` 会**一起**变成 0，
            //   而 `off_hits <= 2` 照样绿 ⇒ "没画勾"被读成"未选中态正确"。
            //   现在先钉住"画出来的东西有内容、且选中态严格多于未选中态"。
            check(on_hits > 0 && off_hits >= 0 && on_hits > off_hits,
                  QStringLiteral("⑭ 勾选框探针**真的画出了东西**（选中 %1 / 未选中 %2；两者同为 0 ⇒ "
                                 "primitive 没被接管，下面那条判据就是空转）")
                      .arg(on_hits)
                      .arg(off_hits));
            check(on_hits >= 12,
                  QStringLiteral("⑭ 勾选框选中态要真的画出勾（亮笔画像素 %1 < 12）").arg(on_hits));
            check(off_hits <= 2,
                  QStringLiteral("⑭ 勾选框未选中态不许有勾（实测 %1 个亮笔画像素）").arg(off_hits));
            // 尺寸令牌也一起钉住：勾随界面缩放（px(14)@1.5 时是 21）
            {
                const double saved = xrkit::core::ui_scale();
                xrkit::core::set_ui_scale(1.5);
                const bool scaled = px(14) == 21;
                xrkit::core::set_ui_scale(saved);
                check(scaled, QStringLiteral("⑭ 勾选框尺寸走 px() 令牌（px(14)@1.5 应为 21）"));
            }
        }

        // ⑤ 拖放高亮仍然是中性色（强调色只许三处）：这条既有口径在本轮改动后重验一次
        {
            const QString sheet = stylesheet_for(xrkit::core::ThemeMode::Dark);
            check(sheet.contains(QStringLiteral("*[drop_hover=\"true\"]")),
                  QStringLiteral("⑭ 拖放高亮规则还在（改样式别把它弄丢）"));
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ★★ 2026-10-06（D2 批）：编码转换补齐 / XML 详情栏与多列 / 系统字体选择器
    //
    // 这一批的判据都按"能不能判红"来写：把新东西拆掉、把旧行为改回去，下面任意一条
    // 都会立刻变红（每条的红/绿原始输出见交付报告）。三组的共同口径：
    //   · **零静默**：跳过/排除/失败/降级都要能读出计数或理由；
    //   · **同类东西一套组件**：批处理在 `engine::text::batch`、表格装配在 `pair_tree.h`、
    //     字体判据在 `engine::font`、起点判据在 `dialog_start.h` —— 页面里**不许**再抄一份。
    // ═══════════════════════════════════════════════════════════════════════

    // ── （D2-①）编码转换页：1.x 那 9 处"不必要阉割"逐条回接 ─────────────────
    {
        namespace etxt = xrkit::engine::text;
        ConvertPage page;
        page.resize(900, page_min_h());
        page.show();
        QApplication::processEvents();

        // ① 新控件在位且**默认值照 1.x**：扩展名 `.xml`、递归**开**、备份**开**、排除空
        check(page.ext_edit() != nullptr && page.ext_edit()->text() == QStringLiteral(".xml"),
              QStringLiteral("D2① 扩展名默认 `.xml`（实得「%1」）")
                  .arg(page.ext_edit() == nullptr ? QString() : page.ext_edit()->text()));
        check(page.recursive_box() != nullptr && page.recursive_box()->isChecked(),
              QStringLiteral("D2② 递归子目录**默认开**（1.x `:70` value=True）"));
        check(page.backup_box() != nullptr && page.backup_box()->isChecked(),
              QStringLiteral("D2③ 备份**默认开**（1.x `:71` value=True）"));
        check(page.exclude_edit() != nullptr && page.exclude_edit()->text().isEmpty(),
              QStringLiteral("D2④ 排除模式默认为空"));

        // 附：★ `ascii` 只在**目标**编码里（1.x 目标下拉有它、源下拉没有）；
        //     引擎侧的有损编解码对拍见 `tests/golden_lossy_codec.py`（补对拍时抓到真缺陷）。
        const auto has_item = [](QComboBox* combo, const QString& text) {
            if (combo == nullptr) return false;
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemData(i).toString() == text) return true;
            }
            return false;
        };
        check(has_item(page.target_combo(), QStringLiteral("ascii")),
              QStringLiteral("D2附 目标编码下拉里有 ascii（1.x 目标表里有它）"));
        check(!has_item(page.source_combo(), QStringLiteral("ascii")),
              QStringLiteral("D2附 源编码下拉里**没有** ascii（源是猜的，1.x 源表里也没有）"));
        check(page.target_combo() != nullptr &&
                  page.target_combo()->currentData().toString() == QStringLiteral("utf-8"),
              QStringLiteral("D2附 目标编码默认仍是 utf-8"));

        // ── （D3）三张东亚表上架：下拉里出现三项，且**与引擎编码名一一对应** ──
        //   ★ 判据写"一一对应"不是"包含"：下拉的 itemData 必须是**引擎那三个常量本身**
        //     （`etxt::kBig5/kShiftJis/kEucKr` 的字面值），否则界面选了 `big5`、
        //     引擎收到的是别的名字，就会静默落到"未知编码"。
        //     拆掉源表里的对应项 ⇒ 下面任意一条立刻红。
        {
            const char* kNew[] = {etxt::kBig5, etxt::kShiftJis, etxt::kEucKr};
            for (const char* name : kNew) {
                const QString want = QString::fromUtf8(name);
                check(has_item(page.source_combo(), want),
                      QStringLiteral("D3① 源编码下拉里有 %1（1.x ENCODING_CHOICES 源侧有它）")
                          .arg(want));
                check(has_item(page.target_combo(), want),
                      QStringLiteral("D3① 目标编码下拉里也有 %1（引擎两侧都实现了）").arg(want));
                // 一一对应：下拉项 data → 页面取到的编码名
                int idx = -1;
                for (int i = 0; i < page.source_combo()->count(); ++i) {
                    if (page.source_combo()->itemData(i).toString() == want) {
                        idx = i;
                        break;
                    }
                }
                check(idx >= 0, QStringLiteral("D3② 源下拉里能定位到 %1").arg(want));
                if (idx >= 0) {
                    const int keep = page.source_combo()->currentIndex();
                    page.source_combo()->setCurrentIndex(idx);
                    check(QString::fromStdString(page.current_source_encoding()) == want,
                          QStringLiteral("D3② 源下拉选中 %1 ⇒ 交给引擎的编码名就是 %1（实得 %2）")
                              .arg(want)
                              .arg(QString::fromStdString(page.current_source_encoding())));
                    page.source_combo()->setCurrentIndex(keep);
                }
            }
            // 三项**顺序**与 1.x 的 ENCODING_CHOICES 一致（gb2312 之后、windows-1251 之前）
            const QStringList want_order = {QStringLiteral("big5"),
                                            QStringLiteral("shift_jis"),
                                            QStringLiteral("euc-kr")};
            QStringList got_order;
            for (int i = 0; i < page.source_combo()->count(); ++i) {
                const QString d = page.source_combo()->itemData(i).toString();
                if (d == QStringLiteral("big5") || d == QStringLiteral("shift_jis") ||
                    d == QStringLiteral("euc-kr")) {
                    got_order << d;
                }
            }
            check(got_order == want_order,
                  QStringLiteral("D3③ 三项在源下拉里的相对顺序照 1.x 的 ENCODING_CHOICES"
                                 "（big5 → shift_jis → euc-kr，实得 %1）")
                      .arg(got_order.join(QStringLiteral(" → "))));
            check(!has_item(page.source_combo(), QStringLiteral("ascii")),
                  QStringLiteral("D2附 加了三个新项之后，源下拉里**仍然没有** ascii"));

            // ★ 全库"编码名列表"同类搜索的落点：XML 校对页也有一张编码下拉，但它的
            //   模型是 **1.x `xml_compare_app.py:302` 的 `ENCODINGS`**（六项，**不含**
            //   big5/shift_jis/euc-kr）⇒ 那一页**故意不动**，这里把它钉住，免得日后
            //   有人"顺手统一"成转换页那张表（那会偏离 1.x 的界面口径）。
            {
                XmlPage xml;
                xml.show();
                QApplication::processEvents();
                const auto enc_has = [&xml](const QString& text) {
                    if (xml.enc_a_combo() == nullptr) return false;
                    for (int i = 0; i < xml.enc_a_combo()->count(); ++i) {
                        if (xml.enc_a_combo()->itemData(i).toString() == text) return true;
                    }
                    return false;
                };
                for (const char* name : kNew) {
                    const QString want = QString::fromUtf8(name);
                    check(!enc_has(want),
                          QStringLiteral("D3④ XML 校对页的编码下拉**不含** %1"
                                         "（1.x 那一页的 ENCODINGS 只有六项）")
                              .arg(want));
                }
                check(enc_has(QStringLiteral("windows-1251")),
                      QStringLiteral("D3④ XML 校对页的编码下拉仍是 1.x 那六项之一"));
                xml.close();
            }
        }

        // ③ 输出目录非空 ⇒ 备份勾选框**禁用且取消勾选**（1.x `on_output_dir_change` `:575-580`）
        page.output_edit()->setText(QStringLiteral("E:/xrkit_d2_out"));
        check(!page.backup_box()->isEnabled() && !page.backup_box()->isChecked(),
              QStringLiteral("D2③ 输出目录非空 ⇒ 备份禁用且取消勾选（实得 enabled=%1 checked=%2）")
                  .arg(page.backup_box()->isEnabled() ? 1 : 0)
                  .arg(page.backup_box()->isChecked() ? 1 : 0));
        page.output_edit()->clear();
        check(page.backup_box()->isEnabled(),
              QStringLiteral("D2③ 输出目录清空 ⇒ 备份勾选框恢复可点"));

        // ②⑥⑦⑧⑨ 五颗"1.x 有、2.x 没有"的按钮都在
        check(page.restore_button() != nullptr && page.clear_backup_button() != nullptr &&
                  page.open_output_button() != nullptr && page.export_button() != nullptr,
              QStringLiteral("D2⑥⑦⑧⑨ 恢复备份/清空备份/打开输出/导出统计四颗按钮都在"));

        // ★ 「取消」要**先确认**（1.x `:629-637` `askyesno`）—— 确认钩子挂在**共用**任务壳上
        check(page.task() != nullptr && page.task()->has_cancel_confirm(),
              QStringLiteral("D2⑤ 取消确认钩子挂上了（1.x 取消前 askyesno）"));
        {
            bool asked = false;
            page.task()->set_cancel_confirm([&asked] {
                asked = true;
                return false;   // 用户点"否"
            });
            page.task()->run(QStringLiteral("探针"), [](const ProgressSink&, const MessageSink&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            });
            page.task()->cancel_button()->click();
            check(asked && !page.task()->cancel_requested(),
                  QStringLiteral("D2⑤ 点取消时先确认；答「否」⇒ 不置位、不留痕"));
            for (int i = 0; i < 200 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            page.task()->set_cancel_confirm([] { return true; });
        }

        // ②③④⑤ 端到端：真的递归进子目录 + 备份到 源目录\backup + 行数/计数如实
        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_d2_convert"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/sub"));
        const auto write_file = [](const QString& path, const QByteArray& body) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return;
            file.write(body);
        };
        // 源是 cp1251 的俄文 XML 声明；子目录里再放一个 —— **旧实现只读顶层，会静默漏掉它**
        const QByteArray cp1251_decl = "<?xml version=\"1.0\" encoding=\"windows-1251\"?>\n<r>\n"
                                       "<s id=\"a\">\xCF\xF0\xE8\xE2\xE5\xF2</s>\n</r>\n";
        write_file(root + QStringLiteral("/top.xml"), cp1251_decl);
        write_file(root + QStringLiteral("/sub/deep.xml"), cp1251_decl);
        write_file(root + QStringLiteral("/skip.txt"), "not xml\n");
        write_file(root + QStringLiteral("/temp_skip.xml"), cp1251_decl);

        page.source_edit()->setText(root);
        page.exclude_edit()->setText(QStringLiteral("*temp*"));
        // 1.x 的备份勾选框被"输出目录非空"取消过之后**不会自己勾回来**（它只恢复可点）
        // ⇒ 原地转换这一轮要显式勾上（这正是 1.x 的行为，不是绕过）
        page.backup_box()->setChecked(true);
        page.start_button()->click();
        for (int i = 0; i < 600 && page.task()->running(); ++i) {
            QThread::msleep(5);
            QApplication::processEvents();
        }
        QApplication::processEvents();
        const etxt::BatchOutcome outcome = page.last_outcome();
        check(page.last_scan_text().contains(QStringLiteral("递归 2 层")),
              QStringLiteral("D2② 台账报出递归层数（%1）").arg(page.last_scan_text()));
        check(outcome.collect.skipped_excluded == 1 && outcome.collect.skipped_ext == 1,
              QStringLiteral("D2④ 排除/后缀不符各自记账（排除 %1 / 后缀 %2）")
                  .arg(outcome.collect.skipped_excluded)
                  .arg(outcome.collect.skipped_ext));
        // ★★ 这一条就是"子目录被静默漏掉"那个缺陷的锁：候选必须是 2 个（顶层 + 子目录）
        check(outcome.candidates == 2 && outcome.success == 2,
              QStringLiteral("D2② **递归进子目录**：候选 %1（应为 2）、成功 %2")
                  .arg(outcome.candidates)
                  .arg(outcome.success));
        const bool saw_deep = outcome.processed == 2 && QFileInfo::exists(root + QStringLiteral("/sub/deep.xml"));
        check(saw_deep && outcome.collect.depth == 2,
              QStringLiteral("D2② 台账里的层数 = 2（实得 %1）").arg(outcome.collect.depth));
        // ③ 备份落在 `源目录\backup\<相对路径>.bak`
        check(QFileInfo::exists(root + QStringLiteral("/backup/top.xml.bak")) &&
                  QFileInfo::exists(root + QStringLiteral("/backup/sub/deep.xml.bak")),
              QStringLiteral("D2③ 备份落在 源目录\\backup\\<相对路径>.bak（新增 %1 个）")
                  .arg(outcome.backed_up));
        // ★ 输出目录模式要**保留相对结构**（否则不同子目录的同名文件会互相覆盖）
        page.output_edit()->setText(root + QStringLiteral("/out"));
        page.source_edit()->setText(root);
        page.start_button()->click();
        for (int i = 0; i < 600 && page.task()->running(); ++i) {
            QThread::msleep(5);
            QApplication::processEvents();
        }
        QApplication::processEvents();
        check(QFileInfo::exists(root + QStringLiteral("/out/sub/deep.xml")),
              QStringLiteral("D2② 输出目录保留相对结构（out/sub/deep.xml 在）"));

        // ⑨ 导出统计：utf-8-sig + 字段与 1.x 同序
        check(page.stats_ready(), QStringLiteral("D2⑨ 跑完一批后有统计快照"));
        const QString csv = root + QStringLiteral("/stats.csv");
        QString problem;
        check(page.export_stats(csv, &problem), QStringLiteral("D2⑨ 导出统计成功（%1）").arg(problem));
        {
            QFile file(csv);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray body = file.readAll();
                const QString text = QString::fromUtf8(body);
                check(body.startsWith("\xEF\xBB\xBF"), QStringLiteral("D2⑨ 统计 CSV 是 utf-8-sig"));
                check(text.contains(QStringLiteral("编码转换统计报告")),
                      QStringLiteral("D2⑨ 统计 CSV 有标题行"));
                check(text.contains(QStringLiteral("输出目录,")) &&
                          text.contains(QStringLiteral("耗时,")) &&
                          text.contains(QStringLiteral("候选文件数,")),
                      QStringLiteral("D2⑨ 统计 CSV 有 1.x 的那几行"));
            } else {
                check(false, QStringLiteral("D2⑨ 统计 CSV 读不回来"));
            }
        }

        // ⑥ 恢复备份（页面按钮 ⇒ 引擎三件）：把文件改坏再点按钮 ⇒ 内容回到原样
        {
            const QByteArray good = QByteArray(cp1251_decl);
            write_file(root + QStringLiteral("/top.xml"), "WRECKED");
            page.output_edit()->clear();
            page.source_edit()->setText(root);
            page.restore_button()->click();
            for (int i = 0; i < 600 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            QApplication::processEvents();
            QFile back(root + QStringLiteral("/top.xml"));
            QByteArray got;
            if (back.open(QIODevice::ReadOnly)) got = back.readAll();
            check(got == good,
                  QStringLiteral("D2⑥ 恢复备份把内容还原（实得 %1 字节，应为 %2）")
                      .arg(got.size())
                      .arg(good.size()));
        }
        // ⑦ 清空备份：点了之后 backup 里的 .bak 真没了，且状态栏报数
        {
            page.clear_backup_button()->click();
            for (int i = 0; i < 600 && page.task()->running(); ++i) {
                QThread::msleep(5);
                QApplication::processEvents();
            }
            QApplication::processEvents();
            check(!QFileInfo::exists(root + QStringLiteral("/backup/top.xml.bak")) &&
                      page.status() != nullptr &&
                      page.status()->text().contains(QStringLiteral("清空完成")),
                  QStringLiteral("D2⑦ 清空备份真的删了且报数（状态栏「%1」）")
                      .arg(page.status() == nullptr ? QString() : page.status()->text()));
        }
        // ⑧ 打开输出：目录不存在要**明确说**（不许点了没反应）
        page.output_edit()->setText(root + QStringLiteral("/no_such_dir"));
        page.open_output();
        check(page.status() != nullptr &&
                  page.status()->text().contains(QStringLiteral("目录不存在")),
              QStringLiteral("D2⑧ 打开输出：目录不存在时说清楚（「%1」）")
                  .arg(page.status() == nullptr ? QString() : page.status()->text()));
        QDir(root).removeRecursively();
        page.hide();
    }

    // ── （D2-②）XML 校对页：结果表多列 + 详情栏 + 逐条复制id + 双击弹窗 ──────
    {
        namespace exml = xrkit::engine::xml;
        XmlPage page;
        page.resize(980, page_min_h());
        page.show();
        QApplication::processEvents();
        // 1.x `:965-971` `_hide_detail`：没选中行时详情栏**不占位**
        check(page.detail_panel() != nullptr && !page.detail_panel()->isVisible(),
              QStringLiteral("D2-XML 未选中时详情栏不占位（1.x `_hide_detail`）"));

        const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_d2_xml"));
        QDir(root).removeRecursively();
        QDir().mkpath(root + QStringLiteral("/a"));
        QDir().mkpath(root + QStringLiteral("/b"));
        const auto write = [](const QString& path, const QString& body) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return;
            file.write(body.toUtf8());
        };
        write(root + QStringLiteral("/a/diff.xml"),
              QStringLiteral("<r>\n<s id=\"x\"><text>v</text></s>\n</r>\n"));
        write(root + QStringLiteral("/b/diff.xml"),
              QStringLiteral("<r>\n<s id=\"y\"><text>V</text></s>\n"
                             "<s id=\"z\"><text>W</text></s>\n</r>\n"));
        page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                          QStringLiteral("auto"), QStringLiteral("auto"));
        page.select_visible_row(0);
        QApplication::processEvents();

        // 10.1/★ 多列：列数/列头/列策略**来自模式声明**（1.x `_apply_columns`）
        const exml::XmlModeSpec* spec = exml::find_mode(page.current_mode_key());
        check(page.result_tree() != nullptr && spec != nullptr &&
                  page.result_tree()->columnCount() == static_cast<int>(spec->columns.size()),
              QStringLiteral("D2-XML 结果表列数 = 模式声明（实得 %1 / 声明 %2）")
                  .arg(page.result_tree() == nullptr ? -1 : page.result_tree()->columnCount())
                  .arg(spec == nullptr ? -1 : static_cast<int>(spec->columns.size())));
        if (page.result_tree() != nullptr && spec != nullptr) {
            bool headers_ok = true;
            for (int col = 0; col < static_cast<int>(spec->columns.size()); ++col) {
                if (page.result_tree()->headerItem()->text(col) !=
                    QString::fromStdString(spec->columns[static_cast<size_t>(col)].heading)) {
                    headers_ok = false;
                }
            }
            check(headers_ok, QStringLiteral("D2-XML 表头文本逐列等于模式声明"));
            // 列策略：一律 Interactive ＋ **末列吃剩余宽度**（用户 09-29 口径；
            // 把某列设成 Stretch ⇒ 丢掉"列可拖"，这条立刻红）
            int stretched = 0;
            bool all_interactive = true;
            for (int col = 0; col < page.result_tree()->columnCount(); ++col) {
                const auto mode = page.result_tree()->header()->sectionResizeMode(col);
                if (mode == QHeaderView::Interactive) continue;
                if (mode == QHeaderView::Stretch) {
                    ++stretched;
                } else {
                    all_interactive = false;
                }
            }
            if (page.result_tree()->header()->stretchLastSection()) ++stretched;
            check(all_interactive && stretched == 1,
                  QStringLiteral("D2-XML 列策略＝一律 Interactive ＋ 末列拉伸（stretch=%1 all_interactive=%2）")
                      .arg(stretched)
                      .arg(all_interactive ? 1 : 0));
            // 行数与筛后行数一致；第 0 行第 0 列就是 rel（表格与 row_text 同源）
            check(page.result_tree()->topLevelItemCount() == page.visible_count(),
                  QStringLiteral("D2-XML 表格行数 = 筛后行数（%1 / %2）")
                      .arg(page.result_tree()->topLevelItemCount())
                      .arg(page.visible_count()));
        }

        // ② 选中 ⇒ 详情栏出现：头行 `[ rel ] ✓/✗ …`、正文非空、逐条 id 列表行数对得上
        check(page.detail_panel() != nullptr && page.detail_panel()->isVisible(),
              QStringLiteral("D2-XML 选中一行 ⇒ 详情栏出现（1.x `_show_detail`）"));
        const QString head = page.detail_head() == nullptr ? QString() : page.detail_head()->text();
        check(head.startsWith(QStringLiteral("[ ")) &&
                  (head.contains(QStringLiteral("✗")) || head.contains(QStringLiteral("✓"))) &&
                  head.contains(QStringLiteral("ID 数: A=")),
              QStringLiteral("D2-XML 头行形如 `[ rel ] ✗ 状态 ID 数: A= B=`（实得「%1」）").arg(head));
        check(page.detail_view() != nullptr && !page.detail_view()->toPlainText().isEmpty(),
              QStringLiteral("D2-XML 详情正文非空（%1 字节）")
                  .arg(page.detail_view() == nullptr ? 0
                                                     : page.detail_view()->toPlainText().size()));
        const std::vector<XmlDetailId> ids = page.detail_ids_for(page.selected_visible_row());
        check(page.id_list() != nullptr &&
                  page.id_list()->topLevelItemCount() == static_cast<int>(ids.size()) &&
                  !ids.empty(),
              QStringLiteral("D2-XML 逐条 id 列表行数 = 该行差异 id 数（%1 / %2）")
                  .arg(page.id_list() == nullptr ? -1 : page.id_list()->topLevelItemCount())
                  .arg(ids.size()));
        // ★ 每行一个「复制id」按钮 ⇒ 点它真的进剪贴板（1.x `:1183`/`_copy_one_id`）
        // ★★ 2026-10-06（审计处理批·0-3 之 B1-9）：修前 `!ids.empty()` 只是**上一句里的断言**、
        //   不是控制流 ⇒ 夹具真退化（`ids` 为空而列表行数 > 0）时下面 `ids.front()` 是空 vector
        //   取下标（UB/崩溃）。现在把"ids 非空"也写进控制流条件里。
        if (page.id_list() != nullptr && page.id_list()->topLevelItemCount() > 0 && !ids.empty()) {
            QTreeWidgetItem* item = page.id_list()->topLevelItem(0);
            auto* button = qobject_cast<QPushButton*>(page.id_list()->itemWidget(item, 0));
            check(button != nullptr && button->text() == QStringLiteral("复制id"),
                  QStringLiteral("D2-XML 每条 id 后面有「复制id」按钮"));
            if (button != nullptr) {
                QGuiApplication::clipboard()->clear();
                button->click();
                check(QGuiApplication::clipboard()->text() == ids.front().id &&
                          page.status() != nullptr &&
                          page.status()->text().contains(QStringLiteral("已复制 id")),
                      QStringLiteral("D2-XML 点「复制id」进剪贴板（实得「%1」，应为「%2」）")
                          .arg(QGuiApplication::clipboard()->text(), ids.front().id));
            }
        }

        // ③ 双击 ⇒ 明细弹窗（1.x `:907-914`）：不 exec，只看内容
        {
            QDialog* dialog = page.make_detail_dialog(0, &page);
            check(dialog != nullptr, QStringLiteral("D2-XML 双击能造出明细弹窗"));
            if (dialog != nullptr) {
                dialog->show();
                QApplication::processEvents();
                auto* text = dialog->findChild<QPlainTextEdit*>(QStringLiteral("xml_dialog_text"));
                auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("xml_dialog_id_list"));
                auto* head_label = dialog->findChild<QLabel*>(QStringLiteral("xml_dialog_head"));
                check(text != nullptr && !text->toPlainText().isEmpty(),
                      QStringLiteral("D2-XML 弹窗里有完整明细正文"));
                check(list != nullptr && list->topLevelItemCount() == static_cast<int>(ids.size()),
                      QStringLiteral("D2-XML 弹窗里也有逐条 id（%1）")
                          .arg(list == nullptr ? -1 : list->topLevelItemCount()));
                check(head_label != nullptr && head_label->text() == head,
                      QStringLiteral("D2-XML 弹窗头行与详情栏**同一份**判据"));
                check(page.make_detail_dialog(9999, &page) == nullptr,
                      QStringLiteral("D2-XML 越界下标不造弹窗（不许空指针解引用）"));
                dialog->close();
                dialog->deleteLater();
            }
        }

        // ★ 表格与 CSV **同源**（`cell_text` 唯一一份）：CSV 第一个数据格 == 表格第 0 行第 0 列
        {
            const QString csv = root + QStringLiteral("/same_source.csv");
            check(page.export_csv(csv, QStringLiteral("2026-10-06 00:00:00")),
                  QStringLiteral("D2-XML 导出仍成功"));
            QFile file(csv);
            QString body;
            if (file.open(QIODevice::ReadOnly)) body = QString::fromUtf8(file.readAll());
            const QStringList lines = body.split(QStringLiteral("\r\n"));
            const QString first_cell =
                lines.size() > 11 ? lines[11].section(QLatin1Char(','), 0, 0) : QString();
            const QString tree_cell =
                page.result_tree() != nullptr && page.result_tree()->topLevelItemCount() > 0
                    ? page.result_tree()->topLevelItem(0)->text(0)
                    : QString();
            check(!tree_cell.isEmpty() && first_cell.contains(tree_cell),
                  QStringLiteral("D2-XML 表格与 CSV 同源（CSV「%1」/ 表格「%2」）")
                      .arg(first_cell, tree_cell));
        }
        QDir(root).removeRecursively();
        page.hide();
    }

    // ── （D2-③）汉化包生成页：系统字体选择器 / 尺寸档位 / 打开输出 / 语言后缀 ────
    {
        namespace fnt = xrkit::engine::font;
        FontPackPage page;
        page.resize(980, page_min_h());
        page.show();
        QApplication::processEvents();

        // ★ 1.x `toolkit_platform.system_font_dirs` `:222-234`：**不写死 C:\Windows**
        //   ★★ P8a：目录清单的**唯一一份**在引擎（`system_dirs.cpp`），宿主门面与选择器都从它拿
        //   —— 自检也直接问它，别再去摸一份"页面版"（那正是被抽掉的东西）。
        QStringList dirs;
        for (const std::string& dir : xrkit::engine::font::system_font_dirs()) {
            dirs << QString::fromStdString(dir);
        }
        const QString win_root = qEnvironmentVariable("SystemRoot");
        check(!dirs.isEmpty(), QStringLiteral("D2-字体 系统字体目录非空（%1 个）").arg(dirs.size()));
        check(win_root.isEmpty() ||
                  QDir::cleanPath(dirs.first()) ==
                      QDir::cleanPath(QDir(win_root).filePath(QStringLiteral("Fonts"))),
              QStringLiteral("D2-字体 第一个目录来自 %%SystemRoot%%（实得「%1」，SystemRoot=「%2」）")
                  .arg(dirs.isEmpty() ? QString() : dirs.first(), win_root));

        // ★ 过滤：白名单后缀 + 文件名子串 + **完整路径**去重（1.x `:494-511`）
        const QStringList all_fonts = font_candidates(QString());
        const QStringList msyh_fonts = font_candidates(QStringLiteral("MSYH"));  // 大小写不敏感
        int bad_ext = 0;
        int bad_keyword = 0;
        for (const QString& path : msyh_fonts) {
            const QString name = QFileInfo(path).fileName().toLower();
            if (!name.endsWith(QStringLiteral(".ttf")) && !name.endsWith(QStringLiteral(".ttc")) &&
                !name.endsWith(QStringLiteral(".otf"))) {
                ++bad_ext;
            }
            if (!name.contains(QStringLiteral("msyh"))) ++bad_keyword;
        }
        check(bad_ext == 0 && bad_keyword == 0,
              QStringLiteral("D2-字体 候选项都在白名单后缀内且命中子串（坏后缀 %1 / 未命中 %2）")
                  .arg(bad_ext)
                  .arg(bad_keyword));
        check(QSet<QString>(all_fonts.begin(), all_fonts.end()).size() == all_fonts.size(),
              QStringLiteral("D2-字体 候选项按**完整路径**去重（%1 项）").arg(all_fonts.size()));
        check(all_fonts.size() >= msyh_fonts.size(),
              QStringLiteral("D2-字体 子串过滤真的在过滤（全部 %1 ≥ 命中 %2）")
                  .arg(all_fonts.size())
                  .arg(msyh_fonts.size()));

        // ★ 默认字体三级优先级（1.x `:129-149`）：上次用过的（存在）→ 系统雅黑 → 仓库根
        // ★★ 2026-10-06（审计处理批·0-3 之 B1-5 同族）：修前两条锁包在 `if (!msyh_fonts.isEmpty())` 里，
        //   候选集为空时（本机没装 MSYH ⇒ `font_candidates("MSYH")` 返回空）断言**整条消失**
        //   且无痕迹 ⇒ 换台机器两条锁静默没了还看着像绿。现在：空集 ⇒ 明确记 note（可见），
        //   并把"这一组锁到底跑没跑"作为单独一条判据（不许静默）。
        if (msyh_fonts.isEmpty()) {
            result.notes.append(QStringLiteral("D2-字体 系统里没有 MSYH 候选字体 ⇒ "
                                               "「默认字体继承」与「列表标记同一判据」两条锁**未覆盖**"));
        }
        // ★ 空集**不算通过**：这条判据钉的是"过滤器真的在按关键词过滤"这件事本身 ——
        //   候选集为空就等于没过滤过（`all_fonts.size() >= 0` 恒真）。本机确实没装 MSYH 时
        //   由上面那条 note 明确记"未覆盖"，这里不再用恒真式糊过去。
        check(!msyh_fonts.isEmpty(),
              QStringLiteral("D2-字体 子串过滤有命中（MSYH 候选 %1 个；空 ⇒ 过滤器/字体扫描"
                             "至少一边是坏的，且下面两条锁没跑）")
                  .arg(msyh_fonts.size()));
        if (!msyh_fonts.isEmpty()) {
            check(default_font_path(msyh_fonts.first()) ==
                      QDir::toNativeSeparators(msyh_fonts.first()),
                  QStringLiteral("D2-字体 默认字体：user.ltx 里那个仍存在 ⇒ 原样继承"));
        }
        const QString fallback = default_font_path(QStringLiteral("Z:/nope/none.ttf"));
        check(fallback.isEmpty() || QFileInfo::exists(fallback),
              QStringLiteral("D2-字体 默认字体兜底必须是个**真存在**的文件（实得「%1」）")
                  .arg(fallback));

        // 11.8 的"同一判据"：列表标记缓存 == 引擎的四态标记（不许页面自己判一遍）
        //   ★★ P8a：选择器抽到 `font_picker.cpp`（宿主唯一一份）⇒ 缓存也用那份实现
        if (!msyh_fonts.isEmpty()) {
            check(font_mark_of(msyh_fonts.first()) ==
                      QString::fromUtf8(fnt::cjk_state_mark(
                          page.check_font(msyh_fonts.first()).state)),
                  QStringLiteral("D2-字体 列表标记与预览标题**同一判据**（引擎 cjk_state_mark）"));
        }

        // ★ 预览缩放：1.x `set_zoom` —— 步进 0.5、夹在 [0.5, 4.0]。
        //   ★★ P8a：缩放状态**只属于那个对话框**（原来挂在页面上），所以这条判据改成
        //   **真的点按钮**（用户就是这么用的），而不是去摸页面方法。

        // ★ 自建字体选择器（**不是**下拉、**不是**单纯文件框）：结构逐项对上 1.x
        {
            // ★ 点**真按钮**（用户在字体那一行按的「选择…」）打开选择器 —— 验的是产品那条链；
//   对话框 parent 是页面 ⇒ 从页面里找它（`open_font_picker` 是 private，不该为自检开口子）。
if (auto* pick_button = page.findChild<QPushButton*>(QStringLiteral("pick_font_button"))) {
    pick_button->click();
    QApplication::processEvents();
}
QDialog* dialog = page.findChild<QDialog*>(QStringLiteral("font_picker"));
            check(dialog != nullptr, QStringLiteral("D2-字体 「选择…」造得出自建字体选择器"));
            if (dialog != nullptr) {
                dialog->show();
                QApplication::processEvents();
                auto* list = dialog->findChild<QListWidget*>(QStringLiteral("font_picker_list"));
                auto* filter = dialog->findChild<QLineEdit*>(QStringLiteral("font_picker_filter"));
                auto* preview = dialog->findChild<QLabel*>(QStringLiteral("font_picker_preview"));
                auto* zoom = dialog->findChild<QLabel*>(QStringLiteral("font_picker_zoom"));
                auto* caption = dialog->findChild<QLabel*>(QStringLiteral("font_picker_caption"));
                check(list != nullptr && filter != nullptr && preview != nullptr &&
                          zoom != nullptr && caption != nullptr,
                      QStringLiteral("D2-字体 左过滤+列表 / 右预览+缩放+标题 都在"));
                check(list != nullptr && list->count() == all_fonts.size(),
                      QStringLiteral("D2-字体 列表项数 = 候选项数（%1 / %2）")
                          .arg(list == nullptr ? -1 : list->count())
                          .arg(all_fonts.size()));
                check(zoom != nullptr && zoom->text() == QStringLiteral("100%"),
                      QStringLiteral("D2-字体 缩放标签初始 100%%（实得「%1」）")
                          .arg(zoom == nullptr ? QString() : zoom->text()));
                // ★ 缩放上下限：**点按钮 12 次**（步进 0.5）⇒ 夹在 4.0 / 0.5
                //   （原来这条是直接调页面方法；P8a 把它改成"用户真会做的事"）
                if (auto* zoom_in_btn =
                        dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_in"));
                    zoom_in_btn != nullptr && zoom != nullptr) {
                    for (int i = 0; i < 12; ++i) zoom_in_btn->click();
                    QApplication::processEvents();
                    check(zoom->text() == QStringLiteral("400%"),
                          QStringLiteral("D2-字体 缩放上限 4.0（点 12 次「+」后实得「%1」）")
                              .arg(zoom->text()));
                    if (auto* zoom_out_btn = dialog->findChild<QPushButton*>(
                            QStringLiteral("font_picker_zoom_out"));
                        zoom_out_btn != nullptr) {
                        for (int i = 0; i < 12; ++i) zoom_out_btn->click();
                        QApplication::processEvents();
                        check(zoom->text() == QStringLiteral("50%"),
                              QStringLiteral("D2-字体 缩放下限 0.5（点 12 次「−」后实得「%1」）")
                                  .arg(zoom->text()));
                    }
                }
                check(dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_in")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_out")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_pick")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_browse")) !=
                              nullptr &&
                          dialog->findChild<QPushButton*>(QStringLiteral("font_picker_cancel")) !=
                              nullptr,
                      QStringLiteral("D2-字体 ±缩放 / 选择 / 浏览其他目录 / 取消 都在"));
                // ★ **双击＝选择**（1.x `:641`）：直接发那个信号，看字体框是否真的被填上
                //   ⚠ 这条会**真的写 user.ltx**（"任何选择路径都记住"就是它的语义）⇒
                //   先备份、后复原：自检不许把"上次选择的字体"留在构建机（发行包更是
                //   在**包目录里**跑 `--smoke` —— 留着就等于往包里塞一个 user.ltx）。
                const QString ltx_path = QString::fromStdString(xrkit::core::user_ltx_path());
                const bool ltx_existed = QFileInfo::exists(ltx_path);
                QByteArray ltx_before;
                bool ltx_backed_up = false;
                if (ltx_existed) {
                    QFile before(ltx_path);
                    if (before.open(QIODevice::ReadOnly)) {
                        ltx_before = before.readAll();
                        ltx_backed_up = true;
                    }
                }
                if (list != nullptr && list->count() > 0) {
                    QListWidgetItem* item = list->item(0);
                    list->setCurrentItem(item);
                    QMetaObject::invokeMethod(list, "itemDoubleClicked", Qt::DirectConnection,
                                              Q_ARG(QListWidgetItem*, item));
                    check(page.font_edit()->text() ==
                              QDir::toNativeSeparators(item->data(Qt::UserRole).toString()),
                          QStringLiteral("D2-字体 **双击＝选择**（字体框「%1」/ 候选项「%2」）")
                              .arg(page.font_edit()->text(),
                                   item->data(Qt::UserRole).toString()));
                }
                // user.ltx 复原（判据已经在上面取过，这里只是把机器状态还回去）
                if (ltx_existed) {
                    QFile restore(ltx_path);
                    if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        restore.write(ltx_before);
                    }
                } else {
                    QFile::remove(ltx_path);
                }
                QString ltx_why_font;
                const bool ltx_ok_font =
                    user_ltx_restored(ltx_path, ltx_existed, ltx_before, &ltx_why_font);
                check(!ltx_existed || ltx_backed_up,
                      QStringLiteral("D2-字体 自检前**真的读到了** user.ltx 的备份"));
                check(ltx_ok_font,
                      QStringLiteral("D2-字体 自检把 user.ltx **逐字节**还原（存在=%1；%2）")
                          .arg(ltx_existed ? 1 : 0)
                          .arg(ltx_ok_font ? QStringLiteral("一致") : ltx_why_font));
                // ± 缩放按钮真的改标签（★ P8a：不看页面状态，只看用户能看到的那个标签）
                if (auto* zoom_in =
                        dialog->findChild<QPushButton*>(QStringLiteral("font_picker_zoom_in"));
                    zoom_in != nullptr && zoom != nullptr) {
                    const QString before_label = zoom->text();
                    zoom_in->click();
                    QApplication::processEvents();
                    check(zoom->text() != before_label,
                          QStringLiteral("D2-字体 「+」真的放大并刷新标签（%1 → %2）")
                              .arg(before_label, zoom->text()));
                }
                dialog->close();
                dialog->deleteLater();
            }
        }

        // 尺寸档位（1.x `:84-89` ⇒ `PackageOptions::offset`）
        check(page.size_combo() != nullptr && page.size_combo()->count() == 5 &&
                  page.size_combo()->currentData().toInt() == 0,
              QStringLiteral("D2-字体 尺寸档位 5 项且默认「标准」= 0"));
        if (page.size_combo() != nullptr) {
            page.size_combo()->setCurrentIndex(1);
            check(page.size_offset() == 3,
                  QStringLiteral("D2-字体 +3 ⇒ offset=3（实得 %1）").arg(page.size_offset()));
            page.size_combo()->setCurrentIndex(0);
        }

        // 语言"自定义 + 输入框"（1.x `_lang_changed` `:202-209`）
        {
            const int custom = page.lang_combo()->findText(QStringLiteral("自定义"));
            check(custom >= 0, QStringLiteral("D2-字体 加载语言有「自定义」"));
            page.lang_combo()->setCurrentIndex(custom);
            QApplication::processEvents();
            check(page.lang_edit() != nullptr && page.lang_edit()->isVisible(),
                  QStringLiteral("D2-字体 选「自定义」⇒ 语言输入框出现"));
            page.lang_edit()->setText(QStringLiteral("jpn"));
            engine::font::PackageOptions options =
                page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.lang == "jpn",
                  QStringLiteral("D2-字体 自定义语言进 options（实得「%1」）")
                      .arg(QString::fromStdString(options.lang)));
            page.lang_combo()->setCurrentIndex(page.lang_combo()->findText(QStringLiteral("chs")));
            QApplication::processEvents();
            check(page.lang_edit() != nullptr && !page.lang_edit()->isVisible(),
                  QStringLiteral("D2-字体 切回 chs ⇒ 输入框收起"));
        }
        // 后缀预设 无/_chs/自定义（1.x `_suf_changed` `:211-220`）
        {
            page.suffix_combo()->setCurrentIndex(0);
            engine::font::PackageOptions options =
                page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix.empty(), QStringLiteral("D2-字体 后缀「无」⇒ 空串"));
            page.suffix_combo()->setCurrentIndex(1);
            options = page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix == "_chs", QStringLiteral("D2-字体 后缀「_chs」⇒ \"_chs\""));
            const int custom = page.suffix_combo()->findText(QStringLiteral("自定义"));
            page.suffix_combo()->setCurrentIndex(custom);
            QApplication::processEvents();
            page.suffix_edit()->setText(QStringLiteral("_rus"));
            options = page.options_for(QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"));
            check(options.suffix == "_rus" &&
                      page.suffix_edit() != nullptr && page.suffix_edit()->isVisible(),
                  QStringLiteral("D2-字体 自定义后缀进 options（实得「%1」）")
                      .arg(QString::fromStdString(options.suffix)));
            page.suffix_combo()->setCurrentIndex(0);
        }

        // 打开输出目录（1.x `:649-659`：目录无效 / 打开失败都要有反馈）
        {
            page.out_edit()->setText(QStringLiteral("Z:/definitely_missing_dir"));
            QString problem;
            check(!page.open_output(&problem) && !problem.isEmpty(),
                  QStringLiteral("D2-字体 打开输出：无效目录给理由（「%1」）").arg(problem));
            page.out_edit()->clear();
            problem.clear();
            check(!page.open_output(&problem) && problem.contains(QStringLiteral("无效")),
                  QStringLiteral("D2-字体 打开输出：空目录也明说（「%1」）").arg(problem));
        }
        page.hide();
    }

    // ★★ 2026-10-05 用户第 3 条（原话）：「**所有下拉老样式**…我实际想要的样式是 1.x 里的
    //   那种，**1.x 线条比 2.0.0 硬朗多了**」——判据**读生成出来的 QSS**，不看截图观感。
    //
    //   ★★ 判据一律**按真值（token 已经替换完的色值/尺寸）**，**不按 token 名**：
    //     `stylesheet_for()` 返回的是**已解析**的样式表 —— `@{pad4}` → `4px`、
    //     `@{surface2}` → `#2b2f33`。拿 `contains("pad4")` 这种去判是**假判据**
    //     （永远红，跟实现对不对无关）。这里改成拿 `theme_color_current()` / `px()`
    //     算出的真值去比 —— 改回旧值（旧 token）照样立刻红，但红得**有依据**。
    //
    //   ★ 为什么这条能判红（逐条对应"旧值"）：
    //     ① `::down-arrow` 槽底 2.0.0 从来没有这条规则（旧值＝零规则）；箭头本体由
    //        `CheckMarkStyle` 画（见下 ①′ 的像素探针 —— 不画/画成 accent 都红）；
    //     ② 下拉内距 2.0.0 是 `pad8 pad10`（＝`8px 10px`）⇒ 写回旧值立刻红；
    //     ③ 弹层圆角：2.0.0 是 `r2`（`2px`，1.x 是**棱角** `0`）⇒ 写回 `2px` 立刻红；
    //     ④ `QMenu { … }` 在 2.0.0 里**一条规则都没有**（走 Qt 默认 ⇒ 系统蓝高亮 +
    //        立体弹层）⇒ 一条都不写就红；而且要求**恰好三条**（见下）。
    {
        const QString sheet = xrkit::shell::stylesheet_for(xrkit::core::theme_mode());

        // 取出某选择器里**生效的那一条**规则体。★ 必须取**最后一条**：Qt 的 QSS 是
        // "后写的赢"（`theme.cpp:447-451` 的注释自己写明了），而 `QComboBox, QLineEdit,
        // QSpinBox` 在样式表里出现**两次**（基础表 + 尺寸档位段）—— 修前这里用 `indexOf`
        // 取**第一条**，于是"把生效的那条改回缺陷写法"锁仍然绿（审计 B1-3）。
        const auto rule_body = [&sheet](const QString& selector) -> QString {
            return qss_rule_body(sheet, selector, /*last=*/true);
        };
        const auto has_rule = [&sheet](const QString& selector) {
            return sheet.contains(selector);
        };
        // 主题**真值**（色值已解析到十六进制；尺寸走 px()，与 QSS 生成时同一口径）
        const auto hex = [](const char* role) {
            const char* v = xrkit::core::theme_color_current(role);
            return QString::fromUtf8(v != nullptr ? v : "");
        };
        const QString c_surface2 = hex("surface2");
        const QString c_textdim = hex("text_dim");
        const QString c_selected = hex("selected");
        const QString c_accent = hex("accent");

        // ① `::down-arrow` / `::drop-down`：箭头几何与"谁画箭头"这两件事必须在 QSS 里钉住。
        //    ★★ 本批实测出来的机制（真 combo grab 后数 `text_dim` 像素，四种变体）：
        //       · 有 `::drop-down` 规则（带 surface2 底 / 只有 `border: 0`）⇒ 箭头像素 **0**
        //         （Qt 样式表样式接管绘制、不再回调基类，而它自己不会画箭头）；
        //       · **去掉 `::drop-down` 规则** ⇒ 基类被调、箭头出来（6 像素）。
        //    ⇒ 定案：**`::drop-down` 规则必须不存在**（否则用户看到空槽）；
        //      `::down-arrow` 只留几何（不许带 background/image —— 带了 Qt 又会自己涂完不画箭头）；
        //      槽底 + 三角整块由 `CheckMarkStyle::drawComplexControl(CC_ComboBox)` 画。
        const QString arrow = rule_body(QStringLiteral("QComboBox::down-arrow"));
        const QString arrow12 = QStringLiteral("%1px").arg(xrkit::core::px(12));
        check(has_rule(QStringLiteral("QComboBox::down-arrow")) &&
                  !has_rule(QStringLiteral("QComboBox::drop-down")) && arrow.contains(arrow12) &&
                  !arrow.contains(QStringLiteral("background")) &&
                  !arrow.contains(QStringLiteral("image")) &&
                  (c_accent.isEmpty() || !arrow.contains(c_accent)),
              QStringLiteral("下拉① `::down-arrow` 只给几何 %1、**不许有 `::drop-down` 规则**"
                             "（有它 Qt 就不画箭头了 —— 实测 0 像素）；实测 arrow=「%2」 drop-down规则=%3")
                  .arg(arrow12)
                  .arg(arrow)
                  .arg(has_rule(QStringLiteral("QComboBox::drop-down")) ? 1 : 0));

        // ①′ **箭头真的出现在真 combo 的槽里**（端到端像素判据 —— 这一条才是"用户看得见"）。
        //    ★ 为什么必须**端到端**量、不能只量 `drawPrimitive`：本批实测，**QSS 里只要存在
        //      一条 `::drop-down` 规则（哪怕只有 `border: 0`），Qt 就接管绘制、连基类都不回调**
        //      ⇒ 光量 primitive 会给出**假绿**（探针绿、真 combo 的槽里一个箭头都没有，实测 0）。
        //      所以这里**真造一个 `QComboBox`**、`grab()` 下来，在最右那一列（＝箭头槽）里数
        //      `text_dim` 像素。本批实测值：规则去掉后 ≈ 15 个（1.x 那种硬朗三角）；
        //      带 `::drop-down` 规则时 = **0**。
        //    ★ 判红办法：① 把 `::drop-down` 规则加回 QSS；② 给 `::down-arrow` 加
        //      `background`；③ 在 `CheckMarkStyle::drawComplexControl` 里不画那个三角
        //      ⇒ 三条路都让下面这条红。
        {
            QComboBox probe_combo;
            probe_combo.addItem(QStringLiteral("x"));
            probe_combo.resize(xrkit::core::px(200), xrkit::core::px(34));
            probe_combo.show();
            QApplication::processEvents();
            const QImage shot = probe_combo.grab().toImage();
            probe_combo.hide();
            const QColor want(c_textdim);
            int hits = 0;
            const int slot_w = std::max(4, xrkit::core::px(14));
            for (int y = 0; y < shot.height(); ++y) {
                for (int x = std::max(0, shot.width() - slot_w); x < shot.width(); ++x) {
                    const QColor got = shot.pixelColor(x, y);
                    if (got.alpha() > 0 && got.red() == want.red() && got.green() == want.green() &&
                        got.blue() == want.blue()) {
                        ++hits;
                    }
                }
            }
            // ≥6：一个"看得见的三角"至少要这么多个像素（本批实测 15；Fusion 那个小箭头只有 6）
            check(want.isValid() && hits >= 6,
                  QStringLiteral("下拉①′ **真 combo** 的箭头槽里量到 %1 个 text_dim %2 像素"
                                 "（%3×%4 的 grab，右 %5 px；<6 ⇒ 箭头没画出来 / 被 QSS 吃掉）")
                      .arg(hits)
                      .arg(c_textdim)
                      .arg(shot.width())
                      .arg(shot.height())
                      .arg(slot_w));
        }

        // ①″ 箭头本体在 `CheckMarkStyle` 那一层**确实被调到**（往离屏 QImage 上打一笔
        //    `PE_IndicatorArrowDown` —— `CC_ComboBox` 的 `SC_ComboBoxArrow` 画的就是这个
        //    primitive）⇒ 必须量到 `text_dim` 像素。★ 与 ①′ 分工：①′ 证"用户看得见"，
        //    ①″ 证"那一笔是主题色（不是写死的、不是 accent）"。
        {
            const int side = std::max(8, xrkit::core::px(16));
            QImage probe(side, side, QImage::Format_ARGB32_Premultiplied);
            probe.fill(Qt::transparent);
            {
                QPainter painter(&probe);
                QStyleOption opt;
                opt.rect = QRect(0, 0, side, side);
                opt.palette = QApplication::palette();
                opt.state = QStyle::State_Enabled;
                QApplication::style()->drawPrimitive(QStyle::PE_IndicatorArrowDown, &opt, &painter,
                                                     nullptr);
            }
            const QColor want(c_textdim);
            int hits = 0;
            for (int y = 0; y < probe.height(); ++y) {
                for (int x = 0; x < probe.width(); ++x) {
                    const QColor got = probe.pixelColor(x, y);
                    if (got.alpha() > 0 && got.red() == want.red() && got.green() == want.green() &&
                        got.blue() == want.blue()) {
                        ++hits;
                    }
                }
            }
            check(want.isValid() && hits > 0,
                  QStringLiteral("下拉①″ 箭头那一笔是**主题色** text_dim %1（%2×%2 上量到 %3 个像素）")
                      .arg(c_textdim)
                      .arg(side)
                      .arg(hits));
        }

        // ② 主规则的内距 ＝ `pad4 pad8`（＝ `4px 8px`；1.x `TCombobox padding=(6,4)` 的收紧版）。
        //    ★ `::drop-down` 那条规则**必须不在**（见 ①：它在 ⇒ 箭头没了）；槽的宽度因此
        //      由 `CheckMarkStyle` 从 `SC_ComboBoxArrow` 量，QSS 里只剩 `::down-arrow` 的几何。
        const QString pad4 = QStringLiteral("%1px").arg(xrkit::core::px(4));
        const QString pad8 = QStringLiteral("%1px").arg(xrkit::core::px(8));
        const QString pad10 = QStringLiteral("%1px").arg(xrkit::core::px(10));
        const QString base = rule_body(QStringLiteral("QComboBox, QLineEdit, QSpinBox"));
        check(base.contains(QStringLiteral("padding: %1 %2").arg(pad4, pad8)) &&
                  !base.contains(pad10),
              QStringLiteral("下拉② 内距＝pad4 pad8（即 `%1 %2`；实测「%3」）—— 写回 pad8 pad10 即红")
                  .arg(pad4, pad8, base));
        check(!has_rule(QStringLiteral("QComboBox::drop-down")) &&
                  arrow.contains(QStringLiteral("width: %1").arg(arrow12)),
              QStringLiteral("下拉② 箭头槽的几何只由 `::down-arrow` 给（%1）、且没有 `::drop-down` 规则"
                             "（实测 drop-down规则=%2 / arrow=「%3」）")
                  .arg(arrow12)
                  .arg(has_rule(QStringLiteral("QComboBox::drop-down")) ? 1 : 0)
                  .arg(arrow));

        // ③ 弹层：**圆角 0**（1.x 是硬朗的直角）+ 1px solid 边框。
        const QString popup = rule_body(QStringLiteral("QComboBox QAbstractItemView"));
        check(popup.contains(QStringLiteral("border-radius: 0")) &&
                  popup.contains(QStringLiteral("1px solid")),
              QStringLiteral("下拉③ 弹层圆角 0 + 1px solid 边框（实测「%1」）").arg(popup));

        // ④ `QMenu` 一套：**恰好三条属性**（background=surface2 / relief=flat /
        //    border=1px solid）。★ `relief: flat` 不是装饰：Qt 只从 `QMenu {…}` 认这个 hint。
        //    ★ 三条**全是真值**比：底＝surface2 的解析色、描边＝border 的解析色。
        const QString menu = rule_body(QStringLiteral("QMenu"));
        int menu_props = 0;
        {
            QString tmp = menu;
            tmp.replace(QLatin1Char(';'), QLatin1Char('\n'));
            for (const QString& part : tmp.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                if (!part.trimmed().isEmpty()) ++menu_props;
            }
        }
        const QString c_border = hex("border");
        check(!menu.isEmpty() && menu_props == 3 && menu.contains(c_surface2) &&
                  menu.contains(QStringLiteral("relief: flat")) &&
                  menu.contains(QStringLiteral("1px solid ") + c_border) &&
                  (c_accent.isEmpty() || !menu.contains(c_accent)),
              QStringLiteral("下拉④ QMenu 一套（恰好 3 条属性 / 实测 %1 条：「%2」；"
                             "background=%3 + relief flat + 1px solid %4，不引 accent）")
                  .arg(menu_props)
                  .arg(menu, c_surface2, c_border));

        // ④′ 选中行用**中性灰**（用户口径：**不许系统蓝高亮**）。
        //     ★ 判据看 `QMenu::item:selected` 那一条（选中态的真身就在那里）——
        //     按**值**判：必须出现 `selected` 的解析色，且不许出现 `accent` 的解析色
        //     （accent 就是"系统蓝那一族"；Qt 默认高亮也在这条锁的射程内）。
        const QString sel_rule = rule_body(QStringLiteral("QMenu::item:selected"));
        check(!c_selected.isEmpty() && sel_rule.contains(c_selected) &&
                  (c_accent.isEmpty() || !sel_rule.contains(c_accent)),
              QStringLiteral("下拉④ QMenu 选中行＝中性灰 %1（`::item:selected` 实测「%2」；"
                             "不许系统蓝/accent %3）")
                  .arg(c_selected, sel_rule, c_accent));
    }

    // ★★ 2026-10-05（用户报告 #2）右栏＝**解包产物预览**，必须体现「**包名壳**」。
    //
    // 用户原话（2026-10-03）：「**这是一个解包的工具，而右边罗列的是包的内容，换句话说就是
    //   解包产物的预览。**」；本批原话：「`…\IWP\新建文件夹`还是那样。」
    // ⇒ 判据：把**同一份落点规则**用在预览上 —— `IWP\database\config\xr.db` 的条目
    //   `gamemtl.xr` 在右栏必须显示成 `xr/gamemtl.xr`（修前显示根级 `gamemtl.xr`）。
    //   反向：**自带壳**的包（`ai.db` 的条目已经 `ai/…`）**不叠第二层**（仍是 `ai/…`）。
    // ★ 判红办法：把 `rebuild_tree()` 里那句 `efs::preview_entries_in_layout(...)` 换回
    //   `entries_[i]`（＝"原始包内路径"）⇒ 下面两条立刻红。
    //
    // ★★ 2026-10-06（审计处理批·0-3 之 B1-2）：修前这一整组（三条断言）包在
    //   `if (std::getenv("XRKIT_SKIP_LOCK2") == nullptr) { … }` 里 —— 一个环境变量就能
    //   **无声**关掉"#2 解包产物预览"的整组断言，而且跳过后**既不写 note 也不计数**，
    //   自检照样全绿（"门禁可以被静默关掉"正是本轮最重的根因）。
    //   现在**删掉后门**：这三条断言**无条件执行**（它们只依赖引擎函数与临时夹具，
    //   在离屏自检里本来就跑得通 —— 后门是历史遗留，不是必要前提）。
    {
        namespace efs = xrkit::engine::fs;
        // ① 同一份落点规则算出来的**相对路径**（不是另写一遍字符串拼接）
        const std::vector<efs::EntryInfo> raw = {efs::EntryInfo{std::string("gamemtl.xr"), false, 5}};
        const std::vector<efs::EntryInfo> shown =
            efs::preview_entries_in_layout("E:/IWP/database/config/xr.db", raw);
        bool has_shell = false;
        bool root_leak = false;
        for (const efs::EntryInfo& e : shown) {
            if (e.path == "xr/gamemtl.xr") has_shell = true;
            if (e.path == "gamemtl.xr") root_leak = true;
        }
        check(has_shell && !root_leak,
              QStringLiteral("#2 预览① 不带壳的包：落点规则的下一条路径＝`xr/gamemtl.xr`"
                             "（实测 %1 条 / 根级泄漏 %2）")
                  .arg(shown.size())
                  .arg(root_leak ? 1 : 0));
        // ② 反向：自带壳（条目已在 `ai/…` 下）**不叠第二层**
        const std::vector<efs::EntryInfo> shelled = {
            efs::EntryInfo{std::string("ai/alife/x.efd"), false, 3},
            efs::EntryInfo{std::string("ai/configs/y.ltx"), false, 4}};
        const std::vector<efs::EntryInfo> shelled_shown =
            efs::preview_entries_in_layout("E:/x/ai.db", shelled);
        bool doubled = false;
        bool original = false;
        for (const efs::EntryInfo& e : shelled_shown) {
            if (e.path.rfind("ai/ai/", 0) == 0) doubled = true;
            if (e.path == "ai/alife/x.efd") original = true;
        }
        check(original && !doubled,
              QStringLiteral("#2 预览② 自带壳的包**不叠第二层**（原样 ai/alife/x.efd=%1 / 叠了 ai/ai=%2）")
                  .arg(original ? 1 : 0)
                  .arg(doubled ? 1 : 0));
        // ③ 真页面：右栏树上**看得见**那层壳（不是只有引擎函数对）
        const QString shell_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_shell_preview"));
        QDir(shell_dir).removeRecursively();
        QDir().mkpath(shell_dir + QStringLiteral("/database/config"));
        {
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem file;
            file.path = "gamemtl.xr";
            file.data = {'G'};
            items.push_back(file);
            const std::vector<uint8_t> packed = efs::pack_db(items, *efs::format_key_to_db("xdb"));
            std::ofstream out((shell_dir + QStringLiteral("/database/config/xr.db")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(packed.data()),
                      static_cast<std::streamsize>(packed.size()));
        }
        FsPage shell_page;
        shell_page.resize(760, page_min_h());
        shell_page.show();
        QApplication::processEvents();
        // ★ 诊断块已撤：这里**只留判据**。左栏的包行与右栏的条目走两条不同的装配路径，
        //   用**真实入口** `add_paths()` 加包（也是用户拖入/加包走的那条），不是内部
        //   `add_source`（后者只在别处的夹具里用）。
        const int added = shell_page.add_paths({shell_dir + QStringLiteral("/database/config/xr.db")});
        QApplication::processEvents();
        // ★ 2026-10-05（本批）：**先展开壳那一层再找深层项** —— 右栏是**懒建**的
        //   （`refresh_rows` 只建顶层，子项在 `itemExpanded`/`expand_entry` 时才建），
        //   不展开就找 `xr/gamemtl.xr` 必然是 nullptr（实测：加包 1 / 找到 0 / 根级 0 —— 假红）。
        //   ★ 这一句**不放松判据**：`gamemtl.xr`（根级）那一条照样必须找不到（壳没生效就红），
        //     而"展开成功"本身也进了判据（`shell_expanded`）—— 壳不在了这一步就红。
        const bool shell_expanded = shell_page.expand_entry(QStringLiteral("xr"));
        QApplication::processEvents();
        const bool shell_visible =
            added == 1 && shell_expanded &&
            shell_page.find_entry_item(QStringLiteral("xr/gamemtl.xr")) != nullptr &&
            shell_page.find_entry_item(QStringLiteral("gamemtl.xr")) == nullptr;
        check(shell_visible,
              QStringLiteral("#2 预览③ 右栏树上显示的是**解包后的路径** `xr/gamemtl.xr`"
                             "（加包 %1 个 / 展开壳 %2 / 找到 xr/gamemtl.xr=%3 / 根级 gamemtl.xr=%4）")
                  .arg(added)
                  .arg(shell_expanded ? 1 : 0)
                  .arg(shell_page.find_entry_item(QStringLiteral("xr/gamemtl.xr")) != nullptr ? 1 : 0)
                  .arg(shell_page.find_entry_item(QStringLiteral("gamemtl.xr")) != nullptr ? 1 : 0));
        shell_page.hide();
        QDir(shell_dir).removeRecursively();
    }

    // ★★ 2026-10-05（用户报告 #3「勾选汇总断链（目录级＋源级；用户说「普遍存在」）」）。
    //
    // 口径：**自下而上汇总 + 自上而下传播**必须**逐级**成立 ——
    //   子全勾 ⇒ 父 ✓／**部分 ⇒ 父 ■**／全不勾 ⇒ 父 □。
    // 夹具（对应用户给的两张图；★判据**读状态值** `Qt::CheckState`，不靠截图观感）：
    //   ① `config/text/` 全勾而父 `config/` 未勾 ⇒ `config/` 必须 ■；
    //   ② `scripts/addon/*`（来源 `gamedata.db0`）全勾而**源行**未勾 ⇒ 源行必须 ■。
    // 另断言"一侧变⇒另一侧立刻跟随""多来源合并节点同样三态自洽"。
    // ★ 判红办法：把 `on_entry_item_changed` / `on_package_item_changed` 里那句
    //   `refresh_package_checks()` 去掉（源行卡在 □）⇒ ② 立刻红；
    //   把 `sync_entry_states()` 去掉（父目录卡在 □）⇒ ① 立刻红。
    {
        namespace efs = xrkit::engine::fs;
        FsPage tri2;
        tri2.add_source(QStringLiteral("gamedata.db0"),
                        {efs::EntryInfo{std::string("config/text/a.ltx"), false, 5},
                         efs::EntryInfo{std::string("config/text/b.ltx"), false, 6},
                         efs::EntryInfo{std::string("config/root.ltx"), false, 7},
                         efs::EntryInfo{std::string("scripts/addon/x.script"), false, 8},
                         efs::EntryInfo{std::string("scripts/addon/y.script"), false, 9}});
        tri2.resize(900, page_min_h());
        tri2.show();
        QApplication::processEvents();
        // 展开到能看见叶子（懒建：逐级展开）
        tri2.expand_entry(QStringLiteral("gamedata"));
        tri2.expand_entry(QStringLiteral("gamedata/config"));
        tri2.expand_entry(QStringLiteral("gamedata/config/text"));
        tri2.expand_entry(QStringLiteral("gamedata/scripts"));
        tri2.expand_entry(QStringLiteral("gamedata/scripts/addon"));
        QApplication::processEvents();

        const auto state_of = [&](const QString& path) -> Qt::CheckState {
            QTreeWidgetItem* it = tri2.find_entry_item(path);
            return it != nullptr ? it->checkState(0) : Qt::Unchecked;
        };
        // 前提：夹具要能跑到（找得到那几个节点）
        check(tri2.find_entry_item(QStringLiteral("gamedata/config/text")) != nullptr &&
                  tri2.find_entry_item(QStringLiteral("gamedata/config")) != nullptr,
              QStringLiteral("#3 前提：目录链建出来了（config/text=%1 / config=%2）")
                  .arg(tri2.find_entry_item(QStringLiteral("gamedata/config/text")) != nullptr ? 1 : 0)
                  .arg(tri2.find_entry_item(QStringLiteral("gamedata/config")) != nullptr ? 1 : 0));

        // ① 勾 `config/text/`（目录）⇒ 它的两个文件全勾 ⇒ 父 `config/` 必须**部分勾选**
        //    （`config/` 下面还有 `root.ltx` 没勾 ⇒ 不是全勾）
        if (QTreeWidgetItem* text_dir = tri2.find_entry_item(QStringLiteral("gamedata/config/text"))) {
            text_dir->setCheckState(0, Qt::Checked);   // 触发 itemChanged ⇒ 回灌模型
        }
        QApplication::processEvents();
        check(state_of(QStringLiteral("gamedata/config/text")) == Qt::Checked &&
                  state_of(QStringLiteral("gamedata/config")) == Qt::PartiallyChecked &&
                  state_of(QStringLiteral("gamedata")) == Qt::PartiallyChecked,
              QStringLiteral("#3 ① 子全勾 ⇒ 父 ■（text=%1 / config=%2 / 壳=%3；"
                             "修前 config 卡在 □）")
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config/text"))))
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config"))))
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata")))));
        // ② **源行**必须立刻跟随成 ■（`scripts/addon/*` 还没勾，所以这一包不是全勾）
        {
            QTreeWidgetItem* src = find_package_source_node(tri2.package_list(),
                                                            QStringLiteral("gamedata.db0"));
            check(src != nullptr && src->checkState(0) == Qt::PartiallyChecked,
                  QStringLiteral("#3 ② 右栏部分勾 ⇒ **源行**立刻 ■（实测 %1；修前卡在 □）")
                      .arg(src != nullptr ? static_cast<int>(src->checkState(0)) : -1));
        }
        // ③ `scripts/addon/*` 两个文件全勾 ⇒ 那一支的父目录 ✓；config 支不变（仍 ■）
        if (QTreeWidgetItem* one = tri2.find_entry_item(QStringLiteral("gamedata/scripts/addon/x.script"))) {
            one->setCheckState(0, Qt::Checked);
        }
        if (QTreeWidgetItem* two = tri2.find_entry_item(QStringLiteral("gamedata/scripts/addon/y.script"))) {
            two->setCheckState(0, Qt::Checked);
        }
        QApplication::processEvents();
        check(state_of(QStringLiteral("gamedata/scripts/addon")) == Qt::Checked &&
                  state_of(QStringLiteral("gamedata/scripts")) == Qt::Checked &&
                  state_of(QStringLiteral("gamedata/config")) == Qt::PartiallyChecked,
              QStringLiteral("#3 ③ 另一支全勾 ⇒ 那一支 ✓、config 支仍 ■（addon=%1 / scripts=%2 / config=%3）")
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/scripts/addon"))))
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/scripts"))))
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config")))));
        // ④ 源行：这一包里 4/5 勾 ⇒ 仍 ■；再把 `config/root.ltx` 勾上 ⇒ 全勾 ⇒ 源行 ✓
        {
            QTreeWidgetItem* src = find_package_source_node(tri2.package_list(),
                                                            QStringLiteral("gamedata.db0"));
            const Qt::CheckState mid = src != nullptr ? src->checkState(0) : Qt::Unchecked;
            if (QTreeWidgetItem* rest = tri2.find_entry_item(QStringLiteral("gamedata/config/root.ltx"))) {
                rest->setCheckState(0, Qt::Checked);
            }
            QApplication::processEvents();
            check(mid == Qt::PartiallyChecked && src != nullptr &&
                      src->checkState(0) == Qt::Checked,
                  QStringLiteral("#3 ④ 源行跟随（全勾前 ■=%1 → 全勾后 ✓=%2）")
                      .arg(static_cast<int>(mid))
                      .arg(src != nullptr ? static_cast<int>(src->checkState(0)) : -1));
        }
        // ⑤ **自上而下传播**：源行取消勾 ⇒ 它下面的条目全不勾 ⇒ 目录链回 □
        {
            QTreeWidgetItem* src = find_package_source_node(tri2.package_list(),
                                                            QStringLiteral("gamedata.db0"));
            if (src != nullptr) src->setCheckState(0, Qt::Unchecked);
            QApplication::processEvents();
            check(state_of(QStringLiteral("gamedata/config")) == Qt::Unchecked &&
                      state_of(QStringLiteral("gamedata/scripts/addon")) == Qt::Unchecked &&
                      tri2.checked_file_count() == 0,
                  QStringLiteral("#3 ⑤ 源行取消 ⇒ 自上而下全清（config=%1 / addon=%2 / 勾选数=%3）")
                      .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config"))))
                      .arg(static_cast<int>(state_of(QStringLiteral("gamedata/scripts/addon"))))
                      .arg(tri2.checked_file_count()));
        }
        // ⑥ **多来源合并节点也要三态自洽**：第二个包在同一路径下再加一个文件 ⇒
        //    `gamedata/config/text` 变成"两来源合并"的节点，勾上它 ⇒ 上面两级都要 ■
        tri2.add_source(QStringLiteral("extra.db0"),
                        {efs::EntryInfo{std::string("config/text/c.ltx"), false, 10}});
        QApplication::processEvents();
        tri2.expand_entry(QStringLiteral("gamedata"));
        tri2.expand_entry(QStringLiteral("gamedata/config"));
        tri2.expand_entry(QStringLiteral("gamedata/config/text"));
        QApplication::processEvents();
        if (QTreeWidgetItem* text_dir = tri2.find_entry_item(QStringLiteral("gamedata/config/text"))) {
            text_dir->setCheckState(0, Qt::Checked);
        }
        QApplication::processEvents();
        check(state_of(QStringLiteral("gamedata/config/text")) == Qt::Checked &&
                  state_of(QStringLiteral("gamedata/config")) == Qt::PartiallyChecked,
              QStringLiteral("#3 ⑥ 多来源合并节点三态自洽（text=%1 / config=%2）")
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config/text"))))
                  .arg(static_cast<int>(state_of(QStringLiteral("gamedata/config")))));
        tri2.hide();
    }

    // ★★ 2026-10-05（#4 作为**回归锁**保留；用户已复测"勾选不会刷状态"）。
    //
    // 断言：**勾选前后所有已展开节点的 `isExpanded()` 快照不变、item 指针不变、
    // 滚动位置不变**（含"按钮触发"的勾选）。
    // ★ 判红办法：往 `on_package_item_changed` / `on_entry_item_changed` 里注入
    //   `refresh_rows()`（＝clear + rebuild）⇒ 下面几条立刻红。
    {
        namespace efs = xrkit::engine::fs;
        FsPage keep2;
        // ★ 源名带 `.db0`：落点壳让两个源落到**同一个顶层目录**（`keep2/`）——
        //   这正是本锁要的形态（一个顶层节点、它下面有子项可展开、可滚动）。
        keep2.add_source(QStringLiteral("keep2_a.db0"),
                         {efs::EntryInfo{std::string("d"), true, 0},
                          efs::EntryInfo{std::string("d/a.txt"), false, 10},
                          efs::EntryInfo{std::string("d/b.txt"), false, 11},
                          efs::EntryInfo{std::string("d/c.txt"), false, 12}});
        keep2.add_source(QStringLiteral("keep2_b.db0"),
                         {efs::EntryInfo{std::string("d"), true, 0},
                          efs::EntryInfo{std::string("d/e.txt"), false, 13}});
        keep2.resize(420, page_min_h());   // 窄一点 ⇒ 右栏内容超出视口 ⇒ 滚动位置有意义
        keep2.show();
        QApplication::processEvents();
        QTreeWidgetItem* root_item = keep2.entry_tree()->topLevelItem(0);
        if (root_item != nullptr) root_item->setExpanded(true);
        QApplication::processEvents();
        QTreeWidgetItem* sub_item = root_item != nullptr ? root_item->child(0) : nullptr;
        if (sub_item != nullptr) sub_item->setExpanded(true);
        QApplication::processEvents();

        // —— 快照 ——
        struct Snapshot {
            QTreeWidgetItem* top = nullptr;
            QTreeWidgetItem* sub = nullptr;
            bool top_expanded = false;
            bool sub_expanded = false;
            int top_kids = -1;
            int sub_kids = -1;
            int scroll = -1;
        };
        const auto snap = [&]() {
            Snapshot s;
            s.top = keep2.entry_tree()->topLevelItem(0);
            s.sub = s.top != nullptr ? s.top->child(0) : nullptr;
            s.top_expanded = s.top != nullptr && s.top->isExpanded();
            s.sub_expanded = s.sub != nullptr && s.sub->isExpanded();
            s.top_kids = s.top != nullptr ? s.top->childCount() : -1;
            s.sub_kids = s.sub != nullptr ? s.sub->childCount() : -1;
            QScrollBar* bar = keep2.entry_tree()->verticalScrollBar();
            if (bar != nullptr) {
                bar->setValue(std::min(bar->maximum(), bar->singleStep() * 2));
                s.scroll = bar->value();
            }
            return s;
        };
        const Snapshot before = snap();
        check(before.top != nullptr && before.top_expanded && before.sub != nullptr,
              QStringLiteral("#4 前提：右栏顶层项已展开且有子节点（top=%1 / sub=%2）")
                  .arg(before.top != nullptr ? 1 : 0)
                  .arg(before.sub != nullptr ? 1 : 0));

        // ① **勾包行**（`itemChanged` 触发的那条路）⇒ 树不许重建、展开态/指针/滚动位置全不变
        {
            QTreeWidgetItem* src = find_package_source_node(keep2.package_list(),
                                                           QStringLiteral("keep2_a.db0"));
            if (src != nullptr) src->setCheckState(0, Qt::Checked);
            QApplication::processEvents();
            const Snapshot after = snap();
            check(after.top == before.top && after.sub == before.sub &&
                      after.top_expanded == before.top_expanded &&
                      after.sub_expanded == before.sub_expanded &&
                      after.top_kids == before.top_kids && after.sub_kids == before.sub_kids,
                  QStringLiteral("#4 ① 勾包行 ⇒ 右栏**不重建**（指针 top=%1/sub=%2 · 展开 %3/%4 · "
                                 "子项 %5→%6/%7→%8）")
                      .arg(after.top == before.top ? 1 : 0)
                      .arg(after.sub == before.sub ? 1 : 0)
                      .arg(after.top_expanded ? 1 : 0)
                      .arg(after.sub_expanded ? 1 : 0)
                      .arg(before.top_kids)
                      .arg(after.top_kids)
                      .arg(before.sub_kids)
                      .arg(after.sub_kids));
            check(after.scroll == before.scroll,
                  QStringLiteral("#4 ①′ 勾包行 ⇒ 右栏**滚动位置不变**（%1→%2）")
                      .arg(before.scroll)
                      .arg(after.scroll));
        }
        // ② **按钮触发**的勾选（包内全选）同样不许刷状态
        {
            const Snapshot pre_btn = snap();
            if (keep2.check_all_button() != nullptr) keep2.check_all_button()->click();
            QApplication::processEvents();
            const Snapshot post_btn = snap();
            check(post_btn.top == pre_btn.top && post_btn.sub == pre_btn.sub &&
                      post_btn.top_expanded && post_btn.sub_expanded &&
                      post_btn.scroll == pre_btn.scroll,
                  QStringLiteral("#4 ② 「包内全选」按钮触发 ⇒ 状态同样不变（指针=%1/%2 · 展开=%3/%4 · "
                                 "滚动 %5→%6）")
                      .arg(post_btn.top == pre_btn.top ? 1 : 0)
                      .arg(post_btn.sub == pre_btn.sub ? 1 : 0)
                      .arg(post_btn.top_expanded ? 1 : 0)
                      .arg(post_btn.sub_expanded ? 1 : 0)
                      .arg(pre_btn.scroll)
                      .arg(post_btn.scroll));
        }
        keep2.hide();
    }

    // ★★ 2026-10-05（用户报告 #6）「**编码转换的中间那块区域并没有设计滚动条。**」
    //
    // 判据（滚动不变量 ①②，**不是**"条子看不看得见"）：
    //   · 内容超高 ⇒ **必须能滚**（条子存在且 `maximum() > 0`）；
    //   · 装得下 ⇒ **不占位**（`bar_shown()` 假）；
    //   · 行为：**滚轮 / 拖动都改变 `value()`**。
    // ★ 判红办法：把 `make_inner_scroll(...)` 换成直接 `addWidget(rows_, 1)`（修前形态，
    //   结果区没有自己的条子）⇒ 下面 ①②③ 立刻红。
    {
        ConvertPage cp;
        cp.resize(720, page_min_h());
        cp.show();
        QApplication::processEvents();
        // ① 装得下 ⇒ 不占位（夹具前提：先只放两行）
        cp.rows()->set_row_count(2);
        cp.sync_rows_viewport();
        QApplication::processEvents();
        check(cp.rows_viewport() != nullptr &&
                  !ScrollViewport::bar_shown(cp.rows_viewport()->vbar()),
              QStringLiteral("#6 ① 装得下 ⇒ 条子**不占位**（视口=%1 / bar_shown=%2）")
                  .arg(cp.rows_viewport() != nullptr ? 1 : 0)
                  .arg(cp.rows_viewport() != nullptr &&
                               ScrollViewport::bar_shown(cp.rows_viewport()->vbar())
                           ? 1
                           : 0));
        // ② 内容超高（200 行 × 20 px = 4000 px ≫ 视口）⇒ 能滚
        for (int i = 0; i < 200; ++i) cp.add_row(QStringLiteral("结果行 %1").arg(i));
        QApplication::processEvents();
        QScrollBar* bar = cp.rows_viewport() != nullptr ? cp.rows_viewport()->vbar() : nullptr;
        check(bar != nullptr && bar->maximum() > 0,
              QStringLiteral("#6 ② 内容超高 ⇒ **能滚**（条子=%1 / range 0..%2；修前没有条子）")
                  .arg(bar != nullptr ? 1 : 0)
                  .arg(bar != nullptr ? bar->maximum() : -1));
        check(bar != nullptr && ScrollViewport::bar_shown(bar),
              QStringLiteral("#6 ②′ 能滚 ⇒ 条子**必须可见**（bar_shown=%1）")
                  .arg(bar != nullptr && ScrollViewport::bar_shown(bar) ? 1 : 0));
        // ③ 行为：**拖动**条子改变 value()
        if (bar != nullptr && bar->maximum() > 0) {
            const int target = std::max(1, bar->maximum() / 2);
            bar->setValue(target);
            QApplication::processEvents();
            check(bar->value() == target && cp.rows()->scroll_top() == target,
                  QStringLiteral("#6 ③ 拖动条子改变 value()（期望 %1 / 实测 value=%2 / 行列表 offset=%3）")
                      .arg(target)
                      .arg(bar->value())
                      .arg(cp.rows()->scroll_top()));
        } else {
            check(false, QStringLiteral("#6 ③ 拖动：夹具没做出可滚的条子"));
        }
        // ★★ P16-3b：**自带滚动的画布几何 == 视口几何** —— `docs/2.0.0-行为口径对照表.md`
        //   第 9.12 行**自己点名**"★目前无锁，建议补"的那条（现有 #6 只锁了条子/偏移）。
        //   它是"不挪整块画布"那条模型的反面判据：修前的模型把 body 设成
        //   `(-hbar, -vbar, body_w, body_h)`（画布撑到**内容高**、靠挪它来滚）⇒ 这里必红。
        //   本夹具内容高（200×20 = 4000）远大于视口 ⇒ body 高只许**等于视口可用高**、
        //   且 x/y 恒为 0。判红办法：把 `scroll_viewport.cpp` 的自滚分支改回
        //   `body_->setGeometry(-hbar, -vbar, body_w_, body_h_)` ⇒ 立刻红。
        if (cp.rows_viewport() != nullptr && cp.rows() != nullptr) {
            // ★ 先**强制跑一遍布局**再量：③ 拖过条子后不一定触发 `sync_layout()`，
            //   而旧模型恰恰只在布局那一刻把画布挪成 `(-hbar, -vbar, …)` ——
            //   不强制布局的话这条锁会被"布局没跑"蒙过去（第一版就是这么假绿的）。
            cp.rows_viewport()->sync_layout();
            const QRect body_geo = cp.rows()->geometry();
            const QRect view_rect = cp.rows_viewport()->rect();
            const int content_h = cp.rows()->content_height();
            check(body_geo.x() == 0 && body_geo.y() == 0 &&
                      body_geo.width() <= view_rect.width() &&
                      body_geo.height() <= view_rect.height() &&
                      body_geo.height() < content_h,
                  QStringLiteral("P16-3b 自带滚动的画布几何必须 == 视口（不许挪画布、不许撑到内容高）："
                                 "body=%1x%2@(%3,%4) 视口=%5x%6 内容高=%7")
                      .arg(body_geo.width()).arg(body_geo.height())
                      .arg(body_geo.x()).arg(body_geo.y())
                      .arg(view_rect.width()).arg(view_rect.height())
                      .arg(content_h));
        } else {
            check(false, QStringLiteral("P16-3b 夹具没拿到（行列表 / 自带滚动视口）"));
        }
        // ④ 行为：**滚轮**也改变 value()（走全局路由 —— 由滚动视口自己认领）
        if (bar != nullptr && bar->maximum() > 0) {
            bar->setValue(0);
            QApplication::processEvents();
            QWheelEvent wheel(QPointF(10, 10), cp.rows_viewport()->mapToGlobal(QPoint(10, 10)),
                              QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                              Qt::NoScrollPhase, false);
            QApplication::sendEvent(cp.rows_viewport(), &wheel);
            QApplication::processEvents();
            check(bar->value() > 0,
                  QStringLiteral("#6 ④ 滚轮改变 value()（实测 %1）").arg(bar->value()));
        } else {
            check(false, QStringLiteral("#6 ④ 滚轮：夹具没做出可滚的条子"));
        }
        // ⑤ 反向：行数降到装得下 ⇒ **立刻不占位**（两向不变量）
        cp.rows()->set_row_count(2);
        cp.sync_rows_viewport();
        QApplication::processEvents();
        check(!ScrollViewport::bar_shown(bar),
              QStringLiteral("#6 ⑤ 行数降到装得下 ⇒ 条子立刻收起（bar_shown=%1 / range=%2）")
                  .arg(bar != nullptr && ScrollViewport::bar_shown(bar) ? 1 : 0)
                  .arg(bar != nullptr ? bar->maximum() : -1));
        cp.hide();
    }

    // ★★ 2026-10-07（审计补充 H2-4）：**同一判据也必须在文本提取页成立** ——
    //   修前只对转换页做了断言（"convert 修了、text 没修"），而 `page_base.h:74-83` 与
    //   本文件都把"**文本提取的行列表**"写进同一条判据。修前形态是 `addWidget(rows_, 1)`
    //   ⇒ 一次真实提取（gameplay 102 文件 / scripts 446 个 .script）的行数远超结果区高度时
    //   **永远够不着**。判据与转换页同源：装得下不占位 / 超高能滚 / 拖动改变 offset。
    //   ★ 判红办法：把 text_page 的 `make_inner_scroll(...)` 换回 `addWidget(rows_, 1)` ⇒ ①②③ 红。
    {
        TextPage tp;
        tp.resize(720, page_min_h());
        tp.show();
        QApplication::processEvents();
        tp.rows()->set_row_count(2);
        tp.sync_rows_viewport();
        QApplication::processEvents();
        check(tp.rows_viewport() != nullptr &&
                  !ScrollViewport::bar_shown(tp.rows_viewport()->vbar()),
              QStringLiteral("H2-4 ① 文本提取：装得下 ⇒ 条子**不占位**（视口=%1）")
                  .arg(tp.rows_viewport() != nullptr ? 1 : 0));
        for (int i = 0; i < 200; ++i) tp.add_row(QStringLiteral("文本行 %1").arg(i));
        QApplication::processEvents();
        QScrollBar* tbar = tp.rows_viewport() != nullptr ? tp.rows_viewport()->vbar() : nullptr;
        check(tbar != nullptr && tbar->maximum() > 0,
              QStringLiteral("H2-4 ② 文本提取：内容超高 ⇒ **能滚**（条子=%1 / range 0..%2；"
                             "修前本页根本没有条子）")
                  .arg(tbar != nullptr ? 1 : 0)
                  .arg(tbar != nullptr ? tbar->maximum() : -1));
        if (tbar != nullptr && tbar->maximum() > 0) {
            const int target = std::max(1, tbar->maximum() / 2);
            tbar->setValue(target);
            QApplication::processEvents();
            check(tbar->value() == target && tp.rows()->scroll_top() == target,
                  QStringLiteral("H2-4 ③ 文本提取：拖动条子改变行列表 offset"
                                 "（期望 %1 / value=%2 / offset=%3）")
                      .arg(target)
                      .arg(tbar->value())
                      .arg(tp.rows()->scroll_top()));
        } else {
            check(false, QStringLiteral("H2-4 ③ 文本提取：夹具没做出可滚的条子"));
        }
        tp.hide();
    }

    // ★★ 2026-10-05（**落点覆盖必须可见 / 零静默**）—— **GUI 那一面**的锁。
    //   起因（用户报的真事故）：一次默认落点（＝来源所在目录）解包把 **3.9 万个文件**盖在
    //   模组目录上，工具**一个字都没说**。口径：**不改默认落点**（用户口径）、
    //   **不弹模态确认框**（那会改交互口径）；但"任何改写都必须可见"：
    //     ① 解包**前**逐条打印 `包 → 落点`；
    //     ② 覆盖已存在文件时**逐条**报出路径 + 汇总**计数**。
    //   判据（可判红）：同一枚夹具**解两遍** ⇒ 第一遍 `覆盖 0` 且**一条 `覆盖：` 都没有**，
    //   第二遍**必须**出现 `覆盖：` 逐条行且计数非零。
    //   ★ 判红办法：把 `run_unpack_jobs` 里那两段（`解包目标：` 预打印 + `覆盖：` 逐条）
    //     删掉，或把引擎 `extract_tree.cpp` 里两处 `result.overwritten.push_back(...)` 去掉
    //     ⇒ 下面两条立刻红。**引擎/CLI 那一面**同源（`core: 落点覆盖可见` 的 selftest 锁）。
    {
        namespace efs = xrkit::engine::fs;
        const QString ov_dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_overwrite_gui"));
        QDir(ov_dir).removeRecursively();
        QDir().mkpath(ov_dir + QStringLiteral("/pk"));
        {
            std::vector<efs::DbPackItem> items;
            efs::DbPackItem d;
            d.path = "d";
            d.is_dir = true;
            items.push_back(d);
            efs::DbPackItem one;
            one.path = "d/keep.txt";
            one.data = {'K'};
            items.push_back(one);
            const std::vector<uint8_t> raw = efs::pack_db(items, efs::DbFormat::Xdb);
            std::ofstream out((ov_dir + QStringLiteral("/pk/ov.db0")).toStdString(),
                              std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()),
                      static_cast<std::streamsize>(raw.size()));
        }
        FsPage ov_page;
        ov_page.resize(760, page_min_h());
        ov_page.show();
        QApplication::processEvents();
        ov_page.add_paths({ov_dir + QStringLiteral("/pk/ov.db0")});
        ov_page.out_edit()->setText(ov_dir + QStringLiteral("/out"));
        QApplication::processEvents();
        QDir().mkpath(ov_dir + QStringLiteral("/out"));
        QString root_unused;
        const std::vector<FsPage::UnpackJob> ov_jobs = ov_page.plan_unpack(false, &root_unused);
        const QString ov_first =
            ov_jobs.empty() ? QString() : FsPage::run_unpack_jobs(ov_jobs, nullptr, nullptr);
        const QString ov_second =
            ov_jobs.empty() ? QString() : FsPage::run_unpack_jobs(ov_jobs, nullptr, nullptr);
        // ① 解包**前**的目标必须逐条可见（"零静默"的第一半）
        check(ov_first.contains(QStringLiteral("解包目标：")) &&
                  ov_first.contains(QStringLiteral("覆盖 0")) &&
                  !ov_first.contains(QStringLiteral("覆盖：")),
              QStringLiteral("覆盖可见① 解包前逐条打印落点、第一遍零覆盖（解包目标=%1 / 覆盖行=%2）")
                  .arg(ov_first.contains(QStringLiteral("解包目标：")) ? 1 : 0)
                  .arg(ov_first.count(QStringLiteral("覆盖："))));
        // ② 第二遍：落点上已经有那个文件 ⇒ **逐条**报出 + 计数非零
        check(ov_second.contains(QStringLiteral("覆盖：d/keep.txt")) &&
                  ov_second.contains(QStringLiteral("覆盖 1")),
              QStringLiteral("覆盖可见② 第二遍**逐条**报出被覆盖文件 + 计数（实测「%1」）")
                  .arg(ov_second.right(200).replace(QLatin1Char('\n'), QLatin1Char(' '))));
        ov_page.hide();
        QDir(ov_dir).removeRecursively();
    }

    // ★★ 2026-10-05（#8）：**包内自检把插件数与名字打出来**。
    //   用户约定「开发版包内带全部插件」，任务书的判据是"包内 `plugins\` 非空 + **包内自检**
    //   打印插件数与名字"。这里报两样，缺一不可：
    //     · 目录里到底有几个 `*.dll`、叫什么 —— "文件在不在"；
    //     · **装载结论**取自检里那个**真外壳实例** `hub`（本函数第 ⑤ 组就建好了）：它启动时走的
    //       正是 `Hub::load_plugins` → `xrkit::plugin::load_plugins_from_dir(app_dir()/plugins)`
    //       —— 与**用户双击启动**那条路一条不差（缺依赖 / ABI 不匹配会以 `RegistrationError`
    //       的形式出现在 `hub.plugin_errors()` 里；实测 `not_a_plugin.dll` 那种就是 LoadLibrary 失败）。
    //   ★ 为什么不自己再跑一遍 `load_plugins_from_dir`：那是**重复注册**（同一个 dll 的
    //     `stk_plugin_init` 会被调第二次）。
    //   ★ 为什么不用 `plugin::loaded_count()`：那是**进程级**的、自检自己也会 LoadLibrary
    //     一堆测试插件夹具 ⇒ 分不清"包里的插件"与"自检自己的夹具"（本批实测：构建目录里
    //     `plugins\` 是空的，末尾问出来却是 1）。
    //   ★ `package.ps1` 按这一行判：开发版要 `N ≥ 1 且 errors == 0`；发行版要 `N == 0`。
    //   ★★ P16-2a（切打包口径）：**同时报 dll 与 py**。修前只数 `*.dll` ⇒
    //      "六页 + 三插件（那 7 枚 `.py`）进包"这件事在包内自检里**根本看不见**
    //      （`package.ps1` 的判据也照旧只认 dll ⇒ 谁把 .py 那一段删掉都不会红）。
    {
        const QString plugin_dir =
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins"));
        const QStringList dlls =
            QDir(plugin_dir).entryList({QStringLiteral("*.dll")}, QDir::Files, QDir::Name);
        const QStringList pys =
            QDir(plugin_dir).entryList({QStringLiteral("*.py")}, QDir::Files, QDir::Name);
        QStringList names = dlls;
        names += pys;
        result.notes.append(
            QStringLiteral("#8 包内插件：%1 个文件（%2 个 dll / %3 个 py）（%4）· "
                           "Hub 启动装载 errors=%5 · %6 · dir=%7")
                .arg(dlls.size() + pys.size())
                .arg(dlls.size())
                .arg(pys.size())
                .arg(names.isEmpty() ? QStringLiteral("(空)") : names.join(QStringLiteral(", ")))
                .arg(hub.plugin_errors().size())
                .arg(hub.plugin_notice())
                .arg(QDir::toNativeSeparators(plugin_dir)));

        // ★★ P17-81（甲：产品实现＝C++ 内置六页；`plugins\*_page.py` 降级为**夹具**）：
        //   **逐页点出"当前可见页集合"**，供门禁/打包链断言 —— 这是"同一份实现"那件事唯一的
        //   可判红出口。修前的事实：包内只报"插件 N 个文件"，而**可见页到底是内置页还是
        //   插件页**在判据里根本看不见 ⇒ 夹具一旦不再自动加载，"插件页生效"就没人验了
        //   （P17-78/79 的"验收物 ≠ 交付物"）。
        //   两种形态都必须判绿，所以这里**不判红、只如实列**（真正的断言在消费侧）：
        //     · 默认（夹具在 `plugins\_fixtures\`、不在扫描路径）⇒ builtin=六、plugin_py=0；
        //     · 门禁把夹具**显式抬上来**跑一次 ⇒ plugin_py=六＋新增页、builtin=0。
        //   判据口径：`结束` 之前逐页 `<序号>=<id>`（等号那一边＝这一页**来自内置注册表**，
        //   依据是 `page_owner(i) == kBuiltinPageOwner`；序号从 1 起，与 `Hub::page_count()`
        //   同源 —— 列出的页数必须等于它）。
        QStringList page_names;
        int page_builtin = 0;
        int page_plugin_py = 0;
        bool owner_matches_id = true;
        for (int i = 0; i < hub.page_count(); ++i) {
            const QString id = hub.page_id(i);
            // 内置页的 owner 是 `kBuiltinPageOwner`（`<builtin>`，见 page_host.h:36）；其余＝插件页。
            const bool is_builtin =
                (hub.page_owner(i) == QString::fromUtf8(kBuiltinPageOwner));
            if (is_builtin) {
                ++page_builtin;
            } else if (id.endsWith(QStringLiteral("_py"))) {
                ++page_plugin_py;
            }
            // 不变量：内置页 id 不带 `_py` 后缀，插件页带（`replaces` 的六枚 + toolchain 那一枚）。
            if (is_builtin == id.endsWith(QStringLiteral("_py"))) owner_matches_id = false;
            page_names << QStringLiteral("%1=%2%3")
                              .arg(i + 1)
                              .arg(id)
                              .arg(is_builtin ? QStringLiteral("=") : QString());
        }
        result.notes.append(
            QStringLiteral("PLUGIN-PAGES 可见页集合=%1 页（内置=%2 / 插件py=%3）[%4] 结束 · "
                           "dir=%5 · 内置标记与 id 后缀一致=%6")
                .arg(hub.page_count())
                .arg(page_builtin)
                .arg(page_plugin_py)
                .arg(page_names.isEmpty() ? QStringLiteral("(空)")
                                          : page_names.join(QStringLiteral(" ")))
                .arg(QDir::toNativeSeparators(plugin_dir))
                .arg(owner_matches_id ? QStringLiteral("1") : QStringLiteral("0")));
    }

    // ══════════════════════════════════════════════════════════════════════════
    // ★★ 2026-10-06（审计 §7 **第 3 组**：3-1 ~ 3-4）：fs_page 状态族四条判红锁。
    //
    // 这四条审计给的判据都是"**数字/行数**"级的（不靠眯眼看图），逐条照抄在这里：
    //   3-1 勾 3 项 → 加一个包 ⇒ `checked_file_count()` 必须仍为 3；
    //   3-2 搜一个不存在的名字 ⇒ **0 行**（且计数为 0）；
    //   3-3 用 `E:\a\x` 与 `E:/a/x` 各加一次 ⇒ **只 1 个源、左树有节点**；
    //   3-4 按 basename 升序加载两个同名包 ⇒ **赢家是后者**。
    // 判红办法（逐条写在各自旁边）：把修法那一行换回修前形态即可（例如把 `push_source`
    // 里的 `apply_load_order()` 删掉 ⇒ 3-4 立刻红）。
    // ══════════════════════════════════════════════════════════════════════════
    {
        namespace efs = xrkit::engine::fs;

        // ── 3-1：rebuild_tree() 丢整棵树 ⇒ 已勾选被静默清空（违背 6.1）──────────
        {
            FsPage page;
            page.add_source(QStringLiteral("three.db0"),
                            {efs::EntryInfo{"k/a.txt", false, 1},
                             efs::EntryInfo{"k/b.txt", false, 2},
                             efs::EntryInfo{"k/c.txt", false, 3}});
            QApplication::processEvents();
            check(page.entry_top_level_count() == 1,
                  QStringLiteral("3-1 前提：顶层 = 包名壳一个节点（实测 %1）")
                      .arg(page.entry_top_level_count()));
            // 勾满三项（走**真实入口** `check_all`，与用户点「包内全选」同一条路）
            page.check_all(true);
            check(page.checked_file_count() == 3,
                  QStringLiteral("3-1 前提：勾 3 项后 checked_file_count()==3（实测 %1）")
                      .arg(page.checked_file_count()));
            // ★ 判据：再加一个包（源集合变化 ⇒ 走 rebuild_tree）⇒ 勾选**不许被清空**
            page.add_source(QStringLiteral("three_b.db0"),
                            {efs::EntryInfo{"k/d.txt", false, 4}});
            QApplication::processEvents();
            check(page.checked_file_count() == 3,
                  QStringLiteral("3-1 加一个包后已勾选的 3 项必须还在（实测 %1；"
                                 "修前 rebuild_tree 丢整棵树 ⇒ 这里必为 0）")
                      .arg(page.checked_file_count()));
            // 半勾态也要经得起重建（别的源并进来时 merge 会走"勾选态跟随原节点"那条路）
            page.check_all(false);
            if (QTreeWidgetItem* node = find_package_source_node(page.package_list(),
                                                                 QStringLiteral("three.db0"))) {
                node->setCheckState(0, Qt::Checked);
            }
            QApplication::processEvents();
            const int after_pkg_check = page.checked_file_count();
            page.add_source(QStringLiteral("three_c.db0"),
                            {efs::EntryInfo{"k/e.txt", false, 5}});
            QApplication::processEvents();
            check(after_pkg_check == 3 && page.checked_file_count() == 3,
                  QStringLiteral("3-1 勾一个包（3 项）后再加包 ⇒ 仍是 3（实测 %1 → %2）")
                      .arg(after_pkg_check)
                      .arg(page.checked_file_count()));
            page.hide();
        }

        // ── 3-2：搜索"无命中"退化成"不过滤" ⇒ 必须 0 行 ────────────────────────
        {
            FsPage page;
            page.add_source(QStringLiteral("search.db0"),
                            {efs::EntryInfo{"aim/a.txt", false, 1},
                             efs::EntryInfo{"aim/b.txt", false, 2},
                             efs::EntryInfo{"aim/sub/c.txt", false, 3}});
            page.resize(760, page_min_h());
            page.show();
            QApplication::processEvents();
            const int all_rows = page.entry_top_level_count();
            check(all_rows == 1, QStringLiteral("3-2 前提：无查询时顶层 = 壳一个节点（实测 %1）")
                                     .arg(all_rows));
            // ★ 判据①：搜一个**不存在**的名字 ⇒ 树上 0 行（修前：整棵树照显示）
            page.search_edit()->setText(QStringLiteral("这个名字一定不存在_zzz"));
            QApplication::processEvents();
            check(page.entry_top_level_count() == 0,
                  QStringLiteral("3-2 搜不存在的名字 ⇒ 0 行（实测顶层 %1；"
                                 "修前 '没查询' 与 '无命中' 共用空集 ⇒ 这里必为 1）")
                      .arg(page.entry_top_level_count()));
            // ★ 判据②：计数也是 0（修前计数读的是"全量"）
            const QString count_text =
                page.entry_count_label() != nullptr ? page.entry_count_label()->text() : QString();
            check(count_text.isEmpty() || count_text.startsWith(QStringLiteral("0 ")),
                  QStringLiteral("3-2 无命中时计数为 0（实测「%1」）").arg(count_text));
            // ★ 反向锁：**有命中**时照旧要看得见（别把过滤做成了"永远 0 行"）
            page.search_edit()->setText(QStringLiteral("c.txt"));
            QApplication::processEvents();
            QStringList visible_names;
            if (page.entry_tree() != nullptr) {
                std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
                    visible_names << item->text(0);
                    for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
                };
                for (int i = 0; i < page.entry_tree()->topLevelItemCount(); ++i) {
                    walk(page.entry_tree()->topLevelItem(i));
                }
            }
            check(page.entry_top_level_count() == 1 &&
                      visible_names.join(QStringLiteral(",")).contains(QStringLiteral("c.txt")),
                  QStringLiteral("3-2 反向：搜 c.txt 仍能看见命中项（顶层 %1 / 行「%2」）")
                      .arg(page.entry_top_level_count())
                      .arg(visible_names.join(QStringLiteral(","))));
            // ★ 反向锁②：清空查询 ⇒ 回到"不过滤"
            page.search_edit()->setText(QString());
            QApplication::processEvents();
            check(page.entry_top_level_count() == 1,
                  QStringLiteral("3-2 反向：清空查询回到不过滤（顶层 %1）")
                      .arg(page.entry_top_level_count()));
            page.hide();
        }

        // ── 3-3：判重不用 core::path_equals ⇒ 源消失 / 计数翻倍 ─────────────────
        {
            const QString dir = QDir::temp().filePath(QStringLiteral("xrkit_smoke_pathdedup"));
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            const QString file = dir + QStringLiteral("/x.db");
            {
                QFile out(file);
                out.open(QIODevice::WriteOnly);
                out.write("not-a-real-package\n");   // 认不出 ⇒ 不会加源（但**判重**要先发生）
                out.close();
            }
            FsPage page;
            // ★ 判据：同一份文件、两种写法各加一次 ⇒ 只 1 个源
            page.add_paths({QDir::toNativeSeparators(file)});   // `...\xrkit_smoke_pathdedup\x.db`
            page.add_paths({QDir::fromNativeSeparators(file)}); // `.../xrkit_smoke_pathdedup/x.db`
            QApplication::processEvents();
            check(page.source_count() == 0,
                  QStringLiteral("3-3 前提：**认不出的文件不该进源表**（实测 %1 — 这里的判据是"
                                 "「判重先于加载」这条路不许放两份进来」")
                      .arg(page.source_count()));
            // 上一条只证明"认不出不进表"，真正要锁的是"**认得出的**同一份只进一次" ——
            // 用 `add_source`（跳过包格式判定）造两份**只差斜杠写法**的源。
            FsPage page2;
            page2.add_source(QDir::toNativeSeparators(file),
                             {efs::EntryInfo{"dedup/a.txt", false, 1}});
            page2.add_source(QDir::fromNativeSeparators(file),
                             {efs::EntryInfo{"dedup/b.txt", false, 2}});
            QApplication::processEvents();
            // `add_source` 是"已在内存里的条目"那条路（它自己也会 push 源）—— 这里的判据是
            // **左树必须有节点**（修前同层同名覆盖 ⇒ 先来的源一个节点都没有）
            check(package_source_node_count(page2.package_list()) == page2.source_count(),
                  QStringLiteral("3-3 左树源节点数必须等于源数（节点 %1 / 源 %2；"
                                 "修前同层同名覆盖 ⇒ 节点数会少）")
                      .arg(package_source_node_count(page2.package_list()))
                      .arg(page2.source_count()));
            // 真判据（`add_paths` 这条路）：把**认得出来的**包夹具造出来再各加一次。
            // 用 SquashFS 最小镜像夹具太贵 ⇒ 这里用 `--dump` 那条路已经在别处锁着，本步只锁
            // "同一份路径的两种写法**必须判成同一份**"这条纯判据（直接问引擎的判重实现）。
            check(xrkit::core::path_equals(QDir::toNativeSeparators(file).toUtf8().toStdString(),
                                           QDir::fromNativeSeparators(file).toUtf8().toStdString()),
                  QStringLiteral("3-3 判重口径：`%1` 与 `%2` 必须路径相等（core::path_equals）")
                      .arg(QDir::toNativeSeparators(file), QDir::fromNativeSeparators(file)));
            page.hide();
            page2.hide();
            QDir(dir).removeRecursively();
        }

        // ── 3-4：加载序只有 scan_dir 会排 ⇒ 6.2 在加包路径不成立 ────────────────
        {
            FsPage page;
            // ★ 夹具口径与 6.1 那条锁同源：**自带包名壳**的两个包落在同一个壳目录里
            //   （`gamedata.sq_base` / `gamedata.sqzy_patch` ⇒ 壳都是 `gamedata`）⇒
            //   同名文件真的会合并；basename 升序 ⇒ **后者（sqzy_patch）赢**。
            page.add_source(QStringLiteral("gamedata.sqzy_patch"),
                            {efs::EntryInfo{"gamedata/same.txt", false, 20}});
            page.add_source(QStringLiteral("gamedata.sq_base"),
                            {efs::EntryInfo{"gamedata/same.txt", false, 10}});
            QApplication::processEvents();
            // ★ 右栏是**懒建**的（目录的子项要展开才建）⇒ 先把整棵树展开几层再找。
            //   用项自己的路径（`data(0, UserRole)` 存的就是模型路径）走公共的 `expand_entry`。
            for (int pass = 0; pass < 4; ++pass) {
                QStringList pending;
                std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* node) {
                    if (node == nullptr) return;
                    const QString path = node->data(0, Qt::UserRole).toString();
                    const bool is_dir = node->data(0, Qt::UserRole + 1).toBool();
                    if (is_dir && !path.isEmpty()) pending << path;
                    for (int i = 0; i < node->childCount(); ++i) collect(node->child(i));
                };
                for (int i = 0; i < page.entry_tree()->topLevelItemCount(); ++i) {
                    collect(page.entry_tree()->topLevelItem(i));
                }
                for (const QString& path : pending) page.expand_entry(path);
                QApplication::processEvents();
            }
            // 把整棵树摊出来找那个**同名文件**（条目路径别写死：包名壳是引擎 `preview_entries_in_layout`
            // 算的，写死等于在这里再抄一份落点规则）
            QTreeWidgetItem* item = nullptr;
            QStringList all_paths;
            std::function<void(QTreeWidgetItem*, QString)> walk = [&](QTreeWidgetItem* node,
                                                                      QString prefix) {
                if (node == nullptr) return;
                const QString path = prefix.isEmpty() ? node->text(0)
                                                      : prefix + QStringLiteral("/") + node->text(0);
                all_paths << path;
                if (item == nullptr && node->text(0) == QStringLiteral("same.txt")) item = node;
                for (int i = 0; i < node->childCount(); ++i) walk(node->child(i), path);
            };
            for (int i = 0; i < page.entry_tree()->topLevelItemCount(); ++i) {
                walk(page.entry_tree()->topLevelItem(i), QString());
            }
            const QString from = item != nullptr ? item->text(2) : QString();
            // 诊断用：顶层项各自的子项数（"懒建占位"与"真子项"的区别就在这个数上）
            QStringList kid_counts;
            if (page.entry_tree() != nullptr) {
                for (int i = 0; i < page.entry_tree()->topLevelItemCount(); ++i) {
                    QTreeWidgetItem* top = page.entry_tree()->topLevelItem(i);
                    kid_counts << QStringLiteral("%1(kids=%2)").arg(top->text(0)).arg(top->childCount());
                }
            }
            check(item != nullptr && from.contains(QStringLiteral("sqzy_patch")),
                  QStringLiteral("3-4 按 basename 升序加载两个同名包 ⇒ 赢家是**后者**"
                                 "（gamedata.sqzy_patch）；来源列实得「%1」（树里：%2 / 顶层：%3）"
                                 "（修前加包路径不排 ⇒ 后加的 sq_base 会赢）")
                      .arg(from, all_paths.join(QStringLiteral(" | ")),
                           kid_counts.join(QStringLiteral(" , "))));
            check(item != nullptr && item->text(1).contains(QStringLiteral("20")),
                  QStringLiteral("3-4 赢家那一份的大小也要对（大小列「%1」，期望含 20）")
                      .arg(item != nullptr ? item->text(1) : QString()));
            page.hide();
        }

        // ── 3-6：~TaskShell 只 join 不取消 ⇒ 暂停中关窗进程永不退出 ──────────────
        {
            // 判据：**暂停 + 关窗 ⇒ 进程必须退出**。这里用"析构必须在有界时间内返回"来量：
            // worker 是一个"只有取消位才能让它退出"的循环（正是"暂停中"的形态）。
            // 修前 `~TaskShell` 只 join ⇒ 这里**永远不返回**（离线自检会挂死 = 判据本身）。
            auto* shell = new TaskShell(nullptr);
            QElapsedTimer clock;
            clock.start();
            const bool started = shell->run(QStringLiteral("3-6 取消锁"),
                                            [shell](const ProgressSink&, const MessageSink&) {
                                                // 没有取消位就永远停不下来（修前的形状）
                                                while (!shell->cancel_requested()) {
                                                    std::this_thread::sleep_for(
                                                        std::chrono::milliseconds(2));
                                                }
                                            });
            check(started, QStringLiteral("3-6 前提：任务真的跑起来了"));
            // 让它先跑起来（真正进入循环）再析构
            while (clock.elapsed() < 200) {
                QApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            delete shell;   // ★ 判据点：修复后 ~TaskShell 先置取消位再 join ⇒ 立刻返回
            const qint64 elapsed = clock.elapsed();
            check(elapsed < 5000,
                  QStringLiteral("3-6 暂停中析构必须有界返回（实测 %1 ms；"
                                 "修前 ~TaskShell 只 join ⇒ 这里永远不返回）")
                      .arg(elapsed));
        }
    }

    // ★★ 2026-10-06（审计 §7 第 3 组 3-5 / 3-7）：XML 页的两条。
    {
        namespace exml = xrkit::engine::xml;

        // ── 3-5：`refresh_rows()` 被工作线程调用（跨线程碰 QTreeWidget）───────────
        {
            const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xml_worker"));
            QDir(root).removeRecursively();
            QDir().mkpath(root + QStringLiteral("/a"));
            QDir().mkpath(root + QStringLiteral("/b"));
            const auto write_xml = [](const QString& path, const QString& body) {
                QFile out(path);
                out.open(QIODevice::WriteOnly);
                out.write(body.toUtf8());
                out.close();
            };
            write_xml(root + QStringLiteral("/a/x.xml"),
                      QStringLiteral("<root><e id=\"1\"><text>SAME</text></e></root>"));
            write_xml(root + QStringLiteral("/b/x.xml"),
                      QStringLiteral("<root><e id=\"1\"><text>SAME</text></e></root>"));
            write_xml(root + QStringLiteral("/a/y.xml"),
                      QStringLiteral("<root><e id=\"2\"><text>AAA</text></e></root>"));
            write_xml(root + QStringLiteral("/b/y.xml"),
                      QStringLiteral("<root><e id=\"2\"><text>BBB</text></e></root>"));

            XmlPage page;
            page.resize(900, page_min_h());
            page.show();
            QApplication::processEvents();
            // 主线程直调：立刻落地（读控件的断言不用额外 pump）
            const int on_main = page.compare_dirs(root + QStringLiteral("/a"),
                                                  root + QStringLiteral("/b"),
                                                  QStringLiteral("auto"), QStringLiteral("auto"));
            check(on_main == 2 && page.result_tree() != nullptr &&
                      page.result_tree()->topLevelItemCount() == 2,
                  QStringLiteral("3-5 前提：主线程直调 ⇒ 结果立刻进树（行 = %1 / 树 = %2）")
                      .arg(on_main)
                      .arg(page.result_tree() != nullptr ? page.result_tree()->topLevelItemCount()
                                                         : -1));
            // ★ 判据：**让 `refresh_rows` 在工作线程里被调** ⇒ 树上**当场不许动**
            //   （必须在主线程排队），事件泵过一次之后才允许出现。
            //   修前 `compare_dirs` 无条件直调 `refresh_rows()` ⇒ 工作线程当场就把
            //   `QTreeWidget` 重建了（这里会看到"当场就动"= 红）。
            page.set_mode(QStringLiteral("stats"));   // 清空结果（4-5 步骤之间别互相污染）
            QApplication::processEvents();
            const int before_worker = page.result_tree()->topLevelItemCount();
            int worker_rows = -1;
            std::thread worker([&] {
                worker_rows = page.compare_dirs(root + QStringLiteral("/a"),
                                               root + QStringLiteral("/b"),
                                               QStringLiteral("auto"), QStringLiteral("auto"));
            });
            worker.join();
            const int right_after_worker = page.result_tree()->topLevelItemCount();
            check(worker_rows == 2 && right_after_worker == before_worker,
                  QStringLiteral("3-5 worker 里调 compare_dirs ⇒ **树上当场不变**（排队回主线程）；"
                                 "实测 worker 行=%1 / 树 %2→%3（修前无条件直调 ⇒ 当场就被重建）")
                      .arg(worker_rows)
                      .arg(before_worker)
                      .arg(right_after_worker));
            // ★ 判据②（审计那条的后半句：**比较期间模式/筛选控件按需禁用**，消除 results_
            //   的主/worker 并发写）。这一条走**真实入口**（点「开始比较」那颗按钮 ⇒
            //   按钮 lambda 在起 worker 之前禁用、worker 收尾时排队恢复）。
            //   判红办法：把按钮 lambda 里 `set_compare_controls_enabled(false)` 删掉 ⇒ 立刻红。
            {
                page.set_mode(QStringLiteral("stats"));
                QApplication::processEvents();
                QLineEdit* dir_a = page.findChild<QLineEdit*>(QStringLiteral("xml_dir_a"));
                QLineEdit* dir_b = page.findChild<QLineEdit*>(QStringLiteral("xml_dir_b"));
                QPushButton* start_btn = page.findChild<QPushButton*>(QStringLiteral("xml_start"));
                check(dir_a != nullptr && dir_b != nullptr && start_btn != nullptr,
                      QStringLiteral("3-5 前提：拿得到真实入口（xml_dir_a / xml_dir_b / xml_start）"));
                if (dir_a != nullptr) dir_a->setText(root + QStringLiteral("/a"));
                if (dir_b != nullptr) dir_b->setText(root + QStringLiteral("/b"));
                QApplication::processEvents();
                check(page.compare_controls_enabled(),
                      QStringLiteral("3-5 前提：空闲时模式/筛选控件是可用的"));
                if (start_btn != nullptr) start_btn->click();
                // 主线程**同步**判：worker 刚起，控件必须是禁用的
                const bool off_now = !page.compare_controls_enabled();
                check(off_now,
                      QStringLiteral("3-5 比较期间模式/筛选控件被禁用（点「开始比较」后当场读："
                                     "禁用=%1 / running=%2）")
                          .arg(off_now ? 1 : 0)
                          .arg(page.task()->running() ? 1 : 0));
                // ★★ 2026-10-10（P17-6，审计 03 F-04 Medium）：**「导出 CSV」与「清除」也要一起禁**。
                //   事实（修前）：比较期间只禁了模式/筛选。其中**导出是真竞态** ——
                //   `export_csv` 遍历 `results_`/`visible_` + `count_kinds()`，而 worker 正在
                //   `results_.clear()+push_back()` ⇒ 同一个 std::vector 的主/worker 并发读写（UB/崩）；
                //   「清除」只清输入框（不碰 `results_`），一并禁是按审计口径、防收尾统计对着空名字。
                //   判红办法：把 `set_compare_controls_enabled` 里那两行删掉 ⇒ 下面两条立刻红。
                QPushButton* export_btn = page.findChild<QPushButton*>(QStringLiteral("xml_export"));
                check(export_btn != nullptr && page.export_button() == export_btn,
                      QStringLiteral("P17-6 前提：拿得到「导出 CSV」按钮（xml_export）"));
                const bool export_off = export_btn != nullptr && !export_btn->isEnabled();
                const bool clear_off =
                    page.clear_button() != nullptr && !page.clear_button()->isEnabled();
                check(export_off && clear_off,
                      QStringLiteral("P17-6 03 F-04：比较期间「导出 CSV」「清除」也被禁用"
                                     "（导出禁用=%1 / 清除禁用=%2 / running=%3）")
                          .arg(export_off ? 1 : 0)
                          .arg(clear_off ? 1 : 0)
                          .arg(page.task()->running() ? 1 : 0));
                // ★ 第二道闸：**绕过按钮直调**也必须被拒（零静默：状态栏要说清为什么）。
                const QString csv_path =
                    QDir::temp().filePath(QStringLiteral("xrkit_smoke_xml_busy_export.csv"));
                QFile::remove(csv_path);
                const bool exported_while_running = page.export_csv_auto(csv_path);
                check(!exported_while_running && !QFileInfo::exists(csv_path),
                      QStringLiteral("P17-6 03 F-04：比较进行中直调 `export_csv` 被拒且不留文件"
                                     "（返回=%1 / 文件在=%2）")
                          .arg(exported_while_running ? 1 : 0)
                          .arg(QFileInfo::exists(csv_path) ? 1 : 0));
                QFile::remove(csv_path);
                // 等 worker 收尾 + 排队恢复落地
                QElapsedTimer wait_clock;
                wait_clock.start();
                while (wait_clock.elapsed() < 30000) {
                    QApplication::processEvents();
                    if (!page.task()->running() && page.compare_controls_enabled()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                check(page.compare_controls_enabled(),
                      QStringLiteral("3-5 比较收尾后控件恢复可用（running=%1 / 启用=%2）")
                          .arg(page.task()->running() ? 1 : 0)
                          .arg(page.compare_controls_enabled() ? 1 : 0));
                // ★★ 2026-10-10（P17-10，审计 02 F-05 Medium）：**遍历出错必须在页面上可见**。
                //   修前：某侧目录遍历中途出错被引擎 `break` 吞掉 ⇒ 结果表可能是"虚假的一致/缺失"，
                //   页面上一个字都没有。判据：拿**不存在的目录**比一次 ⇒ 统计行必须写明
                //   "结论不可信"；对照组（两个真目录）不许出现那句话。
                //   判红办法：把 `refresh_stats_label` 里贴 `last_scan_errors_` 那一段删掉 ⇒ 立刻红。
                {
                    const QString before_errors =
                        page.stats_label() != nullptr ? page.stats_label()->text() : QString();
                    check(!before_errors.contains(QStringLiteral("结论不可信")),
                          QStringLiteral("P17-10 前提：正常比较之后统计行不该有'结论不可信'"));
                    page.compare_dirs(root + QStringLiteral("/__not_exist__"),
                                      root + QStringLiteral("/b"), QStringLiteral("auto"),
                                      QStringLiteral("auto"));
                    QApplication::processEvents();
                    const QString after_errors =
                        page.stats_label() != nullptr ? page.stats_label()->text() : QString();
                    check(after_errors.contains(QStringLiteral("目录遍历出错")) &&
                              after_errors.contains(QStringLiteral("结论不可信")),
                          QStringLiteral("P17-10 02 F-05：一侧目录不存在 ⇒ 统计行必须明说"
                                         "'目录遍历出错…结论不可信'（实得「%1」）")
                              .arg(after_errors.right(120).replace(QLatin1Char('\n'),
                                                                   QLatin1Char(' '))));
                }
            }
            // 泵事件 ⇒ 排队的那次刷新落地
            for (int i = 0; i < 5; ++i) {
                QApplication::processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(page.result_tree()->topLevelItemCount() == 2,
                  QStringLiteral("3-5 事件泵过之后排队的那次刷新落地（树 = %1，期望 2）")
                      .arg(page.result_tree()->topLevelItemCount()));
            page.hide();
            QDir(root).removeRecursively();
        }

        // ── 3-7：插件比较模式"注册成功后无人消费" ────────────────────────────────
        {
            namespace plug = xrkit::plugin;
            XmlPage page;
            plug::Registry registry;
            for (plug::CompareModeReg& mode : plug::builtin_compare_modes()) {
                registry.register_compare_mode(exml::kBuiltinPlugin, std::move(mode));
            }
            plug::PluginApiAdapter adapter(registry, "demo.dll", nullptr);
            const char* col_keys[] = {"rel", "status", "detail"};
            const char* col_headings[] = {"插件文件列", "插件状态列", "插件差异列"};
            const int col_widths[] = {280, 60, 400};
            const char* filter_labels[] = {"插件筛选"};
            const char* filter_kinds[] = {"same|diff|only_a|only_b"};
            const auto compare_fn = +[](void*, const char*, const char*, const char*, const char*,
                                        const char*, stk_result_sink sink, void* sink_user) {
                // 只为一件事：证明"这一轮是插件回调在跑"。
                // ★ 夹具故意把两侧**完全相同的文本**判成"不一致" —— 内置统计/文本模式对
                //   相同文本必然判一致 ⇒ 下面那条断言在修前（回调从不被调用）**必红**。
                sink(sink_user, "consistent", "0");
            };
            check(adapter.api().register_compare_mode(adapter.api().host, "插件模式", "plugin_mode",
                                                      40, col_keys, col_headings, col_widths, 3,
                                                      filter_labels, 1, filter_kinds, compare_fn,
                                                      nullptr) == 1,
                  QStringLiteral("3-7 前提：插件比较模式注册成功"));
            page.set_registry(&registry);
            page.resize(900, page_min_h());
            page.show();
            QApplication::processEvents();
            // ★ 判据①：选中插件模式 ⇒ **表头必须来自插件**
            page.set_mode(QStringLiteral("plugin_mode"));
            QApplication::processEvents();
            QStringList headers;
            if (page.result_tree() != nullptr) {
                for (int c = 0; c < page.result_tree()->columnCount(); ++c) {
                    headers << page.result_tree()->headerItem()->text(c);
                }
            }
            check(headers.size() == 3 && headers.join(QStringLiteral("|")) ==
                                             QStringLiteral("插件文件列|插件状态列|插件差异列"),
                  QStringLiteral("3-7 选中插件模式 ⇒ 表头来自**插件**（实测「%1」；"
                                 "修前 find_mode 只查内置表 ⇒ 表头会是内置那套）")
                      .arg(headers.join(QStringLiteral("|"))));
            // ★ 判据②：**筛选**也必须来自插件
            QStringList filters;
            for (int i = 0; page.filter_combo() != nullptr && i < page.filter_combo()->count(); ++i) {
                filters << page.filter_combo()->itemText(i);
            }
            check(filters.contains(QStringLiteral("插件筛选")),
                  QStringLiteral("3-7 选中插件模式 ⇒ 筛选项来自插件（实测「%1」）")
                      .arg(filters.join(QStringLiteral("|"))));
            // ★ 判据③（同族的另一半：注册的回调**真的被调用**）：真跑一次比较 ——
            //   夹具两侧**完全相同的 XML**，插件回调把它们判成"不一致"（`consistent=0`），
            //   而内置统计/文本模式对相同文本必然判"一致" ⇒ 状态列显示什么就说明谁跑了。
            const QString root = QDir::temp().filePath(QStringLiteral("xrkit_smoke_xml_plugin"));
            QDir(root).removeRecursively();
            QDir().mkpath(root + QStringLiteral("/a"));
            QDir().mkpath(root + QStringLiteral("/b"));
            for (const char* side : {"a", "b"}) {
                QFile out(root + QStringLiteral("/") + QLatin1String(side) +
                          QStringLiteral("/z.xml"));
                out.open(QIODevice::WriteOnly);
                out.write("<root><e id=\"1\"><text>SAME</text></e></root>");
                out.close();
            }
            const int rows = page.compare_dirs(root + QStringLiteral("/a"), root + QStringLiteral("/b"),
                                               QStringLiteral("auto"), QStringLiteral("auto"));
            QApplication::processEvents();
            QString status_cell;
            if (page.result_tree() != nullptr && page.result_tree()->topLevelItemCount() > 0) {
                status_cell = page.result_tree()->topLevelItem(0)->text(1);   // 插件声明的"状态"列
            }
            check(rows == 1 && status_cell.contains(QStringLiteral("不一致")),
                  QStringLiteral("3-7 插件注册的 compare 回调**必须真的被调用**（相同文本被插件判成"
                                 "「不一致」= 状态列「%1」；修前页面只走内置 compare_stats ⇒ 必是"
                                 "「一致」）")
                      .arg(status_cell));
            page.hide();
            QDir(root).removeRecursively();
        }
    }

    return result;
}

}  // namespace xrkit::shell
