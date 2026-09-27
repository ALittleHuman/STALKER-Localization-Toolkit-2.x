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
#include <QListWidget>
#include <QProgressBar>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <string>

#include "screen_scale.h"
#include "log_box.h"          // shell 通用日志组件（LogBox：自带攒批/截断/只读）
#include "theme.h"            // shell 主题（与主程序同一套令牌，别自己造样式）          // shell 通用日志组件（LogBox：自带攒批/截断/只读）
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
        // 图标与 1.x 同款（app_icon.ico，随 exe 一起放；找不到就跳过，不阻塞）
        {
            const QStringList icon_candidates{
                QCoreApplication::applicationDirPath() + "/app_icon.ico",
                QCoreApplication::applicationDirPath() + "/../../../assets/app_icon.ico",
                QCoreApplication::applicationDirPath() + "/../../../../assets/app_icon.ico",
            };
            for (const QString& ic : icon_candidates) {
                const QString clean = QDir::cleanPath(ic);
                if (QFileInfo::exists(clean)) {
                    setWindowIcon(QIcon(clean));
                    break;
                }
            }
        }
        auto* root_layout = new QVBoxLayout(this);

        // ★ 界面重构：顶部加**页标题 + 说明句**，与主程序六页同一套视觉语言
        {
            auto* head = new QLabel(QStringLiteral("构建"), this);
            head->setObjectName(QStringLiteral("gui_header"));
            QFont f = head->font();
            f.setPointSizeF(f.pointSizeF() + 3.0);
            f.setBold(true);
            head->setFont(f);
            subtitle_ = new QLabel(
                QStringLiteral("选版本与标记 → 点「开始构建」；成功件落在 dist\\，失败件落在 dist\\FAIL\\"),
                this);
            subtitle_->setObjectName(QStringLiteral("gui_subtitle"));
            subtitle_->setWordWrap(true);
            root_layout->addWidget(head, 0);
            root_layout->addWidget(subtitle_, 0);
        }

        auto* box = new QGroupBox(QStringLiteral("版本与标记（唯一事实来源：version.json）"), this);
        form_box_ = box;
        auto* form = new QFormLayout(box);
        // ★ 2026-09-27（用户："两个软件 UI 排版都有很大问题"）：表单区紧凑化 —— 口径是 **≤ 200px**，
        //   行距/边距都收紧，避免顶部一大块留白把内容区挤下去。
        form->setContentsMargins(xrkit::core::px(8), xrkit::core::px(6), xrkit::core::px(8),
                                 xrkit::core::px(6));
        form->setVerticalSpacing(4);
        form->setHorizontalSpacing(8);

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
        // ★ 界面重构口径：**强调色只用于「当前/主操作/进行中」** ⇒ 只有它能是 accent 实心
        button_build_->setObjectName(QStringLiteral("primary"));
        button_dist_ = new QPushButton(QStringLiteral("打开 dist"), this);
        button_reload_ = new QPushButton(QStringLiteral("重新读取"), this);
        row->addWidget(button_build_);
        row->addWidget(button_dist_);
        row->addWidget(button_reload_);
        row->addStretch(1);
        root_layout->addLayout(row);

        // 构建状态：进度条（不确定态）+ 一行文字 —— "构建时进度可见"
        status_ = new QLabel(QStringLiteral("就绪"), this);
        progress_ = new QProgressBar(this);
        progress_->setRange(0, 0);          // 不确定态（CLI 目前不吐百分比，就别假装有进度）
        progress_->setVisible(false);
        progress_->setMaximumHeight(xrkit::core::px(12));
        auto* status_row = new QHBoxLayout();
        status_row->addWidget(status_);
        status_row->addWidget(progress_, 1);
        root_layout->addLayout(status_row);

        // 构建史：树里的每条记录一行；双击在资源管理器里打开产物
        // ★ 界面重构（与主程序日志面板同语言）：给列表加一个小节标题，别让裸列表孤零零一片。
        //   直接复用主程序已有的次要色选择器 page_subtitle ⇒ 零 QSS 改动、观感一致。
        auto* hist_label = new QLabel(QStringLiteral("构建史（双击一行在资源管理器里打开产物）"), this);
        hist_label->setObjectName(QStringLiteral("page_subtitle"));
        hist_label->setProperty("gui_role", QStringLiteral("gui_history_label"));
        root_layout->addWidget(hist_label, 0);

        history_ = new QListWidget(this);
        history_->setMinimumHeight(xrkit::core::px(110));   // 口径：构建史 ≥ 110px（原来只有 max ⇒ 会被压没）
        history_->setMaximumHeight(xrkit::core::px(160));
        connect(history_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            const QString path = item->data(Qt::UserRole).toString();
            if (path.isEmpty()) return;
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });
        root_layout->addWidget(history_);

        log_ = new xrkit::shell::LogBox(this);
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

public:
    int form_height() const { return form_box_ != nullptr ? form_box_->height() : -1; }
    int history_height() const { return history_ != nullptr ? history_->height() : -1; }
    int log_height() const { return log_ != nullptr ? log_->height() : -1; }
    // 类外的自检（--layout-report 在 main 里）**只能**走公开访问器：
    // 上一轮我直接在 main 里用 subtitle_ / findChild 编译不过（C2065/C3861）。
    int subtitle_height() const { return subtitle_ != nullptr ? subtitle_->height() : -1; }
    bool has_primary_button() const {
        return findChild<QPushButton*>(QStringLiteral("primary")) != nullptr;
    }

private:
    static bool logpct_le(int pct) { return pct >= 0 && pct <= 50; }
    static constexpr const char* kNewVersion = "＋ 新建版本…";

    void log_line(const QString& text) {
        // 用 shell 通用组件 LogBox 的攒批（它按 80ms 合并、>5000 行整体截到 3000 行），
        // 不再自己造一套 timer —— "小组件用通用的"（用户口径）。
        log_->append_batched(text);
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
        refresh_history();
    }

    void refresh_history() {
        history_->clear();
        for (const auto& v : store_.tree) {
            for (const auto& m : v.second) {
                for (const auto& i : m.second) {
                    const xrkit::core::BuildRecord& rec = i.second;
                    QString text = QStringLiteral("%1 %2.%3   %4   %5")
                                       .arg(QString::fromStdString(v.first),
                                            QString::fromStdString(m.first))
                                       .arg(i.first)
                                       .arg(QString::fromStdString(rec.built),
                                            QString::fromStdString(rec.commit));
                    auto* item = new QListWidgetItem(text, history_);
                    const QString last = rec.artifacts.empty()
                                             ? QString()
                                             : QString::fromStdString(rec.artifacts.back());
                    item->setData(Qt::UserRole, last);
                    if (!last.isEmpty()) item->setToolTip(QStringLiteral("双击打开：%1").arg(last));
                }
            }
        }
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
        progress_->setVisible(true);
        status_->setText(QStringLiteral("构建中… %1 %2.%3（日志在下方）").arg(version, pre)
                             .arg(index));
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
                    progress_->setVisible(false);
                    status_->setText(code == 0 ? QStringLiteral("构建成功（产物在 dist\\）")
                                               : QStringLiteral("构建失败（版本已回退，失败件在 "
                                                                "dist\\FAIL\\）"));
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
    QGroupBox* form_box_ = nullptr;
    QLabel* subtitle_ = nullptr;   // 顶部说明句（自检断言用）
    xrkit::shell::LogBox* log_ = nullptr;
    QProgressBar* progress_ = nullptr;   // 不确定态：构建中就一直走
    QListWidget* history_ = nullptr;     // 构建史条目（双击打开产物）
    QLabel* status_ = nullptr;
    QPushButton* button_build_ = nullptr;
    QPushButton* button_dist_ = nullptr;
    QPushButton* button_reload_ = nullptr;
    QProcess* process_ = nullptr;
    QString store_dir_;
    QString new_version_;
    std::string store_path_;
    xrkit::core::VersionStore store_;
};

#include <QApplication>
#include <QGuiApplication>

int main(int argc, char** argv) {
    // ★ 高 DPI 策略（屏幕自适应口径，2026-09-27）：让 Qt **保留分数缩放**。
    //   Qt6 默认 `Round` 会把 125%/133% 这类缩放先取整一次，而我们的 `snap_ui_scale()`
    //   还会再吸附一档 ⇒ 两处都在取整、结果不可预期。**必须在构造 QApplication 之前**设。
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    // ★ `--layout-report`：**排版判据**（不看截图）。离屏构建窗口 → 量各区高度 → 按口径给结论：
    //   表单区 ≤ 200px、构建史 ≥ 110px、日志区 ≤ 窗口高 50%。退出码 0/1 供门禁使用。
    if (argc > 1 && std::string(argv[1]) == "--layout-report") {
        xrkit::shell::apply_theme(app, xrkit::core::ThemeMode::Dark);
        BuildWindow window;
        window.resize(xrkit::core::px(900), xrkit::core::px(700));
        window.show();
        QApplication::processEvents();
        if (window.layout() != nullptr) window.layout()->activate();
        QApplication::processEvents();
        const int total = window.height();
        const int form = window.form_height();
        const int hist = window.history_height();
        const int logh = window.log_height();
        const int log_pct = total > 0 ? logh * 100 / total : -1;
        std::printf("LAYOUT total=%d form=%d history=%d log=%d log_pct=%d%%\n", total, form, hist,
                    logh, log_pct);
        const int sub_h = window.subtitle_height();
        const bool has_primary = window.has_primary_button();
        std::printf("LAYOUT subtitle=%d primary=%d\n", sub_h, has_primary ? 1 : 0);
        const bool ok = form > 0 && form <= 200 && hist >= 110 && log_pct >= 0 && log_pct <= 50 &&
                        sub_h > 0 && has_primary;

        // ★ 屏幕自适应判据（用户："2.5K屏…做一个自适应，所有屏幕都能正常用"）——
        //   与上面"实际窗口几何"分开：这里会**临时改全局 ui_scale**，量完必须还原，
        //   免得影响上面那组结论（顺序敏感：先量窗口，再做缩放检查）。
        bool scale_ok = true;
        {
            const double ambient = xrkit::core::ui_scale();
            const struct {
                double scale;
                int want_px100;
            } steps[] = {{1.0, 100}, {1.25, 125}, {1.5, 150}, {2.0, 200}};
            for (const auto& st : steps) {
                xrkit::core::set_ui_scale(st.scale);
                if (xrkit::core::px(100) != st.want_px100) scale_ok = false;
            }
            xrkit::core::set_ui_scale(ambient);  // 还原
            // 吸附与 clamp（1.33 ⇒ 1.25；太小 ⇒ 1.0；太大 ⇒ 2.0）
            if (xrkit::shell::snap_ui_scale(1.33) != 1.25) scale_ok = false;
            if (xrkit::shell::snap_ui_scale(0.50) != 1.00) scale_ok = false;
            if (xrkit::shell::snap_ui_scale(3.00) != 2.00) scale_ok = false;
            // 窗口尺寸：小屏(1366x768)不溢出、大屏(2560x1440)不缩成一小块
            const QSize min_win = xrkit::shell::app_min_window_size();
            const QSize screens[] = {QSize(1366, 768), QSize(2560, 1440)};
            for (const QSize& avail : screens) {
                const QSize win = xrkit::shell::adaptive_window_size(avail, min_win);
                const bool fits = win.width() <= avail.width() && win.height() <= avail.height();
                const bool big_enough = win.width() >= min_win.width() && win.height() >= min_win.height();
                const bool not_tiny = win.width() >= avail.width() / 2;
                if (!fits || !big_enough || !not_tiny) scale_ok = false;
                std::printf("SCALE screen=%dx%d window=%dx%d min=%dx%d\n", avail.width(),
                            avail.height(), win.width(), win.height(), min_win.width(),
                            min_win.height());
            }
        }
        std::printf("SCALE steps=4 %s\n", scale_ok ? "PASS" : "FAIL");
        const bool all_ok = ok && scale_ok;
        std::printf("LAYOUT %s\n", all_ok ? "PASS" : "FAIL");
        window.hide();
        return all_ok ? 0 : 1;
    }
    // 与主程序同一套主题（用户口径"小组件用通用的"；也是"两个软件观感一致"的前提）
    xrkit::shell::apply_theme(app, xrkit::core::ThemeMode::Dark);
    // ★ 与主程序同一套自适应：按屏幕逻辑 DPI 设 ui_scale（--layout-report 走的是另一条
    //   分支、保持确定性，不受这里影响）
    xrkit::shell::apply_screen_ui_scale();
    BuildWindow window;
    xrkit::shell::apply_dark_titlebar(&window, xrkit::core::ThemeMode::Dark);
    window.resize(xrkit::core::px(760), xrkit::core::px(520));
    window.show();
    return app.exec();
}
