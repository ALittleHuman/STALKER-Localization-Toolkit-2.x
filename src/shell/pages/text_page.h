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

    // 跑一个分支（界面与自检共用同一条入口）
    engine::text::ExtractStats extract_one(const QString& source_dir, const QString& target_dir,
                                           const QString& prefix, const QString& id_prefix,
                                           bool clean);

    void add_row(const QString& text);
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
    std::vector<QString> row_texts_;
};

}  // namespace xrkit::shell
