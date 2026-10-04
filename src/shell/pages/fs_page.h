#pragma once
// xrkit::shell —— 文件系统页（M2-6b2）。
//
// 页面只做三件事：把模型画出来、把用户动作翻译成模型调用、把结果写进日志/状态栏。
// **一条算法都不在这里**（合并/路径判据/导出都在 `engine::fs`）—— 这是 1.x 最贵的一条
// 教训：同一件事在 App 与引擎里各写一份，改一处漏一处。
//
// 本页专门收着对照表 ⑥ 的几条"界面形态"判据：
//   · 6.3 `selected`（包列表选中）与 `checked`（包内文件勾选）**两套状态两个按钮**；
//   · 6.4 **没有 `drop_zone` 控件**：拖放直接注册在既有控件上（1.x 明文写着）；
//   · 6.11 选中解包与批量解包**同一个入口**（`extract_to`），不再有两条计数不同的路；
//   · 6.13 格式下拉的**取值是内部键**（显示名只用于展示）。

#include "page_base.h"
#include "../task_shell.h"

#include "xrkit/engine/fs/extract_tree.h"
#include "xrkit/engine/plugin/registry.h"
#include "xrkit/engine/fs/overlay.h"

#include <QHash>
#include <QSet>
#include <QPair>
#include <QString>
#include <QStringList>

#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

class QSplitter;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace xrkit::shell {

class FileDropFilter;

class FsPage : public PageBase {
public:
    explicit FsPage(QWidget* parent = nullptr);

    // 载入一个包（真程序里由拖放/文件对话框给路径；自检直接喂条目清单）。
    // 6.2：加载顺序按 basename 小写升序 —— 页面**不自己排**，交给模型。
    void add_source(const QString& source,
                    const std::vector<engine::fs::EntryInfo>& entries);
    // 散装层（用户 2026-09-26"最后并入层"）：把一个**磁盘目录**当"包"加进来 ——
    // 条目表由 `engine::fs::loose_entries` 产出，走的是**同一个** `add_source`，
    // 所以"同名条目以后加载的为准"对散装层与包完全一致（不出现第二套合并逻辑）。
    bool add_loose_dir(const QString& dir, QString* error);

    // ★ 2026-09-27（用户："1.x 的部分功能或选择没了" + 对照表 6.4）：
    // 从**磁盘路径**加载（包文件或散装目录）—— 「加包」按钮与拖放**共用这一条**：
    //   ① 先过插件解密闸门（⑫12.1）：拒绝就**不加载**，理由写日志；
    //   ② 是目录 ⇒ `loose_entries`；是文件 ⇒ `package_entries`（SquashFS 按 magic、
    //      DB 按自洽判定，歧义时用格式下拉里选的那个）；
    //   ③ 认不出/解不开 ⇒ **可见失败**（日志里有理由，绝不静默加一个空包）。
    // 返回真正加载成功的条数（0 表示一个都没加载）。
    int add_paths(const QStringList& paths);
    // 上一次 `add_paths` 里**每一份认不出的路径 + 理由**（自检要断言"可见失败"；
    // 界面侧同一句话也会进日志栏 —— 不留全局变量，随调用返回值走）
    QStringList load_errors() const { return load_errors_; }
    // 自检用：拖放过滤器到底装在哪些控件上（判据是**接线**，见 drop_filter.h）
    FileDropFilter* drop_filter() const { return drop_filter_; }
    // ★ 2026-10-01 用户第 3 条：「扫描时有Bug的，选择包模式的时候，是扫不出金球奖的db的」
    //   ⇒ 加包对话框的过滤器用**家族通配**（`*.db*` / `*.sq*` / `*.xdb*` 各一段，2026-10-04 补第三段），
    //   自检直接断言它，避免有人再回去枚举 `*.db0 *.db1 …`（那正是看不到 `gamedata.db_textures_1` 的原因）。
    static QString package_dialog_filter();

    int source_count() const { return static_cast<int>(sources_.size()); }
    // 已加载的源数（`refresh_counts` 的"已加载 k"与自检都用它）
    int loaded_source_count() const {
        int loaded = 0;
        for (const Source& source : sources_) {
            if (source.loaded) ++loaded;
        }
        return loaded;
    }
    // ★ 2026-09-27：卡片里的"空状态提示块"与重复提示行**按用户要求删除**（原话"把图中玩意
    //   删掉"）⇒ 本访问器恒为 false，只供自检做"这条不许再冒出来"的反向断言。
    bool entry_hint_visible() const;
    QString source_at(int index) const;
    int conflict_count() const { return tree_.conflict_count(); }

    // 6.3：两套状态 —— 包列表选中态归「全选/加载选中/取消加载/移除」用
    engine::fs::FsSelection& selection() { return selection_; }
    // 包内勾选态归「全选/全不选/解包」用
    void check_all(bool checked);
    int checked_file_count() const { return tree_.checked_file_count(); }

    // 6.11：**唯一**的导出入口。选中集（Listed）与批量（All）走同一条路、同一套计数。
    // 1.x 是"逐项 --cat（300 s/项）"与"整镜像 --unpack-path（3600 s）"两条路，
    // 成功计数与文案还不同源。
    // ⑫12.1 decryptor 消费端（用户 2026-09-26 决策：NLC 解密是**外部闭源插件**，
    // 工具内不放解密逻辑）。加载包之前问一遍插件解密器：
    //   · 没有解密器命中 ⇒ 原样放行（行为完全不变）；
    //   · 命中并解开 ⇒ 路径换成解密产物，照常加载；
    //   · 命中却解不开 ⇒ **拒绝加载**并把理由显示出来（绝不静默当普通包读）。
    void set_registry(const plugin::Registry* registry) { registry_ = registry; }
    bool gate_source(QString& path_in_out, QString* error_out);
    QString last_gate_error() const { return last_gate_error_; }
    void log_message(const QString& text);

    engine::fs::ExtractTreeResult extract_to(const QString& image_path, const QString& out_root,
                                            engine::fs::Selection selection_kind,
                                            const std::vector<std::string>& only = {});

    QComboBox* format_combo() const { return format_combo_; }
    // ★ 2026-10-01 用户第 6 条：「包列表和文件列表引用的子组件是不是不一样？…所以，使用同一个
    //   小组件来完成。包列表的内容和文件列表的结构实际上是一样的。统一使用文件列表的那个，
    //   因为那个已经打磨过。」⇒ 由 `QListWidget` 换成与 `entry_tree_` **同一个组件、同一套配置**
    //   的 `QTreeWidget`（三列：包 / 大小 / ✕ 按钮列）。
    QTreeWidget* package_list() const { return package_list_; }
    // ★ 2026-09-28（用户贴 1.x 对照截图："差距。"）：包内文件区**从扁平路径文本改成真树**
    //   —— 可展开/折叠（**懒建子节点**，几万条目的大包不卡）、每行**三态勾选框**
    //   （能勾单个文件/整个目录，回灌引擎 `MergeTree`）、右列**大小**（1.x `_fmt_size` 同格式）
    //   与**来源包**（6.1"看得出这一份来自哪个包"）。
    QTreeWidget* entry_tree() const { return entry_tree_; }
    QTabWidget* tabs() const { return tabs_; }   // 整页两标签（解包 / 封包）
    QSplitter* split_list_tree() const { return split_list_tree_; }
    // ★ 2026-09-30 结构重排（用户 ①「堆起来很丑」+ 授权自行审美）：包列表与条目树各自成
    //   **一个小面板**（工具行 + 它服务的那个列表，同一父控件）。自检据此断言"工具行归位"
    //   ——修前条目工具行落在分隔条**下面**（父控件是解包页），看着像页级控件。
    QWidget* package_pane() const { return package_pane_; }
    QWidget* entry_pane() const { return entry_pane_; }
    QWidget* package_bar() const { return package_bar_; }
    QWidget* entry_bar() const { return entry_bar_; }
    int entry_top_level_count() const;
    int entry_built_file_count() const;
    QTreeWidgetItem* find_entry_item(const QString& path) const;
    bool expand_entry(const QString& path);
    QPushButton* add_button() const { return add_button_; }
    // ★ 2026-09-28（用户："FS 的方便程度比 1.x 下降了不少"）—— 对齐 1.x 1.1.0：
    QLineEdit* out_edit() const { return out_edit_; }
    QLineEdit* search_edit() const { return search_edit_; }
    QLabel* entry_count_label() const { return entry_count_label_; }
    QPushButton* open_out_button() const { return open_out_button_; }
    // ★ 2026-10-01 用户第 4 条：加包增加**文件夹模式** ⇒ 「扫描目录」按钮删掉（`scan_dir` 保留，
    //   文件夹模式走 `add_paths({dir})`，它自己就会递归找包并加载）。
    QPushButton* clear_button() const { return clear_button_; }
    QPushButton* check_none_button() const { return check_none_button_; }
    QPushButton* unpack_checked_button() const { return unpack_checked_button_; }
    QPushButton* unpack_all_button() const { return unpack_all_button_; }

    // 解包输出根目录（1.x `_resolve_output`）：为空时按第一个源所在目录推断，并**回填**到输入框
    //（界面必须反映真实落盘位置 —— 1.x 的 H1 就是"框里是空/旧值，实际落到别处"）。
    QString resolve_output_dir();
    QString output_dir() const;
    // 1.x `_package_folder`：gamedata.db0 / gamedata.sq_base → "gamedata"
    static QString package_folder_name(const QString& package_path);

    // 解包（1.x `_unpack_checked` / `_unpack_batch` 的等价物；两者都走**同一条** `extract_to`）：
    //   ① 按"条目归属哪个包"分组，逐包导出到 `<输出>/<包名去掉扩展名>/`；
    //   ② 返回写出的文件数，`summary` 里带成功/失败与落盘根目录（界面只用它写日志与状态）。
    // 不做模态确认、不阻塞 —— 界面按钮与自检**共用**这一条（自检据此断言真产物）。
    // 合并树里某个条目的**大小**（0 = 目录或没有这个条目）——树的右列要用它，
    // 自检也用它证明"大小真的从包/目录里带进了模型"（不是界面上瞎写一个数）
    uint64_t node_size(const QString& path) const {
        const engine::fs::MergeNode* node = tree_.find(path.toUtf8().toStdString());
        return node != nullptr ? node->size : 0;
    }
    int unpack_selected(QString* summary);
    int unpack_all(QString* summary);

    // ★ 2026-09-28 第三步（对齐 1.x 的 `_load_async`/`TaskRunner`）：解包与封包**走任务壳**
    //   —— 13.1 重入闸门在主线程同步置位、13.2 **工作线程里不碰任何 UI**。
    //   做法：**计划**（读模型/控件）在主线程算好成 `UnpackJob`/`PackJob`，**执行**是纯引擎调用，
    //   两者之间只传值 ⇒ 同一个执行函数被"按钮（后台线程）"与"自检（前台）"共用，
    //   不会出现"线程里读到半个界面状态"那类事故（1.x 的 H2/H3 就是快照没做干净）。
    struct UnpackJob {
        std::string image;
        std::string target;
        engine::fs::Selection kind = engine::fs::Selection::All;
        std::vector<std::string> only;
        std::string label;
        std::optional<engine::fs::DbFormat> db_fallback;
    };
    struct PackJob {
        std::string out;
        std::string key;                                   // 内部格式键（"sqfs" / "xdb" …）
        std::vector<engine::fs::PackFileItem> items;
    };
    std::vector<UnpackJob> plan_unpack(bool checked_only, QString* out_root);
    static QString run_unpack_jobs(const std::vector<UnpackJob>& jobs,
                                   const ProgressSink& progress, const MessageSink& message);
    PackJob plan_pack(bool* ok, QString* why);
    static QString run_pack_job(const PackJob& job, const ProgressSink& progress,
                                const MessageSink& message);
    void start_unpack(bool checked_only);   // 按钮：计划（主线程）+ task_->run
    void start_pack();
    TaskShell* task() const { return task_; }

    QPushButton* select_all_button() const { return select_all_button_; }
    QPushButton* check_all_button() const { return check_all_button_; }
    // ★ 1.x 的封包面板（`_build_pack_panel` / `_pack` 一族）
    QComboBox* pack_format_combo() const { return pack_format_combo_; }
    QLineEdit* pack_out_edit() const { return pack_out_edit_; }
    QListWidget* pack_list() const { return pack_list_; }
    QPushButton* pack_button() const { return pack_button_; }
    QLabel* pack_count_label() const { return pack_count_label_; }
    int pack_item_count() const { return static_cast<int>(pack_items_.size()); }
    int add_pack_files(const QStringList& paths);
    int add_pack_dir(const QString& dir);
    int remove_pack_selected();
    void clear_pack();
    void refresh_pack_list();
    // 包列表勾选框（1.x `_db_toggle`/`_db_chk_state`）
    void on_package_item_changed(QTreeWidgetItem* item);
    void refresh_package_checks();
    // ★ 2026-10-01 用户第 5 条：行尾 ✕ 与右键菜单「移除选中」**共用这一条**；
    //   ★ 2026-10-02 包列表树化之后，参数是**节点上存的 `sources_` 下标**（分组节点为 -1、直接忽略），
    //   **不是行号** —— 行号＝源下标这个巧合已被树结构打破。
    void remove_source_at(int source_index);
    // 1.x `_pack_root`：设了输入目录就用它（2.x 用「解包到」当这个"根"）
    QString pack_base_dir() const;
    // 1.x `_pack_output_basename`（没有"输入目录"那一支）：顶层同名 ⇒ 用那个名字；
    // 单文件 ⇒ 去扩展名；否则 None ⇒ 用 `packed`。
    QString pack_output_basename() const;
    // 1.x `_pack_ext`：sqfs ⇒ `.sqfs`；其余 ⇒ `.<内部键>.db`（六种格式产物不同名）
    static QString pack_ext_for(const QString& key);
    // 1.x `_resolve_pack_target`：定路径 + **回填到输出框**（1.x 的 H1 就是"set 写在 return 之后"）
    QString resolve_pack_target();
    // 真写盘：sqfs ⇒ `pack_squashfs`；其余 ⇒ `pack_db` + 原子写
    bool run_pack(QString* summary);

    // 6.13 的判据：下拉每一项的 `data` 必须是内部键，且显示名解析不出格式
    void fill_format_combo();
    // 当前选中的格式键（内部键；"sqfs" 表示走 SquashFS 路径）
    std::string current_format_key() const;

    // 把模型的一行行刷进虚拟列表（行文本带"这一份来自哪个包"）
    void refresh_rows();

    // ★ 1.x 的"已知包列表"模型（`db_files` + `loaded{}`）在 2.x 的落地：
    //   `sources_` = **已知的包**（加包/拖放/扫描进来的都在这儿，带"是否已加载"），
    //   合并树由**已加载的那些**按加载顺序重建 ⇒ "加载选中/取消加载/移除/清空"才有意义
    //   （否则列表里全是已加载的，那几个按钮等于摆设 —— 这正是"不如 1.x 方便"的一半）。
    void rebuild_tree();
    void refresh_counts();
    // 扫描目录（1.x `_scan_dir`）：递归找包文件（名字规则在引擎 `is_package_file_name`），
    // 加入已知列表但**不自动加载**（1.x 是加进列表，用户勾/选后再加载）。返回新增个数。
    int scan_dir(const QString& dir);
    // 对包列表里**选中**的项：加载（解析条目进树）/ 取消加载（从树里摘掉）/ 移除 / 全部清空
    int load_selected();
    int unload_selected();
    int remove_selected();
    void clear_all();

private:
    struct Source {
        QString path;
        std::string key;
        bool loaded = false;   // 是否已解析条目并并进合并树（1.x `loaded`）
    };

    // 模型/界面收尾（闸门已过）：记进已知包列表 → 重建列表与合并树。
    // `add_source`（已在内存里的条目）与 `add_paths`（从磁盘列条目）**共用**这一段，
    // 所以"同名条目以后加载的为准"只有一份实现（`rebuild_tree` 里的 MergeTree 合并）。
    void push_source(const QString& source,
                     const std::vector<engine::fs::EntryInfo>& entries);
    bool row_matches(const std::string& path) const;
    void rebuild_package_list();
    // 合并树里的 `source_index` → 已知包路径（见 `loaded_order_` 的说明）
    QString source_path_for_index(int source_index) const;
    // ── 树（包内条目）──────────────────────────────────────────────
    static QString format_size(uint64_t size);                 // 照 1.x `_fmt_size`
    QString source_label_for(const engine::fs::MergeNode* node) const;
    void rebuild_dir_states();                                 // 目录 (文件数, 已勾数)
    QTreeWidgetItem* make_entry_item(const engine::fs::MergeNode& node);
    void populate_entry_children(QTreeWidgetItem* item);       // 懒建（展开时）
    void build_all_descendants(const engine::fs::MergeNode& node, QTreeWidgetItem* item);
    void sync_entry_states();                                  // 勾选后按模型刷三态
    void on_entry_item_changed(QTreeWidgetItem* item);
    void on_entry_item_clicked(QTreeWidgetItem* item);   // 单击展开 / 单击条目勾选
    // ★ 2026-10-02：「包树直接引用树的统一函数就行啊」⇒ 包树也接 `itemClicked`，与条目树
    //   共用 `click_on_indicator` 判据（差异只有"叶子不切勾选"那一条，理由在实现里）。
    void on_package_item_clicked(QTreeWidgetItem* item);
    // ★ 2026-10-02（用户报"包树崩了"之后）：勾选框的槽里**绝不能重建包树**（那是 Qt 正在处理
    //   item 时发出的信号）⇒ 就地加载抽成这一步；分组节点勾选＝连带勾/去勾它的所有源（不代加载）。
    bool load_one_source(int source_index, QString* error_out);
    void toggle_group_at(QTreeWidgetItem* group, bool on);
    bool entry_visible(const engine::fs::MergeNode& node) const;
    // ★ 2026-10-01：搜索改成**按"这一项自己的名字"匹配**（文件与目录一视同仁），
    //   命中项进 `matched_paths_`（界面上加粗），祖先链进 `filter_keep_` 撑层级。
    bool name_matches(const std::string& name) const;
    void rebuild_filter_keep();
    // 反向：`sources_` 的下标 → 合并树里的 `source_index`（未加载返回 -1）
    int loaded_index_for_source(size_t row) const;

    engine::fs::MergeTree tree_;
    // 每个已知源解析出来的条目表（`load_selected` 之后才填；`rebuild_tree` 用它重放合并）
    std::vector<std::vector<engine::fs::EntryInfo>> entries_;
    // 合并树里的 `source_index` 是"**已加载**集合里的序号"，而界面要的是 `sources_` 的下标
    //（已知列表里还有未加载的）—— 这张表就是两者的映射，重建树时同步更新。
    std::vector<size_t> loaded_order_;
    // 任务壳（解包/封包在后台跑；13.1/13.2）
    TaskShell* task_ = nullptr;
    // ★ 唯一一处调用引擎导出的地方（门禁静态锁：extract_package( 恰好 1 处）
    // 静态：只吃 UnpackJob 的值（工作线程与自检都能直接调，不需要页面对象）
    static engine::fs::ExtractTreeResult extract_job(const UnpackJob& job);
    engine::fs::FsSelection selection_;
    std::vector<Source> sources_;
    // 树：条目路径 → 项（懒建；只有建出来的那些在表里）
    QHash<QString, QTreeWidgetItem*> built_items_;
    // 每个**目录**在模型里的 (文件数, 已勾数) —— 勾选后按它刷新三态（一次 flatten 建全表）
    QHash<QString, QPair<int, int>> dir_states_;
    QSet<QString> filter_keep_;   // 搜索过滤要显示的路径（命中项 + 祖先链）；空 = 不过滤
    QSet<QString> matched_paths_; // 真正**命中**搜索的路径（界面上加粗的那些）
    bool syncing_tree_ = false;
    const plugin::Registry* registry_ = nullptr;
    QString decrypt_dir_;
    QString last_gate_error_;
    QStringList load_errors_;

    QTreeWidget* package_list_ = nullptr;
    QComboBox* format_combo_ = nullptr;
    QTreeWidget* entry_tree_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QSplitter* split_list_tree_ = nullptr;   // 包列表 ↔ 包内树
    QWidget* package_pane_ = nullptr;        // 包列表面板（工具行 + 列表）
    QWidget* entry_pane_ = nullptr;          // 条目树面板（工具行 + 树）
    QWidget* package_bar_ = nullptr;
    QWidget* entry_bar_ = nullptr;
    QVBoxLayout* unpack_layout_ = nullptr;
    QVBoxLayout* pack_page_layout_ = nullptr;
    QPushButton* add_button_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
    QLineEdit* search_edit_ = nullptr;
    QLabel* entry_count_label_ = nullptr;
    QLabel* package_count_label_ = nullptr;
    QPushButton* open_out_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QPushButton* check_none_button_ = nullptr;
    QPushButton* unpack_checked_button_ = nullptr;
    QPushButton* unpack_all_button_ = nullptr;
    QComboBox* pack_format_combo_ = nullptr;
    QLineEdit* pack_out_edit_ = nullptr;
    QListWidget* pack_list_ = nullptr;
    QPushButton* pack_button_ = nullptr;
    QLabel* pack_count_label_ = nullptr;
    std::vector<engine::fs::PackFileItem> pack_items_;
    bool rebuilding_package_list_ = false;   // 重建包列表时抑制 itemChanged 回灌
    FileDropFilter* drop_filter_ = nullptr;
    QPushButton* select_all_button_ = nullptr;
    QPushButton* check_all_button_ = nullptr;
};

}  // namespace xrkit::shell
