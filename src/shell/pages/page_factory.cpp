#include "page_factory.h"

#include "convert_page.h"
#include "fontpack_page.h"
#include "fs_page.h"
#include "page_base.h"
#include "text_page.h"
#include "video_page.h"
#include "xml_page.h"

#include <QString>

namespace xrkit::shell {
namespace {

// 六个内置页（**顺序与名字照抄 1.x `BUILTIN_TOOLS`**；id 是我们自己的稳定标识）
// ★ 副标题（`subtitle`）：用户 2026-09-27「先把我截图的文字全删了」⇒ 他截到的两句
//   （文件系统 / 汉化包生成）**留空**（`set_subtitle("")` 会让它不占位）。
// ★★ 2026-10-04：用户**又截了四张图**（编码转换 / 文本提取 / XML校对 / 视频转换）说
//   「**我截图的文字全部删除。**」⇒ 剩下这四句也一并删掉 —— **六页副标题至此全部为空**。
//   六条都进 `smoke.cpp` 的禁用字串锁（被点名删掉的文字不许回来，逐字串判红）。
//   卡片里没有别的说明段（反向后仍成立：那组判据只看 QLabel / 占位符 / 下拉项）。
//
// ★ P1：`order` 就是**带内顺序**（内置带 kBuiltinPageBand）。插件页在另一个带里
//   （kBuiltinPageBand/kPluginPageBand 的说明见 page_host.h）⇒ 内置六页固定在前，
//   与 1.x `stalker_toolkit.py:215-221`「内置六工具固定在前」逐字一致。
constexpr PageInfo kBuiltinPages[] = {
    {"fs", "文件系统", "", 0},
    {"convert", "编码转换", "", 1},
    {"text", "文本提取", "", 2},
    {"xml", "XML校对", "", 3},
    {"video", "视频转换", "", 4},
    {"fontpack", "汉化包生成", "", 5},
};

constexpr int kBuiltinPageCount =
    static_cast<int>(sizeof(kBuiltinPages) / sizeof(kBuiltinPages[0]));

// 具体页类 → 控件：**全工程唯一的一份映射**（Hub 不认识这些类）。
// 不认识的 id ⇒ `PageBase` 兜底页（正常路径到不了这里：登记在册的 id 都有实现）。
QWidget* build_builtin(const QString& id, const QString& title, QWidget* parent) {
    if (id == QStringLiteral("fs")) return new FsPage(parent);
    if (id == QStringLiteral("convert")) return new ConvertPage(parent);
    if (id == QStringLiteral("text")) return new TextPage(parent);
    if (id == QStringLiteral("xml")) return new XmlPage(parent);
    if (id == QStringLiteral("video")) return new VideoPage(parent);
    if (id == QStringLiteral("fontpack")) return new FontPackPage(parent);
    return new PageBase(id, title, parent);
}

}  // namespace

int builtin_page_count() { return kBuiltinPageCount; }

const PageInfo& builtin_page(int index) {
    static const PageInfo kFallback{"", ""};
    if (index < 0 || index >= kBuiltinPageCount) return kFallback;
    return kBuiltinPages[index];
}

void register_builtin_pages(PageRegistry& pages) {
    for (int i = 0; i < kBuiltinPageCount; ++i) {
        const PageInfo& info = kBuiltinPages[i];
        PageSpec spec;
        spec.id = info.id;
        spec.title = QString::fromUtf8(info.title);
        spec.subtitle = QString::fromUtf8(info.subtitle);
        spec.order = info.order;
        spec.band = kBuiltinPageBand;  // 内置带（见 page_host.h）
        const QString id = QString::fromUtf8(info.id);
        const QString title = QString::fromUtf8(info.title);
        pages.register_page(kBuiltinPageOwner, spec,
                            [id, title](QWidget* parent) { return build_builtin(id, title, parent); });
    }
}

QWidget* create_builtin_page_widget(const std::string& id, QWidget* parent) {
    const QString key = QString::fromStdString(id);
    for (int i = 0; i < kBuiltinPageCount; ++i) {
        if (key == QString::fromUtf8(kBuiltinPages[i].id)) {
            return build_builtin(key, QString::fromUtf8(kBuiltinPages[i].title), parent);
        }
    }
    return build_builtin(key, key, parent);
}

PageBase* create_page(int index, QWidget* parent) {
    if (index < 0 || index >= kBuiltinPageCount) return nullptr;
    const PageInfo& info = kBuiltinPages[index];
    // 各页陆续换成自己的实现类；还没轮到的戴 `PageBase` 的框架
    //（标题 / 页内视口 / 贴底操作栏 —— 形态从第一天起就是一致的）。
    const QString id = QString::fromUtf8(info.id);
    const QString title = QString::fromUtf8(info.title);
    return dynamic_cast<PageBase*>(build_builtin(id, title, parent));
}

}  // namespace xrkit::shell
