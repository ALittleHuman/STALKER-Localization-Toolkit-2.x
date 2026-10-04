#pragma once
// xrkit::shell —— 汉化包生成页（M2-6g）。
//
// 出包本身早已完成（`engine::font::build_package`，门禁 [22] 与 1.x 整棵产物树逐字节一致），
// 本页只补 ⑪ 的两条**界面口径**：
//   · 11.8 字体四态（`✓ 中文 / ⚠ 日语字体 / ⚠ 疑似缺字形 / ? 检测失败`）——
//     列表标记与预览标题**共用同一个判据**（`classify_cjk_font`），页面不许自己再判一遍；
//   · 11.9 缺字形是**拒绝式检查、且在建目录之前**（引擎负责，页面只把拒绝理由显示出来，
//     并且**不留下半成品 gamedata**）；`font_unrenderable` 只告警不拒绝 —— 两者语义不同。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"

#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace xrkit::shell {

class TaskShell;

class FontPackPage : public PageBase {
public:
    explicit FontPackPage(QWidget* parent = nullptr);

    // 11.8：字体状态（**唯一判据**在引擎里；页面只是显示）
    engine::font::CjkFontCheck check_font(const QString& font_path) const;
    QString font_state_text(const QString& font_path) const;
    // 11.9：先判后建 —— 缺字形直接拒绝（返回 false + 理由），**不建目录**
    bool build(const QString& xml_dir, const QString& font_path, const QString& out_dir,
               QString* reason);

    QLineEdit* xml_edit() const { return xml_edit_; }
    QLineEdit* font_edit() const { return font_edit_; }
    QLineEdit* out_edit() const { return out_edit_; }
    QComboBox* game_combo() const { return game_combo_; }
    QComboBox* lang_combo() const { return lang_combo_; }
    QLabel* state_label() const { return state_label_; }
    QLabel* result_label() const { return result_label_; }
    TaskShell* task() const { return task_; }
    int refused_count() const { return refused_count_; }

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 三个"选择"调用点的起点判据
    //   （字体 ← 文本 XML 所在的上一级；输出目录 ← 文本 XML 目录）
    DialogStart xml_dialog_start() const;
    DialogStart font_dialog_start() const;
    DialogStart out_dialog_start() const;

    engine::font::PackageOptions options_for(const QString& xml_dir, const QString& font_path,
                                             const QString& out_dir) const;

private:
    void refresh_state();

    QLineEdit* xml_edit_ = nullptr;
    QLineEdit* font_edit_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
    QComboBox* game_combo_ = nullptr;
    QComboBox* lang_combo_ = nullptr;
    QLineEdit* suffix_edit_ = nullptr;
    QCheckBox* cyrillic_box_ = nullptr;
    QLabel* state_label_ = nullptr;
    QLabel* result_label_ = nullptr;
    TaskShell* task_ = nullptr;
    int refused_count_ = 0;
};

}  // namespace xrkit::shell
