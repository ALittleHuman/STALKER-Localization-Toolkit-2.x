#include "fs_page.h"

#include "xrkit/core/theme.h"
#include "xrkit/core/atomic_write.h"
#include "xrkit/core/path.h"
#include "xrkit/engine/fs/squashfs.h"

#include "../drop_filter.h"
#include "../pair_tree.h"
#include "../row_action_delegate.h"


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
// ★★ 2026-10-06（审计 3-3）：判重用的是引擎现成的 `core::path_equals`
//   （lexically_normal + 大小写不敏感 + 正反斜杠归一）—— 别在这里自己抄一份。
namespace core = xrkit::core;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// ★ 用户第 3 条原话：「并且扫描时有Bug的，选择包模式的时候，是扫不出金球奖的db的。这是个Bug。
//   不要想着增加后缀的样式（.db0，.db_base等），因为这是遍历不完的。然后点击"所有文件"才能看见那些包。」
//   ⇒ 过滤器用**家族通配**一次覆盖所有分卷与自定义后缀（老写法把 `*.db0 *.db1 …` 逐个列出来 ⇒ 必然漏）。
// ★★ 2026-10-04 补 `*.xdb*`：用户问「`.xdb`，我们的工具能识别吗？」⇒ 旧过滤器 `*.db*` 要求字面 `.db`
//   ⇒ **"选包"对话框里看不见 `.xdb`**（实测 `sop_standalone.7z` 里的 `patch_00.xdb` 就是 `format=xdb`）。
constexpr const char* kPackageDialogFilter = "包 (*.db* *.sq* *.xdb*);;所有文件 (*.*)";

// ★★ 2026-10-02 用户：「为什么包树和文件树的形式不同。文件树的形式已经相当成熟，
//   包树直接引用树的统一函数就行啊。」⇒ 两棵树的**装配**与**单击判据**各抽成一份：
//   · `apply_pair_tree_spec` ＝ 列/行高/多选/拖放/最小高/**根装饰**/**单击才展开** 的唯一一份；
//   · `click_on_indicator` ＝ "这一下点没点在那个小方块（勾选框/展开箭头）上"的唯一一份。
//   两树**唯一**保留的差异在**叶子**：条目树的叶子＝点身体切勾选；包树的叶子＝点身体只选中
//   （不顺手改"加载"状态 —— 那会把"点一下"变成一次加载/卸载，属于语义，不是观感）。
//
// ★ 2026-10-06（D2 批）：这两件**搬到了 `src/shell/pair_tree.h`**（XML 校对页的结果表也要用，
//   用户口径「同类东西只维护一套组件」）。这里的注释保留，实现不再留在本文件里。

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
    // 源现在**全是文件**（2026-10-03 起"文件夹＝散装层源"已作废）⇒ 一律取所在目录参与比较。
    // （旧口径下目录型源取它的**父目录**，好让它自己成为树里一个节点 —— 那条已不需要。）
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
    // ★★ 2026-10-03（新口径：源**全是文件**）—— 锚点规则两条，正好满足两个相反的判据：
    //   · 所有源**都直接躺在 anchor 里**（没有源落在更深的子目录）⇒ **anchor 自己不画**，
    //     文件就是根节点 ⇒ 保住"只拖一个包时，树里就一个根节点、**不画**它的祖先目录"。
    //   · 否则（有源落在子目录里）⇒ **anchor 自己成为一个分组节点**，源挂在它下面 ⇒
    //     拖 `IWP\database`（57 个包分布在 config/ maps/ mods/ patch/ resource/ sound/）时
    //     根＝`database/`（**分组**节点：`src=-1`、可勾选三态、不显大小、无 ✕）。
    bool all_direct = true;
    for (const QString& parent : parents) {
        if (!anchor.isEmpty() && parent.compare(anchor, Qt::CaseInsensitive) != 0) {
            all_direct = false;
            break;
        }
    }
    // 基准目录：**都在同一层** ⇒ 以公共目录为基准 ⇒ 文件直接作根（不画那个目录）；
    // 否则 ⇒ 以公共目录的**父目录**为基准 ⇒ 公共目录自己成为根节点（分组）。
    const QString base =
        all_direct ? anchor : QFileInfo(anchor).absolutePath();
    std::vector<PkgTreeNode> roots;
    for (int i = 0; i < paths.size(); ++i) {
        QString rel = base.isEmpty()
                          ? QFileInfo(paths[i]).fileName()
                          : QDir(base).relativeFilePath(QDir::fromNativeSeparators(paths[i]));
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
    // ★★ P17-120（复审计 `P2-5`）：读**结构化标志**。修前任何非空 `error` 都被当硬失败，
    //   而“到上限了”当时也写在 `error` 里 ⇒ 超深/超多目录**整份拒载**，与意图相反。
    //   现在“截断”与“没读成”**分开**：截断=加进去＋警告；真错=拒绝加载。
    //   同一个标志，CLI 那边读的也是它（不再各自猜中文）。
    engine::fs::LooseInfo loose_info;
    const auto entries = engine::fs::loose_entries(dir.toUtf8().toStdString(), err, &loose_info);
    if (!err.empty()) {
        // 不存在的目录/读不动 ⇒ **可见失败**（不静默加一个空源）
        if (error != nullptr) *error = QString::fromStdString(err);
        log_message(QStringLiteral("散装层拒绝加载：%1").arg(QString::fromStdString(err)));
        return false;
    }
    if (loose_info.truncated) {
        // 截断**不是失败**：照样加载，但必须**可见**（零静默）
        log_message(QStringLiteral("散装层条目表被截断（仍加载）：%1")
                        .arg(QString::fromStdString(loose_info.truncated_reason)));
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
    // ★ 2026-10-04：解密产物落点改为**引擎那一份口径**（`plugin::decrypt_output_dir()`）。
    //   闸门现在在引擎读入口也跑（CLI 同样走它），落点若由外壳自己算（原来这里是
    //   `QDir::temp()`），两边就可能不是同一个目录 ⇒ 引擎那道闸门认不出"这是产物"、
    //   会二次解密。GUI 看到的目录不变（两边都是系统临时目录下的 `xrkit_decrypted`）。
    decrypt_dir_ = QString::fromStdString(plugin::decrypt_output_dir());
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
    // ★★ 2026-10-02 用户：「为什么包树和文件树的形式不同。文件树的形式已经相当成熟，包树直接引用
    //   树的统一函数就行啊。」⇒ 装配改成**调同一个 `apply_pair_tree_spec`**（列/行高/多选/拖放/
    //   最小高/根装饰/单击才展开 只有一份），两树的差异只剩"各是哪三列、哪一列吃剩余宽度"。
    //   列宽来由（用户 2026-10-02 圈着表头打「？」那条）：三列若都 Interactive 且没有一列吃剩余宽度
    //   ⇒ 总宽 ≪ 面板宽 ⇒ 表头/每行右半边是死区 ⇒ **名称列吃满、后两列钉右**。
    apply_pair_tree_spec(package_list_, PairTreeSpec{
                                            {QStringLiteral("包"), QStringLiteral("大小"), QString()},
                                            {300, 90, 48},
                                            120,
                                            true});
    // 行尾 ✕（第 5 条）：委托画 + 自己处理点击 ⇒ 删掉工具行里的「移除」按钮
    //   ★ 2026-10-02 树化之后回调给**节点**，源下标从 `data(0, Qt::UserRole)` 读（分组节点 -1 ⇒ 不画）
    // ★★ P13-1：换成**共用**的行尾委托（`row_action_delegate.h`；与门面树同一份）。
    //   两条口径照原样：可见性＝第 0 列 `UserRole` 的源下标 ≥ 0；
    //   ★ 摘源**必须排队到下一轮**（不能在这条 `editorEvent` 里重建包树 —— 用户报过的崩溃）。
    package_list_->setItemDelegateForColumn(
        2, new RowActionDelegate(
               2, "\u2715",
               [](const QModelIndex& index) {
                   if (!index.isValid()) return false;
                   return index.sibling(index.row(), 0).data(Qt::UserRole).toInt() >= 0;
               },
               [this](const QModelIndex& index) {
                   if (!index.isValid()) return;
                   const int source_index =
                       index.sibling(index.row(), 0).data(Qt::UserRole).toInt();
                   if (source_index < 0) return;
                   QMetaObject::invokeMethod(
                       this, [this, source_index] { remove_source_at(source_index); },
                       Qt::QueuedConnection);
               }));
    // ★ 与条目树**同一条路**（用户：「包树直接引用树的统一函数就行啊」）：单击节点身体＝展开/折叠。
    QObject::connect(package_list_, &QTreeWidget::itemClicked, this,
                     [this](QTreeWidgetItem* item, int) { on_package_item_clicked(item); });

    // ── 中：格式下拉（6.13：取值 = 内部键）+ 条目列表（虚拟化）
    format_combo_ = new QComboBox;
    format_combo_->setObjectName(QStringLiteral("fs_format_combo"));
    fill_format_combo();
    // ★ 2026-10-06（④）：格式说明标签（1.x `fs_app.py:207` `self.fmt_desc` +
    //   `_on_fmt_change`：auto ⇒ "自动检测"，其余 ⇒ 该格式名）。
    format_hint_ = new QLabel;
    format_hint_->setObjectName(QStringLiteral("fs_format_hint"));
    QObject::connect(format_combo_, &QComboBox::currentIndexChanged, this,
                     [this](int) { format_hint_->setText(format_hint_text()); });
    format_hint_->setText(format_hint_text());

    // ★ 真树（1.x CanvasTree 的对位件）：三列 = 名称（带三态勾选框）/ 大小 / 来源包；
    //   子节点**懒建**（展开时才建），所以几万条目的大包打开也不慢。
    entry_tree_ = new QTreeWidget;
    entry_tree_->setObjectName(QStringLiteral("fs_entry_tree"));
    // ★★ 与包列表**同一个装配函数**（用户 2026-10-02：「包树直接引用树的统一函数就行啊」）——
    //   既然要对齐"已经打磨过的那棵"，就把两边的列策略也统一成同一条规则：
    //   **名称列吃剩余宽度、后两列 Interactive 钉右**（用户 09-29 要的"列之间能拖"不丢）。
    //   这条同时消掉条目树的老毛病：名称列固定 340 被截、而"来源"列（短字符串）却吃着剩余宽度。
    apply_pair_tree_spec(entry_tree_, PairTreeSpec{
                                         {QStringLiteral("包内条目"), QStringLiteral("大小"),
                                          QStringLiteral("来源")},
                                         {340, 110, 220},
                                         160,
                                         false});
    // 勾选/展开都回灌模型（见 on_entry_item_changed / populate_entry_children）
    QObject::connect(entry_tree_, &QTreeWidget::itemChanged, this,
                     [this](QTreeWidgetItem* item, int) { on_entry_item_changed(item); });
    QObject::connect(entry_tree_, &QTreeWidget::itemExpanded, this,
                     [this](QTreeWidgetItem* item) { populate_entry_children(item); });
    // ★ 用户 2026-09-29 第 5 条：「文件夹要双击才能打开，改成单击。然后单个文件要点框才能勾选，
    //   改成点击条目就能勾选（那个勾选框只是视觉效果，没有额外判定）」。
    //   `apply_pair_tree_spec` 里已经 `setExpandsOnDoubleClick(false)`；这里接"单击"这一条路：
    //   文件夹单击即展开/折叠；文件**点条目身体**即切勾选（点勾选框本身走 itemChanged，两条路都回灌模型）。
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
    auto* row_holder = new QWidget;
    auto* row = new QHBoxLayout(row_holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(new QLabel(QStringLiteral("打包/解包格式：")), 0);
    row->addWidget(format_combo_, 0);
    row->addWidget(format_hint_, 0);   // ④：格式说明（auto ⇒ 自动检测）
    row->addSpacing(xrkit::core::px(12));
    row->addWidget(new QLabel(QStringLiteral("解包到：")), 0);
    out_edit_ = new QLineEdit;
    out_edit_->setObjectName(QStringLiteral("fs_output_dir"));
    // ★ 2026-10-04：输入框自己**不接**拖放 —— 接住它的是**整行区域**（`drop_zones_`）。
    out_edit_->setPlaceholderText(QStringLiteral("输出目录（留空＝与包同目录）"));
    row->addWidget(out_edit_, 1);
    auto* browse_out = new QPushButton(QStringLiteral("选择…"));
    browse_out->setObjectName(QStringLiteral("fs_browse_output"));
    row->addWidget(browse_out, 0);
    open_out_button_ = new QPushButton(QStringLiteral("打开"));
    open_out_button_->setObjectName(QStringLiteral("fs_open_output"));
    row->addWidget(open_out_button_, 0);
    // ★ 2026-10-04 用户第 1 条：「所有目录都需要拖拽，而且是**拖拽到相应区域**。」
    //   整行成容器 ⇒ 丢在标签/按钮/空白处都接住（丢进来的路径填「解包到」那一格）。
    drop_zones_.attach_row(row_holder, this);
    unpack_layout_->addWidget(row_holder);
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

        auto* pack_row_holder = new QWidget;
        auto* row = new QHBoxLayout(pack_row_holder);
        row->setContentsMargins(0, 0, 0, 0);
        row->addWidget(new QLabel(QStringLiteral("封包格式：")), 0);
        pack_format_combo_ = new QComboBox;
        pack_format_combo_->setObjectName(QStringLiteral("fs_pack_format"));
        for (const efs::FsFormatItem& item : efs::fs_formats()) {
            // 1.x 的封包下拉 = 非 auto 的全部格式（六种 DB + SquashFS）；取值仍是**内部键**。
            // ★ 2026-10-06（④）：`fs_formats()` 现在第一位是 auto（1.x `fs_app.py:196`
            //   `pack_fmt_keys = [k for k in fmt_keys if k != "auto"]`）⇒ 这里按 **is_auto 标志**
            //   跳过，而不是再手写一次字符串比较（判"是不是 auto"只有引擎那一个谓词）。
            if (item.is_auto) continue;
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
        // ★ 2026-10-04 用户第 1 条：整行成容器 ⇒ 丢进来的路径填「封包输出」那一格
        drop_zones_.attach_row(pack_row_holder, this);
        pack_page_layout_->addWidget(pack_row_holder);

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
            // ★ 2026-10-04 用户第 2 条：起点＝①上次的选择 → ②唯一输入目录 → ③工具自身所在文件夹
            const QStringList picked = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择要打进去的文件"),
                resolve_dialog_start(pack_add_files_dialog_start()),
                QStringLiteral("所有文件 (*.*)"));
            if (!picked.isEmpty()) add_pack_files(picked);
        });
        QObject::connect(add_dir, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(
                this, QStringLiteral("选择要打进去的目录（递归，自动排除包文件）"),
                resolve_dialog_start(pack_add_files_dialog_start()));
            if (!dir.isEmpty()) add_pack_dir(dir);
        });
        QObject::connect(rm, &QPushButton::clicked, this, [this] { remove_pack_selected(); });
        QObject::connect(clr, &QPushButton::clicked, this, [this] { clear_pack(); });
        QObject::connect(browse_pack, &QPushButton::clicked, this, [this] {
            // 1.x 走 `_resolve_pack_target` 的保存框分支；这里显式选一次（选完回填）
            const QString picked = QFileDialog::getSaveFileName(
                this, QStringLiteral("封包输出文件"),
                resolve_dialog_start(pack_output_dialog_start()),
                QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
            if (!picked.isEmpty()) pack_out_edit_->setText(QDir::toNativeSeparators(picked));
        });
        QObject::connect(pack_button_, &QPushButton::clicked, this, [this] { start_pack(); });
    }

    // ── 任务壳（13.1/13.2）：解包/封包在**后台线程**跑，界面不冻（1.x TaskRunner 同族；
    //    2.x 的 convert/text 页也是这么接的 —— 用户要的「方便」里包含「大包不卡界面」）。
    // ★★ 2026-10-06（② 用户拍板）：「封包和字库的取消，在这里本质是完全没有的，故直接删除即可。」
    //   ⇒ 本页按 `CancelButton::No` 造任务壳：**连取消按钮都不创建**。
    //   事实依据：封包是一次原子写（`write_file_atomic`），硬停只会留下半个包；解包也没法
    //   在一份包的中途停干净 ⇒ 这颗按钮在本页"本质是完全没有的"（用户口径）。
    //   其余三页（编码转换 / 文本提取 / XML 校对）的取消照旧保留。
    task_ = new TaskShell(this, TaskShell::CancelButton::No);
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
                this, QStringLiteral("选择要加载的文件夹"),
                resolve_dialog_start(load_package_dialog_start()));
            if (!dir.isEmpty()) add_paths({dir});   // 目录 ⇒ 散装层 + 递归找包并加载
            return;
        }
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("选择要加载的包（可多选）"),
            resolve_dialog_start(load_package_dialog_start()),
            QString::fromUtf8(kPackageDialogFilter));
        if (!picked.isEmpty()) add_paths(picked);
    });
    QObject::connect(browse_out, &QPushButton::clicked, this, [this] {
        // ★ 2026-10-04 用户第 2 条（原话）：「比如**解包的目标目录**，点击"选择"，默认定向到
        //   工具所在的目录。**改成定向到输入的目录**。」⇒ 起点＝来源包**所在目录**（唯一才用），
        //   空/多元 ⇒ 工具自身所在文件夹。
        const QString picked = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择解包输出目录"),
            resolve_dialog_start(unpack_output_dialog_start()));
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
                         // ★★ 2026-10-06（①）：记下**真右键路径**上弹出的这个菜单（自检要看到它）
                         last_package_context_menu_ = &menu;
                         QAction* act_load = menu.addAction(QStringLiteral("加载选中"));
                         QAction* act_unload = menu.addAction(QStringLiteral("取消加载"));
                         menu.addSeparator();
                         QAction* act_select_all = menu.addAction(QStringLiteral("全选"));
                         menu.addSeparator();
                         QAction* act_remove = menu.addAction(QStringLiteral("移除选中"));
                         QAction* act_clear = menu.addAction(QStringLiteral("清空"));
                         // ★★ 2026-10-06（①）：`context` 槽 —— 插件注册的右键命令追加在
                         //   **原有项之后**（「原有项 → 分隔符 → 插件项」，1.x
                         //   `attach_plugin_context_menu` / `PLUGINS.md` §3.3）。
                         //   context **每次弹菜单现算**（`plugin_context()`）⇒ `when` 不是
                         //   构造时冻结的死条件（1.x `fs_app.py:314-349` 同口径）。
                         append_plugin_context_items(&menu, plugin_context());
                         QAction* chosen =
                             menu.exec(package_list_->viewport()->mapToGlobal(pos));
                         last_package_context_menu_ = nullptr;
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

// ★★ 2026-10-06（①）：右键菜单（`context` 槽）的插件 context —— 照 1.x `fs_app.py:339`
// 传的就是 `{"loaded": bool(self.loaded)}`（"有没有已加载的包"），每次弹菜单现算。
// C ABI 的 `when` 值是字符串（见 `abi.h` 的 `stk_when`）⇒ 这里写 `"1"` / `"0"`：
// 插件侧 `when={"loaded": "1"}` 就是 1.x 的 `{"loaded": True}`。
plugin::Context FsPage::plugin_context() const {
    plugin::Context context;
    context["loaded"] = std::string(loaded_source_count() > 0 ? "1" : "0");
    return context;
}

void FsPage::fill_format_combo() {    format_combo_->clear();
    for (const efs::FsFormatItem& item : efs::fs_formats()) {
        // 显示名给人看，**取值给机器用**（内部键）—— 1.x 把显示名当取值存，
        // 于是 `ALL_FMTS.get(fmt)` 必然取不到，SquashFS 被静默按 xdb 打包。
        // ★ 2026-10-06（④）：`fs_formats()` 的第一项就是 auto（1.x `fs_app.py:154` 的默认值）
        //   ⇒ auto 自动落在**第一位**，无需在这里特殊处理。
        format_combo_->addItem(utf8(item.label), utf8(item.key));
    }
}

QString FsPage::format_hint_text() const {
    // 1.x `_on_fmt_change`：auto ⇒ "自动检测"；其余 ⇒ 该格式的显示名。
    if (format_combo_ == nullptr) return QString();
    const std::string key = format_combo_->currentData().toString().toStdString();
    if (efs::format_key_is_auto(key)) {
        return QStringLiteral("自动检测（不给格式：按包自洽判定；歧义时显式拒绝）");
    }
    return format_combo_->currentText();
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
    // ★★ 2026-10-06（审计 3-3）：**判重只此一份**（`core::path_equals`）。理由见
    //   `add_paths` 里那段：整串比较认不出 `E:\a\x` 与 `E:/a/x` 是同一份 ⇒ 同层同名
    //   覆盖 ⇒ 先来的那个源在左树里**一个节点都没有**、计数却翻倍。
    //   ⚠ 这里的判重**不能**用"文件名"（同名的两个包在不同目录是两份源，各有各的 ✕）；
    //     比的是**整条归一化路径**。命中 ⇒ 用这次的条目刷新那一份（同路径 = 同一份）。
    // ★ 为什么先算下标、最后只调**一次** `apply_load_order()`：两条分支各写一次调用，
    //   静态判据就只能数"≥1"（少调一处也照样绿 —— 本批实测踩到过）。收成一次调用之后
    //   判据可以写成"**恰好一次**"，那才是真的锁。
    bool replaced = false;
    for (size_t i = 0; i < sources_.size(); ++i) {
        if (!core::path_equals(sources_[i].path.toUtf8().toStdString(),
                               source.toUtf8().toStdString())) {
            continue;
        }
        sources_[i].loaded = true;
        if (i < entries_.size()) entries_[i] = entries;
        replaced = true;
        break;
    }
    if (!replaced) {
        Source record;
        record.path = source;
        record.key = source.toUtf8().toStdString();
        record.loaded = true;  // 「加包/拖放/散装层」进来的就是已加载（1.x 拖入目录=全部加载）
        sources_.push_back(record);
        entries_.push_back(entries);
    }
    apply_load_order();        // ★★ 见下方（审计 3-4：加包路径也必须按 basename 升序）
    rebuild_package_list();
    rebuild_tree();
}

// ★★ 2026-10-06（审计 3-4）：**加载序只有一份实现**，而且**每条加源路径都要走它**。
//
// 审计原文：「加载序只有 `scan_dir` 会排 ⇒ "6.2 字典序靠后者赢"在加包路径不成立」
//   （`fs_page.cpp:845-856/1634-1646`）。修前 `sort_sources_by_basename` **只在
//   `scan_dir` 里调**：`push_source`（加包/拖放/散装层）只 `push_back` ⇒ 后加的那个
//   永远赢，哪怕它的 basename 字典序在前。两套顺序就是两套行为（同一份包列表，走
//   "扫描目录"与走"加包"合并出来的赢家不一样）。
// 反向副作用（审计同时点名的）：调一次 `scan_dir` 会**重排全部已加载源** ⇒ 用户没动
//   那些包，合并赢家/来源列却变了。修后**每条路径都排** ⇒ 顺序只由 basename 决定，
//   "用户怎么进来的"不再影响合并结果（`scan_dir` 的重排也就不再是"意外"）。
//
// 实现要点：`sources_` 与 `entries_` **必须成对搬运**（下标一一对应，`loaded_order_`
// 也是按下标记的）；排序返回的是**旧下标的新顺序**。
void FsPage::apply_load_order() {
    if (sources_.size() < 2) return;
    std::vector<std::string> keys;
    keys.reserve(sources_.size());
    for (const Source& source : sources_) keys.push_back(source.key);
    const std::vector<size_t> order = efs::sort_sources_by_basename(keys);
    if (order.size() != sources_.size()) return;   // 防御：实现不配套就不动（别把表搬歪）
    std::vector<Source> sorted_sources;
    std::vector<std::vector<efs::EntryInfo>> sorted_entries;
    sorted_sources.reserve(sources_.size());
    sorted_entries.reserve(entries_.size());
    for (size_t index : order) {
        if (index >= sources_.size() || index >= entries_.size()) return;
        sorted_sources.push_back(sources_[index]);
        sorted_entries.push_back(entries_[index]);
    }
    sources_.swap(sorted_sources);
    entries_.swap(sorted_entries);
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
        // ★★ 2026-10-03 口径（用户原话，最高优先级）：「这里母文件夹的"已加载"指的是**文件夹**已加载
        //   而不是**包的内容**已加载。怎么能这样呢？**右边永远不显示包体，除非包体本身在左边的包内，
        //   而不是文件夹内。**」＋「**这是一个解包的工具，而右边罗列的是包的内容，换句话说就是
        //   解包产物的预览。**」⇒ 右栏＝**解包产物预览**：只含**已加载的包**的内容；**文件夹不贡献任何条目**。
        //   ⇒ 目录分支只剩一句：把里面的包**列进列表**（未加载）；目录自己**不成源**
        //     （不进右栏、不显示"已加载"、不计入 `source_count()`）。
        //   ★ 这**作废**了 2026-09-29 的旧口径"拖进来的目录＝散装层源"（那时目录自己是一个源，
        //     于是右栏既出现包的内容、又出现"包文件本身"，逻辑关系错乱）。
        if (QFileInfo(gated).isDir()) {
            const int scanned = scan_dir(gated);
            // ★★ 2026-10-03 用户口径（原话）：「勾文件夹要不要顺带加载**肯定要加载**。**只要拖入包或者
            //   文件夹就加载上去**。如果拖入 `IWP\database` 一类的文件夹，**直接加载就可以，右边就是
            //   预览文件结构、大小、名称等的。这样可以选择性解包。**」
            //   ⇒ 目录里的包**全部加载**（撤销 2026-10-02 的"扫出来的只入列表"）；用户实测零点几秒，
            //     而且 DB 读盘已是 seek 式 ⇒ 每个包只读"块头 + 尾部条目表"。
            if (scanned > 0) {
                int loaded_now = 0;
                const QString prefix = QDir::fromNativeSeparators(gated) + QLatin1Char('/');
                for (size_t i = 0; i < sources_.size(); ++i) {
                    if (sources_[i].loaded) continue;
                    const QString candidate = QDir::fromNativeSeparators(sources_[i].path);
                    if (!candidate.startsWith(prefix, Qt::CaseInsensitive)) continue;
                    if (load_one_source(static_cast<int>(i), nullptr)) ++loaded_now;
                }
                if (loaded_now > 0) rebuild_tree();   // 新加载的内容要进右栏（＝解包产物预览）
                // ★ 2026-10-03：**返回值要如实** —— `add_paths` 的返回是"这次调用加载了几个源"，
                //   目录分支里加载的那些必须计进去（自检那条"拖目录 ⇒ 返回 1"的锁当场抓出这里漏了）。
                loaded += loaded_now;
                log_message(QStringLiteral("目录里的 %1 个包已加载（右栏＝产物预览，可选择性解包）")
                                .arg(loaded_now));
            } else {
                const QString why = QStringLiteral("这个目录里没有认得出的包：%1").arg(raw);
                log_message(why);
                load_errors_ << why;
            }
            refresh_package_checks();
            refresh_counts();
            continue;   // ★ 目录自己**不成为源**（非包文件不进左树、也不进右栏）
        }
        // ② 文件 ⇒ 包（SquashFS 按 magic、DB 按自洽判定，
        //    歧义时用「打包/解包格式」下拉里选的那个，见 package_entries 的说明）
        // ★★ 2026-10-06（审计 3-3）：判重必须用引擎现成的 `core::path_equals`
        //   （`core/path.cpp` 做 lexically_normal + 大小写 + 正反斜杠），**不许拿整串比**。
        //   审计原文：「判重不用 `core::path_equals` ＋ 同层同名覆盖 ⇒ 源消失/计数翻倍」
        //   —— 修前这里是 `QString::compare(source.path, gated, Qt::CaseInsensitive)`：
        //   `E:\a\x.db` 与 `E:/a/x.db`（正/反斜杠）以及含 `.`/`..` 段的写法**认不出是同一份**
        //   ⇒ 重复入源；随后 `pkg_build_nodes` 以"同层同名＝同一节点"把 `it->source_index`
        //   覆盖成后一个 ⇒ **先来的那个源在左树里没有任何节点**（不能选、没有 ✕），却仍在
        //   `sources_` 里参与合并与计数（`loaded_entry_count` 翻倍、与 CLI `--dump entries=`
        //   对不上）。判据（审计给的）：用 `E:\a\x` 与 `E:/a/x` 各加一次 ⇒ **只 1 个源、
        //   左树有节点**。
        bool already_known = false;
        for (const Source& source : sources_) {
            if (core::path_equals(source.path.toUtf8().toStdString(),
                                  gated.toUtf8().toStdString())) {
                // 命中重复时**保留最初的写法**（用户第一次看到的那一行不变），只拒绝这一次。
                already_known = true;
                break;
            }
        }
        if (already_known) {
            const QString why = QStringLiteral("已经在列表里，未重复添加：%1").arg(gated);
            log_message(why);
            load_errors_ << why;
            continue;
        }
        std::string error;
        std::vector<efs::EntryInfo> entries;
        // ★★ P17-85（01 F-09 收尾）：把"这次是怎么读的"接出来（`read_info` 出参）
        efs::DbReadInfo read_info;
        entries = efs::package_entries(gated.toUtf8().toStdString(),
                                       efs::format_key_to_db(current_format_key()), error,
                                       registry_, &read_info);
        if (!error.empty()) {
            // ③ **可见失败**：认不出/解不开都要说清是哪一份、为什么（绝不静默加空包）
            const QString why = QStringLiteral("加载失败：%1 —— %2").arg(raw, utf8(error));
            log_message(why);
            load_errors_ << why;
            continue;
        }
        push_source(gated, entries);
        ++loaded;
        log_message(QStringLiteral("已加载：%1（%2 项）")
                        .arg(raw)
                        .arg(static_cast<int>(entries.size())));
        // ★ 整包读才打（正常包一个字都不打）—— 这是"读得多但结果正确"那种中间状态的唯一出口
        note_package_read(QStringLiteral("列条目"), read_info.header_bytes, read_info.used_seek,
                          read_info.whole_file_reason, raw);
    }
    return loaded;
}

bool FsPage::note_package_read(const QString& tail, uint64_t header_bytes, bool used_seek,
                               const std::string& whole_file_reason,
                               const QString& display_path) {
    // ★★ P17-85（01 F-09 收尾）：可观测的**唯一出口**在页面这一层 —— 口径写在这里，别处不要各写一份。
    //   `used_seek == true`（只读了块头与尾部表）⇒ **一个字都不打**：没退回就不许报理由，
    //   否则"整包读"这句话会变成人人都在喊的噪音（那等于没有观测）。
    if (used_seek) return false;
    const double mb = static_cast<double>(header_bytes) / (1024.0 * 1024.0);
    const QString size_text = mb >= 1.0 ? QStringLiteral("%1 MB").arg(mb, 0, 'f', 1)
                                        : QStringLiteral("%1 KB")
                                              .arg(static_cast<double>(header_bytes) / 1024.0, 0,
                                                   'f', 1);
    const QString why = whole_file_reason.empty()
                            ? QStringLiteral("（没有给出理由 —— 这本身就该修：见 db.cpp 的退回点）")
                            : utf8(whole_file_reason);
    log_message(QStringLiteral("%1：%2 —— 整包读 %3 字节（%4）：%5")
                    .arg(tail, display_path)
                    .arg(QString::number(header_bytes), size_text, why));
    return true;
}

QString FsPage::source_at(int index) const {
    if (index < 0 || index >= static_cast<int>(sources_.size())) return QString();
    return sources_[static_cast<size_t>(index)].path;
}

void FsPage::check_all(bool checked) {
    tree_.toggle_all(checked);
    // ★★ 2026-10-05（#4 回归锁）：勾选**不许刷状态** —— 用户已复测「勾选不会刷状态」，
    //   而本批把它钉成硬判据：**勾选前后所有已展开节点的 `isExpanded()` 快照、item 指针、
    //   滚动位置逐字不变**，而且明确包含"**按钮触发**的勾选"（`smoke` 的 #4 ②）。
    //
    //   修前这里是 `refresh_rows()`（＝ `entry_tree_->clear()` + 整棵重建）⇒ 展开态、
    //   item 指针、滚动位置**全被抹掉**：「包内全选」这条按钮路径因此当场违反 #4
    //   （自检实测：指针 0/0、展开 0/0、滚动 2→0）。
    //   ⇒ 与 `on_entry_item_changed` 走**同一条**"就地刷状态"的路：
    //     `rebuild_dir_states()` 重算目录三态的**模型**，
    //     `sync_entry_states()` 只改 `setCheckState`（不碰控件树、不碰展开态）。
    //   ★ 为什么不是"再补一个重建后恢复展开态"的补丁：那等于把重建留在热路径上，
    //     大包（3.9 万条）每点一次按钮就重建一次，而且 item 指针必然变 ⇒ 判据照样红。
    //   ★ 判红锁：`smoke` #4 ②（把这两句换回 `refresh_rows()` ⇒ 立刻红）。
    rebuild_dir_states();
    sync_entry_states();
    // ★★ 2026-10-03：右栏的勾选变了 ⇒ 左栏**包节点的三态**也要跟着刷
    //   （用户：「包的内容没有全部勾选的时候，包的勾选框也应当处于部分勾选的状态。」）。
    //   原来这里漏了 → "包内全选"之后包节点仍显示未勾（新锁当场抓出来）。
    refresh_package_checks();
    refresh_counts();
}

bool FsPage::entry_hint_visible() const {
    // ★ 2026-09-27：空状态提示块按用户要求**已删**（见构造函数注释）⇒ 恒为 false。
    //   访问器保留是因为自检里还有"这条不该再出现"的反向断言（见 smoke.cpp）。
    return false;
}

void FsPage::rebuild_tree() {
    // 合并树 = **已加载的那些源**按 `sources_` 顺序重放一遍（后者为准）。
    // 「取消加载/移除/清空」都走它 —— 不引入"从树里局部删节点"这种第二套逻辑。
    // ★★ 2026-10-06（审计 3-1）：**重建整棵树不许把用户勾过的丢了**。
    //   审计原文：「`rebuild_tree()` 丢整棵树 ⇒ 已勾选被静默清空（违背 6.1）」
    //   （位置 `fs_page.cpp:988`）。修前这里直接 `tree_ = MergeTree{}` —— 而勾选态
    //   **只活在树里**（`Source`/`EntryInfo` 都没有这个字段），所以"加载选中/取消加载/
    //   移除/清空/加包"任何一次源集合变化都会把用户勾过的全部静默清空；`merge()` 里那句
    //   "6.1 勾选态跟随原节点"只在**增量**合并时成立，而本函数每次都从空树重放 ⇒ 永远
    //   走不到那条保护。判据（审计给的）：**勾 3 项 → 加一个包 ⇒ `checked_file_count()`
    //   必须仍为 3**。
    //   修法：重建**之前**按路径快照，重放**之后**按路径回灌（路径＝"原节点"的身份）。
    const std::vector<std::string> checked_before = tree_.checked_paths();
    tree_ = efs::MergeTree{};
    loaded_order_.clear();
    for (size_t i = 0; i < sources_.size(); ++i) {
        const Source& source = sources_[i];
        if (!source.loaded) continue;
        efs::MergeInput input;
        input.source = source.key;
        // ★★ 2026-10-05（用户报告 #2「右栏预览未体现「包名壳」」）：右栏＝**解包产物预览**
        //   （用户 2026-10-03 原话：「**这是一个解包的工具，而右边罗列的是包的内容，换句话说
        //   就是解包产物的预览。**」）⇒ 每一条显示的路径必须**恰好等于**它真正会被写到的
        //   落点相对路径。修前这里直接喂**包内原始路径** ⇒ `IWP\database\config\xr.db` 的
        //   条目显示成根级 `gamemtl.xr`，而实际解出来是 `xr\gamemtl.xr`（用户原话：
        //   「`…\IWP\新建文件夹`还是那样。」）。
        //   ★ 拼法**只此一份**：引擎 `efs::preview_entries_in_layout`（与 `unpack_target_dir`
        //     同源）—— 谁把"加壳"写回页面里，就等于又造了第二份落点规则。
        //   ★ 自带壳的包（`ai.db` 的条目已经 `ai/…`）⇒ 该函数直接原样返回，**不叠第二层**。
        input.entries = efs::preview_entries_in_layout(source.key, entries_[i]);
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
    // ★★ 2026-10-06（审计 3-1）：回灌勾选态（`applied` 只作对账，不参与判定）。
    //   顺序不能反：必须在**全部源重放完**之后回灌（合并期间胜者会换，回灌认的是最终树）。
    const int applied = tree_.apply_checked_paths(checked_before);
    (void)applied;
    refresh_counts();
    refresh_rows();
    // ★ 3-1 的配套：左栏包节点的三态是**从模型读**的（`source_check_state`），
    //   勾选态刚被回灌 ⇒ 必须跟着刷，否则包行还显示"未勾"（而 `checked_file_count()` 已是 3）。
    refresh_package_checks();
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
            // ★★ 2026-10-06（审计 3-2）：计数与树上"真的显示了几行"同一条判据
            //   （`entry_visible`）—— 修前这里写 `!filter_keep_.isEmpty() && ...`，
            //   搜一个不存在的名字时 `filter_keep_` 是空的 ⇒ 计数**全量**（"39374 文件"
            //   而树上一行都没有的那类自相矛盾）。现在过滤态由 `filter_active_` 说。
            if (!entry_visible(*node)) continue;
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

int FsPage::loaded_entry_count() const {
    // 已加载源列出来的条目总数（各源 `entries_` 之和）—— 与 CLI `--dump` 的 `entries=`
    // 同一个口径（引擎对同一份包列出的条目表长度）。`--shot --load` 用它做真机对拍。
    int total = 0;
    for (size_t i = 0; i < sources_.size() && i < entries_.size(); ++i) {
        if (!sources_[i].loaded) continue;
        total += static_cast<int>(entries_[i].size());
    }
    return total;
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
    // 点的是勾选框那一小块吗？是的话交给 Qt（itemChanged 会回灌模型），这里不重复处理。
    // ★ 2026-10-02：判据与包树**共用同一个 `click_on_indicator`**（两树行为只有一份实现）。
    const bool on_indicator = click_on_indicator(entry_tree_, item);
    if (is_dir) {
        if (!on_indicator) item->setExpanded(!item->isExpanded());   // 单击文件夹 = 展开/折叠
        return;
    }
    if (on_indicator) return;   // 勾选框自身交给 Qt
    // 文件：**点条目身体**即切换勾选（勾选框只是视觉效果）
    item->setCheckState(0, item->checkState(0) == Qt::Checked ? Qt::Unchecked : Qt::Checked);
}

void FsPage::on_package_item_clicked(QTreeWidgetItem* item) {
    // ★ 2026-10-02 用户口径：「包树和文件树逻辑的**唯一差别**，就是包树多一个是否加载。
    //   **其他行为应当完全一样**。」⇒ 与 `on_entry_item_clicked` 共用 `click_on_indicator`：
    //   点箭头/勾选框那一小块交给 Qt；点**条目身体**——有子节点就展开/折叠，是叶子就**切换勾选**
    //   （与条目树的文件完全一致；勾选一个未加载的包＝加载它，沿用 1.x `_db_toggle`，由 itemChanged 处理）。
    if (item == nullptr) return;
    if (click_on_indicator(package_list_, item)) return;
    if (item->childCount() > 0) {
        item->setExpanded(!item->isExpanded());
        return;
    }
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
    // ★★ 2026-10-03 用户：「**包的内容没有全部勾选的时候，包的勾选框也应当处于部分勾选的状态。**」
    //   ⇒ 在**右栏**改动条目的勾选之后，必须同步刷**左栏包节点**的三态；原来这里漏了这一句，
    //     于是勾掉包内某个条目后，左边那个包**仍然显示全勾**（自检新锁当场抓出来）。
    refresh_package_checks();
    refresh_counts();
}

bool FsPage::entry_visible(const efs::MergeNode& node) const {
    // ★★ 2026-10-06（审计 3-2）：**"没查询"与"查询无命中"不许编码成同一个空集**。
    //   审计原文：「搜索"无命中"退化成"不过滤"（把"没查询"与"查询无命中"编码成同一个空集）
    //   ⇒ 判据：搜一个不存在的名字 ⇒ **0 行**（且计数为 0）」。
    //   修前这里是 `filter_keep_.isEmpty() || filter_keep_.contains(...)`：查询没命中时
    //   `filter_keep_` 也是空的 ⇒ 与"没查询"同形 ⇒ **显示整棵树 + 计数全量**（用户搜一个
    //   不存在的名字，看到的是"什么都没过滤"）。现在两者分开：`filter_active_` 说
    //   "这一轮到底有没有在过滤"（有查询词就为真），空集合在过滤态下就是"0 行"。
    if (!filter_active_) return true;   // 真的没查询 ⇒ 不过滤
    return filter_keep_.contains(utf8(node.path));
}

void FsPage::rebuild_filter_keep() {
    filter_keep_.clear();
    matched_paths_.clear();
    filter_active_ = false;
    if (search_edit_ == nullptr) return;
    if (search_edit_->text().trimmed().isEmpty()) return;
    filter_active_ = true;   // ★ 有查询词 ⇒ 进入过滤态；此时空集合 = 无命中（0 行）
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
    if (!filter_active_) {
        for (const efs::MergeNode& child : tree_.root.children) {
            entry_tree_->addTopLevelItem(make_entry_item(child));
        }
    } else {
        // ★★ 2026-10-06（审计 3-2）：过滤态下**空集合就是"0 行"**（不是"不过滤"）——
        //   `walk` 自然一条都进不来（`entry_visible` 恒假），树因此是空的、计数也是 0。
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

QStringList FsPage::source_paths() const {
    QStringList out;
    for (const Source& source : sources_) out << source.path;
    return out;
}

// ★ 2026-10-04 用户第 2 条（"目录定向"）：这四个函数是各"选择"调用点的判据唯一实现 ——
//   按钮 lambda 直接调它们（见构造期那几处 `resolve_dialog_start(...)`），自检也断言它们。
DialogStart FsPage::unpack_output_dialog_start() const {
    // 用户原话里的例子：「**解包的目标目录**…**改成定向到输入的目录**」
    //   ⇒ 来源包**所在目录**（包是文件 ⇒ 取它所在目录）；0 个或 ≥2 个源 ⇒ 落到工具自身所在文件夹
    return dialog_start_for(output_dir(), source_paths(), true);
}

DialogStart FsPage::pack_output_dialog_start() const {
    // 封包输出没有"唯一输入目录"这种语义 ⇒ 继承上次的选择（框里现有的值）
    return dialog_start_for(pack_out_edit_ != nullptr ? pack_out_edit_->text() : QString(),
                            QStringList(), false);
}

DialogStart FsPage::pack_add_files_dialog_start() const {
    // ＋文件 / ＋目录：待打包列表是**多个**文件 ⇒ 天然落到工具自身所在文件夹；
    //   真正能当"输入目录"的只有**唯一**的一份待打包内容（例：刚＋进来的那个目录）
    QStringList inputs;
    if (pack_items_.size() == 1) inputs << QString::fromStdString(pack_items_.front().source_path);
    return dialog_start_for(pack_base_dir(), inputs, false);
}

DialogStart FsPage::load_package_dialog_start() const {
    // 加包：还没有任何源时**没有"输入目录"可继承** ⇒ 工具自身所在文件夹（不再隐式用"解包到"）
    return dialog_start_for(QString(), source_paths(), true);
}

QString FsPage::resolve_output_dir() {
    // 1.x `_resolve_output`：空则按"第一个包所在目录"推断，并**回填**输入框
    //（界面必须反映真实落盘位置 —— 1.x 的 H1 就是"框里空着，实际落到别处"）。
    // ★★ 2026-10-04：**推断规则搬到引擎**（`efs::resolve_output_root`），本页只剩"调它 + 回填"。
    //   用户当天的架构红线（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能
    //   比 CLI 还多的。**」—— 否则命令行算不出同一个缺省根（CLI 的 `--unpack-package` 省略根时
    //   走的就是引擎这一份）。
    QString out = output_dir();
    if (out.isEmpty()) {
        const QString front = sources_.empty() ? QString() : sources_.front().path;
        out = QString::fromStdString(
            efs::resolve_output_root(front.toStdString(), QDir::currentPath().toStdString()));
        if (out_edit_ != nullptr) out_edit_->setText(QDir::toNativeSeparators(out));
    }
    return out;
}

QString FsPage::package_folder_name(const QString& package_path) {
    // ★★ 2026-10-04：**实现搬到引擎**（`efs::package_folder_name`），这里只留一层薄包装。
    //   用户当天拍的架构红线（原话）：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能
    //   比 CLI 还多的。**」—— 这条规则原先只写在本页里 ⇒ **CLI 复现不了** ⇒ 真机"产物布局"
    //   （顶层＝包名去扩展名）只能在 GUI / 合成夹具上验。现在 shell 与 CLI 共用引擎那一份。
    return QString::fromStdString(efs::package_folder_name(package_path.toUtf8().toStdString()));
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
        // ★★ 2026-10-03 用户口径（原话）：「如果就拖入 `E:\...\IWP\database\config` 的 `configs`，
        //   那么**解包出来是一个 `configs` 文件夹**（和 `gamedata.db*`/`sq*` 解出 `gamedata` 一样），
        //   **不包含 `database` 和 `config`**。」；追问"包内路径本身就带一层 `configs/` 会不会变成
        //   `configs/configs`"他定案：**「这个案例，就解出来一个 configs。」**
        //   ⇒ 规则：产物顶层＝**包名（去扩展名）**；但**包内路径已经自带这一层同名壳时不再叠一层**。
        //   ★★ 2026-10-04：这条规则**整段搬到引擎**（`efs::unpack_target_dir`）—— 用户红线要求
        //     "GUI 的功能 CLI 全有"，而 CLI 复现不了写在本页里的落点规则。这里只把该包的条目路径
        //     收集好交给它；**行为一字不变**（自检锁：本页算出的落点集合 == 引擎算的）。
        std::vector<std::string> entry_paths;
        entry_paths.reserve(entries_[i].size());
        for (const efs::EntryInfo& entry : entries_[i]) entry_paths.push_back(entry.path);
        job.target = efs::unpack_target_dir(root.toUtf8().toStdString(),
                                            sources_[i].path.toUtf8().toStdString(), entry_paths);
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
                                const MessageSink& message, int* written_out) {
    // **纯执行**：不读任何控件、不写任何界面（任务壳的 worker 与自检共用这一条）
    // ★★ 2026-10-06（② 用户拍板）：**解包不再有取消** —— 用户原话「封包和字库的取消，
    //   在这里本质是完全没有的，故直接删除即可。」修前这里在**包边界**轮询取消位并提前
    //   返回"已取消：在包边界停下…"；那段（连同任务壳的取消按钮）整块删掉了。
    int written = 0;
    int failed = 0;
    int overwritten = 0;
    QStringList per_package;
    // ★★ 2026-10-05（**落点覆盖必须可见 / 零静默**）：**解包前先把每个包的落点打出来**。
    //   起因（用户报的真事故）：一次默认落点（＝**来源所在目录**）解包，把 **3.9 万个文件**
    //   盖在了模组目录上，而工具**一个字都没说**。
    //   口径（用户口径，一个字不改）：**不改默认落点**、**不弹模态确认框**（那会改交互口径）
    //   —— 要的只是"**任何改写都必须看得见：逐条原因 + 计数**"。
    //   所以这里做两件事：① 解包**前**把 `包 → 落点` 逐条打出来（下面这个循环）；
    //   ② 解包**中/后**把被覆盖的已存在文件**逐条**报出来 + 汇总计数（见下面的 `overwritten`）。
    for (size_t i = 0; i < jobs.size(); ++i) {
        const QString pre = QStringLiteral("解包目标：%1 → %2").arg(utf8(jobs[i].label), utf8(jobs[i].target));
        per_package << pre;
        if (message) message(pre.toUtf8().toStdString());
    }
    for (size_t i = 0; i < jobs.size(); ++i) {
        const efs::ExtractTreeResult result = FsPage::extract_job(jobs[i]);
        written += static_cast<int>(result.files_written);
        failed += static_cast<int>(result.failures.size());
        // ★★ 2026-10-05（零静默）：**逐条**报出这次被**覆盖**掉的已存在文件（引擎侧的
        //   `overwritten` 是"盘上真发生过的"那份真值 —— 见 `extract_tree.cpp` 的说明：
        //   记在**写成功之后**，失败的写不算）。计数在下面的汇总行里。
        //   ★ 判红锁（smoke 的本页夹具）：解两遍 ⇒ 第二遍必须出现「覆盖：…」且计数非零；
        //     把这一段删掉（或把引擎那两处 `overwritten.push_back` 去掉）⇒ 立刻红。
        for (const std::string& one_path : result.overwritten) {
            const QString one = QStringLiteral("覆盖：%1").arg(utf8(one_path));
            per_package << one;
            if (message) message(one.toUtf8().toStdString());
        }
        overwritten += static_cast<int>(result.overwritten_size());
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
        if (written_out != nullptr) *written_out = 0;
        return QStringLiteral("没有可解的内容（先「加包」/「扫描目录」并勾选，或先加载包）");
    }
    // ★★ P17-4（审计 03 F-03）：计数在这里**带出去** —— 调用方（`unpack_selected/unpack_all`）
    //   修前是"再跑一遍 extract_job 自己数"，那等于整批写盘两遍（见 fs_page.h 的说明与
    //   run_ci 里那条静态锁）。
    if (written_out != nullptr) *written_out = written;
    return QStringLiteral("解包：%1 个文件、覆盖 %2、失败 %3\n  %4")
        .arg(written)
        .arg(overwritten)
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
    if (task_ == nullptr || !task_->run(title, [this, jobs](const ProgressSink& progress,
                                                             const MessageSink& message) {
            run_unpack_jobs(jobs, progress, message);
        })) {
        log_message(QStringLiteral("有任务在跑，稍后再试"));
    }
}

int FsPage::unpack_selected(QString* summary) {
    QString root;
    const std::vector<UnpackJob> jobs = plan_unpack(true, &root);
    // ★★ P17-4（审计 03 F-03 High）：**同一批只解一遍**。
    //   修前：`run_unpack_jobs`（它内部已逐 job `extract_job` 并报日志/进度）之后**又**对同一个
    //   jobs 逐条 `extract_job` ⇒ 整批写盘两遍（第二遍覆盖第一遍产物、对只读/被占用文件凭空
    //   多出一轮失败、日志多出一串「覆盖：…」、耗时翻倍）。界面按钮走 `start_unpack`（不双跑）
    //   ⇒ 只有这两个 public 入口行为不一致 —— 判据：`run_ci` 的 shell-smoke 步里那条静态锁
    //   （这两个函数体内不许再出现 `extract_job(`）。
    int written = 0;
    const QString text = run_unpack_jobs(jobs, nullptr, nullptr, &written);
    if (summary != nullptr) *summary = text;
    return written;
}

int FsPage::unpack_all(QString* summary) {
    QString root;
    const std::vector<UnpackJob> jobs = plan_unpack(false, &root);
    // ★★ P17-4：同 `unpack_selected` —— 执行只许走 `run_unpack_jobs` 一条路。
    int written = 0;
    const QString text = run_unpack_jobs(jobs, nullptr, nullptr, &written);
    if (summary != nullptr) *summary = text;
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
            // ★★ 2026-10-06（审计 3-3）：与 `add_paths` **同一条判重口径**（`core::path_equals`）
            //   —— 修前这里也是整串 `compare`，于是"扫描目录"这条路径会放进第二份只差斜杠
            //   写法的源（同层同名覆盖 ⇒ 先来的那个在左树里没有节点、计数却翻倍）。
            if (core::path_equals(source.path.toUtf8().toStdString(),
                                  path.toUtf8().toStdString())) {
                known = true;
            }
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
    // ★★ 2026-10-06（审计 3-4）：这一段的逻辑**抽成了 `apply_load_order()`** ——
    //   加包/拖放路径（`push_source`）也要走同一份实现，否则"两条路两套合并赢家"。
    apply_load_order();
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
                // 分组节点：只有名字，**但可勾选**（用户口径"其他行为应当完全一样" ⇒ 文件夹也带三态勾选框，
                // 勾它＝连带勾/去勾它下面的所有源；三态由 `refresh_package_checks()` 统一刷）。
                // 仍然没有大小、没有 ✕。
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setText(0, label);
            }
            if (parent == nullptr) {
                package_list_->addTopLevelItem(item);
            } else {
                parent->addChild(item);
            }
            for (const PkgTreeNode& child : node.children) add_node(child, item);
            // ★ 2026-10-02 用户：「重建之后，默认收起。」⇒ 目录节点建出来**不自动展开**
            //   （上一版默认展开：拖一个大目录时几十个包一次性铺开，列表太长）。
            //   展开态由用户自己点箭头；这里刻意不 `setExpanded(true)`。
            (void)item;
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
    const int index = item->data(0, Qt::UserRole).toInt();
    const bool want = item->checkState(0) != Qt::Unchecked;
    if (index < 0) {
        // 分组节点（用户口径：「包树和文件树逻辑的唯一差别，就是包树多一个是否加载。其他行为应当完全一样。」
        //   ⇒ 分组节点也带三态勾选框，勾/去勾它＝连带勾/去勾它下面的**所有源**）
        toggle_group_at(item, want);
        return;
    }
    if (index >= static_cast<int>(sources_.size())) return;
    if (!sources_[static_cast<size_t>(index)].loaded) {
        // 1.x `_db_toggle`：勾选一个**未加载**的包 ⇒ 先加载，再整包勾上
        if (!want) return;
        // ★★ 2026-10-02 用户报「包树崩了」的另一条根因（我埋的）：这里原来调
        //   `load_selected()` + `rebuild_package_list()` —— 而**这个槽就是 Qt 正在处理 item 时
        //   发出来的 `itemChanged`**，`clear()` 会把 item 删掉 ⇒ 槽返回后 Qt 继续用它 ⇒ 崩。
        //   ⇒ 改成**就地加载这一个源**（只动模型与右栏，`rebuild_tree()` 不碰包树），再刷它自己那一行。
        if (!load_one_source(index, nullptr)) {
            rebuilding_package_list_ = true;          // 加载失败 ⇒ 勾选态退回，别让它停在"已勾"
            item->setCheckState(0, Qt::Unchecked);
            rebuilding_package_list_ = false;
            refresh_package_checks();
            return;
        }
        rebuild_tree();
        rebuilding_package_list_ = true;              // 改自己那一行（文本/勾选）时别回灌
        item->setText(0, QStringLiteral("%1　[已加载]").arg(QFileInfo(sources_[static_cast<size_t>(index)].path).fileName()));
        item->setCheckState(0, Qt::Checked);
        rebuilding_package_list_ = false;
    }
    const int merged = loaded_index_for_source(static_cast<size_t>(index));
    if (merged < 0) return;
    tree_.toggle_source(merged, want);
    // ★★ 2026-10-03 用户：「除了上级勾选下级不继承之外，还有一个就是母文件夹的加载状态不继承到
    //   子文件（夹），这应该是同一个问题。」⇒ 根因＝包树的勾选**只在分组节点上做了自下而上汇总，
    //   却没有自上而下传播**；而被拖进来的那个**文件夹自己也是"源节点"**，点它原来只作用于
    //   它自己的条目（右栏）。
    //   ⇒ 修法：**凡是有子节点的节点**（不管是分组还是"文件夹源"），勾/去勾都**连带**它下面的所有源。
    //     连带时只**标记**、不代加载（一次点击解 57 个包会卡死，真正加载仍由「加载选中」触发）。
    toggle_group_at(item, want);
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

bool FsPage::load_one_source(int source_index, QString* error_out) {
    // ★ 2026-10-02：把"加载一个源"抽成**不碰包树**的一步 —— 勾选框（`itemChanged` 槽里）与
    //   「加载选中」都能用。以前勾选走 `load_selected()`（它会 `rebuild_package_list()`）⇒ 崩。
    if (source_index < 0 || source_index >= static_cast<int>(sources_.size())) return false;
    Source& source = sources_[static_cast<size_t>(source_index)];
    if (source.loaded) return true;
    QString gated = source.path;
    QString gate_error;
    if (!gate_source(gated, &gate_error)) {
        const QString why = QStringLiteral("拒绝加载（需要解密插件）：%1 —— %2").arg(source.path, gate_error);
        log_message(why);
        load_errors_ << why;
        if (error_out != nullptr) *error_out = why;
        return false;
    }
    std::string error;
    std::vector<efs::EntryInfo> entries;
    // ★★ P17-85（01 F-09 收尾）：同上（`load_one_source` 那一处）
    efs::DbReadInfo read_info;
    // ★ 2026-10-03：`isDir` 这一支**现在走不到了**（目录不再成为源 —— 见 `add_paths`）；
    //   留着是为了"万一有历史/外部构造的目录型源"仍能加载，不至于静默失败。
    if (QFileInfo(gated).isDir()) {
        {
            // ★★ P17-120（复审计 `P2-5`）：截断走**结构化标志**，不写进 `error`
            efs::LooseInfo loose_info;
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error, &loose_info);
            if (loose_info.truncated) {
                log_message(QStringLiteral("散装层条目表被截断（仍加载）：%1")
                                .arg(QString::fromStdString(loose_info.truncated_reason)));
            }
        }
    } else {
        entries = efs::package_entries(gated.toUtf8().toStdString(),
                                       efs::format_key_to_db(current_format_key()), error,
                                       registry_, &read_info);
    }
    if (!error.empty()) {
        const QString why = QStringLiteral("加载失败：%1 —— %2").arg(source.path, utf8(error));
        log_message(why);
        load_errors_ << why;
        if (error_out != nullptr) *error_out = why;
        return false;
    }
    source.path = gated;
    source.key = gated.toUtf8().toStdString();
    source.loaded = true;
    entries_[static_cast<size_t>(source_index)] = std::move(entries);
    note_package_read(QStringLiteral("列条目"), read_info.header_bytes, read_info.used_seek,
                      read_info.whole_file_reason, gated);
    return true;
}

void FsPage::toggle_group_at(QTreeWidgetItem* group, bool on) {
    // 分组节点：**连带**它下面的所有源。
    // ★★ 2026-10-03 用户口径：「勾文件夹要不要顺带加载**肯定要加载**」⇒ 勾上时对**未加载**的源
    //   真的 `load_one_source()`（不再是"只标记"）；**加载失败的源勾选态退回未勾**（不留假勾），
    //   失败理由由 `load_one_source` 记进日志与 `load_errors_`。
    if (group == nullptr) return;
    std::vector<QTreeWidgetItem*> nodes;
    std::vector<int> touched;
    rebuilding_package_list_ = true;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* node) {
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            const int index = child->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < static_cast<int>(sources_.size())) {
                child->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
                nodes.push_back(child);
                touched.push_back(index);
            }
            walk(child);
        }
    };
    walk(group);
    rebuilding_package_list_ = false;
    bool loaded_any = false;
    for (size_t k = 0; k < touched.size(); ++k) {
        const int index = touched[k];
        if (!sources_[static_cast<size_t>(index)].loaded) {
            if (!on) continue;                        // 去勾不需要加载
            if (!load_one_source(index, nullptr)) {   // 加载失败 ⇒ 别把它留在"已勾"
                rebuilding_package_list_ = true;
                nodes[k]->setCheckState(0, Qt::Unchecked);
                rebuilding_package_list_ = false;
                continue;
            }
            loaded_any = true;
        }
        const int merged = loaded_index_for_source(static_cast<size_t>(index));
        if (merged >= 0) tree_.toggle_source(merged, on);
    }
    if (loaded_any) rebuild_tree();   // 源集合变了 ⇒ 右栏（产物预览）要重放
    rebuild_dir_states();
    sync_entry_states();
    refresh_package_checks();
    refresh_counts();
}

void FsPage::refresh_package_checks() {
    // ★ 2026-10-02 树化：勾选态要**递归**刷（源节点散在树里）；**分组节点刷成三态**
    //   （用户口径：包树与文件树"其他行为应当完全一样" ⇒ 文件夹也带三态勾选框）。
    rebuilding_package_list_ = true;
    pkg_for_each_source(package_list_, [&](QTreeWidgetItem* item, int index) {
        if (index >= static_cast<int>(sources_.size())) return;
        const bool loaded = sources_[static_cast<size_t>(index)].loaded;
        // ★★ 2026-10-02：**未加载的源不要拿模型去覆盖勾选态** —— 它的条目表是空的（`files=0`
        //   ⇒ 会被算成 `Unchecked`），于是"用户勾上准备加载"这个**标记**会被立刻抹掉
        //   （自检那条"勾分组＝连带勾下面所有源"就是这么红的）。未加载 ⇒ 勾选态就是标记，原样保留。
        if (!loaded) return;
        int files = 0;
        int checked = 0;
        tree_.source_check_state(loaded_index_for_source(static_cast<size_t>(index)), &files,
                                 &checked);
        item->setCheckState(0, checked == 0 ? Qt::Unchecked
                                            : (checked == files ? Qt::Checked
                                                                : Qt::PartiallyChecked));
    });
    // 分组节点三态：统计整棵子树里的**源节点**勾选情况
    std::function<QPair<int, int>(QTreeWidgetItem*)> tally = [&](QTreeWidgetItem* node) -> QPair<int, int> {
        int total = 0;
        int checked = 0;
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (child->data(0, Qt::UserRole).toInt() >= 0) {
                ++total;
                if (child->checkState(0) != Qt::Unchecked) ++checked;
            }
            const QPair<int, int> sub = tally(child);
            total += sub.first;
            checked += sub.second;
        }
        return {total, checked};
    };
    std::function<void(QTreeWidgetItem*)> paint_groups = [&](QTreeWidgetItem* node) {
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            paint_groups(child);
            if (child->data(0, Qt::UserRole).toInt() >= 0) continue;   // 源节点已刷过
            const QPair<int, int> stat = tally(child);
            if (stat.first == 0) continue;
            child->setCheckState(0, stat.second == 0
                                        ? Qt::Unchecked
                                        : (stat.second == stat.first ? Qt::Checked
                                                                     : Qt::PartiallyChecked));
        }
    };
    for (int i = 0; i < package_list_->topLevelItemCount(); ++i) {
        paint_groups(package_list_->topLevelItem(i));
    }
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
        // ★★ P17-85（01 F-09 收尾）："加载选中"这条批量路也要能报整包读
        efs::DbReadInfo read_info;
        // ★ 2026-10-03：同 `load_one_source` —— 目录型源已不再产生，这一支走不到（留着以防万一）。
        if (QFileInfo(gated).isDir()) {
            {
            // ★★ P17-120（复审计 `P2-5`）：截断走**结构化标志**，不写进 `error`
            efs::LooseInfo loose_info;
            entries = efs::loose_entries(gated.toUtf8().toStdString(), error, &loose_info);
            if (loose_info.truncated) {
                log_message(QStringLiteral("散装层条目表被截断（仍加载）：%1")
                                .arg(QString::fromStdString(loose_info.truncated_reason)));
            }
        }
        } else {
            entries = efs::package_entries(gated.toUtf8().toStdString(),
                                           efs::format_key_to_db(current_format_key()), error,
                                           registry_, &read_info);
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
        note_package_read(QStringLiteral("列条目"), read_info.header_bytes, read_info.used_seek,
                          read_info.whole_file_reason, gated);
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
    // ★★ 2026-10-04：实现**搬到引擎**（`efs::pack_ext_for`）—— 用户红线「GUI 的功能，CLI 肯定要
    //   全部拥有」⇒ 命令行封包时必须按同一规则补扩展名。本页只剩一层薄包装。
    return QString::fromStdString(efs::pack_ext_for(key.toStdString()));
}

QString FsPage::resolve_pack_target() {
    // ★★ 2026-10-04：**推断规则搬到引擎**（`efs::resolve_pack_target`），本页只剩"调它 + 该弹框时弹框 + 回填"。
    //   引擎返回 `needs_frontend_dialog` ⇒ 这里才弹 `getSaveFileName`（弹框属于前端交互，不进引擎）。
    const std::string key = pack_format_combo_->currentData().toString().toStdString();
    const std::string base = pack_output_basename().toStdString();
    const efs::PackTarget t =
        efs::resolve_pack_target(pack_out_edit_->text().trimmed().toStdString(), base, key);
    QString out = QString::fromStdString(t.path);
    if (t.needs_frontend_dialog) {
        const QString chosen = QFileDialog::getSaveFileName(
            this, QStringLiteral("封包保存到"), QDir::toNativeSeparators(out),
            QStringLiteral("包 (*.db *.sqfs);;所有文件 (*.*)"));
        if (chosen.isEmpty()) return QString();
        out = chosen;
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
    // ★★ 2026-10-06（② 用户拍板）：**封包不再有取消** —— 用户原话「封包和字库的取消，
    //   在这里本质是完全没有的，故直接删除即可。」修前这里有一段"开始之前"的空转取消
    //   （`should_stop()` ⇒ 直接返回"已取消：封包尚未开始"）；整块删掉了。
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
    // ★ 2026-10-06（⑥ 零静默红线）：接住"名字被改写"的台账 —— cp1251 表示不了的名字
    //   会被逐码点顶替成 `?`（引擎不认中文名），用户**必须看得见**改了哪些、改成了什么。
    //   **不阻塞**：包照样写出来（用户口径：不需要支持中文名，但不能静默）。
    std::vector<efs::DbNameRewrite> rewrites;
    // ★★ 2026-10-10（P17-66，审计 01 F-13 的**官方页接线**）：这条原来会把空包/错包写出去。
    //   给了 `&pack_err` ⇒ 启用"4 GiB 窄化"两条检查（单条目 / 累计偏移）；
    //   命中就**直接报错返回、绝不写盘**（P17-62 先查件点名的"比静默截断更糟"那条路）。
    std::string pack_err;
    const std::vector<uint8_t> raw = efs::pack_db(items, *fmt, &rewrites, &pack_err);
    if (!pack_err.empty()) {
        return QStringLiteral("封包失败（%1）").arg(utf8(pack_err));
    }
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
    // ⑥：台账**逐条**打出来 + 计数（没发生改写也说明"0"，让"没打印"与"没发生"能区分）
    if (rewrites.empty()) {
        say(QStringLiteral("名字改写：无（%1 个条目名都能用 cp1251 原样写出）")
                .arg(static_cast<int>(items.size())));
    } else {
        say(QStringLiteral("警告：%1 个条目名被改写（cp1251 表示不了的字符逐码点顶替成 '?'；"
                           "包已照常产出，请核对下列名字）")
                .arg(static_cast<int>(rewrites.size())));
        for (const efs::DbNameRewrite& r : rewrites) {
            say(QStringLiteral("  改名：%1 → %2").arg(utf8(r.source), utf8(r.written)));
        }
    }
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
                                        [this, job](const ProgressSink& progress,
                                                    const MessageSink& message) {
                                            run_pack_job(job, progress, message);
                                        })) {
        log_message(QStringLiteral("有任务在跑，稍后再试"));
    }
}

}  // namespace xrkit::shell
