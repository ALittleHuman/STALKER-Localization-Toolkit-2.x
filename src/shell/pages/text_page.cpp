#include "text_page.h"

#include "xrkit/core/theme.h"

#include "../task_shell.h"
#include "../virtual_list.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace xrkit::shell {
namespace {

namespace etxt = xrkit::engine::text;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

TextPage::TextPage(QWidget* parent)
    : PageBase(QStringLiteral("text"), QStringLiteral("文本提取"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    source_edit_ = new QLineEdit;
    source_edit_->setObjectName(QStringLiteral("text_source"));
    source_edit_->setAcceptDrops(true);  // 6.4 同族：拖放挂在既有控件上
    source_edit_->setPlaceholderText(QStringLiteral("gameplay 或 scripts 文件夹"));
    target_edit_ = new QLineEdit;
    target_edit_->setObjectName(QStringLiteral("text_target"));
    target_edit_->setAcceptDrops(true);
    target_edit_->setPlaceholderText(QStringLiteral("输出目录（必填，不能是源目录的父目录）"));

    const auto row = [this](const QString& label, QLineEdit* edit, const QString& title) {
        auto* layout = new QHBoxLayout;
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, title] {
            const QString picked = QFileDialog::getExistingDirectory(this, title, edit->text());
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        return layout;
    };
    body_layout()->addLayout(row(QStringLiteral("源目录："), source_edit_,
                                 QStringLiteral("选择 gameplay / scripts 文件夹")));
    body_layout()->addLayout(row(QStringLiteral("输出目录："), target_edit_,
                                 QStringLiteral("选择输出目录")));

    prefix_edit_ = new QLineEdit(utf8(etxt::kDefaultIdPrefix));
    prefix_edit_->setObjectName(QStringLiteral("text_prefix"));
    prefix_edit_->setMaximumWidth(xrkit::core::px(90));
    // ⑦7.3：ID 前缀模板是 `sgtat_{prefix}_{abbr}_{ts}`，`abbr` 由**类型**决定（gp/sc）。
    // 留空时引擎按 prefix+类型生成 ts=0 的形态；要可复现（7.2）就显式填。
    id_prefix_edit_ = new QLineEdit;
    id_prefix_edit_->setObjectName(QStringLiteral("text_id_prefix"));
    id_prefix_edit_->setPlaceholderText(QStringLiteral("ID 前缀（留空＝按 prefix+类型自动，对拍时显式给）"));
    clean_box_ = new QCheckBox(QStringLiteral("先清空输出目录里的旧产物"));
    clean_box_->setObjectName(QStringLiteral("text_clean"));
    clean_box_->setChecked(true);

    auto* opt_row = new QHBoxLayout;
    opt_row->addWidget(new QLabel(QStringLiteral("文本/ID 前缀：")), 0);
    opt_row->addWidget(prefix_edit_, 0);
    opt_row->addWidget(id_prefix_edit_, 1);
    opt_row->addWidget(clean_box_, 0);
    body_layout()->addLayout(opt_row);

    rows_ = new VirtualList;
    rows_->setObjectName(QStringLiteral("text_rows"));
    rows_->set_row_height(xrkit::core::px(20));
    rows_->set_row_text_provider([this](int index) -> QString {
        if (index < 0 || index >= static_cast<int>(row_texts_.size())) return QString();
        return row_texts_[static_cast<size_t>(index)];
    });
    body_layout()->addWidget(rows_, 1);

    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    task_->set_message_handler([this](const QString& text) { add_row(text); });
    body_layout()->addWidget(task_->panel(), 0);

    status_ = new QLabel(task_->status());
    status_->setObjectName(QStringLiteral("text_status"));

    auto* start = add_action(QStringLiteral("开始提取"));
    start->setObjectName(QStringLiteral("text_start"));
    QObject::connect(start, &QPushButton::clicked, this, [this] { run_batch(); });
    rows_->set_row_count(0);
}

QString TextPage::validate(const QString& source_dir, const QString& target_dir) const {
    // 判据只在引擎那一份（`validate_request`）—— 页面拿到的就是给用户看的那句话
    etxt::ExtractRequest request;
    request.source_dir = source_dir.toUtf8().toStdString();
    request.target_dir = target_dir.toUtf8().toStdString();
    request.prefix = prefix_edit_->text().trimmed().toUtf8().toStdString();
    return utf8(etxt::validate_request(request));
}

engine::text::ExtractStats TextPage::extract_one(const QString& source_dir,
                                                 const QString& target_dir,
                                                 const QString& prefix, const QString& id_prefix,
                                                 bool clean) {
    etxt::ExtractKind kind = etxt::ExtractKind::Gameplay;
    std::string err;
    etxt::ExtractStats stats;
    if (!etxt::kind_from_dir_name(source_dir.toUtf8().toStdString(), kind, err)) {
        stats.error = err;
        return stats;
    }
    const QString sub = kind == etxt::ExtractKind::Gameplay ? QStringLiteral("gameplay")
                                                            : QStringLiteral("scripts");
    etxt::ExtractOptions options;
    options.prefix = prefix.toUtf8().toStdString();
    options.id_prefix = id_prefix.toUtf8().toStdString();
    options.clean = clean;
    const QDir target(target_dir);
    return etxt::extract_dir(source_dir.toUtf8().toStdString(), target.filePath(sub).toUtf8().toStdString(),
                             target_dir.toUtf8().toStdString(), options);
}

void TextPage::add_row(const QString& text) {
    row_texts_.push_back(text);
    rows_->set_row_count(static_cast<int>(row_texts_.size()));
}

void TextPage::run_batch() {
    const QString source = source_edit_->text().trimmed();
    const QString target = target_edit_->text().trimmed();
    const QString problem = validate(source, target);
    if (!problem.isEmpty()) {
        // 引擎给的理由原样显示（页面不重写、也不"顺手清一下"什么）
        status_->setText(problem);
        add_row(problem);
        return;
    }
    const QString prefix = prefix_edit_->text().trimmed();
    const QString id_prefix = id_prefix_edit_->text().trimmed();
    const bool clean = clean_box_->isChecked();
    // ⑬13.2：工作线程里只拿值，不读控件
    task_->run(QStringLiteral("文本提取"), [this, source, target, prefix, id_prefix,
                                             clean](const ProgressSink& progress,
                                                    const MessageSink& message) {
        progress(0);
        const etxt::ExtractStats stats = extract_one(source, target, prefix, id_prefix, clean);
        progress(100);
        if (!stats.ok) {
            message("提取失败：" + stats.error);
            return;
        }
        message(QStringLiteral("files=%1 strings=%2 chars=%3 xml=%4")
                    .arg(stats.files)
                    .arg(stats.strings)
                    .arg(stats.chars)
                    .arg(utf8(stats.texts_xml_name))
                    .toUtf8().toStdString());
        for (const std::string& skipped : stats.skipped) message("跳过 " + skipped);
    });
}

}  // namespace xrkit::shell
