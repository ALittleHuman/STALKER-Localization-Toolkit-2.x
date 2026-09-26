// xrkit 构建 GUI（用户 2026-09-27 口径 v2）
//
// 只做三件事：**选**版本/标记、看**下一个序号**与**完整版本号**、按「开始构建」。
// 构建**不在这里实现** —— 一律由 CLI 动作负责（`--version-store` 只管版本文件、
// `--build-package` 管真编译+打包），这样命令行与 GUI 的结果**必然一致**（用户原话：
// "构建的具体实现代码不要单独一份，直接引用 CLI 的就行……这样也不会出现两者结果有差别的情况"）。
//
// 版本文件读写在 `xrkit::core::build_version`（Qt-free 的 core 层）—— 与 CLI 共用同一份实现。

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

#include "xrkit/core/build_version.h"

namespace {

// CLI 位置：与自己的 exe 同目录（构建 GUI 与 CLI 都在 msvc-shell/src/shell 下产出）
QString cli_path() {
    const QString dir = QCoreApplication::applicationDirPath();
    const QStringList names{"xrkit_cli.exe", "xrkit_cli"};
    for (const QString& n : names) {
        const QString p = dir + QDir::separator() + n;
        if (QFileInfo::exists(p)) return p;
    }
    // 开发期回退：仓库根的 build\msvc-release 那份
    const QString rel = dir + "/../../../../build/msvc-release/tools/cli/xrkit_cli.exe";
    return QFileInfo::exists(rel) ? QDir(rel).absolutePath() : QString();
}

}  // namespace

class BuildWindow : public QWidget {
public:
    BuildWindow() {
        setWindowTitle(QStringLiteral("xrkit 构建（2.0.0）"));
        auto* root_layout = new QVBoxLayout(this);

        auto* box = new QGroupBox(QStringLiteral("版本与标记（唯一事实来源：version.json）"), this);
        auto* form = new QFormLayout(box);

        combo_version_ = new QComboBox(box);   // **只能选**，不许手打
        combo_prerelease_ = new QComboBox(box);
        edit_index_ = new QLineEdit(box);      // 只读：下一个构建的序号
        edit_index_->setReadOnly(true);
        edit_full_ = new QLineEdit(box);       // 只读：完整版本号
        edit_full_->setReadOnly(true);
        label_store_ = new QLabel(box);

        form->addRow(QStringLiteral("版本号："), combo_version_);
        form->addRow(QStringLiteral("标记："), combo_prerelease_);
        form->addRow(QStringLiteral("序号（下一个）："), edit_index_);
        form->addRow(QStringLiteral("完整版本号："), edit_full_);
        form->addRow(QStringLiteral("构建史："), label_store_);
        root_layout->addWidget(box);

        auto* row = new QHBoxLayout();
        button_build_ = new QPushButton(QStringLiteral("开始构建"), this);
        button_dist_ = new QPushButton(QStringLiteral("打开 dist"), this);
        button_reload_ = new QPushButton(QStringLiteral("重新读取"), this);
        row->addWidget(button_build_);
        row->addWidget(button_dist_);
        row->addWidget(button_reload_);
        row->addStretch(1);
        root_layout->addLayout(row);

        log_ = new QPlainTextEdit(this);
        log_->setReadOnly(true);
        log_->setFont(QFont(QStringLiteral("Consolas"), 9));
        root_layout->addWidget(log_, 1);

        connect(combo_version_, &QComboBox::currentTextChanged, this,
                [this](const QString&) { refresh_derived(); });
        connect(combo_prerelease_, &QComboBox::currentTextChanged, this,
                [this](const QString&) { refresh_derived(); });
        connect(button_reload_, &QPushButton::clicked, this, [this] { reload(); });
        connect(button_build_, &QPushButton::clicked, this, [this] { start_build(); });
        connect(button_dist_, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(store_dir_ + "/dist"));
        });

        // 点"＋ 新建版本…"时才弹输入框（其余情况**只能选** —— 用户口径）
        connect(combo_version_, &QComboBox::activated, this, [this](int index) {
            if (combo_version_->itemText(index) != kNewVersion) return;
            bool ok = false;
            const QString v = QInputDialog::getText(this, QStringLiteral("新建版本号"),
                                                    QStringLiteral("例如 2.0.1："),
                                                    QLineEdit::Normal, QString(), &ok);
            if (ok && !v.trimmed().isEmpty()) {
                new_version_ = v.trimmed();
                reload();
            }
        });

        reload();
    }

private:
    static constexpr const char* kNewVersion = "＋ 新建版本…";

    void log_line(const QString& text) {
        // 攒批：先塞缓冲，200ms 刷一次（日志栏不逐行狂刷）
        pending_log_ += text + "\n";
        if (!flush_timer_) {
            flush_timer_ = new QTimer(this);
            flush_timer_->setInterval(200);
            connect(flush_timer_, &QTimer::timeout, this, [this] {
                if (pending_log_.isEmpty()) return;
                log_->moveCursor(QTextCursor::End);
                log_->insertPlainText(pending_log_);
                pending_log_.clear();
            });
            flush_timer_->start();
        }
    }

    bool load_store(std::string& err) {
        store_dir_ = QCoreApplication::applicationDirPath();
        // 往上找到仓库根（有 version.json 的那层）
        QDir d(store_dir_);
        for (int i = 0; i < 6; ++i) {
            if (QFileInfo::exists(d.filePath("version.json"))) {
                store_dir_ = d.absolutePath();
                break;
            }
            if (!d.cdUp()) break;
        }
        const QString path = store_dir_ + "/version.json";
        store_path_ = path.toUtf8().toStdString();
        if (!xrkit::core::load_version_store(store_path_, store_, err)) return false;
        return true;
    }

    void reload() {
        std::string err;
        if (!load_store(err)) {
            log_line(QStringLiteral("读 version.json 失败：%1").arg(QString::fromStdString(err)));
            return;
        }
        const QString keep_version = combo_version_->currentText();
        const QString keep_pre = combo_prerelease_->currentText();

        combo_version_->blockSignals(true);
        combo_prerelease_->blockSignals(true);
        combo_version_->clear();
        for (const std::string& v : store_.versions()) {
            combo_version_->addItem(QString::fromStdString(v));
        }
        if (!new_version_.isEmpty() && !store_.tree.count(new_version_.toStdString())) {
            combo_version_->addItem(new_version_);
        }
        combo_version_->addItem(QString::fromLatin1(kNewVersion));
        if (!keep_version.isEmpty()) {
            const int i = combo_version_->findText(keep_version);
            if (i >= 0) combo_version_->setCurrentIndex(i);
        }
        if (combo_version_->currentText().isEmpty() ||
            combo_version_->currentText() == QString::fromLatin1(kNewVersion)) {
            const int i = combo_version_->findText(QString::fromStdString(store_.current_version));
            if (i >= 0) combo_version_->setCurrentIndex(i);
        }
        combo_prerelease_->clear();
        const QString ver = current_version();
        const auto pres = store_.prereleases(ver.toStdString());
        for (const std::string& p : pres) combo_prerelease_->addItem(QString::fromStdString(p));
        if (!keep_pre.isEmpty()) {
            const int i = combo_prerelease_->findText(keep_pre);
            if (i >= 0) combo_prerelease_->setCurrentIndex(i);
        }
        combo_version_->blockSignals(false);
        combo_prerelease_->blockSignals(false);

        size_t history = 0;
        for (const auto& v : store_.tree) {
            for (const auto& m : v.second) history += m.second.size();
        }
        label_store_->setText(QStringLiteral("%1 个版本 / %2 条记录   （%3）")
                                  .arg(store_.tree.size())
                                  .arg(history)
                                  .arg(store_dir_ + "/version.json"));
        refresh_derived();
    }

    QString current_version() const {
        const QString v = combo_version_->currentText();
        return v == QString::fromLatin1(kNewVersion) ? new_version_ : v;
    }

    QString current_prerelease() const { return combo_prerelease_->currentText(); }

    int next_index() const {
        if (current_version().isEmpty() || current_prerelease().isEmpty()) return 0;
        return store_.next_index(current_version().toStdString(),
                                 current_prerelease().toStdString());
    }

    // 版本号 / 标记任一变化 ⇒ 序号与完整版本号**同步刷新**
    void refresh_derived() {
        const int next = next_index();
        edit_index_->setText(next > 0 ? QString::number(next) : QString());
        edit_full_->setText(QStringLiteral("%1 %2.%3")
                                .arg(current_version(), current_prerelease())
                                .arg(next));
    }

    void start_build() {
        if (process_ != nullptr) return;
        const QString version = current_version();
        const QString pre = current_prerelease();
        const int index = next_index();
        if (version.isEmpty() || pre.isEmpty() || index <= 0) {
            QMessageBox::warning(this, QStringLiteral("参数不全"), QStringLiteral("请先选好版本号与标记"));
            return;
        }
        const QString cli = cli_path();
        if (cli.isEmpty()) {
            QMessageBox::critical(this, QStringLiteral("找不到 CLI"),
                                  QStringLiteral("找不到 xrkit_cli（构建 GUI 与 CLI 应在同一目录）"));
            return;
        }
        button_build_->setEnabled(false);
        log_line(QStringLiteral("== 开始构建 %1 %2.%3 ==").arg(version, pre).arg(index));
        log_line(QStringLiteral("   CLI: %1").arg(cli));

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            const QString text = QString::fromUtf8(process_->readAllStandardOutput());
            log_line(text.trimmed());
        });
        connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus) {
                    log_line(QStringLiteral("== 结束：退出码 %1 ==").arg(code));
                    process_->deleteLater();
                    process_ = nullptr;
                    button_build_->setEnabled(true);
                    reload();  // 成功 ⇒ 构建史 +1；失败 ⇒ 版本已回退，界面同步回真值
                });
        process_->start(cli, QStringList{QStringLiteral("--build-package"),
                                         QString::fromStdString(store_path_), version, pre,
                                         QString::number(index)});
    }

    QComboBox* combo_version_ = nullptr;
    QComboBox* combo_prerelease_ = nullptr;
    QLineEdit* edit_index_ = nullptr;
    QLineEdit* edit_full_ = nullptr;
    QLabel* label_store_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPushButton* button_build_ = nullptr;
    QPushButton* button_dist_ = nullptr;
    QPushButton* button_reload_ = nullptr;
    QProcess* process_ = nullptr;
    QTimer* flush_timer_ = nullptr;
    QString pending_log_;
    QString store_dir_;
    QString new_version_;
    std::string store_path_;
    xrkit::core::VersionStore store_;
};

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    BuildWindow window;
    window.resize(760, 520);
    window.show();
    return app.exec();
}
