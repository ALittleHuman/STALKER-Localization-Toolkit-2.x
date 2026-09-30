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
    // ★ 把 Hub 的日志口接给页面（回调；Hub 无 Q_OBJECT，按名字 invokeMethod 会静默失败）
    const auto wire = [](PageBase* page) {
        // ★ 不依赖"工厂收到的 parent 就是 Hub"（Hub 造页时传的可能是它的 stack）：
        //   改成**在日志发生时**从页面自己往上找窗口（`page->window()` = Hub），再 dynamic_cast。
        //   用 dynamic_cast 而不是 qobject_cast：Hub **没有 Q_OBJECT**（本类不用 moc）。
        page->set_log_sink([page](const QString& line) {
            if (auto* hub = dynamic_cast<Hub*>(page->window())) hub->log_line(line);
        });
        return page;
    };
    if (id == QStringLiteral("fs")) return wire(new FsPage(parent));
    if (id == QStringLiteral("convert")) return wire(new ConvertPage(parent));
    if (id == QStringLiteral("text")) return wire(new TextPage(parent));
    if (id == QStringLiteral("xml")) return wire(new XmlPage(parent));
    if (id == QStringLiteral("video")) return wire(new VideoPage(parent));
    if (id == QStringLiteral("fontpack")) return wire(new FontPackPage(parent));
    return new PageBase(id, QString::fromUtf8(info.title), parent);
}

}  // namespace xrkit::shell
