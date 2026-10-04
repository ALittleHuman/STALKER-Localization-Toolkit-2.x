#pragma once
// xrkit::shell —— 汉化包生成页（M2-6g + 2026-10-06 D2 批"系统字体选择器"）。
//
// 出包本身早已完成（`engine::font::build_package`，门禁 [22] 与 1.x 整棵产物树逐字节一致），
// 本页只补 ⑪ 的两条**界面口径**：
//   · 11.8 字体四态（`✓ 中文 / ⚠ 日语字体 / ⚠ 疑似缺字形 / ? 检测失败`）——
//     列表标记与预览标题**共用同一个判据**（`classify_cjk_font`），页面不许自己再判一遍；
//   · 11.9 缺字形是**拒绝式检查、且在建目录之前**（引擎负责，页面只把拒绝理由显示出来，
//     并且**不留下半成品 gamedata**）；`font_unrenderable` 只告警不拒绝 —— 两者语义不同。
//
// ★★ 2026-10-06（D2 批）：用户点名「**字体默认从系统字体里找，对应逻辑看 1.x**」
//   ⇒ 逐条照抄 1.x `apps/font_pack_app.py._browse_font`（`:285-643`）与
//   `toolkit_platform.system_font_dirs`（`:222-234`）：
//     · 去哪找：`%SystemRoot%\Fonts`（**不写死 C:\Windows**）＋ 当前已选字体所在目录；
//     · 过滤：小写白名单 `*.ttf/*.ttc/*.otf` ＋ 文件名**子串**过滤 ＋ **完整路径**去重
//       ＋ 显示 `basename [目录]`（`:484-511`）；
//     · 默认选哪个（优先级，`:129-149`）：`user.ltx` 的 `[font] path`（**文件仍存在**）
//       → `msyh.ttc / msyh.ttf / msyhl.ttc` → 仓库根 `msyh.ttf`；
//     · 形态＝**自建对话框**（左：过滤框＋列表；右：预览＋±缩放 0.5~4.0＋文件名/字体名/警告；
//       底部：选择/浏览其他目录/取消；**双击＝选择**，`:290`/`:641`）；
//     · **记住上次选择**：任何选择路径都写 `user.ltx`（`:598-607`），失败要**明确告警、不静默**。
//   另四条（1.x `:84-89` / `:155` / `:202-220`）：尺寸档位下拉、打开输出目录、
//   加载语言"自定义＋输入框"、后缀预设 无/`_chs`/自定义。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"

#include <QString>
#include <QStringList>

#include <map>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

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
    QLineEdit* lang_edit() const { return lang_edit_; }
    QComboBox* suffix_combo() const { return suffix_combo_; }
    QLineEdit* suffix_edit() const { return suffix_edit_; }
    QComboBox* size_combo() const { return size_combo_; }
    QLabel* state_label() const { return state_label_; }
    QLabel* result_label() const { return result_label_; }
    TaskShell* task() const { return task_; }
    int refused_count() const { return refused_count_; }

    // ── ★ 2026-10-06 D2 批：系统字体选择器（逐条对 1.x `_browse_font`）────────
    // 系统字体目录：`%SystemRoot%\Fonts`（**不写死 C:\Windows**）＋ 当前已选字体所在目录，
    // 去重、存在的排前面（1.x `system_font_dirs` `:222-234`）。
    QStringList system_font_dirs() const;
    // 候选字体：白名单后缀（小写比较）＋ 文件名子串过滤 ＋ **完整路径**去重（1.x `:484-511`）。
    QStringList font_candidates(const QString& keyword) const;
    // 默认字体（1.x `:129-149` 的三级优先级；`saved` 由调用方从 user.ltx 取，便于自检驱动）。
    QString default_font_path(const QString& saved) const;
    // 造"选择字体"对话框（1.x `:290`，**自建**、不是下拉也不是单纯文件框）。**不 exec**。
    QDialog* make_font_picker(QWidget* parent = nullptr);
    // 列表标记的缓存判据（1.x `_font_cjk` 缓存 + `cached_status`）：**唯一判据在引擎**。
    QString cached_mark(const QString& font_path);
    // 预览缩放：1.x `set_zoom` —— 步进 0.5、夹在 [0.5, 4.0]；像素 = `max(12, int(40*zoom))`。
    double preview_zoom() const { return preview_zoom_; }
    void zoom_preview(double delta);
    int preview_pixel_size() const;
    // 选中一个字体：写输入框 + 刷新状态 + **记住到 user.ltx**（1.x：任何选择路径都要记）。
    // 失败**明确告警、不静默**（返回 false + 理由）。
    bool apply_font_choice(const QString& path, QString* problem);
    bool remember_font(const QString& path, QString* problem);
    // 「打开输出目录」（1.x `:649-659`：目录无效 / 打开失败都要有反馈）
    bool open_output(QString* problem);
    // 尺寸档位（1.x `off_var`：标准/+3/+5/+7/+9 ⇒ `PackageOptions::offset`）
    int size_offset() const;

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 三个"选择"调用点的起点判据
    //   （字体 ← 文本 XML 所在的上一级；输出目录 ← 文本 XML 目录）
    DialogStart xml_dialog_start() const;
    DialogStart font_dialog_start() const;
    DialogStart out_dialog_start() const;

    engine::font::PackageOptions options_for(const QString& xml_dir, const QString& font_path,
                                             const QString& out_dir) const;

private:
    void refresh_state();
    void sync_lang_edit();      // 语言选"自定义"才显示输入框（1.x `_lang_changed`）
    void sync_suffix_edit();    // 后缀选"自定义"才显示输入框（1.x `_suf_changed`）
    void open_font_picker();
    // 预览用的两行样本（1.x `:570`）+ 日语字体时按繁体回退替换（用户口径 2026-09-14）
    QString preview_text_line(const QString& font_path, int which) const;

    QLineEdit* xml_edit_ = nullptr;
    QLineEdit* font_edit_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
    QComboBox* game_combo_ = nullptr;
    QComboBox* lang_combo_ = nullptr;
    QLineEdit* lang_edit_ = nullptr;
    QComboBox* suffix_combo_ = nullptr;
    QLineEdit* suffix_edit_ = nullptr;
    QComboBox* size_combo_ = nullptr;
    QCheckBox* cyrillic_box_ = nullptr;
    QPushButton* open_out_button_ = nullptr;
    QLabel* state_label_ = nullptr;
    QLabel* result_label_ = nullptr;
    TaskShell* task_ = nullptr;
    int refused_count_ = 0;
    // 字体四态的**缓存**（1.x `self._font_cjk`：跨对话框复用，避免重复解析同一个字体文件）
    std::map<QString, QString> font_mark_cache_;
    double preview_zoom_ = 1.0;
};

}  // namespace xrkit::shell
