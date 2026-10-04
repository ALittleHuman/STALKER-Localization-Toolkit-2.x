#include "fontpack_page.h"

#include "xrkit/core/theme.h"

#include "../task_shell.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <filesystem>

namespace xrkit::shell {
namespace {

QString utf8(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

FontPackPage::FontPackPage(QWidget* parent)
    : PageBase(QStringLiteral("fontpack"), QStringLiteral("汉化包生成"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    xml_edit_ = new QLineEdit;
    xml_edit_->setObjectName(QStringLiteral("font_xml_dir"));
    xml_edit_->setPlaceholderText(QStringLiteral("文本 XML 目录（只扫顶层）"));
    font_edit_ = new QLineEdit;
    font_edit_->setObjectName(QStringLiteral("font_path"));
    font_edit_->setPlaceholderText(QStringLiteral("字体文件（.ttf/.ttc）"));
    out_edit_ = new QLineEdit;
    out_edit_->setObjectName(QStringLiteral("font_out_dir"));
    out_edit_->setPlaceholderText(QStringLiteral("输出目录（产物是 <输出>/gamedata）"));

    // 11.8：状态标记与预览标题共用同一判据（引擎）
    state_label_ = new QLabel;
    state_label_->setObjectName(QStringLiteral("font_state"));
    result_label_ = new QLabel;
    result_label_->setObjectName(QStringLiteral("font_result"));

    // ★ 2026-10-04 用户第 2 条（"目录定向"）：判据只有成员函数那一份（自检断言同一份）
    const auto xml_judge = [this]() { return xml_dialog_start(); };
    const auto font_judge = [this]() { return font_dialog_start(); };
    const auto out_judge = [this]() { return out_dialog_start(); };

    const auto path_row = [this](const QString& label, QLineEdit* edit, bool directory,
                                 const QString& title, std::function<DialogStart()> judge) {
        // ★ 2026-10-04 用户第 1 条：整行成容器 ⇒ "拖到相应区域"
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this,
                         [this, edit, directory, title, judge] {
            // ★ 2026-10-04 用户第 2 条：①上次的选择 → ②唯一输入目录 → ③工具自身所在文件夹
            const QString start = resolve_dialog_start(judge ? judge() : DialogStart{});
            const QString picked =
                directory ? QFileDialog::getExistingDirectory(this, title, start)
                          : QFileDialog::getOpenFileName(this, title, start,
                                                        QStringLiteral("字体 (*.ttf *.ttc)"));
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
            refresh_state();
        });
        drop_zones_.attach_row(container, this);
        return container;
    };
    body_layout()->addWidget(path_row(QStringLiteral("文本 XML："), xml_edit_, true,
                                      QStringLiteral("选择 XML 目录"), xml_judge));
    body_layout()->addWidget(path_row(QStringLiteral("字体："), font_edit_, false,
                                      QStringLiteral("选择字体文件"), font_judge));
    body_layout()->addWidget(path_row(QStringLiteral("输出目录："), out_edit_, true,
                                      QStringLiteral("选择输出目录"), out_judge));

    game_combo_ = new QComboBox;
    game_combo_->setObjectName(QStringLiteral("font_game"));
    for (const char* game : {"SoC", "ClS", "CoP"}) game_combo_->addItem(utf8(game), utf8(game));
    lang_combo_ = new QComboBox;
    lang_combo_->setObjectName(QStringLiteral("font_lang"));
    for (const char* lang : {"chs", "eng", "rus"}) lang_combo_->addItem(utf8(lang), utf8(lang));
    suffix_edit_ = new QLineEdit;
    suffix_edit_->setObjectName(QStringLiteral("font_suffix"));
    suffix_edit_->setPlaceholderText(QStringLiteral("后缀（如 _chs）"));
    suffix_edit_->setMaximumWidth(xrkit::core::px(120));
    cyrillic_box_ = new QCheckBox(QStringLiteral("包含完整西里尔"));
    cyrillic_box_->setObjectName(QStringLiteral("font_full_cyrillic"));
    cyrillic_box_->setChecked(true);
    auto* opt_row = new QHBoxLayout;
    opt_row->addWidget(new QLabel(QStringLiteral("游戏：")), 0);
    opt_row->addWidget(game_combo_, 0);
    opt_row->addWidget(new QLabel(QStringLiteral("语言：")), 0);
    opt_row->addWidget(lang_combo_, 0);
    opt_row->addWidget(suffix_edit_, 0);
    opt_row->addWidget(cyrillic_box_, 0);
    opt_row->addStretch(1);
    body_layout()->addLayout(opt_row);
    body_layout()->addWidget(state_label_, 0);
    body_layout()->addWidget(result_label_, 0);
    body_layout()->addStretch(1);

    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    body_layout()->addWidget(task_->panel(), 0);

    auto* detect = add_action(QStringLiteral("检测字体"));
    detect->setObjectName(QStringLiteral("font_detect"));
    QObject::connect(detect, &QPushButton::clicked, this, [this] { refresh_state(); });
    auto* build_button = add_action(QStringLiteral("生成汉化包"));
    build_button->setObjectName(QStringLiteral("font_build"));
    QObject::connect(build_button, &QPushButton::clicked, this, [this] {
        const QString xml_dir = xml_edit_->text().trimmed();
        const QString font = font_edit_->text().trimmed();
        const QString out = out_edit_->text().trimmed();
        if (xml_dir.isEmpty() || font.isEmpty() || out.isEmpty()) {
            result_label_->setText(QStringLiteral("请把三个路径都填上"));
            return;
        }
        task_->run(QStringLiteral("生成汉化包"), [this, xml_dir, font, out](
                                                     const ProgressSink& progress,
                                                     const MessageSink& message) {
            progress(0);
            QString reason;
            if (!build(xml_dir, font, out, &reason)) {
                // 11.9：拒绝式检查 —— 这里**没有**任何目录被建出来
                message(reason.toUtf8().toStdString());
                progress(100);
                return;
            }
            message(QStringLiteral("产物写到 %1/gamedata").arg(out).toUtf8().toStdString());
            progress(100);
        });
    });
    refresh_state();
}

engine::font::CjkFontCheck FontPackPage::check_font(const QString& font_path) const {
    // 11.8 的判据**只有引擎那一份**（列表标记与预览标题共用它）
    if (font_path.isEmpty()) return engine::font::CjkFontCheck{};
    return engine::font::classify_cjk_font(font_path.toUtf8().toStdString(), 40);
}

QString FontPackPage::font_state_text(const QString& font_path) const {
    const engine::font::CjkFontCheck check = check_font(font_path);
    QString text = utf8(engine::font::cjk_state_mark(check.state));
    if (!check.missing.empty()) {
        text += QStringLiteral("　缺 %1 个字").arg(check.missing.size());
        if (!check.fallbacks.empty()) {
            text += QStringLiteral("，其中 %1 个可用繁体顶").arg(check.fallbacks.size());
        }
    }
    if (font_path.isEmpty()) text = QStringLiteral("未选择字体");
    return text;
}

void FontPackPage::refresh_state() {
    if (state_label_ == nullptr) return;
    state_label_->setText(QStringLiteral("字体状态：") + font_state_text(font_edit_->text().trimmed()));
}

engine::font::PackageOptions FontPackPage::options_for(const QString& xml_dir,
                                                       const QString& font_path,
                                                       const QString& out_dir) const {
    engine::font::PackageOptions options;
    options.game = game_combo_->currentData().toString().toUtf8().toStdString();
    options.lang = lang_combo_->currentData().toString().toUtf8().toStdString();
    options.xml_dir = xml_dir.toUtf8().toStdString();
    options.font_path = font_path.toUtf8().toStdString();
    options.out_dir = out_dir.toUtf8().toStdString();
    options.suffix = suffix_edit_->text().trimmed().toUtf8().toStdString();
    options.full_cyrillic = cyrillic_box_->isChecked();
    return options;
}

bool FontPackPage::build(const QString& xml_dir, const QString& font_path, const QString& out_dir,
                         QString* reason) {
    // 11.9：**先判后建** —— 缺字形/检测失败直接拒绝，且**不建任何目录**。
    // （`font_unrenderable` 只告警不拒绝：那条路在引擎的 build_package 里以 warning 体现。）
    const engine::font::CjkFontCheck check = check_font(font_path);
    if (check.state == engine::font::CjkFontState::Missing ||
        check.state == engine::font::CjkFontState::Unknown) {
        ++refused_count_;
        if (reason != nullptr) {
            *reason = QStringLiteral("拒绝生成：%1（缺字形或字体不可用；没有创建任何目录）")
                          .arg(utf8(engine::font::cjk_state_mark(check.state)));
        }
        if (result_label_ != nullptr) result_label_->setText(reason != nullptr ? *reason : QString());
        return false;
    }
    const engine::font::PackageResult result = engine::font::build_package(
        options_for(xml_dir, font_path, out_dir));
    if (result_label_ != nullptr) {
        result_label_->setText(
            result.ok ? QStringLiteral("完成：字体 %1 个、XML %2 个%3")
                            .arg(result.fonts_written)
                            .arg(result.xml_written)
                            .arg(result.warnings.empty()
                                     ? QString()
                                     : QStringLiteral("（%1 条告警）").arg(result.warnings.size()))
                      : QStringLiteral("失败：") + utf8(result.error));
    }
    if (!result.ok) {
        if (reason != nullptr) *reason = utf8(result.error);
        return false;
    }
    if (!check.fallbacks.empty() && state_label_ != nullptr) {
        state_label_->setText(state_label_->text() +
                              QStringLiteral("（已按繁体回退生成）"));
    }
    return true;
}

// ★ 2026-10-04 用户第 2 条（"目录定向"）：这三个函数是各"选择"调用点的判据唯一实现 ——
//   按钮 lambda 绑它们（见构造期的 `*_judge`），自检也断言它们。
DialogStart FontPackPage::xml_dialog_start() const {
    // 文本 XML 是这一页的**第一输入** ⇒ 空时落到"工具自身所在文件夹"
    return dialog_start_for(xml_edit_->text(), QStringList(), false);
}

DialogStart FontPackPage::font_dialog_start() const {
    // 字体 ← 文本 XML 所在目录（唯一才用；XML 是目录输入）
    return dialog_start_for(font_edit_->text(), QStringList{xml_edit_->text()}, false);
}

DialogStart FontPackPage::out_dialog_start() const {
    // 输出目录 ← 文本 XML 目录（唯一才用）
    return dialog_start_for(out_edit_->text(), QStringList{xml_edit_->text()}, false);
}

}  // namespace xrkit::shell
