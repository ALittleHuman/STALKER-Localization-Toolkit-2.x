#pragma once
// xrkit::shell —— 文本提取页（M2-6d）。
//
// 页面只收参数、显示结果；**判据全在引擎**（`engine::text`）：① 源目录名解析与
// "会不会覆盖源目录"的守卫在 `validate_request`（⑦7.1/7.5），② 三分支提取与排序在
// `extract_dir`（⑦7.2/7.4/7.6）。页面连那几句拒绝理由都不许自己写（静态锁）。
//
// 对照表里属本页的口径：7.1 只看选中文件夹自己的名字；7.2 同一份源两次运行产物相同
//（页面**显式**传 id_prefix，1.x 的 ID 带 `int(time.time())`，不给就不可复现）；
// 7.3 前缀参数是 `abbr`（gp/sc）；7.5 输出目录必填且不许撞源目录；7.6 自己的产物不重复提取。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/text/extract.h"

#include <QString>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace xrkit::shell {

class TaskShell;
class VirtualList;

class TextPage : public PageBase {
public:
    explicit TextPage(QWidget* parent = nullptr);

    // ⑦7.1/7.5：**直接返回引擎给的理由**（空串＝通过）。页面不重复实现判据。
    QString validate(const QString& source_dir, const QString& target_dir) const;

    // 跑一个分支（界面与自检共用同一条入口）。`should_stop` 非空 ⇒ 引擎在**文件边界**
    // 轮询它（①：取消要真停；停在哪由引擎按文件边界决定，产物不会半截）。
    engine::text::ExtractStats extract_one(const QString& source_dir, const QString& target_dir,
                                           const QString& prefix, const QString& id_prefix,
                                           bool clean,
                                           const std::function<bool()>& should_stop = {});

    void add_row(const QString& text);
    // ── ★ 2026-10-06（②）统计框三行（1.x `text_extract_app.py:647-656` 的
    //   `stat_grid([("gameplay",…),("scripts",…),("总计",…)])`，写值在 `:725-729`）──
    //   gameplay / scripts / 总计，数字**逐字段来自 `ExtractStats`**（引擎给的那一份，
    //   页面不自己数）。1.x 一次跑两个分支，2.x 一次只跑一个（源目录名决定）⇒ 这里**累加**：
    //   跑过 gameplay 就填 gameplay 那一行，总计＝两行之和。
    void note_stats(const engine::text::ExtractStats& stats);
    QString stats_text() const;
    QLabel* stats_label() const { return stats_label_; }
    QLineEdit* source_edit() const { return source_edit_; }
    QLineEdit* target_edit() const { return target_edit_; }
    QLineEdit* prefix_edit() const { return prefix_edit_; }
    QLineEdit* id_prefix_edit() const { return id_prefix_edit_; }
    QCheckBox* clean_box() const { return clean_box_; }
    VirtualList* rows() const { return rows_; }
    TaskShell* task() const { return task_; }
    QLabel* status() const { return status_; }

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 两个"选择"调用点的起点判据
    DialogStart source_dialog_start() const;
    DialogStart target_dialog_start() const;

private:
    void run_batch();
    void fill_kind_combo();

    QLineEdit* source_edit_ = nullptr;
    QLineEdit* target_edit_ = nullptr;
    QLineEdit* prefix_edit_ = nullptr;
    QLineEdit* id_prefix_edit_ = nullptr;
    QCheckBox* clean_box_ = nullptr;
    VirtualList* rows_ = nullptr;
    TaskShell* task_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* stats_label_ = nullptr;
    // 三个统计格子的真值（**只由 `note_stats` 写** ⇒ 界面上看到的数与引擎给的是同一份）
    engine::text::ExtractStats stats_gameplay_;
    engine::text::ExtractStats stats_scripts_;
    std::vector<QString> row_texts_;
};

}  // namespace xrkit::shell
