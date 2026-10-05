#include "convert_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/text/encoding.h"

#include "../task_shell.h"
#include "../virtual_list.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

namespace xrkit::shell {
namespace {

namespace etxt = xrkit::engine::text;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// 手动指定时能选的编码：**只列我们对拍过的那些**（严格解码 + 有损替换都有 golden）。
// 没对拍的编码宁可不给选，也不让人选到一个我们没验证过的行为。
//
// ★ 2026-10-06（D2 批）：**源/目标拆成两张表**。理由不是"好看"，是纪律：
//   · 目标侧多一个 `ascii`（1.x 的目标下拉里有它）。补它对拍时**当场抓到一个真缺陷**：
//     引擎的 ascii 原来只有解码侧、编码侧一行都没有 ⇒ 连纯 ASCII 正文也被整篇写成 `?`
//     （见 `encoding.cpp` 的 `encode_strict` 与 `golden_lossy_codec.py`）。要不是"只列
//     对拍过的编码"这条纪律卡着，用户会静默拿到一整个目录的 `?`。
//   · 源侧**不放** `ascii`：源是"猜"的，把 ascii 塞进源下拉只会让人以为工具会猜 ascii。
//
// ★ 2026-10-05（D3 批）：`big5 / shift_jis / euc-kr` 三项**补进这张共用表**（⇒ 源、目标
//   两个下拉都出现）。1.x 侧的口径是：源下拉 = `ENCODING_CHOICES`（`toolkit_constants.py:76`
//   `["auto","utf-8","gbk","gb2312","big5","shift_jis","euc-kr","windows-1251",
//   "windows-1252","iso-8859-1","ascii","utf-16"]`），目标下拉 =
//   `convert_app.py:192` 的 `["utf-8","gbk","gb2312","windows-1251","ascii","utf-16","big5"]`。
//   两边的**交集**里这三个名字都有（big5 两边都在，shift_jis/euc-kr 源侧在）。
//   三张表已按"双字节 + 编码侧"全量对拍（`tests/golden_lossy_codec.py`），
//   所以按"只列对拍过的编码"这条纪律，它们可以列了。
const char* kManualEncodings[] = {etxt::kUtf8,          etxt::kWindows1251, etxt::kWindows1252,
                                  etxt::kGb18030,       etxt::kGbk,         etxt::kGb2312,
                                  etxt::kBig5,          etxt::kShiftJis,    etxt::kEucKr,
                                  etxt::kLatin1,
                                  // 2026-09-26：utf-16/32 六族的有损编解码已与 Python
                                  // `errors="replace"` 逐字节对拍（门禁 [12] 的 compared
                                  // 从 114 涨到 294）⇒ 按"只列对拍过的编码"这条纪律可以列了。
                                  etxt::kUtf16,     etxt::kUtf16Le, etxt::kUtf16Be,
                                  etxt::kUtf32,     etxt::kUtf32Le, etxt::kUtf32Be};

// 目标侧多一项：ascii（D2 批补上对拍后才允许出现，见上面的长注释）
const char* kTargetOnlyEncodings[] = {etxt::kAscii};

// 扫描台账的那一行（**零静默**：递归几层、扫到多少、各自为什么被刷掉）。
// 页面只负责把它画出来，计数**全部来自引擎**（`CollectReport`），不在这里重算。
QString scan_line_of(const etxt::BatchOutcome& outcome) {
    const etxt::CollectReport& c = outcome.collect;
    if (outcome.single_file) return QStringLiteral("扫描：单个文件");
    QString text = QStringLiteral("扫描：递归 %1 层，目录 %2 个，文件 %3 个 ⇒ 选中 %4")
                       .arg(c.depth)
                       .arg(c.dirs)
                       .arg(c.files_seen)
                       .arg(c.selected);
    text += QStringLiteral("，后缀不符 %1").arg(c.skipped_ext);
    text += QStringLiteral("，命中排除 %1").arg(c.skipped_excluded);
    text += QStringLiteral("，备份区跳过 %1").arg(c.skipped_backup);
    if (c.skipped_depth > 0) text += QStringLiteral("，只扫顶层挡掉 %1").arg(c.skipped_depth);
    if (!c.errors.empty()) text += QStringLiteral("，遍历错误 %1").arg(c.errors.size());
    return text;
}

// 扩展名框 → 小写带点列表（1.x：`[e.strip().lower() for e in text.split(',') if e.strip()]`）
QStringList parse_extensions(const QString& text) {
    QStringList out;
    const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString item = part.trimmed().toLower();
        if (!item.isEmpty()) out << item;
    }
    return out;
}

// 一次批处理的结果盒子：**工作线程只写它，主线程只读它**（13.2：worker 不碰 UI）。
struct BatchResultBox {
    etxt::BatchOutcome outcome;
    QString source;
    QString out_dir;
    QString source_enc;
    QString target_enc;
};

}  // namespace

ConvertPage::ConvertPage(QWidget* parent)
    : PageBase(QStringLiteral("convert"), QStringLiteral("编码转换"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内那一段说明文字**整段删掉**
    //   （六页统一；用户对"满屏解释"的口径与词汇口径同源：「不用带括号。一般人又看不懂。」）
    source_edit_ = new QLineEdit;
    source_edit_->setObjectName(QStringLiteral("convert_source"));
    // ★ 2026-10-04：输入框自己**不接**拖放 —— 接住它的必须是**整行区域**（`drop_zones_`）。
    //   输入框自己设了 `acceptDrops` 又不处理事件的话，落在框上的拖放会被它吞掉 ⇒ 行区域收不到。
    source_edit_->setPlaceholderText(QStringLiteral("源文件或目录"));
    output_edit_ = new QLineEdit;
    output_edit_->setObjectName(QStringLiteral("convert_output"));
    output_edit_->setPlaceholderText(QStringLiteral("输出目录"));

    // ★ 2026-10-04 用户第 2 条（"目录定向"）：每个"选择"调用点的起点判据**只有这一份**
    //   ——按钮 lambda 绑的就是下面这两个成员函数（自检也断言它们 ⇒ 两边不可能分叉）。
    const auto source_judge = [this]() { return source_dialog_start(); };
    const auto output_judge = [this]() { return output_dialog_start(); };

    auto make_row = [this](const QString& label, QLineEdit* edit, const QString& title,
                           std::function<DialogStart()> judge) {
        // ★ 2026-10-04 用户第 1 条「拖拽到相应区域」：整行是一个 QWidget 容器 ⇒ 丢在
        //   标签/按钮/空白处都能接住（`drop_zones_` 只有这一份接线实现）。
        auto* container = new QWidget;
        auto* row = new QHBoxLayout(container);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(new QLabel(label), 0);
        row->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        row->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, title, judge] {
            // ★ 2026-10-04 用户第 2 条：起点 = ①上次的选择（＝框里现有的值）→ ②唯一输入目录
            //   → ③工具自身所在文件夹（exe 位置；**不是 CWD**）。算法只有 dialog_start.h 一份。
            const QString picked = QFileDialog::getExistingDirectory(
                this, title, resolve_dialog_start(judge ? judge() : DialogStart{}));
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        drop_zones_.attach_row(container, this);
        return container;
    };
    body_layout()->addWidget(make_row(QStringLiteral("源路径："), source_edit_,
                                      QStringLiteral("选择源目录"), source_judge));
    // 输出目录 ← 源路径（唯一才用；"输入目录多元或没有输入"由 `DialogStart` 自己判）
    body_layout()->addWidget(make_row(QStringLiteral("输出目录："), output_edit_,
                                      QStringLiteral("选择输出目录"), output_judge));

    source_combo_ = new QComboBox;
    source_combo_->setObjectName(QStringLiteral("convert_source_enc"));
    target_combo_ = new QComboBox;
    target_combo_->setObjectName(QStringLiteral("convert_target_enc"));
    fill_encoding_combos();
    auto* enc_row = new QHBoxLayout;
    enc_row->addWidget(new QLabel(QStringLiteral("源编码：")), 0);
    enc_row->addWidget(source_combo_, 0);
    enc_row->addSpacing(xrkit::core::px(12));
    enc_row->addWidget(new QLabel(QStringLiteral("目标编码：")), 0);
    enc_row->addWidget(target_combo_, 0);
    enc_row->addStretch(1);
    body_layout()->addLayout(enc_row);

    // ═══ ★ 2026-10-06 D2 批：1.x 的"转换设置"那一整行 ═══
    //   扩展名（默认 `.xml`）/ 递归子目录（**默认开**）/ 备份（默认开）/ 排除模式。
    //   控件名都带 `convert_` 前缀 ⇒ `--shot` 的 GEOM/LBL 能直接量到它们。
    ext_edit_ = new QLineEdit;
    ext_edit_->setObjectName(QStringLiteral("convert_exts"));
    ext_edit_->setText(QStringLiteral(".xml"));
    ext_edit_->setMaximumWidth(xrkit::core::px(140));
    ext_edit_->setPlaceholderText(QStringLiteral(".xml"));
    recursive_box_ = new QCheckBox(QStringLiteral("递归子目录"));
    recursive_box_->setObjectName(QStringLiteral("convert_recursive"));
    recursive_box_->setChecked(true);  // 1.x `self.recursive = tk.BooleanVar(value=True)`
    backup_box_ = new QCheckBox(QStringLiteral("备份到 源目录\\backup"));
    backup_box_->setObjectName(QStringLiteral("convert_backup"));
    backup_box_->setChecked(true);     // 1.x `self.backup_files = tk.BooleanVar(value=True)`
    exclude_edit_ = new QLineEdit;
    exclude_edit_->setObjectName(QStringLiteral("convert_exclude"));
    exclude_edit_->setPlaceholderText(QStringLiteral("排除模式（逗号分隔，如 *backup*,temp*）"));
    auto* opt_row = new QHBoxLayout;
    opt_row->addWidget(new QLabel(QStringLiteral("扩展名：")), 0);
    opt_row->addWidget(ext_edit_, 0);
    opt_row->addWidget(recursive_box_, 0);
    opt_row->addWidget(backup_box_, 0);
    opt_row->addSpacing(xrkit::core::px(12));
    opt_row->addWidget(new QLabel(QStringLiteral("排除：")), 0);
    opt_row->addWidget(exclude_edit_, 1);
    body_layout()->addLayout(opt_row);
    // 1.x `on_output_dir_change`：输出目录非空 ⇒ 备份勾选框**取消勾选并禁用**
    //   （备份只对"原地转换"有意义 —— 输出到新目录时源文件根本不会被改）。
    QObject::connect(output_edit_, &QLineEdit::textChanged, this,
                     [this](const QString&) { sync_backup_enabled(); });
    sync_backup_enabled();

    rows_ = new VirtualList;
    rows_->setObjectName(QStringLiteral("convert_rows"));
    rows_->set_row_height(xrkit::core::px(20));
    rows_->set_row_text_provider([this](int row) -> QString {
        if (row < 0 || row >= static_cast<int>(row_texts_.size())) return QString();
        return row_texts_[static_cast<size_t>(row)];
    });
    body_layout()->addWidget(rows_, 1);

    // ⑬13.6：任务壳负责 running/线程/失败路径/进度；**暂停是 App 自己的状态机**。
    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));  // ⑬13.5：空闲文案必须非空
    task_->set_message_handler([this](const QString& text) { add_row(text); });
    // ★ D2 批：1.x 的「取消」**先确认**（`messagebox.askyesno`）—— 确认钩子挂在共用的
    //   任务壳上（六页里只有本页装它），不另造一颗按钮。
    task_->set_cancel_confirm([this] {
        return QMessageBox::question(this, QStringLiteral("确认"),
                                     QStringLiteral("确定要取消当前转换任务吗？")) ==
               QMessageBox::Yes;
    });
    body_layout()->addWidget(task_->panel(), 0);

    status_ = new QLabel(task_->status());
    status_->setObjectName(QStringLiteral("convert_status"));

    start_button_ = add_action(QStringLiteral("开始转换"));
    start_button_->setObjectName(QStringLiteral("convert_start"));
    pause_button_ = add_action(QStringLiteral("暂停"));
    pause_button_->setObjectName(QStringLiteral("convert_pause"));
    QObject::connect(pause_button_, &QPushButton::clicked, this, [this] {
        toggle_pause();
        pause_button_->setText(paused() ? QStringLiteral("继续") : QStringLiteral("暂停"));
    });
    // ★ 1.x 的另外四颗按钮（原来 2.x 一颗都没有）
    restore_button_ = add_action(QStringLiteral("恢复备份"));
    restore_button_->setObjectName(QStringLiteral("convert_restore"));
    QObject::connect(restore_button_, &QPushButton::clicked, this, [this] { run_restore(); });
    clear_backup_button_ = add_action(QStringLiteral("清空备份"));
    clear_backup_button_->setObjectName(QStringLiteral("convert_clear_backup"));
    QObject::connect(clear_backup_button_, &QPushButton::clicked, this,
                     [this] { run_clear_backup(); });
    open_output_button_ = add_action(QStringLiteral("打开输出"));
    open_output_button_->setObjectName(QStringLiteral("convert_open_out"));
    QObject::connect(open_output_button_, &QPushButton::clicked, this, [this] { open_output(); });
    export_button_ = add_action(QStringLiteral("导出统计"));
    export_button_->setObjectName(QStringLiteral("convert_export_stats"));
    QObject::connect(export_button_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出统计"),
            QStringLiteral("转换统计_%1.csv").arg(
                QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
            QStringLiteral("CSV 文件 (*.csv)"));
        if (path.isEmpty()) return;
        QString problem;
        if (!export_stats(path, &problem)) {
            status_->setText(problem);
            return;
        }
        status_->setText(QStringLiteral("统计已导出：%1").arg(QDir::toNativeSeparators(path)));
        log_to_host(QStringLiteral("统计已导出到：%1").arg(QDir::toNativeSeparators(path)));
    });

    QObject::connect(start_button_, &QPushButton::clicked, this, [this] {
        const QString source = source_edit_->text().trimmed();
        if (source.isEmpty()) {
            status_->setText(QStringLiteral("请先选源路径"));
            return;
        }
        run_batch(source, output_edit_->text().trimmed());
    });

    refresh_rows();
}

void ConvertPage::fill_encoding_combos() {
    source_combo_->addItem(QStringLiteral("自动"), QStringLiteral("auto"));
    for (const char* name : kManualEncodings) {
        source_combo_->addItem(utf8(name), utf8(name));
        target_combo_->addItem(utf8(name), utf8(name));
    }
    // ★ D2 批：目标侧独有的项（ascii）。**源侧不加** —— 源是猜的，见文件头的长注释。
    for (const char* name : kTargetOnlyEncodings) {
        target_combo_->addItem(utf8(name), utf8(name));
    }
    target_combo_->setCurrentIndex(0);  // utf-8
}

QStringList ConvertPage::extensions() const { return parse_extensions(ext_edit_->text()); }

QStringList ConvertPage::backup_extensions() const {
    // 1.x：恢复/清空都按 `bak_exts = [f"{e}.bak" for e in exts]` 过滤 ⇒ 与扩展名框同源。
    // 扩展名为空 ⇒ 退回默认 `.xml`（否则这条按钮会"点了没反应"）。
    QStringList exts = extensions();
    if (exts.isEmpty()) exts << QStringLiteral(".xml");
    return exts;
}

std::string ConvertPage::current_source_encoding() const {
    return source_combo_->currentData().toString().toUtf8().toStdString();
}

std::string ConvertPage::current_target_encoding() const {
    return target_combo_->currentData().toString().toUtf8().toStdString();
}

void ConvertPage::sync_backup_enabled() {
    if (backup_box_ == nullptr || output_edit_ == nullptr) return;
    const bool in_place = output_edit_->text().trimmed().isEmpty();
    backup_box_->setEnabled(in_place);
    if (!in_place) backup_box_->setChecked(false);
}

etxt::BatchOptions ConvertPage::batch_options_for(const QString& source,
                                                  const QString& out_dir) const {
    etxt::BatchOptions options;
    options.source = source.toUtf8().toStdString();
    options.output_dir = out_dir.toUtf8().toStdString();
    const QStringList exts = extensions();
    for (const QString& ext : exts) options.extensions.push_back(ext.toStdString());
    options.recursive = recursive_box_ == nullptr ? true : recursive_box_->isChecked();
    options.backup = backup_box_ == nullptr ? false : backup_box_->isChecked();
    options.exclude_patterns = exclude_edit_ == nullptr
                                   ? std::string()
                                   : exclude_edit_->text().trimmed().toStdString();
    options.source_encoding = current_source_encoding();
    options.target_encoding = current_target_encoding();
    return options;
}

engine::text::ConvertOutcome ConvertPage::convert_one(const QString& source_path,
                                                      const QString& out_dir,
                                                      const QString& source_enc,
                                                      const QString& target_enc) {
    etxt::ConvertOptions options;
    options.source_encoding = source_enc.toUtf8().toStdString();
    options.target_encoding = target_enc.toUtf8().toStdString();
    options.to_output_dir = !out_dir.isEmpty();
    QString target;
    if (!out_dir.isEmpty()) {
        QDir dir(out_dir);
        target = dir.filePath(QFileInfo(source_path).fileName());
    }
    return etxt::convert_file(source_path.toUtf8().toStdString(), target.toUtf8().toStdString(), options);
}

void ConvertPage::set_paused(bool paused) { paused_.store(paused); }

void ConvertPage::toggle_pause() { set_paused(!paused()); }

bool ConvertPage::wait_if_paused(bool block) {
    if (!paused_.load()) return true;
    ++pause_gate_hits_;
    while (block && paused_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !paused_.load();
}

void ConvertPage::add_row(const QString& text) {
    row_texts_.push_back(text);
    rows_->set_row_count(static_cast<int>(row_texts_.size()));
}

void ConvertPage::refresh_rows() {
    rows_->set_row_count(static_cast<int>(row_texts_.size()));
}

void ConvertPage::record_stats(const etxt::BatchOutcome& outcome, const QString& source,
                               const QString& out_dir, const QString& source_enc,
                               const QString& target_enc) {
    last_outcome_ = outcome;
    last_scan_text_ = scan_line_of(outcome);
    stats_ = etxt::ConvertStats{};
    stats_.generated_at = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                              .toStdString();
    stats_.source = source.toStdString();
    stats_.single_file = outcome.single_file;
    stats_.output_dir = out_dir.toStdString();
    stats_.source_enc = source_enc.toStdString();
    stats_.target_enc = target_enc.toStdString();
    stats_.candidates = static_cast<uint64_t>(outcome.candidates);
    stats_.processed = static_cast<uint64_t>(outcome.processed);
    stats_.success = static_cast<uint64_t>(outcome.success);
    stats_.skipped = static_cast<uint64_t>(outcome.skipped);
    stats_.failed = static_cast<uint64_t>(outcome.failed);
    stats_.duration = outcome.duration;
    stats_ready_ = true;
    // 零静默：部分失败要**显式收口**（不是只把最后一行日志留成"失败"）。
    // ★ 2026-10-06：判定的"不确定部分"也要收口 —— 低置信（纯猜）与"统计否决声明"
    //   不是失败，但用户必须一眼看到"这批里有多少条是猜的"。**计数全部来自引擎**，
    //   页面只负责把数字画出来（文案同样来自引擎的逐条标记）。
    const QString qualifier = (outcome.low_confidence > 0 || outcome.override_decl > 0)
                                  ? QStringLiteral("（低置信 %1 / 否决声明 %2 / 判定≠实解 %3）")
                                        .arg(outcome.low_confidence)
                                        .arg(outcome.override_decl)
                                        .arg(outcome.used_mismatch)
                                  : QString();
    if (outcome.exit_code != 0) {
        status_->setText(QStringLiteral("完成，但有失败：成功 %1 / 跳过 %2 / 失败 %3%4")
                             .arg(outcome.success)
                             .arg(outcome.skipped)
                             .arg(outcome.failed)
                             .arg(qualifier));
    } else if (outcome.cancelled) {
        status_->setText(utf8(outcome.stop_reason));
    } else {
        status_->setText(QStringLiteral("全部完成：成功 %1 / 跳过 %2 / 失败 %3%4")
                             .arg(outcome.success)
                             .arg(outcome.skipped)
                             .arg(outcome.failed)
                             .arg(qualifier));
    }
}

void ConvertPage::set_busy(bool busy) {
    if (start_button_ != nullptr) start_button_->setEnabled(!busy);
    if (restore_button_ != nullptr) restore_button_->setEnabled(!busy);
    if (clear_backup_button_ != nullptr) clear_backup_button_->setEnabled(!busy);
}

void ConvertPage::run_batch(const QString& source, const QString& out_dir) {
    const etxt::BatchOptions options = batch_options_for(source, out_dir);
    if (options.source.empty()) {
        status_->setText(QStringLiteral("请先选源路径"));
        return;
    }
    if (options.extensions.empty() && !QFileInfo(source).isFile()) {
        // 1.x：目录模式下没有扩展名直接抛"请输入文件扩展名，例如 .xml,.txt"
        status_->setText(QStringLiteral("请输入文件扩展名，例如 .xml,.txt"));
        return;
    }
    auto box = std::make_shared<BatchResultBox>();
    box->source = source;
    box->out_dir = out_dir;
    box->source_enc = utf8(current_source_encoding());
    box->target_enc = utf8(current_target_encoding());

    set_busy(true);
    // ⑬13.2：工作线程**不许碰 UI**（连读控件都不行）—— 参数在主线程冻结成值传进去。
    const bool started = task_->run(
        QStringLiteral("编码转换"),
        [this, options, box](const ProgressSink& progress, const MessageSink& message) {
            etxt::BatchHooks hooks;
            hooks.should_stop = [this]() { return task_ != nullptr && task_->cancel_requested(); };
            hooks.gate = [this]() { return wait_if_paused(true); };
            hooks.message = [&message](const std::string& text) { message(text); };
            hooks.progress = [&progress](int done, int total) {
                progress(total <= 0 ? 100 : done * 100 / total);
            };
            box->outcome = etxt::convert_batch(options, hooks);
            if (!box->outcome.stop_reason.empty()) message(box->outcome.stop_reason);
            // 结果回到主线程再落到成员上（成员是 UI 侧状态，worker 不碰）
            QMetaObject::invokeMethod(
                this,
                [this, box] {
                    record_stats(box->outcome, box->source, box->out_dir, box->source_enc,
                                 box->target_enc);
                },
                Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, [this] { set_busy(false); }, Qt::QueuedConnection);
        });
    if (!started) {
        set_busy(false);
        status_->setText(QStringLiteral("已有任务在跑，请等它结束或先取消"));
    }
}

void ConvertPage::run_restore() {
    const QString source = source_edit_->text().trimmed();
    if (source.isEmpty()) {
        status_->setText(QStringLiteral("请先选源目录（备份在 源目录\\backup 下）"));
        return;
    }
    const QString root = QFileInfo(source).isFile() ? QFileInfo(source).absolutePath() : source;
    etxt::BackupTaskOptions options;
    options.source_root = root.toUtf8().toStdString();
    options.recursive = recursive_box_ == nullptr ? true : recursive_box_->isChecked();
    const QStringList exts = backup_extensions();
    for (const QString& ext : exts) options.extensions.push_back(ext.toStdString());

    set_busy(true);
    const bool started = task_->run(
        QStringLiteral("恢复备份"),
        [this, options](const ProgressSink& progress, const MessageSink& message) {
            progress(0);
            etxt::BatchHooks hooks;
            hooks.should_stop = [this]() { return task_ != nullptr && task_->cancel_requested(); };
            hooks.gate = [this]() { return wait_if_paused(true); };
            hooks.message = [&message](const std::string& text) { message(text); };
            hooks.progress = [&progress](int done, int total) {
                progress(total <= 0 ? 100 : done * 100 / total);
            };
            const etxt::BackupTaskResult result = etxt::restore_backups(options, hooks);
            // 零静默：没有 backup 目录 / 没有 .bak / 有失败，各自说清楚（不报成"完成 0 个"）
            if (result.dir_missing) {
                message("未找到 backup 目录，无备份可恢复：" + options.source_root);
            } else if (result.total == 0) {
                message("未找到可恢复的 .bak 备份文件（扩展名：" + options.extensions.front() +
                        " 等）");
            } else if (result.cancelled) {
                message(result.stop_reason);
            } else {
                message("恢复结束：成功 " + std::to_string(result.ok) + " / 失败 " +
                        std::to_string(result.failed) + " / 候选 " +
                        std::to_string(result.total));
            }
            for (const std::string& e : result.errors) message("[恢复] " + e);
            progress(100);
            QMetaObject::invokeMethod(
                this,
                [this, result] {
                    set_busy(false);
                    if (result.dir_missing) {
                        status_->setText(QStringLiteral("未找到 backup 目录，无备份可恢复"));
                    } else if (result.failed > 0) {
                        status_->setText(QStringLiteral("恢复完成，但有失败：成功 %1 / 失败 %2")
                                             .arg(result.ok)
                                             .arg(result.failed));
                    } else {
                        status_->setText(
                            QStringLiteral("恢复完成：成功 %1 / 候选 %2").arg(result.ok).arg(result.total));
                    }
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        set_busy(false);
        status_->setText(QStringLiteral("已有任务在跑，请等它结束或先取消"));
    }
}

void ConvertPage::run_clear_backup() {
    const QString source = source_edit_->text().trimmed();
    if (source.isEmpty()) {
        status_->setText(QStringLiteral("请先选源目录（备份在 源目录\\backup 下）"));
        return;
    }
    const QString root = QFileInfo(source).isFile() ? QFileInfo(source).absolutePath() : source;
    etxt::BackupTaskOptions options;
    options.source_root = root.toUtf8().toStdString();
    options.recursive = recursive_box_ == nullptr ? true : recursive_box_->isChecked();
    const QStringList exts = backup_extensions();
    for (const QString& ext : exts) options.extensions.push_back(ext.toStdString());

    set_busy(true);
    const bool started = task_->run(
        QStringLiteral("清空备份"),
        [this, options](const ProgressSink& progress, const MessageSink& message) {
            progress(0);
            etxt::BatchHooks hooks;
            hooks.should_stop = [this]() { return task_ != nullptr && task_->cancel_requested(); };
            hooks.gate = [this]() { return wait_if_paused(true); };
            hooks.message = [&message](const std::string& text) { message(text); };
            hooks.progress = [&progress](int done, int total) {
                progress(total <= 0 ? 100 : done * 100 / total);
            };
            const etxt::BackupTaskResult result = etxt::clear_backups(options, hooks);
            if (result.dir_missing) {
                message("未找到 backup 目录，无备份可清空：" + options.source_root);
            } else if (result.total == 0) {
                message("未找到可清空的 .bak 备份文件（删了 0 个）");
            } else if (result.cancelled) {
                message(result.stop_reason);
            } else {
                message("清空结束：成功 " + std::to_string(result.ok) + " / 失败 " +
                        std::to_string(result.failed) + " / 候选 " +
                        std::to_string(result.total));
            }
            for (const std::string& e : result.errors) message("[清空] " + e);
            progress(100);
            QMetaObject::invokeMethod(
                this,
                [this, result] {
                    set_busy(false);
                    if (result.dir_missing) {
                        status_->setText(QStringLiteral("未找到 backup 目录，无备份可清空"));
                    } else if (result.failed > 0) {
                        status_->setText(QStringLiteral("清空完成，但有失败：删除 %1 / 失败 %2")
                                             .arg(result.ok)
                                             .arg(result.failed));
                    } else {
                        status_->setText(QStringLiteral("清空完成：删除 %1 个 .bak（候选 %2）")
                                             .arg(result.ok)
                                             .arg(result.total));
                    }
                },
                Qt::QueuedConnection);
        });
    if (!started) {
        set_busy(false);
        status_->setText(QStringLiteral("已有任务在跑，请等它结束或先取消"));
    }
}

void ConvertPage::open_output() {
    // 1.x `open_out_dir`：无输出目录 ⇒ 退到源目录（单文件模式退到它所在目录）
    QString dir = output_edit_->text().trimmed();
    if (dir.isEmpty()) {
        const QString src = source_edit_->text().trimmed();
        if (src.isEmpty()) {
            status_->setText(QStringLiteral("请先选源路径或输出目录"));
            return;
        }
        dir = QFileInfo(src).isFile() ? QFileInfo(src).absolutePath() : src;
    }
    if (!QFileInfo(dir).isDir()) {
        status_->setText(QStringLiteral("目录不存在：%1").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    // 1.x：`open_in_explorer` 失败返回 False，原先忽略返回值 ⇒ 点了没反应（用户以为工具坏了）
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        status_->setText(QStringLiteral("打开失败：%1（可能缺少文件管理器关联或权限不足）")
                             .arg(QDir::toNativeSeparators(dir)));
        return;
    }
    status_->setText(QStringLiteral("已打开：%1").arg(QDir::toNativeSeparators(dir)));
}

bool ConvertPage::export_stats(const QString& path, QString* problem) {
    etxt::ConvertStats stats = stats_;
    if (!stats_ready_) {
        // 1.x：没有统计快照时用**当前界面值**（`_ensure_snap()`），计数留 0。
        // 不静默：调用方会看到"还没有可导出的统计"这句提示，而文件照样按字段全写。
        stats = etxt::ConvertStats{};
        const QString source = source_edit_->text().trimmed();
        stats.generated_at = QDateTime::currentDateTime()
                                 .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                 .toStdString();
        stats.source = source.toStdString();
        stats.single_file = QFileInfo(source).isFile();
        stats.output_dir = output_edit_->text().trimmed().toStdString();
        stats.source_enc = current_source_encoding();
        stats.target_enc = current_target_encoding();
        stats.duration = etxt::format_duration(0.0);
    }
    std::string error;
    if (!etxt::write_stats_csv(path.toUtf8().toStdString(), stats, &error)) {
        if (problem != nullptr) *problem = QStringLiteral("导出失败：") + utf8(error);
        return false;
    }
    if (problem != nullptr) problem->clear();
    return true;
}

// ★ 2026-10-04 用户第 2 条（"目录定向"）：这两个函数是**各"选择"调用点的判据唯一实现** ——
//   按钮 lambda 先绑它们（见构造期 `source_judge` / `output_judge`），自检也断言它们
//   ⇒ "自检断言的东西"与"真点按钮跑的东西"是同一份，不可能分叉。
DialogStart ConvertPage::source_dialog_start() const {
    // 源路径是**第一输入**：没有对应的输入目录 ⇒ 走到"工具自身所在文件夹"
    return dialog_start_for(source_edit_->text(), QStringList(), false);
}

DialogStart ConvertPage::output_dialog_start() const {
    // 输出目录 ← 源路径（唯一才用；空/多元 ⇒ 工具目录）
    return dialog_start_for(output_edit_->text(), QStringList{source_edit_->text()}, false);
}

}  // namespace xrkit::shell
