#include "fs_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/squashfs.h"

#include "../drop_filter.h"


#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>

namespace xrkit::shell {
namespace {

namespace efs = xrkit::engine::fs;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// ★★ 2026-10-01 用户第 5 条：「移除这个按钮也可以优化掉，在包的条目后面增加一个叉叉按钮。」
//   用**委托**在 col2 画 ✕ 并自己处理点击 —— **不要用 `setItemWidget`**：那会把整行内容换成
//   自造控件，勾选框/列宽/选择行为全会被搞乱（用户要的只是"行尾一个小按钮"）。
//   点击走 `editorEvent`（判 MouseButtonRelease + 落点在小方块内）⇒ 回调同一份
//   `remove_source_at`；右键菜单的「移除选中」也走它 ⇒ 移除逻辑只有一套。
class PackageRemoveDelegate : public QStyledItemDelegate {
public:
    explicit PackageRemoveDelegate(std::function<void(QTreeWidgetItem*, int)> on_remove)
        : on_remove_(std::move(on_remove)) {}

    // ★ 2026-10-02：源下标**必须从同一行的第 0 列读** —— 数据是用 `setData(0, Qt::UserRole, …)` 存的，
    //   而委托拿到的 `index` 永远在第 2 列 ⇒ `index.data(Qt::UserRole)` 是**无效值**（`toInt()`=0），
    //   那样分组节点会被当成源节点、✕ 也会画到分组节点上。用 `sibling(row, 0)` 才是同一项的那一格。
    static int source_index_of(const QModelIndex& index) {
        if (!index.isValid()) return -1;
        return index.sibling(index.row(), 0).data(Qt::UserRole).toInt();
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        // 只有**源节点**（第 0 列 `UserRole` 存着 `sources_` 下标 ≥0）才画 ✕；
        // **纯分组节点**（目录、不对应任何源）没有 ✕。
        if (index.column() != 2 || source_index_of(index) < 0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setPen(option.palette.color(QPalette::Text));
        QFont font = option.font;
        font.setPointSizeF(font.pointSizeF() + 1.0);
        painter->setFont(font);
        painter->drawText(option.rect, Qt::AlignCenter, QStringLiteral("✕"));
        painter->restore();
    }

    bool editorEvent(QEvent* event, QAbstractItemModel*, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if (index.column() != 2 || event == nullptr) return false;
        const int source_index = source_index_of(index);
        if (source_index < 0) return false;   // 分组节点：没有 ✕ 可点
        if (event->type() != QEvent::MouseButtonRelease) return false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        if (!hit_rect(option).contains(mouse->position().toPoint())) return false;
        // ★ 回调给**节点**（不再是行号）：树化之后行号与 `sources_` 下标已经不是一回事。
        //   `option.widget` 在**合成事件**（自检里手搓的 `QStyleOptionViewItem`）里是空的 ⇒
        //   取不到节点时**也要认这次点击**（源下标已经拿到，别让"✕ 点了没反应"）。
        QTreeWidgetItem* item = nullptr;
        if (auto* tree = qobject_cast<QTreeWidget*>(const_cast<QWidget*>(option.widget))) {
            item = tree->itemFromIndex(index);
        }
        if (on_remove_ != nullptr) on_remove_(item, source_index);
        return true;   // 吃掉：别让它变成"点了一下整行"
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        if (index.column() == 2 && source_index_of(index) >= 0) {
            size.setWidth(xrkit::core::px(30));
        }
        return size;
    }

private:
    static QRect hit_rect(const QStyleOptionViewItem& option) {
        const int box = xrkit::core::px(20);
        const QRect rect = option.rect;
        return QRect(rect.center().x() - box / 2, rect.center().y() - box / 2, box, box);
    }
    std::function<void(QTreeWidgetItem*, int)> on_remove_;
};

// ★ 用户第 3 条原话：「并且扫描时有Bug的，选择包模式的时候，是扫不出金球奖的db的。这是个Bug。
//   不要想着增加后缀的样式（.db0，.db_base等），因为这是遍历不完的。然后点击"所有文件"才能看见那些包。」
//   ⇒ 过滤器**恰好两段**，用 `*.db*` / `*.sq*` 一次覆盖所有分卷与自定义后缀
//   （老写法把 `*.db0 *.db1 …` 逐个列出来 ⇒ `gamedata.db_textures_1` 这类根本不在名单里）。
constexpr const char* kPackageDialogFilter = "包 (*.db* *.sq*);;所有文件 (*.*)";

// 占位子项：`make_entry_item` 给"模型里有子节点的目录"塞的那个空项（只为让展开箭头出现，
// 真子项在展开时才建）。★ 2026-10-01 用户报「在筛选的时候，文件夹下会多出一个空的行」——
// 真因就是**只要接着要建真子项的地方**没把这个占位项摘掉，它就留在第一个位置当空行。
// ⇒ 凡是"马上要往里加真子项"的路径（筛选里的一次性建树、`build_all_descendants`）都必须先摘。
void drop_placeholder(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    if (item->childCount() == 1 && item->child(0)->data(0, Qt::UserRole).toString().isEmpty()) {
        item->removeChild(item->child(0));
    }
}

// ═══ 包列表的"文件树"折叠（2026-10-02）═════════════════════════════════════
// 用户原话：「这玩意，一把 `E:\...\IWP\database` 拖进去就这样。**左边的包列表的结构也应该
// 是一个文件树**，而且**拖入文件夹的一些逻辑也要修复一下，和单个包文件那种结构的逻辑统一**。」
// —— 原来 58 个包**平铺**成一列、还夹着一行 `database [已加载] 0 B`。
// 口径：
//   · **锚点 = 所有源路径的公共祖先目录**；目录型源取它的**父目录**参与比较 ⇒ 这个目录自己会成为
//     树里的一个节点（例：源＝`…\database`(目录) + `…\database\ai.db` ⇒ 树＝`database/` → `ai.db`）。
//   · 只拖一个 `…\Golden Sphere\gamedata.db_base` ⇒ 锚点就是 `…\Golden Sphere`，树里只有
//     `gamedata.db_base` 一个根节点（**不画** `Golden Sphere` 之类的祖先）。
//   · 同一层：**目录在前、文件在后**，各自按名（不区分大小写）。
struct PkgTreeNode {
    QString name;           // 单段名字（不加尾斜杠，画的时候按 `is_dir` 补）
    int source_index = -1;  // ≥0 ⇒ 源节点（对应 `sources_` 下标）；-1 ⇒ **纯分组节点**
    bool is_dir = false;
    std::vector<PkgTreeNode> children;
};

QString pkg_parent_dir(const QString& path) {
    // 目录型源取**父目录**（这样它自己会成为树里一个节点）；文件型源取所在目录
    return QFileInfo(path).absolutePath();
}

QString pkg_common_ancestor(const QStringList& dirs) {
    if (dirs.isEmpty()) return QString();
    const QStringList first =
        QDir::fromNativeSeparators(dirs.front()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int keep = first.size();
    for (int i = 1; i < dirs.size(); ++i) {
        const QStringList parts =
            QDir::fromNativeSeparators(dirs[i]).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        int n = 0;
        while (n < keep && n < parts.size() &&
               first[n].compare(parts[n], Qt::CaseInsensitive) == 0) {
            ++n;
        }
        keep = n;
    }
    return first.mid(0, keep).join(QLatin1Char('/'));
}

void pkg_sort_nodes(std::vector<PkgTreeNode>& nodes) {
    std::sort(nodes.begin(), nodes.end(), [](const PkgTreeNode& a, const PkgTreeNode& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;   // 目录在前
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    for (PkgTreeNode& node : nodes) pkg_sort_nodes(node.children);
}

std::vector<PkgTreeNode> pkg_build_nodes(const QStringList& paths) {
    QStringList parents;
    for (const QString& path : paths) parents << pkg_parent_dir(path);
    const QString anchor = pkg_common_ancestor(parents);
    std::vector<PkgTreeNode> roots;
    for (int i = 0; i < paths.size(); ++i) {
        QString rel = anchor.isEmpty()
                          ? QFileInfo(paths[i]).fileName()
                          : QDir(anchor).relativeFilePath(QDir::fromNativeSeparators(paths[i]));
        if (rel.isEmpty() || rel.startsWith(QStringLiteral(".."))) {
            rel = QFileInfo(paths[i]).fileName();   // 跨盘等退化情形：至少给它一个名字
        }
        const QStringList segments = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segments.isEmpty()) continue;
        std::vector<PkgTreeNode>* level = &roots;
        for (int s = 0; s < segments.size(); ++s) {
            auto it = std::find_if(level->begin(), level->end(), [&](const PkgTreeNode& node) {
                return node.name.compare(segments[s], Qt::CaseInsensitive) == 0;
            });
            if (it == level->end()) {
                PkgTreeNode node;
                node.name = segments[s];
                level->push_back(node);
                it = level->end() - 1;
            }
            if (s + 1 == segments.size()) {
                it->source_index = i;
                it->is_dir = QFileInfo(paths[i]).isDir();   // 目录型源：不显大小、名字带 '/'
            } else {
                it->is_dir = true;                          // 中间段 ⇒ 目录
            }
            level = &it->children;
        }
    }
    pkg_sort_nodes(roots);
    return roots;
}

// 遍历包列表树里的**源节点**（纯分组节点跳过）——"找源"一律读 `data(0, Qt::UserRole)` 的
// **`sources_` 下标**，**不许再按行号**（行号＝源下标这个巧合已经被树结构打破）。
void pkg_for_each_source(QTreeWidget* list,
                         const std::function<void(QTreeWidgetItem*, int)>& fn) {
    if (list == nullptr) return;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0) fn(item, index);
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    for (int i = 0; i < list->topLevelItemCount(); ++i) walk(list->topLevelItem(i));
}

}  // namespace

bool FsPage::gate_source(QString& path, QString* error_out) {
    last_gate_error_.clear();
    if (registry_ == nullptr) return true;  // 没有注册表 ⇒ 一切照旧（不引入新分支）
    const std::string in = path.toUtf8().toStdString();
    const plugin::DecryptGateResult result =
        plugin::gate_db_path(*registry_, in, decrypt_dir_.toUtf8().toStdString());
    if (!result.ok) {
        last_gate_error_ = QString::fromStdString(result.error);
        if (error_out != nullptr) *error_out = last_gate_error_;
        return false;
    }
    if (result.used_plugin) {
        // 解开了：换成解密产物，并在日志里说清"这一份是插件解出来的"
        path = QString::fromStdString(result.path);
        log_message(QStringLiteral("已用插件解密器解开：%1").arg(path));
    }
    return true;
}

bool FsPage::add_loose_dir(const QString& dir, QString* error) {
    std::string err;
    const auto entries = engine::fs::loose_entries(dir.toUtf8().toStdString(), err);
    if (!err.empty()) {
        // 不存在的目录/读不动 ⇒ **可见失败**（不静默加一个空源）
        if (error != nullptr) *error = QString::fromStdString(err);
        log_message(QStringLiteral("散装层拒绝加载：%1").arg(QString::fromStdString(err)));
        return false;
    }
    QString gated = dir;
    QString gate_error;
    if (!gate_source(gated, &gate_error)) {
        if (error != nullptr) *error = gate_error;
        log_message(QStringLiteral("散装层被解密闸门拒绝：%1").arg(gate_error));
        return false;
    }
    add_source(gated, entries);
    log_message(QStringLiteral("散装层已并入：%1（%2 个条目）")
                    .arg(gated)
                    .arg(static_cast<int>(entries.size())));
    return true;
}

void FsPage::log_message(const QString& text) {
    // 页面只转发给宿主（Hub 决定往哪写）。★ 走**回调**而不是 invokeMethod：
    //   Hub 不用 Q_OBJECT ⇒ 按名字调用永远失败（页面日志一条都进不去，用户 2026-09-30 报的就是这个）。
    log_to_host(text);
}

FsPage::FsPage(QWidget* parent) : PageBase(QStringLiteral("fs"), QStringLiteral("文件系统"), parent) {
    decrypt_dir_ = QDir::temp().filePath(QStringLiteral("xrkit_decrypted"));
    QDir().mkpath(decrypt_dir_);
    // ★ 2026-09-28（用户："FS 的方便程度比 1.x 下降了不少，你回头看看 1.1.0"）——
    //   本页布局按 **1.x `apps/fs_app.py` 的控件组织**重排（照代码，不照印象）：
    //     · 包列表带自己的按钮行（加包/扫描目录/全选包/加载选中/取消加载/移除/清空 + 计数）
    //     · 条目列表带自己的行（搜索框 + 包内全选/全不选 + 计数）
    //     · 顶部一行：打包/解包格式 + **解包输出目录**（1.x 的「输出」行，含浏览/拖放/打开）
    //   ——1.x 的"方便"主要就来自这三块控件各自贴着自己的列表，而不是全塞进底部操作栏。
    // ── 左：包列表（6.4：拖放**直接挂在这个既有控件上**，不另造 drop_zone）
    // ★★ 2026-10-01 用户第 6 条：「包列表和文件列表引用的子组件是不是不一样？…所以，使用同一个
    //   小组件来完成。包列表的内容和文件列表的结构实际上是一样的。统一使用文件列表的那个，
    //   因为那个已经打磨过。」⇒ 由 `QListWidget` 换成 `QTreeWidget`，配置照抄下面 `entry_tree_`
    //   那一段（uniformRowHeights / 三列 Interactive + 初始宽 + minimumSectionSize /
    //   ExtendedSelection / acceptDrops / 最小高）。三列＝ 包 / 大小 / ✕。
    package_list_ = new QTreeWidget;
    package_list_->setObjectName(QStringLiteral("fs_package_list"));
    package_list_->setColumnCount(3);
    package_list_->setHeaderLabels({QStringLiteral("包"), QStringLiteral("大小"), QString()});
    package_list_->setUniformRowHeights(true);
    package_list_->setAlternatingRowColors(false);
    package_list_->setRootIsDecorated(false);   // 包列表是平表，不需要展开箭头
    // ★★ 2026-10-02 用户只圈着表头那块打了个「？」—— 真因：三列**都是 Interactive 且没有任何一列吃剩余
    //   宽度** ⇒ 初始总宽 px(180)+px(90)+px(48)=318 ≪ 面板宽（真机约 717）⇒ **表头与每一行的右半边
    //   是死区**（背景还在，却没有列、也没有分隔线，看着像表断了）。
    //   修法＝**名称列吃掉剩余宽度**、后两列靠右钉住：这是文件表格的常规长相（长名字不再被截，
    //   大小与 ✕ 永远贴右边），后两列**仍然可拖**（用户 09-29 要的拖动不许丢）。
    //   ——**条目树不跟着改**（用户没点它，它的"来源"列吃宽是既有观感；要统一说一声即可）。
    package_list_->header()->setStretchLastSection(false);
    package_list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    package_list_->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    package_list_->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    package_list_->header()->resizeSection(1, xrkit::core::px(90));
    package_list_->header()->resizeSection(2, xrkit::core::px(48));
    // 与 `entry_tree_` **同一个** minimumSectionSize（同一套小组件模板；28 那种"因为 ✕ 列窄"
    // 的特例会让两条列表的表头行为不一致，正是用户第 6 条要消掉的东西）。
    package_list_->header()->setMinimumSectionSize(xrkit::core::px(48));
    package_list_->setAcceptDrops(true);
    package_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    package_list_->setMinimumHeight(xrkit::core::px(120));
    // 行尾 ✕（第 5 条）：委托画 + 自己处理点击 ⇒ 删掉工具行里的「移除」按钮
    //   ★ 2026-10-02 树化之后回调给**节点**，源下标从 `data(0, Qt::UserRole)` 读（分组节点 -1 ⇒ 不画）
    package_list_->setItemDelegateForColumn(
        2, new PackageRemoveDelegate([this](QTreeWidgetItem*, int source_index) {
            if (source_index >= 0) remove_source_at(source_index);
        }));

    // ── 中：格式下拉（6.13：取值 = 内部键）+ 条目列表（虚拟化）
    format_combo_ = new QComboBox;
    format_combo_->setObjectName(QStringLiteral("fs_format_combo"));
    fill_format_combo();

    // ★ 真树（1.x CanvasTree 的对位件）：三列 = 名称（带三态勾选框）/ 大小 / 来源包；
    //   子节点**懒建**（展开时才建），所以几万条目的大包打开也不慢。
    entry_tree_ = new QTreeWidget;
    entry_tree_->setObjectName(QStringLiteral("fs_entry_tree"));
    entry_tree_->setColumnCount(3);
    entry_tree_->setHeaderLabels({QStringLiteral("包内条目"), QStringLiteral("大小"),
                                  QStringLiteral("来源")});
    entry_tree_->setUniformRowHeights(true);   // 行高一致 ⇒ 几万行也不慢
    entry_tree_->setAlternatingRowColors(false);
    // ★ 列宽（用户截图：名称列被挤成 "text..."，整棵树没法读）：**首列拉伸**，
    //   后两列按内容自适应；别再用 `setStretchLastSection(true)`（那会拉伸"来源"列、
    //   把名称列留成默认的几十像素）。
    // ★ 用户 2026-09-29 第 4 条：「某些字段还是不能全部显示……最好也是弄一个水平方向的可拖拽分隔条」
    //   ⇒ 三列都改成 **Interactive**（表头之间可以拖），并给一组合适的初始宽度；
    //   不要再 Stretch/ResizeToContents（那样用户没法自己调，字段也容易被挤掉）。
    entry_tree_->header()->setStretchLastSection(false);
    for (int col = 0; col < 3; ++col) {
        entry_tree_->header()->setSectionResizeMode(col, QHeaderView::Interactive);
    }
    entry_tree_->header()->resizeSection(0, xrkit::core::px(340));   // 用户："包内条目/大小/来源还是被截"
    entry_tree_->header()->resizeSection(1, xrkit::core::px(110));
    entry_tree_->header()->resizeSection(2, xrkit::core::px(220));
    entry_tree_->header()->setMinimumSectionSize(xrkit::core::px(48));
    entry_tree_->header()->setStretchLastSection(true);
    // ★ 2026-09-27（用户实测"排版有很大问题"）：条目区给自己一个**最小高度**，
    //   免得没有条目时被压成一条细线（看着像坏了）。
    entry_tree_->setMinimumHeight(xrkit::core::px(160));
    // ★ 用户 2026-10-01：「DB栏能拖动多选，文件栏不行。」⇒ 包内条目也要能**框选/拖选多个**
    //   （包列表一直是 ExtendedSelection）。选中的条目本身不参与解包（解包看勾选态），
    //   但"能多选"是他习惯的操作方式，缺了就像坏了。
    entry_tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    // 勾选/展开都回灌模型（见 on_entry_item_changed / populate_entry_children）
    QObject::connect(entry_tree_, &QTreeWidget::itemChanged, this,
                     [this](QTreeWidgetItem* item, int) { on_entry_item_changed(item); });
    QObject::connect(entry_tree_, &QTreeWidget::itemExpanded, this,
                     [this](QTreeWidgetItem* item) { populate_entry_children(item); });
    // ★ 用户 2026-09-29 第 5 条：「文件夹要双击才能打开，改成单击。然后单个文件要点框才能勾选，
    //   改成点击条目就能勾选（那个勾选框只是视觉效果，没有额外判定）」。
    //   `expandsOnDoubleClick(false)` + itemClicked：文件夹单击就展开/折叠；
    //   文件**点条目身体**即切换勾选（点勾选框本身仍由 Qt 走 itemChanged，两条路都回灌模型）。
    entry_tree_->setExpandsOnDoubleClick(false);
    QObject::connect(entry_tree_, &QTreeWidget::itemClicked, this,
                     [this](QTreeWidgetItem* item, int) { on_entry_item_clicked(item); });

    // ★ 用户 2026-09-27 真机明令：「把图中玩意删掉」——卡片里那坨**空状态文字**连同它的
    //   容器块一起删掉；页内提示只保留**顶部标题行的副标题**那一句（原来卡片里还贴了一遍
    //   几乎同义的话，是重复）。删完卡片里只剩：打包格式行 + 各列表自己的按钮行。
    // ★ 用户 2026-09-29 更正：「其实之前的设计可以，但是**上面解包和下面封包分两个标签**就没问题了。」
    //   ⇒ 整页两标签；**页面内部一切照旧**（不搬动控件、不摊大按钮），底部操作栏也不动。
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("fs_tabs"));
    auto* unpack_page = new QWidget;
    unpack_page->setObjectName(QStringLiteral("fs_unpack_page"));
    unpack_layout_ = new QVBoxLayout(unpack_page);
    unpack_layout_->setContentsMargins(0, 0, 0, 0);
    auto* pack_page = new QWidget;
    pack_page->setObjectName(QStringLiteral("fs_pack_page"));
    pack_page_layout_ = new QVBoxLayout(pack_page);
    pack_page_layout_->setContentsMargins(0, 0, 0, 0);
    tabs_->addTab(unpack_page, QStringLiteral("解包"));
    tabs_->addTab(pack_page, QStringLiteral("封包"));
    // ── 顶部一行 = **整页级**的东西（与两张列表都无关）：打包/解包格式 + 解包输出。
    // 1.x 的「输入」行在这里换成了「解包到」：2.x 的包是"加进来"的（加包/拖放/扫描），
    // 所以缺的不是输入行而是**解包输出行**（原来写死 `<包路径>.out`，用户没法选）。
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("打包/解包格式：")), 0);
    row->addWidget(format_combo_, 0);
    row->addSpacing(xrkit::core::px(12));
    row->addWidget(new QLabel(QStringLiteral("解包到：")), 0);
    out_edit_ = new QLineEdit;
    out_edit_->setObjectName(QStringLiteral("fs_output_dir"));
    out_edit_->setAcceptDrops(true);  // 6.4 同族：拖放挂在既有控件上
    out_edit_->setPlaceholderText(QStringLiteral("输出目录（留空＝与包同目录）"));
    row->addWidget(out_edit_, 1);
    auto* browse_out = new QPushButton(QStringLiteral("选择…"));
    browse_out->setObjectName(QStringLiteral("fs_browse_output"));
    row->addWidget(browse_out, 0);
    open_out_button_ = new QPushButton(QStringLiteral("打开"));
    open_out_button_->setObjectName(QStringLiteral("fs_open_output"));
    row->addWidget(open_out_button_, 0);
    unpack_layout_->addLayout(row);
    // 「加包」**不在这条页级行上**：它服务的是包列表 ⇒ 归包列表工具行（见下）。

    // ── ★ 2026-09-30 结构重排（用户第四轮 ①：「你不认为，堆起来很丑吗？」+ 授权「按自己审美来改」）
    //   症状：解包页原来是"包工具行 / 包列表 / 条目工具行 / 条目树"四条**等宽横条**上下叠着，
    //   而且条目工具行（搜索/全选）落在分隔条**下面** —— 视觉上像是页级控件，实际只服务于树。
    //   改法：每个列表**各自成一个小面板**（工具行 + 列表，同一父控件、同一条左边界、自身只留
    //   px(4) 行距），两个面板一起进既有的竖向分隔条 ⇒ 页级横条从 4 条降到 1 条，
    //   "谁的行规谁管"一眼可辨。**不新增任何说明文字**（用户口径：界面不要解释段）。
    package_pane_ = new QWidget;
    package_pane_->setObjectName(QStringLiteral("fs_package_pane"));
    auto* package_pane_layout = new QVBoxLayout(package_pane_);
    package_pane_layout->setContentsMargins(0, 0, 0, 0);
    package_pane_layout->setSpacing(xrkit::core::px(4));

    entry_pane_ = new QWidget;
    entry_pane_->setObjectName(QStringLiteral("fs_entry_pane"));
    auto* entry_pane_layout = new QVBoxLayout(entry_pane_);
    entry_pane_layout->setContentsMargins(0, 0, 0, 0);
    entry_pane_layout->setSpacing(xrkit::core::px(4));

    // 包列表的工具行（1.x `_build_db_panel` 的 bar：加包/全选/加载选中/取消加载/清空 + 计数）
    // ★ 2026-10-01 用户第 4/5 条：**「扫描目录」删掉**（加包多了"选文件夹…"模式，功能重合）、
    //   **「移除」删掉**（改到每行的行尾 ✕）⇒ 工具行少两个按钮，给列表让出空间。
    package_bar_ = new QWidget;
    package_bar_->setObjectName(QStringLiteral("fs_package_bar"));
    {
        auto* bar = new QHBoxLayout(package_bar_);
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(xrkit::core::px(6));
        add_button_ = new QPushButton(QStringLiteral("加包"));
        add_button_->setObjectName(QStringLiteral("fs_add_package"));
        bar->addWidget(add_button_, 0);
        // ★ 第 1 条：按钮表述改成「全选」（原「全选包」）；行为＝**选中而非勾选**
        select_all_button_ = new QPushButton(QStringLiteral("全选"));
        select_all_button_->setObjectName(QStringLiteral("fs_select_all_packages"));
        bar->addWidget(select_all_button_, 0);
        auto* load_sel = new QPushButton(QStringLiteral("加载选中"));
        load_sel->setObjectName(QStringLiteral("fs_load_selected"));
        bar->addWidget(load_sel, 0);
        QObject::connect(load_sel, &QPushButton::clicked, this, [this] { load_selected(); });
        auto* unload_sel = new QPushButton(QStringLiteral("取消加载"));
        unload_sel->setObjectName(QStringLiteral("fs_unload_selected"));
        bar->addWidget(unload_sel, 0);
        QObject::connect(unload_sel, &QPushButton::clicked, this, [this] { unload_selected(); });
        clear_button_ = new QPushButton(QStringLiteral("清空"));
        clear_button_->setObjectName(QStringLiteral("fs_clear_packages"));
        bar->addWidget(clear_button_, 0);
        bar->addStretch(1);
        package_count_label_ = new QLabel;
        package_count_label_->setObjectName(QStringLiteral("fs_package_count"));
        bar->addWidget(package_count_label_, 0);
    }
    package_pane_layout->addWidget(package_bar_, 0);
    package_pane_layout->addWidget(package_list_, 1);
    // ★ 工具行**永远只占一行高**（2026-09-30 修）：它是普通 QWidget、垂直策略默认 Preferred，
    //   一旦同面板里的列表被隐藏/没有最小高，布局会把剩余空间全塞给它 ⇒ 里面的按钮被垂直居中，
    //   看起来就是"飘在空白中间"。钉成 Fixed 之后，无论列表在不在，工具行都只在顶部一条。
    package_bar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // 条目列表的工具行（1.x `_build_file_panel`：全选/全不选 + **搜索** + 计数）+ 树
    entry_bar_ = new QWidget;
    entry_bar_->setObjectName(QStringLiteral("fs_entry_bar"));
    {
        auto* bar = new QHBoxLayout(entry_bar_);
        bar->setContentsMargins(0, 0, 0, 0);
        bar->setSpacing(xrkit::core::px(6));
        check_all_button_ = new QPushButton(QStringLiteral("包内全选"));
        check_all_button_->setObjectName(QStringLiteral("fs_check_all_files"));
        bar->addWidget(check_all_button_, 0);
        check_none_button_ = new QPushButton(QStringLiteral("包内全不选"));
        check_none_button_->setObjectName(QStringLiteral("fs_check_no_files"));
        bar->addWidget(check_none_button_, 0);
        bar->addWidget(new QLabel(QStringLiteral("搜索")), 0);
        search_edit_ = new QLineEdit;
        search_edit_->setObjectName(QStringLiteral("fs_search"));
        search_edit_->setPlaceholderText(QStringLiteral("按路径过滤（边打边筛）"));
        bar->addWidget(search_edit_, 1);
        entry_count_label_ = new QLabel;
        entry_count_label_->setObjectName(QStringLiteral("fs_entry_count"));
        bar->addWidget(entry_count_label_, 0);
    }
    entry_pane_layout->addWidget(entry_bar_, 0);
    entry_pane_layout->addWidget(entry_tree_, 1);
    entry_bar_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);   // 同上：工具行只占一行

    // ★ 用户 2026-09-30：「上面包列表和下面文件列表之间也要有可拖拽分隔条」；并授权"自己看着
    //   有需要的地方就加" ⇒ 竖向 QSplitter 拖的两个**面板**（工具行跟着自己的列表一起走，
    //   拖分隔条时不会把工具行落在外面）。
    // ★ 2026-10-01（用户：「这个不要做成上下，做成左右。」）⇒ 分隔条改**水平**：
    //   包列表在**左**、包内条目在**右**。两个面板各自「工具行 + 列表」的结构不变。
    split_list_tree_ = new QSplitter(Qt::Horizontal);
    split_list_tree_->setObjectName(QStringLiteral("fs_split_list_tree"));
    split_list_tree_->setChildrenCollapsible(false);
    split_list_tree_->addWidget(package_pane_);
    split_list_tree_->addWidget(entry_pane_);
    split_list_tree_->setSizes({xrkit::core::px(340), xrkit::core::px(900)});
    unpack_layout_->addWidget(split_list_tree_, 1);

    // ── ★ 1.x 的**封包面板**（`_build_pack_panel`）：这一整块是"FS 方便程度"的最后一块拼图
    //    （1.x 能"解包 → 改 → 重新封包"，2.x 原来只解不封）。布局照 1.x 三行：
    //    格式+输出+浏览+封包 / ＋文件＋目录 移除 清空 + 计数 / 待打包列表。
    {
    body_layout()->addWidget(tabs_, 1);   // 标签区＝下方工作区，自己拿拉伸

        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("封包格式：")), 0);
        pack_format_combo_ = new QComboBox;
        pack_format_combo_->setObjectName(QStringLiteral("fs_pack_format"));
        for (const efs::FsFormatItem& item : efs::fs_formats()) {
            // 1.x 的封包下拉 = 非 auto 的全部格式（六种 DB + SquashFS）；取值仍是**内部键**
            pack_format_combo_->addItem(utf8(item.label), utf8(item.key));
        }
        row->addWidget(pack_format_combo_, 0);
        row->addWidget(new QLabel(QStringLiteral("封包输出：")), 0);
        pack_out_edit_ = new QLineEdit;
        pack_out_edit_->setObjectName(QStringLiteral("fs_pack_out"));
        pack_out_edit_->setPlaceholderText(QStringLiteral("输出文件（留空＝弹保存框）"));
        row->addWidget(pack_out_edit_, 1);
        auto* browse_pack = new QPushButton(QStringLiteral("选择…"));
        browse_pack->setObjectName(QStringLiteral("fs_pack_browse"));
        row->addWidget(browse_pack, 0);
        pack_button_ = new QPushButton(QStringLiteral("封包"));
        pack_button_->setObjectName(QStringLiteral("fs_pack"));
        row->addWidget(pack_button_, 0);
        pack_page_layout_->addLayout(row);

        auto* bar = new QHBoxLayout;
        auto* add_files = new QPushButton(QStringLiteral("＋文件"));
        add_files->setObjectName(QStringLiteral("fs_pack_add_files"));
        bar->addWidget(add_files, 0);
        auto* add_dir = new QPushButton(QStringLiteral("＋目录"));
        add_dir->setObjectName(QStringLiteral("fs_pack_add_dir"));
        bar->addWidget(add_dir, 0);
        auto* rm = new QPushButton(QStringLiteral("移除"));
        rm->setObjectName(QStringLiteral("fs_pack_remove"));
        bar->addWidget(rm, 0);
        auto* clr = new QPushButton(QStringLiteral("清空"));
        clr->setObjectName(QStringLiteral("fs_pack_clear"));
        bar->addWidget(clr, 0);
        bar->addStretch(1);
        pack_count_label_ = new QLabel;
        pack_count_label_->setObjectName(QStringLiteral("fs_pack_count"));
        bar->addWidget(pack_count_label_, 0);
        pack_page_layout_->addLayout(bar);

        pack_list_ = new QListWidget;
        pack_list_->setObjectName(QStringLiteral("fs_pack_list"));
        pack_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        pack_list_->setMinimumHeight(xrkit::core::px(90));
        pack_page_layout_->addWidget(pack_list_, 1);

        QObject::connect(add_files, &QPushButton::clicked, this, [this] {
            const QStringList picked = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择要打进去的文件"), pack_base_dir(),
                QStringLiteral("所有文件 (*.*)"));
            if (!picked.isEmpty()) add_pack_files(picked);
        });
        QObject::connect(add_dir, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择要打进去的目录（递归，自动排除包文件）"), pack_base_dir());
            if (!dir.isEmpty()) add_pack_dir(dir);
        });
        QObject::connect(rm, &QPushButton::clicked, this, [this] { remove_pack_selected(); });
        QObject::connect(clr, &QPushButton::clicked, this, [this] { clear_pack(); });
        QObject::connect(browse_pack, &QPushButton::clicked, this, [this] {
            // 1.x 走 `_resolve_pack_target` 的保存框分支；这里显式选一次（选完回填）
            const QString picked = QFileDialog::getSaveFileName(
                this, QStringLiteral("封包输出文件"), pack_out_edit_->text(),
                QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
            if (!picked.isEmpty()) pack_out_edit_->setText(QDir::toNativeSeparators(picked));
        });
        QObject::connect(pack_button_, &QPushButton::clicked, this, [this] { start_pack(); });
    }

    // ── 任务壳（13.1/13.2）：解包/封包在**后台线程**跑，界面不冻（1.x TaskRunner 同族；
    //    2.x 的 convert/text 页也是这么接的 —— 用户要的「方便」里包含「大包不卡界面」）。
    task_ = new TaskShell(this);
    task_->set_idle_status(QStringLiteral("就绪"));
    task_->set_message_handler([this](const QString& text) { log_message(text); });
    body_layout()->addWidget(task_->panel(), 0);

    QObject::connect(add_button_, &QPushButton::clicked, this, [this] {
        // ★ 2026-10-01 用户第 4 条：「能不能在加载包那里增加一个文件夹的模式，这样就能精简掉
        //   一个按钮了。」⇒ 一个「加包」按钮两种模式（弹小菜单），原「扫描目录」按钮删掉。
        QMenu menu(this);
        QAction* pick_files = menu.addAction(QStringLiteral("选包文件…"));
        QAction* pick_dir = menu.addAction(QStringLiteral("选文件夹…"));
        QAction* chosen = menu.exec(add_button_->mapToGlobal(QPoint(0, add_button_->height())));
        if (chosen == nullptr) return;
        if (chosen == pick_dir) {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择要加载的文件夹"), resolve_output_dir());
            if (!dir.isEmpty()) add_paths({dir});   // 目录 ⇒ 散装层 + 递归找包并加载
            return;
        }
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要加载的包（可多选）"), QString(),
            QString::fromUtf8(kPackageDialogFilter));
        if (!picked.isEmpty()) add_paths(picked);
    });
    QObject::connect(browse_out, &QPushButton::clicked, this, [this] {
        const QString picked =
            QFileDialog::getExistingDirectory(this, QStringLiteral("选择解包输出目录"),
                                              out_edit_->text());
        if (!picked.isEmpty()) out_edit_->setText(QDir::toNativeSeparators(picked));
    });
    QObject::connect(open_out_button_, &QPushButton::clicked, this, [this] {
        const QString dir = resolve_output_dir();
        if (!dir.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    // 6.4：拖放挂在**既有控件**上（包列表 + 卡片空白处），不另造 drop_zone
    drop_filter_ = new FileDropFilter([this](const QStringList& paths) { add_paths(paths); }, this);
    drop_filter_->attach(package_list_);
    drop_filter_->attach(card());

    // ── 操作栏（规则 2：贴底、不进视口）：1.x 顶部那两个解包入口
    unpack_checked_button_ = add_action(QStringLiteral("解包选中"));
    unpack_checked_button_->setObjectName(QStringLiteral("fs_unpack_checked"));
    unpack_all_button_ = add_action(QStringLiteral("批量解包"));
    unpack_all_button_->setObjectName(QStringLiteral("fs_unpack_all"));

    // 包列表勾选框（1.x `_db_toggle`：勾一个包 = 加载它 + 该包条目全勾）
    QObject::connect(package_list_, &QTreeWidget::itemChanged, this,
                     [this](QTreeWidgetItem* item, int) { on_package_item_changed(item); });
    QObject::connect(select_all_button_, &QPushButton::clicked, this, [this] {
        // ★★ 2026-10-01 用户第 1 条（原话）：「全选就直接勾选了？不能这样。」「全选包，功能是
        //   选中而非勾选。」⇒ **回退**上一轮"选中 + 勾选"的做法：这里**只改选中态**，
        //   一个勾选框都不碰（勾选＝把这一包并进右边的树，那是"勾选"这个动作自己的语义）。
        std::vector<std::string> visible;
        for (const Source& source : sources_) visible.push_back(source.key);
        selection_.select_all(visible);
        // ★ 2026-10-02 树化：「全选」＝把所有**源节点**选中（分组节点不选 —— 它不对应任何包）
        pkg_for_each_source(package_list_, [](QTreeWidgetItem* item, int) { item->setSelected(true); });
        refresh_counts();
    });
    QObject::connect(check_all_button_, &QPushButton::clicked, this, [this] { check_all(true); });
    QObject::connect(check_none_button_, &QPushButton::clicked, this, [this] { check_all(false); });
    QObject::connect(clear_button_, &QPushButton::clicked, this, [this] { clear_all(); });
    // 搜索：边打边筛（1.x 的 `search_var.trace_add` 同一行为）
    QObject::connect(search_edit_, &QLineEdit::textChanged, this, [this] { refresh_rows(); });
    // 6.11：两个解包入口都走**同一条** `extract_to`（页面里 `extract_tree(` 只许出现一次）
    QObject::connect(unpack_checked_button_, &QPushButton::clicked, this, [this] {
        start_unpack(true);
    });
    QObject::connect(unpack_all_button_, &QPushButton::clicked, this, [this] {
        start_unpack(false);
    });

    // ★ 用户第 8 条：「增加右键选单。要加什么功能自己想。」—— 两个列表各一个，**都调既有函数**
    //   （按钮的同一个底层入口），不另写一套逻辑。
    package_list_->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(package_list_, &QWidget::customContextMenuRequested, this,
                     [this](const QPoint& pos) {
                         QMenu menu(this);
                         QAction* act_load = menu.addAction(QStringLiteral("加载选中"));
                         QAction* act_unload = menu.addAction(QStringLiteral("取消加载"));
                         menu.addSeparator();
                         QAction* act_select_all = menu.addAction(QStringLiteral("全选"));
                         menu.addSeparator();
                         QAction* act_remove = menu.addAction(QStringLiteral("移除选中"));
                         QAction* act_clear = menu.addAction(QStringLiteral("清空"));
                         QAction* chosen =
                             menu.exec(package_list_->viewport()->mapToGlobal(pos));
                         if (chosen == nullptr) return;
                         if (chosen == act_load) load_selected();
                         else if (chosen == act_unload) unload_selected();
                         else if (chosen == act_select_all) select_all_button_->click();
                         else if (chosen == act_remove) remove_selected();
                         else if (chosen == act_clear) clear_all();
                     });
    entry_tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(entry_tree_, &QWidget::customContextMenuRequested, this,
                     [this](const QPoint& pos) {
                         QMenu menu(this);
                         QAction* act_expand = menu.addAction(QStringLiteral("展开一级"));
                         QAction* act_collapse = menu.addAction(QStringLiteral("折叠全部"));
                         menu.addSeparator();
                         QAction* act_check = menu.addAction(QStringLiteral("勾选选中"));
                         QAction* act_uncheck = menu.addAction(QStringLiteral("取消勾选选中"));
                         menu.addSeparator();
                         QAction* act_copy = menu.addAction(QStringLiteral("复制路径"));
                         QAction* chosen = menu.exec(entry_tree_->viewport()->mapToGlobal(pos));
                         if (chosen == nullptr) return;
                         if (chosen == act_expand || chosen == act_collapse) {
                             // 懒建树**不做真 expandAll**（几万条会把整棵树建出来）⇒ 只动顶层一级
                             const bool expand = (chosen == act_expand);
                             for (int i = 0; i < entry_tree_->topLevelItemCount(); ++i) {
                                 entry_tree_->topLevelItem(i)->setExpanded(expand);
                             }
                             return;
                         }
                         if (chosen == act_check || chosen == act_uncheck) {
                             // 走 Qt 的 setCheckState ⇒ itemChanged ⇒ on_entry_item_changed 回灌模型
                             const Qt::CheckState state =
                                 (chosen == act_check) ? Qt::Checked : Qt::Unchecked;
                             for (QTreeWidgetItem* item : entry_tree_->selectedItems()) {
                                 if (item->flags().testFlag(Qt::ItemIsUserCheckable)) {
                                     item->setCheckState(0, state);
                                 }
                             }
                             return;
                         }
                         if (chosen == act_copy) {
                             QStringList paths;
                             for (QTreeWidgetItem* item : entry_tree_->selectedItems()) {
                                 if (!item->toolTip(0).isEmpty()) paths << item->toolTip(0);
                             }
                             if (!paths.isEmpty()) {
                                 QGuiApplication::clipboard()->setText(
                                     paths.join(QLatin1Char('\n')));
                             }
                         }
                     });

    refresh_rows();
}

QString FsPage::package_dialog_filter() { return QString::fromUtf8(kPackageDialogFilter); }

void FsPage::fill_format_combo() {    format_combo_->clear();
    for (const efs::FsFormatItem& item : efs::fs_formats()) {
        // 显示名给人看，**取值给机器用**（内部键）—— 1.x 把显示名当取值存，
        // 于是 `ALL_FMTS.get(fmt)` 必然取不到，SquashFS 被静默按 xdb 打包。
        format_combo_->addItem(utf8(item.label), utf8(item.key));
    }
}

std::string FsPage::current_format_key() const {
    return format_combo_->currentData().toString().toUtf8().toStdString();
}

void FsPage::add_source(const QString& source_in,
                        const std::vector<efs::EntryInfo>& entries) {
    // ⑫12.1：加载前先过插件解密器（拒绝时**不加载**，理由给用户看）
    QString gated = source_in;
    QString gate_error;
    if (!gate_source(gated, &gate_error)) {
        log_message(QStringLiteral("拒绝加载（需要解密插件）：%1").arg(gate_error));
        return;
    }
    push_source(gated, entries);
}

void FsPage::push_source(const QString& source,
                         const std::vector<efs::EntryInfo>& entries) {
    // 路径侧的闸门已经在调用方过掉（`add_source` / `add_paths`）—— 这里只动模型与界面
    Source record;
    record.path = source;
    record.key = source.toUtf8().toStdString();
    record.loaded = true;      // 「加包/拖放/散装层」进来的就是已加载（1.x 拖入目录=全部加载）
    sources_.push_back(record);
    entries_.push_back(entries);
    rebuild_package_list();
    rebuild_tree();
}

int FsPage::add_paths(const QStringList& paths) {
    int loaded = 0;
    load_errors_.clear();
    for (const QString& raw : paths) {
        // ① 先过插件解密闸门（⑫12.1）：拒绝就**不加载**，理由进日志
        QString gated = raw;
        QString gate_error;
        if (!gate_source(gated, &gate_error)) {
            const QString why = QStringLiteral("拒绝加载（需要解密插件）：%1 —— %2")
                                    .arg(raw, gate_error);
            log_message(why);
            load_errors_ << why;
            continue;
        }
        // ② 目录 ⇒ 散装层；文件 ⇒ 包（SquashFS 按 magic、DB 按自洽判定，
        //    歧义时用「打包/解包格式」下拉里选的那个，见 package_entries 的说明）
        std::string error;
        std::vector<efs::EntryInfo> entries;
        if (QFileInfo(gated).isDir()) {
            // ★ 用户 2026-09-29 第 6 条：「像 E:\...\IWP 这种 CoP 的文件管理方式，直接把整个文件夹
            //   拖进去没法将 gamedata 文件树正确展开，而是仅仅显示当中的 db 包文件。」
            //   ⇒ 目录不再"只当散装层"：**散装层（松散文件看得见）+ 递归找包并加载（包内容摊开）**，
            //   即 1.x `_on_drop_input` → `_scan_and_load` 的语义。两条源都进同一个合并树，
            //   所以 `gamedata/` 下既能看到松散文件，也能看到各 `.db` 里的文件。
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error);
        } else {
            entries = efs::package_entries(gated.toUtf8().toStdString(),
                                           efs::format_key_to_db(current_format_key()), error);
        }
        if (!error.empty()) {
            // ③ **可见失败**：认不出/解不开都要说清是哪一份、为什么（绝不静默加空包）
            const QString why = QStringLiteral("加载失败：%1 —— %2").arg(raw, utf8(error));
            log_message(why);
            load_errors_ << why;
            continue;
        }
        push_source(gated, entries);
        ++loaded;
        // 目录：再把里面的包**递归扫进包列表**（★ 2026-10-02 口径：**只入列表、不加载**）。
        // ★★ 用户原话：「拖入文件夹的一些逻辑也要修复一下，和单个包文件那种结构的逻辑统一。」
        //   ⇒ **删掉旧的 `kAutoLoadMax = 8` 阈值**（≤8 个包就全自动加载、>8 才只入列表）——
        //   那让"同一个动作"因为目录里有几个包而表现不同。现在的统一口径是：
        //   **用户拖进来的那个东西 = 加载它**（文件＝该包；文件夹＝它的散装层），
        //   **从文件夹里顺带扫出来的包 = 只入列表**，等用户勾选/「加载选中」。
        //   （旧行为见历史：`IWP\database` 57 个包 18.9G 全解会卡死，当时用阈值打补丁；
        //     现在这条规则对任何目录都成立，不再需要阈值。）
        if (QFileInfo(gated).isDir()) {
            const int scanned = scan_dir(gated);
            if (scanned > 0) {
                log_message(QStringLiteral("目录里的 %1 个包已加进包列表（未加载）⇒ 勾选后点「加载选中」")
                                .arg(scanned));
            }
        }
        log_message(QStringLiteral("已加载：%1（%2 项）")
                        .arg(raw)
                        .arg(static_cast<int>(entries.size())));
    }
    return loaded;
}

QString FsPage::source_at(int index) const {
    if (index < 0 || index >= static_cast<int>(sources_.size())) return QString();
    return sources_[static_cast<size_t>(index)].path;
}

void FsPage::check_all(bool checked) {
    tree_.toggle_all(checked);
    refresh_rows();
}

bool FsPage::entry_hint_visible() const {
    // ★ 2026-09-27：空状态提示块按用户要求**已删**（见构造函数注释）⇒ 恒为 false。
    //   访问器保留是因为自检里还有"这条不该再出现"的反向断言（见 smoke.cpp）。
    return false;
}

void FsPage::rebuild_tree() {
    // 合并树 = **已加载的那些源**按 `sources_` 顺序重放一遍（后者为准）。
    // 「取消加载/移除/清空」都走它 —— 不引入"从树里局部删节点"这种第二套逻辑。
    tree_ = efs::MergeTree{};
    loaded_order_.clear();
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source& source = sources_[i];
        if (!source.loaded) continue;
        efs::MergeInput input;
        input.source = source.key;
        input.entries = entries_[i];
        efs::MergeTree incoming = efs::build_tree(input);
        if (tree_.sources.empty()) {
            // `build_tree` 自己就把这个源写进 `sources`（**别再 push 一次**：多算一个会让
            // 下一个源的 `other_source_index` 变成 2，合并出来的节点 source_index 越界、
            // 界面上"这一份来自哪个包"就整列空了 —— 2026-09-28 实测踩过）
            tree_ = incoming;
        } else {
            // `merge` 会把对方的 sources 追加进来，序号 = 当前已加载源数
            tree_.merge(incoming, static_cast<int>(tree_.sources.size()));
        }
        loaded_order_.push_back(i);   // 合并树里的第 k 个源 ←→ sources_ 的第 i 个
    }
    refresh_counts();
    refresh_rows();
}

QString FsPage::source_path_for_index(int source_index) const {
    if (source_index < 0 || source_index >= static_cast<int>(loaded_order_.size())) return QString();
    const size_t row = loaded_order_[static_cast<size_t>(source_index)];
    if (row >= sources_.size()) return QString();
    return sources_[row].path;
}

int FsPage::loaded_index_for_source(size_t row) const {
    for (size_t i = 0; i < loaded_order_.size(); ++i) {
        if (loaded_order_[i] == row) return static_cast<int>(i);
    }
    return -1;
}

void FsPage::refresh_counts() {
    // 包计数（1.x `db_count`：`f"{len} 个"`；2.x 额外把"已加载几个"说出来）
    if (package_count_label_ != nullptr) {
        int loaded = 0;
        for (const Source& source : sources_) {
            if (source.loaded) ++loaded;
        }
        package_count_label_->setText(sources_.empty()
                                          ? QString()
                                          : QStringLiteral("%1 个（已加载 %2）")
                                                .arg(static_cast<int>(sources_.size()))
                                                .arg(loaded));
    }
    // 条目计数（1.x `entry_count`：`f"{nf} 文件"`；查看的是**过滤后**的数）
    // ★ 2026-10-01：旧写法用 `row_matches`（整条路径 contains）算，于是搜 `ai` 时标签写着
    //   "39374 文件"而树上只有几十行 —— 现在**只数真的在筛选集合里的文件**。
    if (entry_count_label_ != nullptr) {
        int files = 0;
        for (const efs::MergeNode* node : tree_.flatten()) {
            if (node->is_dir) continue;
            if (!filter_keep_.isEmpty() && !filter_keep_.contains(utf8(node->path))) continue;
            ++files;
        }
        entry_count_label_->setText(files == 0 ? QString() : QStringLiteral("%1 文件").arg(files));
    }
}

bool FsPage::row_matches(const std::string& path) const {
    // 1.x `_render`：`q = search.lower().replace("\\","/")`，命中即显示
    if (search_edit_ == nullptr) return true;
    const QString query = search_edit_->text().trimmed().toLower().replace(QLatin1Char('\\'),
                                                                          QLatin1Char('/'));
    if (query.isEmpty()) return true;
    return utf8(path).toLower().contains(query);
}

QString FsPage::format_size(uint64_t size) {
    // 照 1.x `_fmt_size`：≥1 MiB ⇒ "N.N MB"；≥1 KiB ⇒ "N.N KB"；否则 "N B"
    if (size >= 1048576) return QStringLiteral("%1 MB").arg(size / 1048576.0, 0, 'f', 1);
    if (size >= 1024) return QStringLiteral("%1 KB").arg(size / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(size);
}

QString FsPage::source_label_for(const efs::MergeNode* node) const {
    if (node == nullptr || node->is_dir) return QString();
    const QString path = source_path_for_index(node->source_index);
    return path.isEmpty() ? QString() : QFileInfo(path).fileName();
}

void FsPage::rebuild_dir_states() {
    // 每个目录在模型里的 (文件数, 已勾数)：勾选后刷三态用。
    // 只算**这一份源胜出**的节点（`source_index` 已在合并时定死），与"包内全选"同口径。
    dir_states_.clear();
    for (const efs::MergeNode* node : tree_.flatten()) {
        if (node->is_dir) continue;
        const QString path = utf8(node->path);
        // ★ 用户 2026-09-30：「一个三层的结构，最里面的文件之一被选中，第二层的文件夹能够正常显示
        //   部分勾选，但是最外面的还是显示未选中。」⇒ 根因：原来只把文件计入**直接父目录**
        //   （`meshes/actors/actor_models/x.ogf` 只记到 `…/actor_models`），于是 `actors/`、`meshes/`
        //   恒为 0/0 ⇒ 显示未选中。改成**沿祖先链逐级累加**：每个文件计入它的**每一级**祖先目录。
        int slash = path.lastIndexOf(QLatin1Char('/'));
        while (slash > 0) {
            const QString dir = path.left(slash);
            QPair<int, int>& state = dir_states_[dir];
            state.first += 1;
            if (node->checked) state.second += 1;
            slash = dir.lastIndexOf(QLatin1Char('/'));
        }
    }
}

int FsPage::entry_top_level_count() const {
    return entry_tree_ != nullptr ? entry_tree_->topLevelItemCount() : 0;
}

int FsPage::entry_built_file_count() const {
    int files = 0;
    if (entry_tree_ == nullptr) return 0;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item == nullptr) return;
        if (item->data(0, Qt::UserRole).toString().isEmpty()) return;   // 占位子项
        // UserRole+1 存的是 **is_dir** ⇒ 只有 false 才是文件（我第一版写反了，把目录当文件数，entry_built_file_count 因此恒为顶层目录数）
        if (!item->data(0, Qt::UserRole + 1).toBool()) files += 1;
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    for (int i = 0; i < entry_tree_->topLevelItemCount(); ++i) walk(entry_tree_->topLevelItem(i));
    return files;
}

QTreeWidgetItem* FsPage::find_entry_item(const QString& path) const {
    if (entry_tree_ == nullptr) return nullptr;
    QTreeWidgetItem* found = nullptr;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
        if (item == nullptr || found != nullptr) return;
        if (item->data(0, Qt::UserRole).toString() == path) {
            found = item;
            return;
        }
        for (int i = 0; i < item->childCount(); ++i) walk(item->child(i));
    };
    for (int i = 0; i < entry_tree_->topLevelItemCount() && found == nullptr; ++i) {
        walk(entry_tree_->topLevelItem(i));
    }
    return found;
}

bool FsPage::expand_entry(const QString& path) {
    QTreeWidgetItem* item = find_entry_item(path);
    if (item == nullptr) return false;
    item->setExpanded(true);
    // 这里**显式**补一次懒建：Qt 的 `itemExpanded` 是**视图**在（用户/视图）真正展开时发的，
    // 程序化 `QTreeWidgetItem::setExpanded()` 只置项自己的标志、不发信号（自检里实测）。
    // 真机上是用户点箭头 ⇒ 走 `itemExpanded`（构造函数里已接）。`populate_entry_children`
    // 幂等（已有真子项就直接返回），所以两条路都走也不会重复建。
    populate_entry_children(item);
    return true;
}

QTreeWidgetItem* FsPage::make_entry_item(const efs::MergeNode& node) {
    auto* item = new QTreeWidgetItem;
    const QString path = utf8(node.path);
    item->setData(0, Qt::UserRole, path);
    item->setData(0, Qt::UserRole + 1, node.is_dir);
    item->setText(0, utf8(node.name) + (node.is_dir ? QStringLiteral("/") : QString()));
    item->setText(1, node.is_dir ? QString() : format_size(node.size));
    item->setText(2, source_label_for(&node));
    item->setToolTip(0, path);
    // ★ 2026-10-01（用户：「我不知道搜索字段和结果有什么直接关系。」）⇒ 命中项**加粗**，
    //   让"打字 → 哪些行是命中的、哪些只是为了撑层级才露出来的祖先"一眼可辨。
    if (matched_paths_.contains(path)) {
        QFont matched_font = item->font(0);
        matched_font.setBold(true);
        item->setFont(0, matched_font);
    }
    if (node.is_dir) {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        // 目录的三态由**模型**决定（见 sync_entry_states），不靠 Qt 的自动三态
        QPair<int, int> state = dir_states_.value(path, {0, 0});
        item->setCheckState(0, state.first == 0 || state.second == 0
                                   ? (state.second > 0 ? Qt::Checked : Qt::Unchecked)
                                   : (state.second == state.first ? Qt::Checked
                                                                  : Qt::PartiallyChecked));
        // 有子节点就放一个占位子项（展开箭头才会出现；真的子项在展开时才建）
        if (!node.children.empty()) {
            auto* placeholder = new QTreeWidgetItem;
            placeholder->setData(0, Qt::UserRole, QString());
            item->addChild(placeholder);
        }
    } else {
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, node.checked ? Qt::Checked : Qt::Unchecked);
    }
    built_items_.insert(path, item);
    return item;
}

void FsPage::populate_entry_children(QTreeWidgetItem* item) {
    if (item == nullptr || syncing_tree_) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    // 已经建过真子项就不再建（占位子项的特征：它的 path 为空）
    if (item->childCount() == 1 && item->child(0)->data(0, Qt::UserRole).toString().isEmpty()) {
        item->removeChild(item->child(0));
    } else if (item->childCount() > 0) {
        return;
    }
    const efs::MergeNode* node = tree_.find(path.toUtf8().toStdString());
    if (node == nullptr) return;
    syncing_tree_ = true;
    for (const efs::MergeNode& child : node->children) {
        if (!entry_visible(child)) continue;   // 搜索过滤（命中项 + 祖先链才显示）
        item->addChild(make_entry_item(child));
    }
    syncing_tree_ = false;
}

void FsPage::sync_entry_states() {
    // 勾选变化后：把**已建出来**的目录/文件项的三态按模型刷一遍（父项跟着变三态）
    syncing_tree_ = true;
    for (auto it = built_items_.begin(); it != built_items_.end(); ++it) {
        QTreeWidgetItem* item = it.value();
        if (item == nullptr) continue;
        const bool is_dir = item->data(0, Qt::UserRole + 1).toBool();
        if (is_dir) {
            const QPair<int, int> state = dir_states_.value(it.key(), {0, 0});
            item->setCheckState(0, state.second == 0 ? Qt::Unchecked
                                                     : (state.second == state.first
                                                            ? Qt::Checked
                                                            : Qt::PartiallyChecked));
        } else {
            const efs::MergeNode* node = tree_.find(it.key().toUtf8().toStdString());
            item->setCheckState(0, node != nullptr && node->checked ? Qt::Checked
                                                                   : Qt::Unchecked);
        }
    }
    syncing_tree_ = false;
}

void FsPage::on_entry_item_clicked(QTreeWidgetItem* item) {
    if (item == nullptr || syncing_tree_) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;   // 占位子项
    const bool is_dir = item->data(0, Qt::UserRole + 1).toBool();
    // 点的是勾选框那一小块吗？是的话交给 Qt（itemChanged 会回灌模型），这里不重复处理
    bool on_indicator = false;
    if (entry_tree_ != nullptr && entry_tree_->viewport() != nullptr) {
        const QPoint vp = entry_tree_->viewport()->mapFromGlobal(QCursor::pos());
        const QRect row = entry_tree_->visualItemRect(item);
        int depth = 0;
        for (QTreeWidgetItem* p = item->parent(); p != nullptr; p = p->parent()) ++depth;
        const int indent = entry_tree_->indentation() * depth;
        const int indicator = entry_tree_->style()->pixelMetric(QStyle::PM_IndicatorWidth) + 8;
        // 只有"光标确实落在这一行里、且落在那小块勾选框上"才算点勾选框；
        // 离屏/自检里光标不在行上 ⇒ 视为点**条目身体**（否则自检没法验这条）
        on_indicator = row.contains(vp) && vp.x() < row.x() + indent + indicator;
    }
    if (is_dir) {
        if (!on_indicator) item->setExpanded(!item->isExpanded());   // 单击文件夹 = 展开/折叠
        return;
    }
    if (on_indicator) return;   // 勾选框自身交给 Qt
    // 文件：**点条目身体**即切换勾选（勾选框只是视觉效果）
    item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
}

void FsPage::on_entry_item_changed(QTreeWidgetItem* item) {
    if (syncing_tree_ || item == nullptr) return;
    const QString path = item->data(0, Qt::UserRole).toString();
    if (path.isEmpty()) return;
    const bool on = item->checkState(0) != Qt::Unchecked;
    // 回灌模型：**文件与目录都走 `toggle_dir`**（目录递归、文件只影响它自己）——
    // 这就是 1.x `_dir_toggle` 的语义；模型改完再按它刷界面三态。
    tree_.toggle_dir(path.toUtf8().toStdString(), on);
    rebuild_dir_states();
    sync_entry_states();
    refresh_counts();
}

bool FsPage::entry_visible(const efs::MergeNode& node) const {
    // 空集合 = 不过滤；否则只显示「命中项 + 祖先链」（层级才留得住）
    return filter_keep_.isEmpty() || filter_keep_.contains(utf8(node.path));
}

void FsPage::rebuild_filter_keep() {
    filter_keep_.clear();
    matched_paths_.clear();
    if (search_edit_ == nullptr) return;
    if (search_edit_->text().trimmed().isEmpty()) return;
    // ★ 2026-10-01 口径改动（用户：「我不知道搜索字段和结果有什么直接关系。」）：
    //   旧实现拿**整条路径**去 contains（`row_matches`），于是 `ai` 会命中 `.../crouch_down_aim.anm`
    //   这种"只是路径里凑巧有这两个字母"的东西，而**目录名本身永远不参与匹配** —— 结果看起来
    //   和输入无关。现在改成：**按"这一项自己的名字"（最后一段）匹配，文件和目录一视同仁**；
    //   命中项进 `matched_paths_`（界面上加粗），再把它的祖先链塞进 `filter_keep_` 撑层级。
    for (const efs::MergeNode* node : tree_.flatten()) {
        if (!name_matches(node->name)) continue;
        matched_paths_.insert(utf8(node->path));
        QString path = utf8(node->path);
        while (!path.isEmpty()) {
            filter_keep_.insert(path);
            const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
            path = slash > 0 ? path.left(slash) : QString();
        }
    }
}

bool FsPage::name_matches(const std::string& name) const {
    if (search_edit_ == nullptr) return true;
    const QString query = search_edit_->text().trimmed().toLower();
    if (query.isEmpty()) return true;
    return utf8(name).toLower().contains(query);
}

void FsPage::refresh_rows() {
    // 重建整棵树：顶层 = 合并树的根的直接子节点；子节点在展开时懒建。
    // 有搜索词时**不懒建**（把匹配路径的整条链一次建出来）—— 过滤后条目通常很少，
    // 这样"搜一下就看得见"；空搜索时保持懒建（大包性能）。
    if (entry_tree_ == nullptr) return;
    syncing_tree_ = true;
    entry_tree_->clear();
    built_items_.clear();
    rebuild_dir_states();
    // 搜索过滤：先把「命中项 + 它们的祖先链」算成一个集合，再按它建树（层级因此保留）
    rebuild_filter_keep();
    if (filter_keep_.isEmpty()) {
        for (const efs::MergeNode& child : tree_.root.children) {
            entry_tree_->addTopLevelItem(make_entry_item(child));
        }
    } else {
        std::function<void(const efs::MergeNode&, QTreeWidgetItem*)> walk =
            [&](const efs::MergeNode& node, QTreeWidgetItem* parent) {
                for (const efs::MergeNode& child : node.children) {
                    if (!entry_visible(child)) continue;
                    QTreeWidgetItem* item = make_entry_item(child);
                    // ★ 筛选是一次性把链建出来 ⇒ 先把"懒建占位子项"摘掉，否则它会留在第一位
                    //   变成用户看到的"文件夹下多出一个空行"（2026-10-01）。
                    drop_placeholder(item);
                    if (parent == nullptr) {
                        entry_tree_->addTopLevelItem(item);
                    } else {
                        parent->addChild(item);
                    }
                    walk(child, item);
                }
            };
        walk(tree_.root, nullptr);
    }
    syncing_tree_ = false;
    // ★★ 2026-10-01（用户把两个标签页摆在一起比："一个正常一个屎"）——**空状态整块收起**：
    //   没条目时把**整个条目面板**（连同它那一行工具行）收掉，高度全给包列表 ⇒ 解包页的空状态
    //   长得跟封包页一模一样：两行控件 + 一个列表填满整个工作区。有货时面板才出现、分隔条才生效。
    //   （上一版只把树收起、工具行留着一行 —— 那会留下一条孤零零的行 + 一块空白，正是他骂的"屎"。）
    const bool has_entries = entry_tree_->topLevelItemCount() > 0;
    entry_tree_->setVisible(true);   // 树本身始终在面板内，由**面板**决定显隐
    if (entry_pane_ != nullptr) entry_pane_->setVisible(has_entries);
    refresh_counts();
}

void FsPage::build_all_descendants(const efs::MergeNode& node, QTreeWidgetItem* item) {
    // 过滤模式：把这一支整棵建出来（含不匹配的祖先之间已匹配的子树）
    drop_placeholder(item);   // 同上：加真子项前先摘占位项，否则第一个位置留一个空行
    for (const efs::MergeNode& child : node.children) {
        if (!entry_visible(child)) continue;
        QTreeWidgetItem* child_item = make_entry_item(child);
        item->addChild(child_item);
        build_all_descendants(child, child_item);
    }
}
QString FsPage::output_dir() const {
    return out_edit_ != nullptr ? out_edit_->text().trimmed() : QString();
}

QString FsPage::resolve_output_dir() {
    // 1.x `_resolve_output`：空则按"第一个包所在目录"推断，并**回填**输入框
    //（界面必须反映真实落盘位置 —— 1.x 的 H1 就是"框里空着，实际落到别处"）。
    QString out = output_dir();
    if (out.isEmpty()) {
        if (!sources_.empty()) {
            out = QFileInfo(sources_.front().path).absolutePath();
        } else {
            out = QDir::toNativeSeparators(QDir::currentPath());
        }
        if (out_edit_ != nullptr) out_edit_->setText(QDir::toNativeSeparators(out));
    }
    return out;
}

QString FsPage::package_folder_name(const QString& package_path) {
    // 1.x `_package_folder`：`os.path.splitext(os.path.basename(p))[0]`
    //   gamedata.db0 → gamedata；gamedata.sq_base → gamedata；a.b.db0 → a.b
    return QFileInfo(package_path).completeBaseName();
}

efs::ExtractTreeResult FsPage::extract_job(const UnpackJob& job) {
    // ★ 这里是本页**唯一**调用引擎导出的地方（门禁静态锁：整个页面里 `extract_package(` 恰好 1 处）
    //   —— 6.11 的"两条路合一"就靠它。`extract_package` 内部按包类型分派（SquashFS / X-Ray DB），
    //   所以"DB 包也要能解"与"只有一条路"能同时成立；前台（自检）与后台（任务壳 worker）
    //   都经过它，不存在第二条实现。
    efs::ExtractTreeOptions options;
    options.selection = job.kind;
    options.only = job.only;
    return efs::extract_package(job.image, job.target, options, job.db_fallback);
}

efs::ExtractTreeResult FsPage::extract_to(const QString& image_path, const QString& out_root,
                                          efs::Selection selection_kind,
                                          const std::vector<std::string>& only) {
    UnpackJob job;
    job.image = image_path.toUtf8().toStdString();
    job.target = out_root.toUtf8().toStdString();
    job.kind = selection_kind;
    job.only = only;
    job.db_fallback = efs::format_key_to_db(current_format_key());
    return extract_job(job);
}

std::vector<FsPage::UnpackJob> FsPage::plan_unpack(bool checked_only, QString* out_root) {
    // 主线程里把"要解哪些、解到哪、用哪个格式兜底"算成纯值（13.2：工作线程只吃这些值）
    std::vector<UnpackJob> jobs;
    const QString root = resolve_output_dir();
    if (out_root != nullptr) *out_root = root;
    const std::optional<efs::DbFormat> fallback = efs::format_key_to_db(current_format_key());
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (!sources_[i].loaded) continue;
        UnpackJob job;
        job.image = sources_[i].path.toUtf8().toStdString();
        job.target =
            QDir(root).filePath(package_folder_name(sources_[i].path)).toUtf8().toStdString();
        job.label = QFileInfo(sources_[i].path).fileName().toUtf8().toStdString();
        job.db_fallback = fallback;
        if (checked_only) {
            const int merged = loaded_index_for_source(i);
            for (const efs::MergeNode* node : tree_.flatten()) {
                if (node->is_dir || !node->checked) continue;
                if (node->source_index != merged) continue;
                job.only.push_back(node->path);
            }
            if (job.only.empty()) continue;   // 这一包没勾任何文件 ⇒ 不排它
        }
        job.kind = checked_only ? efs::Selection::Listed : efs::Selection::All;
        jobs.push_back(job);
    }
    return jobs;
}

QString FsPage::run_unpack_jobs(const std::vector<UnpackJob>& jobs, const ProgressSink& progress,
                                const MessageSink& message) {
    // **纯执行**：不读任何控件、不写任何界面（任务壳的 worker 与自检共用这一条）
    int written = 0;
    int failed = 0;
    QStringList per_package;
    for (size_t i = 0; i < jobs.size(); ++i) {
        const efs::ExtractTreeResult result = FsPage::extract_job(jobs[i]);
        written += static_cast<int>(result.files_written);
        failed += static_cast<int>(result.failures.size());
        const QString line = QStringLiteral("%1→%2（%3 文件%4）")
                                 .arg(utf8(jobs[i].label), utf8(jobs[i].target))
                                 .arg(result.files_written)
                                 .arg(result.ok ? QString()
                                                : QStringLiteral("，失败：") + utf8(result.error));
        per_package << line;
        if (message) message(line.toUtf8().toStdString());
        if (progress && !jobs.empty()) {
            progress(static_cast<int>((i + 1) * 100 / jobs.size()));   // 13.7：只发 0~100
        }
    }
    if (per_package.isEmpty()) {
        return QStringLiteral("没有可解的内容（先「加包」/「扫描目录」并勾选，或先加载包）");
    }
    return QStringLiteral("解包：%1 个文件、失败 %2\n  %3")
        .arg(written)
        .arg(failed)
        .arg(per_package.join(QStringLiteral("\n  ")));
}

void FsPage::start_unpack(bool checked_only) {
    QString root;
    const std::vector<UnpackJob> jobs = plan_unpack(checked_only, &root);
    if (jobs.empty()) {
        log_message(QStringLiteral("解包：没有可解的内容（先「加包」/「扫描目录」并勾选，或先加载包）"));
        return;
    }
    const QString title = checked_only ? QStringLiteral("解包选中") : QStringLiteral("批量解包");
    log_message(QStringLiteral("%1：%2 个包 → %3").arg(title).arg(static_cast<int>(jobs.size())).arg(root));
    if (task_ == nullptr || !task_->run(title, [jobs](const ProgressSink& progress,
                                                      const MessageSink& message) {
            run_unpack_jobs(jobs, progress, message);
        })) {
        log_message(QStringLiteral("有任务在跑，稍后再试"));
    }
}

int FsPage::unpack_selected(QString* summary) {
    QString root;
    const std::vector<UnpackJob> jobs = plan_unpack(true, &root);
    const QString text = run_unpack_jobs(jobs, nullptr, nullptr);
    if (summary != nullptr) *summary = text;
    int written = 0;
    for (const UnpackJob& job : jobs) {
        const efs::ExtractTreeResult result = extract_job(job);
        written += static_cast<int>(result.files_written);
    }
    return written;
}

int FsPage::unpack_all(QString* summary) {
    QString root;
    const std::vector<UnpackJob> jobs = plan_unpack(false, &root);
    const QString text = run_unpack_jobs(jobs, nullptr, nullptr);
    if (summary != nullptr) *summary = text;
    int written = 0;
    for (const UnpackJob& job : jobs) {
        const efs::ExtractTreeResult result = extract_job(job);
        written += static_cast<int>(result.files_written);
    }
    return written;
}

int FsPage::scan_dir(const QString& dir) {
    // 1.x `_scan_dir`：递归找 `_is_db` / `_is_sq` 命中的文件，加进**已知包列表**（不自动加载），
    // 按 basename 小写排序（6.2 的加载序同源）。
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) return 0;
    QStringList found;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (efs::is_package_file_name(path.toUtf8().toStdString())) found << path;
    }
    found.sort(Qt::CaseInsensitive);
    int added = 0;
    for (const QString& path : found) {
        bool known = false;
        for (const Source& source : sources_) {
            if (source.path.compare(path, Qt::CaseInsensitive) == 0) known = true;
        }
        if (known) continue;
        Source record;
        record.path = path;
        record.key = path.toUtf8().toStdString();
        record.loaded = false;      // 扫进来 ≠ 加载；由「加载选中」或勾选决定
        sources_.push_back(record);
        entries_.emplace_back();
        ++added;
    }
    // 列表按 basename 排序显示（与 6.2 的"后者为准"顺序一致，免得显示顺序与合并顺序打架）
    std::vector<size_t> order = efs::sort_sources_by_basename([this] {
        std::vector<std::string> paths;
        for (const Source& source : sources_) paths.push_back(source.key);
        return paths;
    }());
    std::vector<Source> sorted_sources;
    std::vector<std::vector<efs::EntryInfo>> sorted_entries;
    for (size_t index : order) {
        sorted_sources.push_back(sources_[index]);
        sorted_entries.push_back(entries_[index]);
    }
    sources_.swap(sorted_sources);
    entries_.swap(sorted_entries);
    rebuild_package_list();
    rebuild_tree();
    log_message(QStringLiteral("扫描目录：新增 %1 个包，共 %2 个（未加载）")
                    .arg(added)
                    .arg(static_cast<int>(sources_.size())));
    return added;
}

void FsPage::rebuild_package_list() {
    // ★★ 2026-10-02（用户：「**左边的包列表的结构应该也是一个文件树**」）—— 平表 ⇒ **按目录分组的文件树**。
    //   折叠规则见上方 `pkg_build_nodes`（锚点＝公共祖先目录；目录型源取父目录参与比较）。
    //   两条硬口径：
    //   · **纯分组节点**（目录、不对应任何源）：**不可勾选、无大小、无 ✕**（委托按 `UserRole<0` 跳过）；
    //   · **源节点**：可勾选 + 三态 + ✕；**目录型源不显示大小**（别再出现 `database [已加载] 0 B`）。
    //   ★ 源节点把 **`sources_` 下标**存进 `data(0, Qt::UserRole)` —— 树化之后**行号 ≠ 源下标**，
    //     所有"找源"的地方都读这个下标（`pkg_for_each_source` 是配套遍历器）。
    //   ★ 建出来的目录节点**默认展开**（只展开我们自己建的这些，不做 `expandAll`）。
    //   ★ 勾选态变化**不走这里**（用户第 7 条，见 `on_package_item_changed`）。
    QStringList paths;
    for (const Source& source : sources_) paths << source.path;
    const std::vector<PkgTreeNode> roots = pkg_build_nodes(paths);

    rebuilding_package_list_ = true;
    // 保留选中：按**源下标**记（不能再按行号），重建后恢复
    QSet<int> selected_before;
    pkg_for_each_source(package_list_, [&](QTreeWidgetItem* item, int index) {
        if (item->isSelected()) selected_before.insert(index);
    });
    package_list_->clear();
    std::function<void(const PkgTreeNode&, QTreeWidgetItem*)> add_node =
        [&](const PkgTreeNode& node, QTreeWidgetItem* parent) {
            auto* item = new QTreeWidgetItem;
            const QString label = node.is_dir ? node.name + QLatin1Char('/') : node.name;
            item->setText(2, QString());   // 委托在 col2 画 ✕
            item->setData(0, Qt::UserRole, node.source_index);
            const bool is_source = node.source_index >= 0 &&
                                   node.source_index < static_cast<int>(sources_.size());
            if (is_source) {
                const Source& source = sources_[static_cast<size_t>(node.source_index)];
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setText(0, source.loaded
                                     ? QStringLiteral("%1　[已加载]").arg(label)
                                     : label);
                if (!node.is_dir) {
                    const QFileInfo info(source.path);
                    item->setText(1, format_size(static_cast<uint64_t>(info.size())));
                }
                Qt::CheckState check = Qt::Unchecked;
                if (source.loaded) {
                    int files = 0;
                    int checked = 0;
                    tree_.source_check_state(
                        loaded_index_for_source(static_cast<size_t>(node.source_index)), &files,
                        &checked);
                    check = checked == 0 ? Qt::Unchecked
                                         : (checked == files ? Qt::Checked : Qt::PartiallyChecked);
                }
                item->setCheckState(0, check);
            } else {
                // 纯分组节点：只有名字 —— **显式摘掉** `ItemIsUserCheckable`（别指望"没设过就画不出勾选框"），
                // 也没有大小、没有 ✕（委托按 `UserRole<0` 跳过）
                item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
                item->setText(0, label);
            }
            if (parent == nullptr) {
                package_list_->addTopLevelItem(item);
            } else {
                parent->addChild(item);
            }
            for (const PkgTreeNode& child : node.children) add_node(child, item);
            if (item->childCount() > 0) item->setExpanded(true);
        };
    for (const PkgTreeNode& node : roots) add_node(node, nullptr);
    rebuilding_package_list_ = false;
    pkg_for_each_source(package_list_, [&](QTreeWidgetItem* item, int index) {
        if (selected_before.contains(index)) item->setSelected(true);
    });
    refresh_counts();
}

void FsPage::remove_source_at(int source_index) {
    // ★ 第 5 条：行尾 ✕ 与右键菜单「移除选中」**共用这一条**（按 **`sources_` 下标**摘掉一个源；
    //   树化之后这个下标来自节点 `data(0, Qt::UserRole)`，**不是行号**）。
    if (source_index < 0 || source_index >= static_cast<int>(sources_.size())) return;
    selection_.toggle(sources_[static_cast<size_t>(source_index)].key, false);
    sources_.erase(sources_.begin() + source_index);
    entries_.erase(entries_.begin() + source_index);
    rebuild_package_list();
    rebuild_tree();
}

void FsPage::on_package_item_changed(QTreeWidgetItem* item) {
    if (rebuilding_package_list_ || item == nullptr) return;   // 重建列表时不要把状态回灌
    // ★ 2026-10-02 树化：源下标从 `data(0, Qt::UserRole)` 读（**纯分组节点是 -1**，不参与）
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(sources_.size())) return;
    const bool want = item->checkState(0) != Qt::Unchecked;
    if (!sources_[static_cast<size_t>(index)].loaded) {
        // 1.x `_db_toggle`：勾选一个**未加载**的包 ⇒ 先加载，再整包勾上
        if (!want) return;
        item->setSelected(true);
        load_selected();          // 内容变了 ⇒ 这里**允许**重建（load_selected → rebuild_tree）
        // ★ 这一步之后 `item` 已经**悬空**（列表被重建过）⇒ 下面一律只用下标，别再碰 item
        rebuild_package_list();
    }
    const int merged = loaded_index_for_source(static_cast<size_t>(index));
    if (merged < 0) return;
    tree_.toggle_source(merged, want);
    // ★★ 2026-10-01 用户第 7 条（原话）：「每次勾选或取消勾选包列表当中的文件，右边的文件列表
    //   似乎都会重建（因为在文件列表里勾选已展开的母文件夹，不会收起，而操作包，文件列表的
    //   所有文件夹都会收起）。这明明不需要重建，只有包列表内容改变（增加，减少，替换等）
    //   才需要重建右边的文件树。」
    //   ⇒ 这里**不再调 `refresh_rows()`**（那会 clear + 重建整棵树 ⇒ 展开态全丢），
    //     只按模型刷**勾选态**与计数 —— 与 `on_entry_item_changed` 走同一套（三态 + 计数）。
    rebuild_dir_states();
    sync_entry_states();
    refresh_package_checks();
    refresh_counts();
}

void FsPage::refresh_package_checks() {
    // ★ 2026-10-02 树化：勾选态要**递归**刷（源节点散在树里）；分组节点**不碰**（它没有勾选框）。
    rebuilding_package_list_ = true;
    pkg_for_each_source(package_list_, [&](QTreeWidgetItem* item, int index) {
        if (index >= static_cast<int>(sources_.size())) return;
        const bool loaded = sources_[static_cast<size_t>(index)].loaded;
        int files = 0;
        int checked = 0;
        if (loaded) {
            tree_.source_check_state(loaded_index_for_source(static_cast<size_t>(index)), &files,
                                     &checked);
        }
        item->setCheckState(0, checked == 0 ? Qt::Unchecked
                                            : (checked == files ? Qt::Checked
                                                                : Qt::PartiallyChecked));
    });
    rebuilding_package_list_ = false;
}

int FsPage::load_selected() {
    // 1.x `_load_selected`：只加载**选中的**包，勾选态不碰
    int loaded = 0;
    const QList<QTreeWidgetItem*> chosen = package_list_->selectedItems();
    for (QTreeWidgetItem* item : chosen) {
        // ★ 2026-10-02 树化：读节点上的**源下标**（分组节点 -1 ⇒ 自动跳过）
        const int row = item->data(0, Qt::UserRole).toInt();
        if (row < 0 || row >= static_cast<int>(sources_.size())) continue;
        if (sources_[static_cast<size_t>(row)].loaded) continue;
        QString gated = sources_[static_cast<size_t>(row)].path;
        QString gate_error;
        if (!gate_source(gated, &gate_error)) {
            const QString why = QStringLiteral("拒绝加载（需要解密插件）：%1 —— %2")
                                    .arg(sources_[static_cast<size_t>(row)].path, gate_error);
            log_message(why);
            load_errors_ << why;
            continue;
        }
        std::string error;
        std::vector<efs::EntryInfo> entries;
        if (QFileInfo(gated).isDir()) {
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error);
        } else {
            entries = efs::package_entries(gated.toUtf8().toStdString(),
                                           efs::format_key_to_db(current_format_key()), error);
        }
        if (!error.empty()) {
            const QString why = QStringLiteral("加载失败：%1 —— %2")
                                    .arg(sources_[static_cast<size_t>(row)].path, utf8(error));
            log_message(why);
            load_errors_ << why;
            continue;
        }
        sources_[static_cast<size_t>(row)].path = gated;
        sources_[static_cast<size_t>(row)].key = gated.toUtf8().toStdString();
        sources_[static_cast<size_t>(row)].loaded = true;
        entries_[static_cast<size_t>(row)] = std::move(entries);
        ++loaded;
    }
    rebuild_package_list();
    rebuild_tree();
    log_message(QStringLiteral("加载选中：%1 个（共 %2 个已加载）")
                    .arg(loaded)
                    .arg(static_cast<int>(std::count_if(
                        sources_.begin(), sources_.end(),
                        [](const Source& s) { return s.loaded; }))));
    return loaded;
}

int FsPage::unload_selected() {
    // 1.x `_unload_selected`：只改"加载"状态，勾选态跟随原节点（merge 的既有语义）
    int unloaded = 0;
    for (QTreeWidgetItem* item : package_list_->selectedItems()) {
        // ★ 2026-10-02 树化：读节点上的**源下标**（分组节点 -1 ⇒ 自动跳过）
        const int row = item->data(0, Qt::UserRole).toInt();
        if (row < 0 || row >= static_cast<int>(sources_.size())) continue;
        if (!sources_[static_cast<size_t>(row)].loaded) continue;
        sources_[static_cast<size_t>(row)].loaded = false;
        ++unloaded;
    }
    rebuild_package_list();
    rebuild_tree();
    return unloaded;
}

int FsPage::remove_selected() {
    std::vector<size_t> doomed;
    for (QTreeWidgetItem* item : package_list_->selectedItems()) {
        // ★ 2026-10-02 树化：读节点上的**源下标**
        const int row = item->data(0, Qt::UserRole).toInt();
        if (row >= 0 && row < static_cast<int>(sources_.size())) {
            doomed.push_back(static_cast<size_t>(row));
        }
    }
    std::sort(doomed.begin(), doomed.end(), std::greater<size_t>());
    for (size_t row : doomed) {
        selection_.toggle(sources_[row].key, false);
        sources_.erase(sources_.begin() + static_cast<long>(row));
        entries_.erase(entries_.begin() + static_cast<long>(row));
    }
    rebuild_package_list();
    rebuild_tree();
    return static_cast<int>(doomed.size());
}

void FsPage::clear_all() {
    sources_.clear();
    entries_.clear();
    selection_ = efs::FsSelection{};
    package_list_->clear();
    rebuild_tree();   // 空树 + 刷计数/行
}

// ═══ 封包（1.x `_build_pack_panel` / `_pack` 一族） ═══════════════════════

QString FsPage::pack_base_dir() const {
    // 1.x `_pack_root`：设了输入目录就以它为根（相对路径都相对它算）。
    // 2.x 没有"输入行"，就用「解包到」那一格当根（语义相同：产物相对它归一）。
    return output_dir();
}

int FsPage::add_pack_files(const QStringList& paths) {
    std::vector<std::string> roots;
    for (const QString& path : paths) roots.push_back(path.toUtf8().toStdString());
    const efs::PackCollectResult collected =
        efs::collect_pack_files(pack_base_dir().toUtf8().toStdString(), roots);
    int added = 0;
    for (const efs::PackFileItem& item : collected.items) {
        bool known = false;
        for (const efs::PackFileItem& have : pack_items_) {
            if (have.rel == item.rel) known = true;
        }
        if (known) continue;
        pack_items_.push_back(item);
        ++added;
    }
    refresh_pack_list();
    log_message(QStringLiteral("＋文件：%1 个（排除包文件 %2 个，不在根之下 %3 个）")
                    .arg(added)
                    .arg(collected.skipped_package_files)
                    .arg(collected.skipped_outside_base));
    return added;
}

int FsPage::add_pack_dir(const QString& dir) {
    // 1.x `_add_dir`：base = 输入目录；没设就把"这个目录的父目录"当 base，
    // 这样**目录本身**会成为包里的顶层（用户视角：拖进去的那个文件夹还在）。
    QString base = pack_base_dir();
    if (base.isEmpty()) base = QFileInfo(dir).absolutePath();
    const std::vector<std::string> roots{dir.toUtf8().toStdString()};
    const efs::PackCollectResult collected =
        efs::collect_pack_files(base.toUtf8().toStdString(), roots);
    int added = 0;
    for (const efs::PackFileItem& item : collected.items) {
        bool known = false;
        for (const efs::PackFileItem& have : pack_items_) {
            if (have.rel == item.rel) known = true;
        }
        if (known) continue;
        pack_items_.push_back(item);
        ++added;
    }
    refresh_pack_list();
    log_message(QStringLiteral("＋目录：%1 个（排除包文件 %2 个，不在根之下 %3 个）")
                    .arg(added)
                    .arg(collected.skipped_package_files)
                    .arg(collected.skipped_outside_base));
    return added;
}

int FsPage::remove_pack_selected() {
    QSet<QString> doomed;
    for (QListWidgetItem* item : pack_list_->selectedItems()) doomed.insert(item->text());
    if (doomed.isEmpty()) return 0;
    const int before = static_cast<int>(pack_items_.size());
    std::vector<efs::PackFileItem> keep;
    for (const efs::PackFileItem& item : pack_items_) {
        if (doomed.contains(utf8(item.rel))) continue;
        keep.push_back(item);
    }
    pack_items_.swap(keep);
    refresh_pack_list();
    return before - static_cast<int>(pack_items_.size());
}

void FsPage::clear_pack() {
    pack_items_.clear();
    refresh_pack_list();
}

void FsPage::refresh_pack_list() {
    pack_list_->clear();
    for (const efs::PackFileItem& item : pack_items_) pack_list_->addItem(utf8(item.rel));
    if (pack_count_label_ != nullptr) {
        pack_count_label_->setText(pack_items_.empty()
                                       ? QString()
                                       : QStringLiteral("%1 个文件")
                                             .arg(static_cast<int>(pack_items_.size())));
    }
}

QString FsPage::pack_output_basename() const {
    // 1.x `_pack_output_basename`（"输入目录"那一支在 2.x 没有 ⇒ 走后面两支）
    QStringList tops;
    for (const efs::PackFileItem& item : pack_items_) {
        const QString top = utf8(item.rel).section(QLatin1Char('/'), 0, 0);
        if (!top.isEmpty() && !tops.contains(top)) tops << top;
    }
    if (tops.size() != 1) return QString();
    const QString top = tops.front();
    if (pack_items_.size() == 1 && !utf8(pack_items_.front().rel).contains(QLatin1Char('/'))) {
        return QFileInfo(top).completeBaseName();   // 单文件：去掉原扩展名
    }
    return top;                                      // 目录：保留目录名
}

QString FsPage::pack_ext_for(const QString& key) {
    // 1.x `_pack_ext`：sqfs ⇒ `.sqfs`；其余 ⇒ `.<内部键>.db`（六种格式产物不同名，
    // 否则用户在资源管理器里分不出打的是哪种格式）
    if (efs::format_key_is_sqfs(key.toUtf8().toStdString())) return QStringLiteral(".sqfs");
    return QStringLiteral(".") + key + QStringLiteral(".db");
}

QString FsPage::resolve_pack_target() {
    const std::string key = pack_format_combo_->currentData().toString().toUtf8().toStdString();
    const QString ext = pack_ext_for(utf8(key));
    const QString base = pack_output_basename();
    QString out = pack_out_edit_->text().trimmed();
    if (out.isEmpty()) {
        const QString initial =
            base.isEmpty() ? QStringLiteral("packed") + ext : base + ext;
        out = QFileDialog::getSaveFileName(this, QStringLiteral("封包保存到"), initial,
                                           QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
        if (out.isEmpty()) return QString();
    } else if (QFileInfo(out).isDir()) {
        if (!base.isEmpty()) {
            out = QDir(out).filePath(base + ext);
        } else {
            out = QFileDialog::getSaveFileName(this, QStringLiteral("封包保存到"),
                                               QDir(out).filePath(QStringLiteral("packed") + ext),
                                               QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
            if (out.isEmpty()) return QString();
        }
    } else if (QFileInfo(out).suffix().isEmpty()) {
        out += ext;
    }
    pack_out_edit_->setText(QDir::toNativeSeparators(out));   // 1.x H1：最终路径必须回填
    return out;
}

FsPage::PackJob FsPage::plan_pack(bool* ok, QString* why) {
    // 主线程：算输出路径（可能要弹保存框）+ 快照待打包列表与格式键 ⇒ 交给工作线程的只有值
    PackJob job;
    if (pack_items_.empty()) {
        if (ok != nullptr) *ok = false;
        if (why != nullptr) *why = QStringLiteral("封包：列表是空的（先「＋文件」或「＋目录」）");
        return job;
    }
    const QString out = resolve_pack_target();
    if (out.isEmpty()) {
        if (ok != nullptr) *ok = false;
        if (why != nullptr) *why = QStringLiteral("封包：没选输出文件，已取消");
        return job;
    }
    job.out = out.toUtf8().toStdString();
    job.key = pack_format_combo_->currentData().toString().toUtf8().toStdString();
    job.items = pack_items_;
    if (ok != nullptr) *ok = true;
    return job;
}

QString FsPage::run_pack_job(const PackJob& job, const ProgressSink& progress,
                             const MessageSink& message) {
    // **纯执行**（工作线程里不碰控件）：sqfs ⇒ pack_squashfs；其余 ⇒ pack_db + 原子写
    const auto say = [&message](const QString& text) {
        if (message) message(text.toUtf8().toStdString());
    };
    if (progress) progress(0);
    if (efs::format_key_is_sqfs(job.key)) {
        std::vector<efs::PackItem> items;
        items.reserve(job.items.size());
        for (const efs::PackFileItem& item : job.items) {
            efs::PackItem pack_item;
            pack_item.path = item.rel;
            pack_item.is_dir = false;
            pack_item.source_path = item.source_path;
            items.push_back(pack_item);
        }
        const efs::PackResult result = efs::pack_squashfs(job.out, items);
        if (progress) progress(100);
        if (!result.ok) return QStringLiteral("封包失败（SquashFS）：%1").arg(utf8(result.error));
        const QString text = QStringLiteral("封包完成（SquashFS）：%1 个文件、%2 字节 → %3")
                                 .arg(static_cast<int>(items.size()))
                                 .arg(result.file_size)
                                 .arg(utf8(job.out));
        say(text);
        return text;
    }
    const std::optional<efs::DbFormat> fmt = efs::format_key_to_db(job.key);
    if (!fmt.has_value()) {
        return QStringLiteral("封包：格式键 %1 既不是 DB 也不是 SquashFS").arg(utf8(job.key));
    }
    std::vector<efs::DbPackItem> items;
    items.reserve(job.items.size());
    for (const efs::PackFileItem& item : job.items) {
        std::ifstream in(core::path_from_utf8(item.source_path), std::ios::binary);
        if (!in) return QStringLiteral("封包失败：读不出 %1").arg(utf8(item.source_path));
        efs::DbPackItem pack_item;
        pack_item.path = item.rel;
        pack_item.is_dir = false;
        pack_item.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        items.push_back(std::move(pack_item));
    }
    const std::vector<uint8_t> raw = efs::pack_db(items, *fmt);
    if (progress) progress(70);
    std::string write_error;
    if (!core::write_file_atomic(job.out, raw, true, write_error)) {
        return QStringLiteral("封包失败（写盘）：%1").arg(utf8(write_error));
    }
    if (progress) progress(100);
    const QString text = QStringLiteral("封包完成（%1）：%2 个文件、%3 字节 → %4")
                             .arg(utf8(efs::db_format_key(*fmt)))
                             .arg(static_cast<int>(items.size()))
                             .arg(raw.size())
                             .arg(utf8(job.out));
    say(text);
    return text;
}

bool FsPage::run_pack(QString* summary) {
    bool ok = false;
    QString why;
    const PackJob job = plan_pack(&ok, &why);
    if (!ok) {
        if (summary != nullptr) *summary = why;
        return false;
    }
    const QString text = run_pack_job(job, nullptr, nullptr);
    if (summary != nullptr) *summary = text;
    return !text.startsWith(QStringLiteral("封包失败")) &&
           !text.startsWith(QStringLiteral("封包："));
}

void FsPage::start_pack() {
    bool ok = false;
    QString why;
    const PackJob job = plan_pack(&ok, &why);
    if (!ok) {
        log_message(why);
        return;
    }
    const QString out = utf8(job.out);
    log_message(QStringLiteral("封包（%1）：%2 个文件 → %3")
                    .arg(utf8(job.key))
                    .arg(static_cast<int>(job.items.size()))
                    .arg(out));
    if (task_ == nullptr || !task_->run(QStringLiteral("封包"),
                                        [job](const ProgressSink& progress, const MessageSink& message) {
                                            run_pack_job(job, progress, message);
                                        })) {
        log_message(QStringLiteral("有任务在跑，稍后再试"));
    }
}

}  // namespace xrkit::shell
