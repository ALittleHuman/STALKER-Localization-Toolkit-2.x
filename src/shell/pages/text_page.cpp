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
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

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
    // ★ 2026-10-04：输入框自己**不接**拖放 —— 接住它的必须是**整行区域**（`drop_zones_`）。
    source_edit_->setPlaceholderText(QStringLiteral("gameplay 或 scripts 文件夹"));
    target_edit_ = new QLineEdit;
    target_edit_->setObjectName(QStringLiteral("text_target"));
    target_edit_->setPlaceholderText(QStringLiteral("输出目录（必填，不能是源目录的父目录）"));

    // ★ 2026-10-04 用户第 2 条（"目录定向"）：判据只有成员函数那一份（自检断言同一份）
    const auto source_judge = [this]() { return source_dialog_start(); };
    const auto target_judge = [this]() { return target_dialog_start(); };

    const auto row = [this](const QString& label, QLineEdit* edit, const QString& title,
                            std::function<DialogStart()> judge) {
        // ★ 2026-10-04 用户第 1 条：整行成一个容器 ⇒ 「拖拽到**相应区域**」（不是只有细长输入框）
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, title, judge] {
            // ★ 2026-10-04 用户第 2 条：①上次的选择 → ②唯一输入目录 → ③工具自身所在文件夹
            const QString picked = QFileDialog::getExistingDirectory(
                this, title, resolve_dialog_start(judge ? judge() : DialogStart{}));
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        drop_zones_.attach_row(container, this);
        return container;
    };
    body_layout()->addWidget(row(QStringLiteral("源目录："), source_edit_,
                                 QStringLiteral("选择 gameplay / scripts 文件夹"), source_judge));
    // 输出目录 ← 源目录（唯一才用）
    body_layout()->addWidget(row(QStringLiteral("输出目录："), target_edit_,
                                 QStringLiteral("选择输出目录"), target_judge));

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

    // ★★ 2026-10-06（②）：统计框三行 —— 1.x `text_extract_app.py:647-656` 的
    //   `stat_grid([("gameplay",…),("scripts",…),("总计",…)])`，写值在 `:725-729`。
    //   2.x 原来只有"跑完塞一行消息"（`:170-175`），用户看不到分项与总计。
    stats_label_ = new QLabel;
    stats_label_->setObjectName(QStringLiteral("text_stats"));
    stats_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body_layout()->addWidget(stats_label_, 0);
    note_stats(etxt::ExtractStats{});   // 初值：三行都在（"没跑过"= 0，不是空白）

    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    task_->set_message_handler([this](const QString& text) { add_row(text); });
    body_layout()->addWidget(task_->panel(), 0);

    // ★★ 2026-10-07（审计补充 F3「孤儿顶层控件」）：修前是
    //   `status_ = new QLabel(task_->status());` —— `TaskShell::status()` 返回 **QString**，
    //   于是造出一个**无父**标签（顶层窗口、从不进布局）⇒ 本页的消息（空目录 / 导出成功失败）
    //   用户看不见，控件也没人拥有。口径：页面级消息标签必须有父、进布局。
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("text_status"));
    status_->setWordWrap(true);
    body_layout()->addWidget(status_, 0);

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
                                                 bool clean,
                                                 const std::function<bool()>& should_stop) {
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
    // ①：把"取消请求位"包成引擎的停止谓词（引擎不依赖 Qt ⇒ 走 std::function）
    options.should_stop = should_stop;
    const QDir target(target_dir);
    return etxt::extract_dir(source_dir.toUtf8().toStdString(), target.filePath(sub).toUtf8().toStdString(),
                             target_dir.toUtf8().toStdString(), options);
}

QString TextPage::stats_text() const {
    // 三行 = gameplay / scripts / 总计（数字**只来自引擎给的 ExtractStats**）。
    // 总计＝两行逐字段相加（不是"再数一遍"，免得两处口径分叉）。
    const auto line = [](const QString& title, const etxt::ExtractStats& s) {
        return QStringLiteral("%1：%2 字符串 · %3 文件 · %4 字符")
            .arg(title)
            .arg(static_cast<qulonglong>(s.strings))
            .arg(static_cast<qulonglong>(s.files))
            .arg(static_cast<qulonglong>(s.chars));
    };
    etxt::ExtractStats total;
    total.files = stats_gameplay_.files + stats_scripts_.files;
    total.strings = stats_gameplay_.strings + stats_scripts_.strings;
    total.chars = stats_gameplay_.chars + stats_scripts_.chars;
    return line(QStringLiteral("gameplay"), stats_gameplay_) + QLatin1Char('\n') +
           line(QStringLiteral("scripts"), stats_scripts_) + QLatin1Char('\n') +
           line(QStringLiteral("总计"), total);
}

void TextPage::note_stats(const etxt::ExtractStats& stats) {
    // 一次 run 只跑一个分支（源目录名决定）⇒ 按 `kind` 落到对应那一行；总计是算出来的。
    if (stats.kind == etxt::ExtractKind::Gameplay) {
        stats_gameplay_ = stats;
    } else {
        stats_scripts_ = stats;
    }
    if (stats_label_ != nullptr) stats_label_->setText(stats_text());
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
        // ①：取消谓词 = 本页任务壳的取消位（引擎在**文件边界**轮询它）
        const auto should_stop = [this]() {
            return task_ != nullptr && task_->cancel_requested();
        };
        const etxt::ExtractStats stats =
            extract_one(source, target, prefix, id_prefix, clean, should_stop);
        progress(100);
        // ②：统计框**只能在主线程**更新（⑬13.2：工作线程不碰控件）⇒ 排队回主线程。
        //   先于收尾入队（同一队列保序）⇒ 自检"跑完读标签"时它已经落地。
        etxt::ExtractStats for_ui = stats;
        QMetaObject::invokeMethod(
            this, [this, for_ui] { note_stats(for_ui); }, Qt::QueuedConnection);
        if (stats.cancelled) {
            // ①零静默：取消必须**说清停在哪、已产出多少**（绝不报成"完成"）
            message(QStringLiteral("已取消：在文件边界停下（已提取 %1 个文件 / %2 字符串 / %3 字符；"
                                   "已写出的产物都是完整的）")
                        .arg(static_cast<qulonglong>(stats.files))
                        .arg(static_cast<qulonglong>(stats.strings))
                        .arg(static_cast<qulonglong>(stats.chars))
                        .toUtf8().toStdString());
            return;
        }
        if (!stats.ok) {
            // ★ S1-1（零静默）：失败也**先把台账逐条报出来** —— 产物写不出去时
            //   "哪些文件没落盘 / 为什么"就是用户唯一能自救的线索，不能只剩一行总结。
            for (const std::string& skipped : stats.skipped) message("跳过 " + skipped);
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

// ★ 2026-10-04 用户第 2 条：两个"选择"调用点的起点判据（与按钮 lambda 里那一行同源）
DialogStart TextPage::source_dialog_start() const {
    return dialog_start_for(source_edit_->text(), QStringList(), false);
}

DialogStart TextPage::target_dialog_start() const {
    return dialog_start_for(target_edit_->text(), QStringList{source_edit_->text()}, false);
}

}  // namespace xrkit::shell
