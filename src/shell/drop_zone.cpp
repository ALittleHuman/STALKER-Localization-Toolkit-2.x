#include "drop_zone.h"

#include "drop_filter.h"

#include <QDir>
#include <QLineEdit>
#include <QWidget>

#include <algorithm>

namespace xrkit::shell {

FileDropFilter* DropZones::attach_row(QWidget* container, QWidget* filter_owner) {
    // ★ 2026-10-05：**所有"字段行"容器都在这里**（六个页面都走这一条）⇒ 在这里统一挂名字，
    //   让 QSS 只把这**一类**行置成透明（"同类只维护一套"）。它的底该由所在卡片给：
    //   旧样式让容器自己涂 `bg`，露出来的正是用户截图里标签背后那块"缺了一块"的深灰底板。
    if (container != nullptr && container->objectName().isEmpty()) {
        container->setObjectName(QStringLiteral("field_row"));
    }
    return attach_zone(container, filter_owner, [container](const QStringList& paths) {
        // 只认**本地文件路径**（`FileDropFilter` 已过滤过一遍，这里再挡一次空串）
        QStringList local;
        for (const QString& path : paths) {
            if (!path.isEmpty()) local << path;
        }
        // 按创建顺序取这一行里的输入框；第 k 个路径 → 第 k 个框
        const QList<QLineEdit*> edits = container->findChildren<QLineEdit*>();
        for (int i = 0; i < edits.size() && i < local.size(); ++i) {
            edits[i]->setText(QDir::toNativeSeparators(local[i]));
        }
    });
}

FileDropFilter* DropZones::attach_zone(QWidget* container, QWidget* filter_owner,
                                      FileDropFilter::Handler handler) {
    if (container == nullptr) return nullptr;
    auto* filter = new FileDropFilter(std::move(handler), filter_owner);
    filter->attach(container);
    containers_.push_back(container);
    filters_.push_back(filter);
    return filter;
}

bool DropZones::is_zone(const QWidget* container) const {
    return container != nullptr &&
           std::find(containers_.begin(), containers_.end(), container) != containers_.end();
}

}  // namespace xrkit::shell
