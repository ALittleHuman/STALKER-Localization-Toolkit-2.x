#include "convert_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/text/encoding.h"

#include "../task_shell.h"
#include "../virtual_list.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace xrkit::shell {
namespace {

namespace etxt = xrkit::engine::text;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// 手动指定时能选的编码：**只列我们对拍过的那些**（严格解码 + 有损替换都有 golden）。
// 没对拍的编码宁可不给选，也不让人选到一个我们没验证过的行为。
const char* kManualEncodings[] = {etxt::kUtf8,          etxt::kWindows1251, etxt::kWindows1252,
                                  etxt::kGb18030,       etxt::kGbk,         etxt::kGb2312,
                                  etxt::kLatin1,
                                  // 2026-09-26：utf-16/32 六族的有损编解码已与 Python
                                  // `errors="replace"` 逐字节对拍（门禁 [12] 的 compared
                                  // 从 114 涨到 294）⇒ 按"只列对拍过的编码"这条纪律可以列了。
                                  etxt::kUtf16,     etxt::kUtf16Le, etxt::kUtf16Be,
                                  etxt::kUtf32,     etxt::kUtf32Le, etxt::kUtf32Be};

}  // namespace

ConvertPage::ConvertPage(QWidget* parent)
    : PageBase(QStringLiteral("convert"), QStringLiteral("编码转换"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内那一段说明文字**整段删掉**
    //   （六页统一；用户对"满屏解释"的口径与词汇口径同源：「不用带括号。一般人又看不懂。」）
    source_edit_ = new QLineEdit;
    source_edit_->setObjectName(QStringLiteral("convert_source"));
    source_edit_->setAcceptDrops(true);  // 6.4 同族：拖放挂在既有控件上
    source_edit_->setPlaceholderText(QStringLiteral("源文件或目录"));
    output_edit_ = new QLineEdit;
    output_edit_->setObjectName(QStringLiteral("convert_output"));
    output_edit_->setAcceptDrops(true);
    output_edit_->setPlaceholderText(QStringLiteral("输出目录"));

    auto make_row = [this](const QString& label, QLineEdit* edit, const QString& title) {
        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(label), 0);
        row->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        row->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, title] {
            const QString picked = QFileDialog::getExistingDirectory(this, title, edit->text());
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        return row;
    };
    body_layout()->addLayout(make_row(QStringLiteral("源路径："), source_edit_,
                                      QStringLiteral("选择源目录")));
    body_layout()->addLayout(make_row(QStringLiteral("输出目录："), output_edit_,
                                      QStringLiteral("选择输出目录")));

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
    body_layout()->addWidget(task_->panel(), 0);

    status_ = new QLabel(task_->status());
    status_->setObjectName(QStringLiteral("convert_status"));

    auto* start = add_action(QStringLiteral("开始转换"));
    start->setObjectName(QStringLiteral("convert_start"));
    auto* pause = add_action(QStringLiteral("暂停"));
    pause->setObjectName(QStringLiteral("convert_pause"));
    QObject::connect(pause, &QPushButton::clicked, this, [this, pause] {
        toggle_pause();
        pause->setText(paused() ? QStringLiteral("继续") : QStringLiteral("暂停"));
    });
    QObject::connect(start, &QPushButton::clicked, this, [this] {
        const QString source = source_edit_->text().trimmed();
        if (source.isEmpty()) {
            status_->setText(QStringLiteral("请先选源路径"));
            return;
        }
        QDir dir(source);
        std::vector<QString> files;
        if (dir.exists()) {
            const QStringList names =
                dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString& name : names) files.push_back(dir.filePath(name));
        } else {
            files.push_back(source);
        }
        run_batch(files, output_edit_->text().trimmed(), utf8(current_source_encoding()),
                  utf8(current_target_encoding()));
    });

    refresh_rows();
}

void ConvertPage::fill_encoding_combos() {
    source_combo_->addItem(QStringLiteral("自动"), QStringLiteral("auto"));
    for (const char* name : kManualEncodings) {
        source_combo_->addItem(utf8(name), utf8(name));
        target_combo_->addItem(utf8(name), utf8(name));
    }
    target_combo_->setCurrentIndex(0);  // utf-8
}

std::string ConvertPage::current_source_encoding() const {
    return source_combo_->currentData().toString().toUtf8().toStdString();
}

std::string ConvertPage::current_target_encoding() const {
    return target_combo_->currentData().toString().toUtf8().toStdString();
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

void ConvertPage::run_batch(const std::vector<QString>& files, const QString& out_dir,
                            const QString& source_enc, const QString& target_enc) {
    if (files.empty()) {
        status_->setText(QStringLiteral("没找到可转换的文件"));
        return;
    }
    // ⑬13.2：工作线程**不许碰 UI**（连读控件都不行）—— 参数在主线程冻结成值传进去。
    const int total = static_cast<int>(files.size());
    task_->run(QStringLiteral("编码转换"), [this, files, out_dir, source_enc, target_enc,
                                            total](const ProgressSink& progress,
                                                   const MessageSink& message) {
        int done = 0;
        int failed = 0;
        for (const QString& file : files) {
            if (!wait_if_paused(true)) return;  // 取消/退出：直接收工，由壳收尾
            const etxt::ConvertOutcome outcome =
                convert_one(file, out_dir, source_enc, target_enc);
            if (!outcome.ok) ++failed;
            message(QFileInfo(file).fileName().toUtf8().toStdString() + " | " + outcome.step_label +
                    (outcome.skipped ? " | 已是目标编码" : "") + " → " + target_enc.toUtf8().toStdString());
            ++done;
            // ⑬13.7：**只发一个 0~100 值**，壳里不乘不除
            progress(total == 0 ? 100 : done * 100 / total);
        }
        message(failed == 0 ? "全部完成" : ("有 " + std::to_string(failed) + " 个文件失败"));
    });
}

}  // namespace xrkit::shell
