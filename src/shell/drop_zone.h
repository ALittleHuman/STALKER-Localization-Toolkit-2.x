#pragma once
// xrkit::shell —— 拖放**区域**（2026-10-04 用户第 1 条）：
//   「首先，所有目录都需要拖拽，而且是**拖拽到相应区域**。」
//
// 口径（与 `drop_filter.h` 的分工）：
//   · `FileDropFilter` = **机制**（DragEnter/DragMove/Drop 的判定、光标、本地路径提取）；
//   · 本文件 = **接线**（哪块区域接哪几个输入），只有这一份实现。
//
// 为什么要一层"区域"而不是把过滤器装在输入框上：
//   用户原话是"拖到**相应区域**"——一条输入往往只有 px(200) 宽的细长 QLineEdit，
//   把过滤器挂在它身上，用户把文件丢在这一行的标签/按钮/空白处都**没反应**。
//   区域 = 该输入所在的**整行**（`QHBoxLayout` 装进一个容器 QWidget）或整张卡片。
//
// 一次拖进来多个本地路径时的落地顺序（**只有这一份规则**）：
//   第 k 个路径填该区域里的第 k 个输入框（按控件树的创建顺序，`findChildren` 是深度优先、稳定）；
//   路径比输入框多 ⇒ 多出来的丢掉（区域只负责它自己那几个输入，谁都不许"顺手"改别的控件的值）。

#include "drop_filter.h"   // 机制在本文件里（`FileDropFilter`），这里只做接线

#include <QString>
#include <QStringList>

#include <vector>

class QWidget;

namespace xrkit::shell {

class DropZones {
public:
    DropZones() = default;
    DropZones(const DropZones&) = delete;
    DropZones& operator=(const DropZones&) = delete;

    // 一个目录/文件**输入**所在的整块区域：丢进来的路径按顺序填它里面的输入框。
    //   · `container` = 整行（或整卡片）——用户要的"相应区域"；
    //   · 区域里没有可填的输入框 ⇒ 仍接住，但什么都不改（不静默改别的控件）。
    FileDropFilter* attach_row(QWidget* container, QWidget* filter_owner);

    // 自定义落地（例：FS 页丢进包列表 = 加包，不走输入框）。
    FileDropFilter* attach_zone(QWidget* container, QWidget* filter_owner,
                               FileDropFilter::Handler handler);

    // 自检用：装上的区域清单（判据是**接线**，不是"代码里有个 dropEvent"）。
    const std::vector<QWidget*>& containers() const { return containers_; }
    // 与 `containers()` **同下标**并行（自检直接拿它派发 drop，验的是真过滤器）。
    const std::vector<FileDropFilter*>& filters() const { return filters_; }
    bool is_zone(const QWidget* container) const;
    int zone_count() const { return static_cast<int>(containers_.size()); }

private:
    std::vector<QWidget*> containers_;
    std::vector<FileDropFilter*> filters_;
};

}  // namespace xrkit::shell
