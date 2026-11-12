// ★ 2026-11-10（用户）：**独立进程的插件管理器** —— 本体崩了/被插件拖死，它照样能开。
//
// 设计红线：
//   · **绝不 import/执行插件**：清单由 `xrkit_cli --py-plugins-scan`（纯 ast 解析）给出；
//     CLI 不可用时退化为"只列文件名"，也绝不 import。
//   · 启停持久化在 `<插件目录>/plugins.disabled`（core 的单一实现，宿主加载前跳过）。
//   · 自带 `--scan-json` 无界面模式（判据用；不开 QApplication 之外的窗口）。
//
// 用法：
//   xrkit_plugin_manager.exe [--plugins <目录>] [--reason <文本>]
//   xrkit_plugin_manager.exe --scan-json [--plugins <目录>]
#include "xrkit/core/plugin_disabled.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

QString app_dir() { return QCoreApplication::applicationDirPath(); }

QString find_cli() {
    const QString env = qEnvironmentVariable("XRKIT_CLI");
    if (!env.isEmpty() && QFileInfo::exists(env)) return env;
    // 包内：与本 exe 同目录（Qt DLL 也在这一层）。
    const QString same_dir = app_dir() + QStringLiteral("/xrkit_cli.exe");
    if (QFileInfo::exists(same_dir)) return QFileInfo(same_dir).absoluteFilePath();
    // 开发树：从 exe 目录逐级向上找 `xrkit_cli.exe` 与 `tools/cli/xrkit_cli.exe`。
    QDir dir(app_dir());
    for (int level = 0; level < 8; ++level) {
        const QStringList candidates = {
            dir.absolutePath() + QStringLiteral("/xrkit_cli.exe"),
            dir.absolutePath() + QStringLiteral("/tools/cli/xrkit_cli.exe"),
            dir.absolutePath() + QStringLiteral("/tools/xrkit_cli.exe"),
            dir.absolutePath() + QStringLiteral("/msvc-release/tools/cli/xrkit_cli.exe"),
            dir.absolutePath() + QStringLiteral("/build/msvc-release/tools/cli/xrkit_cli.exe"),
        };
        for (const QString& path : candidates) {
            if (QFileInfo::exists(path)) return QFileInfo(path).absoluteFilePath();
        }
        if (!dir.cdUp()) break;
    }
    return QString();
}

QString default_plugins_dir() {
    // 包内管理器在 `tools\`，插件目录在上一级；开发树 exe 在构建目录里则用 exe 同级。
    QString base = app_dir();
    const QFileInfo info(base);
    if (info.fileName().compare(QStringLiteral("tools"), Qt::CaseInsensitive) == 0) {
        base = info.absolutePath();
    }
    return base + QStringLiteral("/plugins");
}

int version_cmp(const QString& a, const QString& b) {
    const auto parts = [](const QString& v) {
        std::vector<long long> out;
        long long cur = 0;
        bool any = false;
        for (QChar ch : v) {
            if (ch.isDigit()) {
                cur = cur * 10 + ch.digitValue();
                any = true;
            } else if (ch == QLatin1Char('.')) {
                out.push_back(any ? cur : 0);
                cur = 0;
                any = false;
            } else {
                break;
            }
        }
        out.push_back(any ? cur : 0);
        return out;
    };
    std::vector<long long> pa = parts(a), pb = parts(b);
    const std::size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

bool version_satisfies(const QString& version, const QString& range) {
    if (range.trimmed().isEmpty() || range.trimmed() == QStringLiteral("*")) return true;
    for (const QString& raw : range.split(QLatin1Char(','))) {
        QString clause = raw.trimmed();
        if (clause.isEmpty()) continue;
        QString op = QStringLiteral("==");
        for (const QString& cand : {QStringLiteral(">="), QStringLiteral("<="),
                                    QStringLiteral("=="), QStringLiteral("!="),
                                    QStringLiteral(">"), QStringLiteral("<"),
                                    QStringLiteral("=")}) {
            if (clause.startsWith(cand)) {
                op = cand;
                clause = clause.mid(cand.size()).trimmed();
                break;
            }
        }
        const int cmp = version_cmp(version, clause);
        const bool ok = (op == QStringLiteral(">=") && cmp >= 0) ||
                        (op == QStringLiteral(">") && cmp > 0) ||
                        (op == QStringLiteral("<=") && cmp <= 0) ||
                        (op == QStringLiteral("<") && cmp < 0) ||
                        (op == QStringLiteral("!=") && cmp != 0) ||
                        (op == QStringLiteral("=") && cmp == 0) ||
                        (op == QStringLiteral("==") && cmp == 0);
        if (!ok) return false;
    }
    return true;
}

struct ScanItem {
    QString file, id, name, version, repo, note;
    QStringList provides;
    QMap<QString, QString> depends;
};

bool run_scan(const QString& plugins_dir, QString* json_line, QString* error) {
    if (!qEnvironmentVariableIsEmpty("XRKIT_PLUGIN_MANAGER_NO_SCAN")) {
        *error = QStringLiteral("静态扫描被 XRKIT_PLUGIN_MANAGER_NO_SCAN 禁用（判据夹具）");
        return false;
    }
    const QString cli = find_cli();
    if (cli.isEmpty()) {
        *error = QStringLiteral("找不到 xrkit_cli.exe（无法静态扫描；可设 XRKIT_CLI）");
        return false;
    }
    QProcess proc;
    proc.setProgram(cli);
    proc.setArguments({QStringLiteral("--py-plugins-scan"), plugins_dir});
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (!proc.waitForFinished(120000)) {
        *error = QStringLiteral("xrkit_cli --py-plugins-scan 超时/起不来：%1").arg(proc.errorString());
        return false;
    }
    const QString out = QString::fromUtf8(proc.readAll());
    const QStringList lines = out.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.startsWith(QStringLiteral("PYPLUGINSCAN-JSON "))) {
            *json_line = line.mid(QStringLiteral("PYPLUGINSCAN-JSON ").size());
            return true;
        }
    }
    *error = QStringLiteral("扫描没有返回 PYPLUGINSCAN-JSON（rc=%1）").arg(proc.exitCode());
    return false;
}

QVector<ScanItem> parse_items(const QString& json, const QVector<xrkit::plugin::DisabledPluginEntry>& disabled,
                              QString* error) {
    QVector<ScanItem> out;
    QJsonParseError parse_error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isArray()) {
        *error = QStringLiteral("扫描 JSON 解析失败：%1").arg(parse_error.errorString());
        return out;
    }
    for (const QJsonValue& value : doc.array()) {
        const QJsonObject obj = value.toObject();
        ScanItem item;
        item.file = obj.value(QStringLiteral("file")).toString();
        item.id = obj.value(QStringLiteral("id")).toString();
        item.name = obj.value(QStringLiteral("name")).toString(item.id);
        item.version = obj.value(QStringLiteral("version")).toString();
        item.repo = obj.value(QStringLiteral("repo")).toString();
        item.note = obj.value(QStringLiteral("note")).toString();
        for (const QJsonValue& p : obj.value(QStringLiteral("provides")).toArray()) {
            item.provides << p.toString();
        }
        const QJsonObject deps = obj.value(QStringLiteral("depends")).toObject();
        for (auto it = deps.begin(); it != deps.end(); ++it) {
            item.depends.insert(it.key(), it.value().toString());
        }
        Q_UNUSED(disabled);
        out.push_back(item);
    }
    return out;
}

bool file_disabled(const QVector<xrkit::plugin::DisabledPluginEntry>& list, const QString& file) {
    for (const auto& entry : list) {
        if (QString::fromStdString(entry.file).compare(file, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

void write_list(const QString& plugins_dir,
                const QVector<xrkit::plugin::DisabledPluginEntry>& entries, QString* error) {
    std::vector<xrkit::plugin::DisabledPluginEntry> native;
    for (const auto& entry : entries) native.push_back(entry);
    std::string why;
    if (!xrkit::plugin::write_disabled_plugins(plugins_dir.toStdString(), native, why)) {
        *error = QString::fromStdString(why);
    }
}

QString status_of(const ScanItem& item, const QVector<ScanItem>& all,
                  const QVector<xrkit::plugin::DisabledPluginEntry>& disabled) {
    if (file_disabled(disabled, item.file)) return QStringLiteral("已禁用");
    QStringList missing;
    for (auto it = item.depends.begin(); it != item.depends.end(); ++it) {
        bool found = false;
        for (const ScanItem& other : all) {
            if (other.id != it.key() && !other.provides.contains(it.key())) continue;
            found = version_satisfies(other.version, it.value());
            if (found) break;
        }
        if (!found) missing << QStringLiteral("%1(%2)").arg(it.key(), it.value());
    }
    if (!missing.isEmpty()) return QStringLiteral("缺前置：") + missing.join(QStringLiteral(", "));
    return QStringLiteral("已启用");
}

// ★ 2026-11-10（P0-1）：扫描不可用（CLI 缺失 / CPython 起不来 / 解析失败）时的**退化清单**：
//   只按文件名列出 `.py`，仍然允许启用/禁用 —— 这正是"坏插件把 Python 搞挂"时管理器要能救场的场景。
QVector<ScanItem> fallback_items(const QString& plugins_dir) {
    QVector<ScanItem> out;
    const QDir dir(plugins_dir);
    for (const QString& name : dir.entryList({QStringLiteral("*.py")}, QDir::Files, QDir::Name)) {
        ScanItem item;
        item.file = name;
        item.id = QString();           // 无法静态解析 ⇒ id 未知（详情里显式说明）
        item.name = name;
        item.version = QStringLiteral("-");
        item.note = QStringLiteral("静态扫描不可用：仅按文件名列出（仍可禁用/启用）");
        out.push_back(item);
    }
    for (const QString& name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir plugin_dir(dir.filePath(name));
        const bool has_entry = QFileInfo::exists(plugin_dir.filePath(QStringLiteral("plugin.py"))) ||
                               QFileInfo::exists(plugin_dir.filePath(QStringLiteral("__init__.py")));
        if (!has_entry) continue;
        ScanItem item;
        item.file = name;   // 目录插件的禁用键
        item.id = QString();
        item.name = name;
        item.version = QStringLiteral("-");
        item.note = QStringLiteral("静态扫描不可用：仅按目录名列出（仍可禁用/启用）");
        out.push_back(item);
    }
    return out;
}

class ManagerWindow : public QWidget {
public:
    ManagerWindow(QString plugins_dir, QString reason)
        : plugins_dir_(std::move(plugins_dir)) {
        setWindowTitle(QStringLiteral("插件管理器"));
        auto* layout = new QVBoxLayout(this);

        banner_ = new QLabel(this);
        banner_->setWordWrap(true);
        base_banner_ = reason.isEmpty()
                           ? QStringLiteral("插件目录：%1").arg(QDir::toNativeSeparators(plugins_dir_))
                           : QStringLiteral("%1\n插件目录：%2")
                                 .arg(reason, QDir::toNativeSeparators(plugins_dir_));
        banner_->setText(base_banner_);
        layout->addWidget(banner_);

        auto* buttons = new QHBoxLayout;
        auto* refresh_btn = new QPushButton(QStringLiteral("刷新"), this);
        auto* enable_btn = new QPushButton(QStringLiteral("启用"), this);
        auto* disable_btn = new QPushButton(QStringLiteral("禁用"), this);
        auto* open_dir_btn = new QPushButton(QStringLiteral("打开插件目录"), this);
        auto* check_update_btn = new QPushButton(QStringLiteral("检查插件更新"), this);
        auto* close_btn = new QPushButton(QStringLiteral("关闭"), this);
        buttons->addWidget(refresh_btn);
        buttons->addWidget(enable_btn);
        buttons->addWidget(disable_btn);
        buttons->addWidget(open_dir_btn);
        buttons->addWidget(check_update_btn);
        buttons->addStretch(1);
        buttons->addWidget(close_btn);
        layout->addLayout(buttons);

        table_ = new QTableWidget(this);
        table_->setColumnCount(5);
        table_->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("id"),
                                           QStringLiteral("名称"), QStringLiteral("版本"),
                                           QStringLiteral("状态")});
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(table_, 1);

        detail_ = new QTextEdit(this);
        detail_->setReadOnly(true);
        detail_->setMinimumHeight(110);
        layout->addWidget(detail_);

        connect(refresh_btn, &QPushButton::clicked, this, [this] { refresh(); });
        connect(enable_btn, &QPushButton::clicked, this, [this] { set_disabled(false); });
        connect(disable_btn, &QPushButton::clicked, this, [this] { set_disabled(true); });
        connect(open_dir_btn, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(plugins_dir_));
        });
        connect(check_update_btn, &QPushButton::clicked, this, [this] {
            const QString cli = find_cli();
            if (cli.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("检查插件更新"),
                                     QStringLiteral("找不到 xrkit_cli.exe，无法调用联网插件。"));
                return;
            }
            QProcess proc;
            proc.start(cli, {QStringLiteral("--py-plugin-call"), plugins_dir_,
                             QStringLiteral("{\"action\":\"check_updates\"}")});
            if (!proc.waitForFinished(20000)) {
                proc.kill();
                QMessageBox::warning(this, QStringLiteral("检查插件更新"),
                                     QStringLiteral("联网插件调用超时。"));
                return;
            }
            const QString output = QString::fromUtf8(proc.readAllStandardOutput() +
                                                     proc.readAllStandardError());
            const int marker = output.indexOf(QStringLiteral("PYPLUGINCALL result="));
            if (marker < 0) {
                QMessageBox::information(this, QStringLiteral("检查插件更新"),
                                         QStringLiteral("没有可用的联网检查入口：\n") + output.right(1200));
                return;
            }
            const QString json = output.mid(marker + 20).section(QLatin1Char('\n'), 0, 0).trimmed();
            QJsonParseError parse_error;
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parse_error);
            if (doc.isNull() || !doc.isObject() || !doc.object().value(QStringLiteral("ok")).toBool()) {
                QMessageBox::warning(this, QStringLiteral("检查插件更新"),
                                     QStringLiteral("联网插件返回无法解析：\n") + json);
                return;
            }
            QStringList lines;
            for (const QJsonValue& value : doc.object().value(QStringLiteral("plugins")).toArray()) {
                const QJsonObject item = value.toObject();
                lines << QStringLiteral("%1  %2  %3")
                             .arg(item.value(QStringLiteral("id")).toString(),
                                  item.value(QStringLiteral("version")).toString(),
                                  item.value(QStringLiteral("package")).toString());
            }
            QMessageBox::information(this, QStringLiteral("检查插件更新"),
                                     lines.isEmpty() ? QStringLiteral("没有插件条目")
                                                     : lines.join(QLatin1Char('\n')));
        });
        connect(close_btn, &QPushButton::clicked, this, &QWidget::close);
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { show_detail(); });
        resize(760, 480);
        refresh();
    }

private:
    QString selected_file() const {
        const int row = table_->currentRow();
        if (row < 0 || row >= items_.size()) return QString();
        return items_[row].file;
    }

    void refresh() {
        QString json, error;
        items_.clear();
        bool scanned = run_scan(plugins_dir_, &json, &error);
        if (scanned) {
            items_ = parse_items(json, disabled_, &error);
            if (!error.isEmpty()) {
                scanned = false;   // JSON 也读不了 ⇒ 退化为文件名列表
            }
        }
        if (!scanned) {
            items_ = fallback_items(plugins_dir_);
            banner_->setText(base_banner_ +
                             QStringLiteral("\n⚠ 静态扫描不可用：%1\n"
                                            "已退化为文件名列表（仍可禁用/启用）")
                                 .arg(error));
        } else {
            banner_->setText(base_banner_);
        }
        disabled_ = {};
        for (const auto& entry : xrkit::plugin::read_disabled_plugins(plugins_dir_.toStdString())) {
            disabled_.push_back(entry);
        }
        table_->setRowCount(items_.size());
        for (int row = 0; row < items_.size(); ++row) {
            const ScanItem& item = items_[row];
            table_->setItem(row, 0, new QTableWidgetItem(item.file));
            table_->setItem(row, 1, new QTableWidgetItem(item.id));
            table_->setItem(row, 2, new QTableWidgetItem(item.name));
            table_->setItem(row, 3, new QTableWidgetItem(item.version));
            table_->setItem(row, 4, new QTableWidgetItem(status_of(item, items_, disabled_)));
        }
        table_->resizeColumnsToContents();
        if (!scanned) detail_->setPlainText(error);
    }

    void show_detail() {
        const int row = table_->currentRow();
        if (row < 0 || row >= items_.size()) return;
        const ScanItem& item = items_[row];
        QStringList deps;
        for (auto it = item.depends.begin(); it != item.depends.end(); ++it) {
            deps << QStringLiteral("%1 %2").arg(it.key(), it.value());
        }
        detail_->setPlainText(
            QStringLiteral("文件：%1\nid：%2\n名称：%3\n版本：%4\nrepo：%5\n状态：%6\n"
                           "依赖：%7\n备注：%8")
                .arg(item.file, item.id, item.name, item.version,
                     item.repo.isEmpty() ? QStringLiteral("-") : item.repo,
                     status_of(item, items_, disabled_),
                     deps.isEmpty() ? QStringLiteral("-") : deps.join(QStringLiteral(", ")),
                     item.note.isEmpty() ? QStringLiteral("-") : item.note));
    }

    void set_disabled(bool disabled) {
        const QString file = selected_file();
        if (file.isEmpty()) return;
        QVector<xrkit::plugin::DisabledPluginEntry> entries;
        for (const auto& entry : disabled_) entries.push_back(entry);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&file](const auto& e) {
                                         return QString::fromStdString(e.file)
                                                    .compare(file, Qt::CaseInsensitive) == 0;
                                     }),
                      entries.end());
        if (disabled) {
            QString id;
            for (const ScanItem& item : items_) {
                if (item.file == file) id = item.id;
            }
            xrkit::plugin::DisabledPluginEntry entry;
            entry.file = file.toStdString();
            entry.id = id.toStdString();
            entry.reason = "管理器手动禁用";
            entries.push_back(entry);
        }
        QString error;
        write_list(plugins_dir_, entries, &error);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("插件管理器"), error);
            return;
        }
        refresh();
    }

    QString plugins_dir_;
    QVector<ScanItem> items_;
    QVector<xrkit::plugin::DisabledPluginEntry> disabled_;
    QLabel* banner_ = nullptr;
    QString base_banner_;
    QTableWidget* table_ = nullptr;
    QTextEdit* detail_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
    QString plugins_dir;
    QString reason;
    QString disable_file;
    QString enable_file;
    QString disable_id;
    bool scan_json = false;

    // Windows 的 char** argv 是本地 ANSI（中文路径/理由会变成 U+FFFD 菱形问号），
    // 不能提前 QString::fromUtf8(argv[i])。先建 QCoreApplication/QApplication，
    // 再用 QCoreApplication::arguments() 取 Unicode 参数。
    bool headless_arg = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a != nullptr &&
            (std::strcmp(a, "--scan-json") == 0 || std::strcmp(a, "--disable") == 0 ||
             std::strcmp(a, "--enable") == 0)) {
            headless_arg = true;
        }
    }
    std::unique_ptr<QCoreApplication> core_app;
    std::unique_ptr<QApplication> gui_app;
    if (headless_arg) {
        core_app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        gui_app = std::make_unique<QApplication>(argc, argv);
    }
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--plugins") && i + 1 < args.size()) {
            plugins_dir = args[++i];
        } else if (args[i] == QStringLiteral("--reason") && i + 1 < args.size()) {
            reason = args[++i];
        } else if (args[i] == QStringLiteral("--scan-json")) {
            scan_json = true;
        } else if (args[i] == QStringLiteral("--disable") && i + 1 < args.size()) {
            disable_file = args[++i];
        } else if (args[i] == QStringLiteral("--enable") && i + 1 < args.size()) {
            enable_file = args[++i];
        } else if (args[i] == QStringLiteral("--id") && i + 1 < args.size()) {
            disable_id = args[++i];
        }
    }
    // ★ 2026-11-10：`applicationDirPath()` 只有在 QCoreApplication 建好之后才有效。
    const bool headless_mode = scan_json || !disable_file.isEmpty() || !enable_file.isEmpty();
    if (headless_mode && plugins_dir.isEmpty()) plugins_dir = default_plugins_dir();
    if (!disable_file.isEmpty() || !enable_file.isEmpty()) {
        // 无界面启停（自动化/脚本用）；GUI 里是同样的读写路径。
        const QString file = disable_file.isEmpty() ? enable_file : disable_file;
        QVector<xrkit::plugin::DisabledPluginEntry> entries;
        for (const auto& entry : xrkit::plugin::read_disabled_plugins(plugins_dir.toStdString())) {
            entries.push_back(entry);
        }
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&file](const auto& e) {
                                         return QString::fromStdString(e.file)
                                                    .compare(file, Qt::CaseInsensitive) == 0;
                                     }),
                      entries.end());
        if (!disable_file.isEmpty()) {
            xrkit::plugin::DisabledPluginEntry entry;
            entry.file = file.toStdString();
            entry.id = disable_id.toStdString();
            entry.reason = "管理器命令行禁用";
            entries.push_back(entry);
        }
        QString error;
        write_list(plugins_dir, entries, &error);
        if (!error.isEmpty()) {
            fprintf(stderr, "PLUGIN-MANAGER error reason=%s\n", error.toUtf8().constData());
            return 1;
        }
        printf("PLUGIN-MANAGER ok action=%s file=%s\n",
               disable_file.isEmpty() ? "enable" : "disable", file.toUtf8().constData());
        return 0;
    }
    if (scan_json) {
        QString json, error;
        if (!run_scan(plugins_dir, &json, &error)) {
            // ★ P0-1：扫描不可用也输出**文件名清单**（exit 0）—— 调用方仍能禁用坏插件。
            QJsonArray arr;
            for (const ScanItem& item : fallback_items(plugins_dir)) {
                QJsonObject obj;
                obj.insert(QStringLiteral("file"), item.file);
                obj.insert(QStringLiteral("id"), QString());
                obj.insert(QStringLiteral("name"), item.name);
                obj.insert(QStringLiteral("version"), QString());
                obj.insert(QStringLiteral("repo"), QString());
                obj.insert(QStringLiteral("provides"), QJsonArray());
                obj.insert(QStringLiteral("depends"), QJsonObject());
                obj.insert(QStringLiteral("replaces"), QJsonArray());
                obj.insert(QStringLiteral("imports"), QJsonArray());
                obj.insert(QStringLiteral("note"), item.note);
                arr.append(obj);
            }
            const QByteArray compact = QJsonDocument(arr).toJson(QJsonDocument::Compact);
            printf("PYPLUGINSCAN-JSON %s\n", compact.constData());
            printf("PLUGIN-MANAGER warn scan=unavailable reason=%s\n", error.toUtf8().constData());
        } else {
            printf("PYPLUGINSCAN-JSON %s\n", json.toUtf8().constData());
        }
        for (const auto& entry : xrkit::plugin::read_disabled_plugins(plugins_dir.toStdString())) {
            printf("PLUGIN-MANAGER disabled file=%s id=%s reason=%s\n", entry.file.c_str(),
                   entry.id.c_str(), entry.reason.c_str());
        }
        return 0;
    }
    if (gui_app == nullptr) return 2;  // headless 分支必须已 return
    if (plugins_dir.isEmpty()) plugins_dir = default_plugins_dir();
    ManagerWindow window(plugins_dir, reason);
    window.show();
    return gui_app->exec();
}
