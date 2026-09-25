#include "page_factory.h"

#include "../hub.h"
#include "convert_page.h"
#include "fontpack_page.h"
#include "fs_page.h"
#include "page_base.h"
#include "text_page.h"
#include "video_page.h"
#include "xml_page.h"

#include <QString>

namespace xrkit::shell {

PageBase* create_page(int index, QWidget* parent) {
    if (index < 0 || index >= builtin_page_count()) return nullptr;
    const PageInfo& info = builtin_page(index);
    const QString id = QString::fromUtf8(info.id);
    // 各页陆续换成自己的实现类；还没轮到的先戴 `PageBase` 的框架
    //（标题 / 页内视口 / 贴底操作栏 —— 形态从第一天起就是一致的）。
    if (id == QStringLiteral("fs")) return new FsPage(parent);
    if (id == QStringLiteral("convert")) return new ConvertPage(parent);
    if (id == QStringLiteral("text")) return new TextPage(parent);
    if (id == QStringLiteral("xml")) return new XmlPage(parent);
    if (id == QStringLiteral("video")) return new VideoPage(parent);
    if (id == QStringLiteral("fontpack")) return new FontPackPage(parent);
    return new PageBase(id, QString::fromUtf8(info.title), parent);
}

}  // namespace xrkit::shell
