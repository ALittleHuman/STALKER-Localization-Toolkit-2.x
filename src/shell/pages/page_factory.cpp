#include "page_factory.h"

#include "../hub.h"
#include "page_base.h"

#include <QString>

namespace xrkit::shell {

PageBase* create_page(int index, QWidget* parent) {
    if (index < 0 || index >= builtin_page_count()) return nullptr;
    const PageInfo& info = builtin_page(index);
    // M2-6a：先把六页的框架（标题 / 页内视口 / 贴底操作栏）按同一份口径搭起来；
    // M2-6b..g 各片再把自己的业务塞进 body 与操作栏 —— 那里面的算法一律走
    // engine/CLI，不许在 UI 里重写一份（这是 1.x 留下的最贵的一条教训）。
    return new PageBase(QString::fromUtf8(info.id), QString::fromUtf8(info.title), parent);
}

}  // namespace xrkit::shell
