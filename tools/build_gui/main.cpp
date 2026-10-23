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
#include <QGridLayout>
#include <QSplitter>
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

#if defined(_WIN32)
#include <windows.h>  // CREATE_NO_WINDOW（子进程无控制台窗口）
#endif
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
    BuildWindow(bool read_only = false) {
        read_only_ = read_only;
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
        // ★ 用户 2026-09-28："没有可拖拽分隔条" + "上面太挤下面太空" ⇒ 顶部（表单/按钮/状态）
        //   与底部（构建史/日志）之间放一个**竖向 QSplitter**：默认给下面多一点，
        //   但**用户可以自己拖**（1.x 构建器就是 SplitPane）。
        auto* splitter = new QSplitter(Qt::Vertical, this);
        splitter_ = splitter;
        auto* top_panel = new QWidget(splitter);
        auto* top_layout = new QVBoxLayout(top_panel);
        top_layout->setContentsMargins(0, 0, 0, 0);
        auto* bottom_panel = new QWidget(splitter);
        auto* bottom_layout = new QVBoxLayout(bottom_panel);
        bottom_layout->setContentsMargins(0, 0, 0, 0);
        splitter->addWidget(top_panel);
        splitter->addWidget(bottom_panel);
        splitter->setChildrenCollapsible(false);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);

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
        root_layout->addWidget(splitter, 1);
        splitter->setSizes({xrkit::core::px(300), xrkit::core::px(400)});

        auto* box = new QGroupBox(QStringLiteral("版本与标记（唯一事实来源：version.json）"), this);
        form_box_ = box;
        // ★ 2026-09-27：**五行竖排 → 三行两列**。原因不是"好看"，是判据：
        //   用户要求"控件太小 ⇒ 整体调大一档"之后（全局 min-height 28px + 内边距），
        //   五行竖排的表单区实测 **249px**，撞破了本窗口自己的排版口径「表单区 ≤ 200px」
        //   （`--layout-report` 会判红，而"放松阈值"是明确不许做的事）。
        //   五个字段一个不少，只是把"短字段"并排：版本号|标记、序号|完整版本号，构建史独占一行。
        auto* grid = new QGridLayout(box);
        form_grid_ = grid;
        grid->setContentsMargins(xrkit::core::px(8), xrkit::core::px(6), xrkit::core::px(8),
                                 xrkit::core::px(6));
        grid->setVerticalSpacing(4);
        grid->setHorizontalSpacing(8);

        combo_version_ = new QComboBox(box);   // **只能选**，不许手打
        combo_prerelease_ = new QComboBox(box);
        edit_index_ = new QLineEdit(box);      // 只读：下一个构建的序号
        edit_index_->setReadOnly(true);
        edit_full_ = new QLineEdit(box);       // 只读：完整版本号
        edit_full_->setReadOnly(true);
        label_store_ = new QLabel(box);

        grid->addWidget(new QLabel(QStringLiteral("版本号："), box), 0, 0);
        grid->addWidget(combo_version_, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("标记："), box), 0, 2);
        grid->addWidget(combo_prerelease_, 0, 3);
        grid->addWidget(new QLabel(QStringLiteral("序号（下一个）："), box), 1, 0);
        grid->addWidget(edit_index_, 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("完整版本号："), box), 1, 2);
        grid->addWidget(edit_full_, 1, 3);
        grid->addWidget(new QLabel(QStringLiteral("构建史："), box), 2, 0);
        grid->addWidget(label_store_, 2, 1, 1, 3);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        top_layout->addWidget(box);

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
        top_layout->addLayout(row);

        // 构建状态：进度条（不确定态）+ 一行文字 —— "构建时进度可见"
        status_ = new QLabel(QStringLiteral("就绪"), this);
        progress_ = new QProgressBar(this);
        progress_->setRange(0, 0);          // 不确定态（CLI 目前不吐百分比，就别假装有进度）
        progress_->setVisible(false);
        progress_->setMaximumHeight(xrkit::core::px(12));
        auto* status_row = new QHBoxLayout();
        status_row->addWidget(status_);
        status_row->addWidget(progress_, 1);
        top_layout->addLayout(status_row);

        // 构建史：树里的每条记录一行；双击在资源管理器里打开产物
        // ★ 界面重构（与主程序日志面板同语言）：给列表加一个小节标题，别让裸列表孤零零一片。
        //   直接复用主程序已有的次要色选择器 page_subtitle ⇒ 零 QSS 改动、观感一致。
        auto* hist_label = new QLabel(QStringLiteral("构建史（双击一行在资源管理器里打开产物）"), this);
        hist_label->setObjectName(QStringLiteral("page_subtitle"));
        hist_label->setProperty("gui_role", QStringLiteral("gui_history_label"));
        bottom_layout->addWidget(hist_label, 0);

        history_ = new QListWidget(this);
        history_->setMinimumHeight(xrkit::core::px(110));   // 口径：构建史 ≥ 110px（原来只有 max ⇒ 会被压没）
        history_->setMaximumHeight(xrkit::core::px(160));
        connect(history_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
            const QString path = item->data(Qt::UserRole).toString();
            if (path.isEmpty()) return;
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        });
        bottom_layout->addWidget(history_);

        log_ = new xrkit::shell::LogBox(this);
        log_->setFont(QFont(QStringLiteral("Consolas"), 9));
        bottom_layout->addWidget(log_, 1);

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
    QSplitter* splitter() const { return splitter_; }
    // 自检用：界面显示的序号 + 版本库路径（判据：序号口径只有一处，必须与 CLI 一致）
    QString index_text() const { return edit_index_ != nullptr ? edit_index_->text() : QString(); }
    QString store_path_text() const { return QString::fromStdString(store_path_); }
    // ★ 用户 2026-09-28 指出的四点，四条访问器（`--layout-report` 全用它们判红）：
    //   ① 序号不许为 0 或缺 ② 版本下拉显示的必须就是 version.json 的当前版本
    //   ③ "＋ 新建版本…"这一项不许乱码（它是 UTF-8 中文，曾被 fromLatin1 解成乱码）
    //   ④ 标签与右侧控件不许重叠（"上面的字和框也有重叠"）
    bool index_shown() const {
        return edit_index_ != nullptr && !edit_index_->text().isEmpty() &&
               edit_index_->text().toInt() >= 1;
    }
    bool version_text_ok() const {
        return combo_version_ != nullptr &&
               combo_version_->currentText() == QString::fromStdString(store_.current_version);
    }
    bool new_version_item_ok() const {
        if (combo_version_ == nullptr || combo_version_->count() == 0) return false;
        return combo_version_->itemText(combo_version_->count() - 1) ==
               QString::fromUtf8(kNewVersion);
    }
    int form_grid_overlaps() const {
        if (form_grid_ == nullptr) return -1;
        const int pairs[4][2] = {{0, 0}, {0, 2}, {1, 0}, {1, 2}};
        int bad = 0;
        for (const auto& pair : pairs) {
            QLayoutItem* label = form_grid_->itemAtPosition(pair[0], pair[1]);
            QLayoutItem* widget = form_grid_->itemAtPosition(pair[0], pair[1] + 1);
            if (label == nullptr || widget == nullptr || label->widget() == nullptr ||
                widget->widget() == nullptr) {
                return -1;
            }
            if (widget->widget()->x() < label->widget()->x() + label->widget()->width()) ++bad;
        }
        return bad;
    }
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
        combo_version_->addItem(QString::fromUtf8(kNewVersion));
        if (!keep_version.isEmpty()) {
            const int i = combo_version_->findText(keep_version);
            if (i >= 0) combo_version_->setCurrentIndex(i);
        }
        if (combo_version_->currentText().isEmpty() ||
            combo_version_->currentText() == QString::fromUtf8(kNewVersion)) {
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
        } else {
            // ★★ 2026-10-23（同一处一致性缺陷的另一半）：**默认标记必须等于 current**。
            //   修前这里什么都不选 ⇒ 落到列表第一项（字母序 = ALPHA），而 `current` 可能是 BETA
            //   ⇒ 界面按 ALPHA 算 next（258），CLI 按 current（BETA）算 next（259），
            //   `--layout-report` 的「序号口径唯一」那条判据**必然红**；
            //   而修前它之所以"绿"，是因为构造期顺手把 current 改成了 ALPHA —— 用**写盘**掩盖了不一致。
            //   现在（诊断模式不写盘）必须把默认值选对。
            const int i = combo_prerelease_->findText(
                QString::fromStdString(store_.current_prerelease));
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
        return v == QString::fromUtf8(kNewVersion) ? new_version_ : v;
    }

    QString current_prerelease() const { return combo_prerelease_->currentText(); }

    int next_index() const {
        if (current_version().isEmpty() || current_prerelease().isEmpty()) return 0;
        return store_.next_index(current_version().toStdString(),
                                 current_prerelease().toStdString());
    }

    // ★ 用户 spec（2026-09-24）：「"保存版本号与标记"不要加按钮，直接自动保存」+
    //   「序号栏要显示"本轮构建标记的实际序号"」；而**版本口径只有一处**（version.json，
    //   实现只有 CLI 一份）⇒ 这里调 `--version-store <store> set <版本> <标记>` 做自动保存，
    //   再把它打出来的 `next=` 读回来显示，**界面不再自己算一遍**（也就不会"两者结果有差别"）。
    int sync_store_and_read_next(const QString& version, const QString& prerelease, bool* ok) {
        if (ok != nullptr) *ok = false;
        const QString cli = cli_path();
        if (cli.isEmpty()) return 0;
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
        proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NO_WINDOW;   // 不许弹控制台（用户口径「不要打开cmd」）
        });
#endif
        proc.start(cli, QStringList{QStringLiteral("--version-store"),
                                    QString::fromStdString(store_path_), QStringLiteral("set"),
                                    version, prerelease});
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(20000)) return 0;
        const QString out = QString::fromUtf8(proc.readAll());
        const qsizetype at = out.indexOf(QStringLiteral("next="));
        if (at < 0) return 0;
        const QString digits = out.mid(at + 5).section(QLatin1Char(' '), 0, 0).trimmed();
        bool num_ok = false;
        const int next = digits.toInt(&num_ok);
        if (ok != nullptr) *ok = num_ok;
        return num_ok ? next : 0;
    }

    // 版本号 / 标记任一变化 ⇒ **自动保存** + 序号与完整版本号同步刷新
    void refresh_derived() {
        // ★ 用户 2026-09-28："序号是不能为0的" —— 序号是**本轮要构建的序号**，从 1 起算；
        //   界面上**绝不出现 0 或空白**（原来直接 setText("") ⇒ 截图里那个空框 + "ALPHA.0"）。
        int next = 0;
        bool synced = false;
        if (!current_version().isEmpty() && !current_prerelease().isEmpty()) {
            if (read_only_) {
                // 2026-10-23: diagnostic mode computes only, never writes.
                // next_index() reads tree only (same value the CLI reports after `set`),
                // so the display is unchanged while version.json stays untouched.
                next = next_index();
                synced = next > 0;
            } else {
                next = sync_store_and_read_next(current_version(), current_prerelease(), &synced);
            }
        }
        if (!synced) next = next_index();   // CLI 调不动时的兜底（下面会记一行，不静默）
        const int shown = next > 0 ? next : 1;
        edit_index_->setText(QString::number(shown));
        if (synced) {
            // 自动保存是**可见**的：只在选择真的变了时写一行（用户口径：不许静默自救）
            const QString key = current_version() + QLatin1Char(' ') + current_prerelease();
            if (key != last_saved_) {
                last_saved_ = key;
                log_line(QStringLiteral("已自动保存版本与标记：%1（本轮序号 %2）")
                             .arg(key)
                             .arg(shown));
            }
        } else {
            log_line(QStringLiteral("提示：没法调 CLI 自动保存，序号是本机算的（%1）").arg(shown));
        }
        if (next <= 0) {
            log_line(QStringLiteral("提示：version.json 里没有「%1 %2」这条，序号按 1 起算")
                         .arg(current_version(), current_prerelease()));
        }
        edit_full_->setText(QStringLiteral("%1 %2.%3")
                                .arg(current_version(), current_prerelease())
                                .arg(shown));
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
#ifdef _WIN32
    // ★ 用户 2026-09-27：「不要打开cmd」—— 子进程是控制台程序（ffmpeg / CLI），
    //   Qt 默认会**新建一个控制台窗口**；这里加 CREATE_NO_WINDOW 让它无窗口运行。
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
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
    QSplitter* splitter_ = nullptr;
    QString last_saved_;   // 上一次自动保存过的「版本 标记」（用来只在变化时记一行日志）
    QGridLayout* form_grid_ = nullptr;
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
    // 2026-10-23: diagnostic mode (--layout-report) must never write to disk.
    //   Before: the ctor called sync_store_and_read_next() -> `xrkit_cli --version-store set`,
    //   rewriting version.json's current (measured: BETA/1 reset to ALPHA/258).
    //   That is the P17-133 culprit "shell-smoke changed version.json" (it runs --layout-report).
    bool read_only_ = false;
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
        // 2026-10-23: criterion mode => read-only (must not touch version.json).
        BuildWindow window(/*read_only=*/true);
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
        // ★ 2026-09-28 新增（用户截图指出的四处，都能判红）：
        const bool index_ok = window.index_shown();
        const bool version_ok = window.version_text_ok();
        const bool new_item_ok = window.new_version_item_ok();
        const int overlaps = window.form_grid_overlaps();
        const bool splitter_ok = window.splitter() != nullptr && window.splitter()->count() == 2 &&
                                 window.splitter()->handleWidth() > 0;
        // ★ 序号口径只有一处：界面显示的必须 == CLI（version.json）算出的 next=（只读查，不写盘）
        bool index_matches_cli = false;
        int cli_next = -1;
        {
            QProcess probe;
            probe.setProcessChannelMode(QProcess::MergedChannels);
#ifdef _WIN32
            probe.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
                args->flags |= CREATE_NO_WINDOW;
            });
#endif
            probe.start(cli_path(), QStringList{QStringLiteral("--version-store"),
                                                window.store_path_text()});
            if (probe.waitForStarted(5000) && probe.waitForFinished(20000)) {
                const QString out = QString::fromUtf8(probe.readAll());
                const qsizetype at = out.indexOf(QStringLiteral("next="));
                if (at >= 0) {
                    cli_next = out.mid(at + 5).section(QLatin1Char(' '), 0, 0).trimmed().toInt();
                    index_matches_cli = window.index_text().toInt() == cli_next;
                }
            }
        }
        std::printf("LAYOUT index=%d overlaps=%d splitter=%d version_ok=%d newitem_ok=%d "
                    "index_cli=%d/%d\n",
                    index_ok ? 1 : 0, overlaps, splitter_ok ? 1 : 0, version_ok ? 1 : 0,
                    new_item_ok ? 1 : 0, window.index_text().toInt(), cli_next);
        if (!index_matches_cli) {
            std::printf("LAYOUT 序号与 CLI 不一致（界面 %d / CLI %d）⇒ 序号口径不唯一\n",
                        window.index_text().toInt(), cli_next);
        }
        if (index_ok && version_ok && new_item_ok && overlaps == 0 && splitter_ok) {
            std::printf("LAYOUT ui=PASS\n");
        } else {
            std::printf("LAYOUT ui=FAIL（序号 %d / 重叠 %d / 分隔条 %d / 版本文本 %d / 新建项 %d）\n",
                        index_ok ? 1 : 0, overlaps, splitter_ok ? 1 : 0, version_ok ? 1 : 0,
                        new_item_ok ? 1 : 0);
        }
        const bool ok = form > 0 && form <= 200 && hist >= 110 && log_pct >= 0 && log_pct <= 50 &&
                        sub_h > 0 && has_primary && index_ok && version_ok && new_item_ok &&
                        overlaps == 0 && splitter_ok && index_matches_cli;

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
        // ★ 规则判据（口径更正后）：dpr>1 时**不叠加**（防双重缩放）；dpr≈1 时按物理 DPI 放大
        {
            const struct {
                double dpr;
                double pdpi;
                double want;
            } rules[] = {
                {1.50, 125.9, 1.00},  // 真机实测：150% 缩放 ⇒ Qt 已放大 ⇒ 我们保持 1.0
                {1.00, 96.0, 1.00},   // 普通屏
                {1.00, 160.0, 1.75},  // 4K 屏但系统 100% ⇒ 我们自己放大
                {1.00, 300.0, 2.00},  // 极端 ⇒ clamp 到 2.0
            };
            for (const auto& r : rules) {
                const double got = xrkit::shell::ui_scale_for(r.dpr, r.pdpi);
                if (std::abs(got - r.want) > 0.001) {
                    scale_ok = false;
                    std::printf("SCALE rule dpr=%.2f pdpi=%.1f want=%.2f got=%.2f\n", r.dpr, r.pdpi,
                                r.want, got);
                }
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
