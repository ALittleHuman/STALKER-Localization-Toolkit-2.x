#include "fontpack_page.h"

#include "../font_picker.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/font/ixr.h"
#include "xrkit/engine/font/langpack.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/system_dirs.h"

#include "../task_shell.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <memory>

namespace xrkit::shell {
namespace {

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// user.ltx 里记住上次字体用的段名/键名（1.x `_CFG_SECTION = "font"` / `"path"`，`:23`/`:135`）
constexpr const char* kFontSection = "font";
constexpr const char* kFontKey = "path";
// ★★ 2026-10-25：**再记一项"玩家选择的字体"**（家族名，如 `NotoSansSC`）——
//   路径会因移动/改名失效，名字不会。与 `path` 在**同一次** `save_user_values` 里写（不做两次写盘）。
constexpr const char* kFontNameKey = "name";

// 字体后缀白名单（1.x `:501-503`：**小写比较**，逐个 fnmatch）
const char* kFontExts[] = {".ttf", ".ttc", ".otf"};

bool font_ext_ok(const QString& name) {
    const QString lower = name.toLower();
    for (const char* ext : kFontExts) {
        if (lower.endsWith(QString::fromLatin1(ext))) return true;
    }
    return false;
}

}  // namespace

FontPackPage::FontPackPage(QWidget* parent)
    : PageBase(QStringLiteral("fontpack"), QStringLiteral("汉化包生成"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    xml_edit_ = new QLineEdit;
    xml_edit_->setObjectName(QStringLiteral("font_xml_dir"));
    xml_edit_->setPlaceholderText(QStringLiteral("文本 XML 目录（只扫顶层）"));
    font_edit_ = new QLineEdit;
    font_edit_->setObjectName(QStringLiteral("font_path"));
    font_edit_->setPlaceholderText(QStringLiteral("字体文件（.ttf/.ttc/.otf）"));
    out_edit_ = new QLineEdit;
    out_edit_->setObjectName(QStringLiteral("font_out_dir"));
    out_edit_->setPlaceholderText(QStringLiteral("输出目录（产物是 <输出>/gamedata）"));

    // 11.8：状态标记与预览标题共用同一判据（引擎）
    state_label_ = new QLabel;
    state_label_->setObjectName(QStringLiteral("font_state"));
    result_label_ = new QLabel;
    result_label_->setObjectName(QStringLiteral("font_result"));

    // ★ 2026-10-04 用户第 2 条（"目录定向"）：判据只有成员函数那一份（自检断言同一份）
    const auto xml_judge = [this]() { return xml_dialog_start(); };
    const auto font_judge = [this]() { return font_dialog_start(); };
    const auto out_judge = [this]() { return out_dialog_start(); };

    const auto path_row = [this](const QString& label, QLineEdit* edit, bool directory,
                                 const QString& title, std::function<DialogStart()> judge) {
        // ★ 2026-10-04 用户第 1 条：整行成容器 ⇒ "拖到相应区域"
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        // ★ P8a：给按钮一个名字 —— 自检要**点真按钮**打开选择器（不是去调 private 方法）
        browse->setObjectName(directory ? QStringLiteral("pick_dir_button")
                                        : QStringLiteral("pick_font_button"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this,
                         [this, edit, directory, title, judge] {
            if (!directory) {
                // ★ D2 批：字体那一行的「选择…」走 **1.x 的自建系统字体选择器**
                //   （`_browse_font`：列系统字体 + 预览 + 缩放 + 记住上次选择）。
                //   这里不再直接弹裸文件框 —— 那正是用户说的"字体默认从系统字体里找"缺的那半。
                open_font_picker();
                return;
            }
            // ★ 2026-10-04 用户第 2 条：①上次的选择 → ②唯一输入目录 → ③工具自身所在文件夹
            const QString start = resolve_dialog_start(judge ? judge() : DialogStart{});
            const QString picked = QFileDialog::getExistingDirectory(this, title, start);
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
            refresh_state();
        });
        drop_zones_.attach_row(container, this);
        return container;
    };
    body_layout()->addWidget(path_row(QStringLiteral("文本 XML："), xml_edit_, true,
                                      QStringLiteral("选择 XML 目录"), xml_judge));
    body_layout()->addWidget(path_row(QStringLiteral("字体："), font_edit_, false,
                                      QStringLiteral("选择字体文件"), font_judge));
    body_layout()->addWidget(path_row(QStringLiteral("输出目录："), out_edit_, true,
                                      QStringLiteral("选择输出目录"), out_judge));

    // ★★★ 2026-10-27（用户）：参照包这一行**必须走同一个 `path_row`** ——
    //   我第一版手搓了一行，结果两个毛病：
    //     ① 没调 `drop_zones_.attach_row(...)` ⇒ **拖不进去**（其它三行都能拖）；
    //     ② 容器没变成拖放区域 ⇒ 少了区域那层样式（用户看到的"一个有一个黑底、一个没有"）。
    //   修法就是**别再手搓**：同一份 helper 造行，拖放与样式一起继承。
    pack_edit_ = new QLineEdit;
    pack_edit_->setObjectName(QStringLiteral("font_ixr_pack"));
    pack_edit_->setPlaceholderText(
        QStringLiteral("参照整包目录（例如 MODS\\~NotoSansSC，能当模组打的那份）"));
    const auto pack_judge = [this]() {
        return dialog_start_for(pack_edit_->text().trimmed(), QStringList{}, false);
    };
    {
        QWidget* pack_row = path_row(QStringLiteral("参照包："), pack_edit_, true,
                                     QStringLiteral("选择参照整包目录"), pack_judge);
        // 按钮改回它自己的名字（`pick_dir_button` 已被上面三行占用；
        // 同名会让"按名字点按钮"的自检点到别人身上）
        for (QPushButton* button : pack_row->findChildren<QPushButton*>()) {
            button->setObjectName(QStringLiteral("font_ixr_pack_browse"));
        }
        // ★★★ 2026-10-27（用户：「他妈的自己看看颜色」）：**别再给这一行 setObjectName** ——
        //   `DropZones::attach_row` 会给容器挂 `field_row`，QSS 里
        //   `QWidget#field_row { background: transparent; }` 才让它"不涂底"。
        //   我上一版把它改名成 `font_ixr_pack_row` ⇒ 命中泛化规则
        //   `QWidget { background: @{bg}; }` ⇒ 标签背后出现一块深灰底板（用户截图那一条）。
        //   要定位这一行就用动态属性，别动 objectName。
        pack_row->setProperty("ixr_pack_row", true);
        pack_row_ = pack_row;
        body_layout()->addWidget(pack_row);

        // ★★ 2026-10-27（用户：「那扫描生成的怎么弄？」）：**扫描参照包** ——
        //   与 CLI `--langpack-scan` / 插件页同一个引擎实现（`scan_lang_pack`）。
        //   扫描结果直接写在状态行：profile / lang / 段数 / ttf 数（缺什么也写）。
        auto* scan_row = new QWidget;
        scan_row->setObjectName(QStringLiteral("font_ixr_scan_row"));
        auto* scan_layout = new QHBoxLayout(scan_row);
        scan_layout->setContentsMargins(0, 0, 0, 0);
        auto* scan_button = new QPushButton(QStringLiteral("扫描参照包"));
        scan_button->setObjectName(QStringLiteral("font_ixr_scan"));
        QObject::connect(scan_button, &QPushButton::clicked, this, [this] {
            const QString pack = pack_edit_ != nullptr ? pack_edit_->text().trimmed() : QString();
            if (pack.isEmpty()) {
                if (result_label_ != nullptr) result_label_->setText(QStringLiteral("先选「参照包」目录"));
                return;
            }
            const engine::font::LangPackModel model =
                engine::font::scan_lang_pack(pack.toUtf8().toStdString());
            // ★ 自动判断档位：ixr → IX-Ray；cop → CoP；cis → ClS；soc → SoC
            const char* detected = engine::font::profile_name(model.profile);
            if (std::string(detected) != "unknown") {
                const QString want = QString::fromStdString(detected) == QStringLiteral("ixr")
                                         ? QStringLiteral("IX-Ray")
                                     : QString::fromStdString(detected) == QStringLiteral("cop")
                                         ? QStringLiteral("CoP")
                                     : QString::fromStdString(detected) == QStringLiteral("cis")
                                         ? QStringLiteral("ClS")
                                         : QStringLiteral("SoC");
                const int idx = game_combo_->findData(want);
                if (idx >= 0) game_combo_->setCurrentIndex(idx);
            }
            // 预填文本目录（用户没填时才填）：<参照包>/gamedata/configs/text/<lang>
            if (xml_edit_ != nullptr && xml_edit_->text().trimmed().isEmpty() &&
                !model.text_dir.empty()) {
                xml_edit_->setText(QDir::toNativeSeparators(QString::fromStdString(model.text_dir)));
            }
            QString text = QStringLiteral("扫描：profile=%1（已自动切档位）lang=%2 段=%3 ttf=%4")
                               .arg(QString::fromStdString(detected))
                               .arg(QString::fromStdString(model.lang))
                               .arg(model.font_slots.size())
                               .arg(model.ttf_count);
            for (const std::string& line : model.evidence) {
                text += QStringLiteral("｜") + QString::fromStdString(line);
            }
            if (state_label_ != nullptr) state_label_->setText(text);
        });
        scan_layout->addWidget(scan_button, 0);
        scan_layout->addStretch(1);
        scan_row_ = scan_row;
        body_layout()->addWidget(scan_row);
        scan_row->setVisible(true);
        // ★★ 2026-10-27（用户）：「扫描是对全局生效的」—— 参照包行与扫描按钮**始终可见**，
        //   不跟 IX-Ray 绑定；扫描结果会**自动判断** SoC/ClS/CoP/IX-Ray 并切下拉。
        pack_row->setVisible(true);
        QObject::connect(game_combo_, &QComboBox::currentTextChanged, this,
                         [this](const QString&) {
                             const bool ixr = game_combo_->currentData().toString() ==
                                              QStringLiteral("IX-Ray");
                             // 位图专属的两个选项在 IX-Ray 下没意义 ⇒ 置灰（不隐藏，免得用户找不到）
                             if (size_combo_ != nullptr) size_combo_->setEnabled(!ixr);
                             if (cyrillic_box_ != nullptr) cyrillic_box_->setEnabled(!ixr);
                         });
    }
    game_combo_ = new QComboBox;
    game_combo_->setObjectName(QStringLiteral("font_game"));
    // ★★ 2026-10-27（用户：「汉化包生成现在还是没有 IX-Ray 的选项」）：
    //   第四项 = IX-Ray（**矢量**）。它不是位图模板，走外部 fonttools 助手
    //   （见 engine::font::ixr_build 与 tools/ixr_run.py），需要用户给一个"参照整包"。
    for (const char* game : {"SoC", "ClS", "CoP", "IX-Ray"}) {
        game_combo_->addItem(utf8(game), utf8(game));
    }
    // 1.x `:71-82`：加载语言 = eng/rus/chs/**自定义**（自定义才显示输入框）
    lang_combo_ = new QComboBox;
    lang_combo_->setObjectName(QStringLiteral("font_lang"));
    for (const char* lang : {"eng", "rus", "chs"}) lang_combo_->addItem(utf8(lang), utf8(lang));
    lang_combo_->addItem(QStringLiteral("自定义"), QStringLiteral("自定义"));
    lang_combo_->setCurrentText(QStringLiteral("chs"));
    lang_edit_ = new QLineEdit;
    lang_edit_->setObjectName(QStringLiteral("font_lang_custom"));
    lang_edit_->setPlaceholderText(QStringLiteral("自定义"));
    lang_edit_->setMaximumWidth(xrkit::core::px(120));
    lang_edit_->setVisible(false);
    // 1.x `:91-102`：字体后缀 = 无/_chs/**自定义**
    suffix_combo_ = new QComboBox;
    suffix_combo_->setObjectName(QStringLiteral("font_suffix"));
    suffix_combo_->addItem(QStringLiteral("无"), QString());
    suffix_combo_->addItem(QStringLiteral("_chs"), QStringLiteral("_chs"));
    suffix_combo_->addItem(QStringLiteral("自定义"), QStringLiteral("自定义"));
    suffix_edit_ = new QLineEdit;
    suffix_edit_->setObjectName(QStringLiteral("font_suffix_custom"));
    suffix_edit_->setPlaceholderText(QStringLiteral("自定义"));
    suffix_edit_->setMaximumWidth(xrkit::core::px(120));
    suffix_edit_->setVisible(false);
    // 1.x `:84-89`：尺寸档位（标准 = 0，其余是 `PackageOptions::offset`）
    size_combo_ = new QComboBox;
    size_combo_->setObjectName(QStringLiteral("font_size"));
    size_combo_->addItem(QStringLiteral("标准"), 0);
    for (int offset : {3, 5, 7, 9}) size_combo_->addItem(QStringLiteral("+%1").arg(offset), offset);
    cyrillic_box_ = new QCheckBox(QStringLiteral("包含完整西里尔"));
    cyrillic_box_->setObjectName(QStringLiteral("font_full_cyrillic"));
    cyrillic_box_->setChecked(true);
    auto* opt_row = new QHBoxLayout;
    opt_row->addWidget(new QLabel(QStringLiteral("游戏：")), 0);
    opt_row->addWidget(game_combo_, 0);
    opt_row->addWidget(new QLabel(QStringLiteral("语言：")), 0);
    opt_row->addWidget(lang_combo_, 0);
    opt_row->addWidget(lang_edit_, 0);
    opt_row->addWidget(new QLabel(QStringLiteral("尺寸：")), 0);
    opt_row->addWidget(size_combo_, 0);
    opt_row->addWidget(new QLabel(QStringLiteral("后缀：")), 0);
    opt_row->addWidget(suffix_combo_, 0);
    opt_row->addWidget(suffix_edit_, 0);
    opt_row->addWidget(cyrillic_box_, 0);
    opt_row->addStretch(1);
    body_layout()->addLayout(opt_row);
    QObject::connect(lang_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { sync_lang_edit(); });
    QObject::connect(suffix_combo_, &QComboBox::currentTextChanged, this,
                     [this](const QString&) { sync_suffix_edit(); });
    sync_lang_edit();
    sync_suffix_edit();

    body_layout()->addWidget(state_label_, 0);
    body_layout()->addWidget(result_label_, 0);
    body_layout()->addStretch(1);

    // ★★ 2026-10-06（② 用户拍板）：「**封包和字库的取消，在这里本质是完全没有的，故直接删除
    //   即可。**」⇒ 本页按 `CancelButton::No` 造任务壳：**连取消按钮都不创建**。
    //   事实依据：字库打包 + 原子写盘不可中途停止（修前那段"已取消（请求已记录）：…不可中途
    //   停止"就是在如实说明这件事）⇒ 这颗按钮在本页"本质是完全没有的"。
    task_ = new TaskShell(this, TaskShell::CancelButton::No);
    task_->set_idle_status(QStringLiteral("就绪"));
    // ★★ P17-1（03 F-01）：worker 算完的结果由**主线程收尾回调**画到控件上。
    //   这条回调在 `TaskShell::apply_finish` 的最后、于主线程执行 ⇒ 可以碰控件。
    task_->set_finish_handler([this] {
        if (std::shared_ptr<const BuildOutcome> outcome = take_pending_outcome()) {
            apply_result(*outcome);
        }
    });
    body_layout()->addWidget(task_->panel(), 0);

    auto* detect = add_action(QStringLiteral("检测字体"));
    detect->setObjectName(QStringLiteral("font_detect"));
    QObject::connect(detect, &QPushButton::clicked, this, [this] { refresh_state(); });
    // 1.x `:155`：`打开输出目录`（原来 2.x 没有这颗 —— 用户没入口去看产物）
    open_out_button_ = add_action(QStringLiteral("打开输出目录"));
    open_out_button_->setObjectName(QStringLiteral("font_open_out"));
    QObject::connect(open_out_button_, &QPushButton::clicked, this, [this] {
        QString problem;
        if (!open_output(&problem)) result_label_->setText(problem);
    });
    auto* build_button = add_action(QStringLiteral("生成汉化包"));
    build_button->setObjectName(QStringLiteral("font_build"));
    QObject::connect(build_button, &QPushButton::clicked, this, [this] {
        const QString xml_dir = xml_edit_->text().trimmed();
        const QString font = font_edit_->text().trimmed();
        const QString out = out_edit_->text().trimmed();
        if (xml_dir.isEmpty() || font.isEmpty() || out.isEmpty()) {
            result_label_->setText(QStringLiteral("请把三个路径都填上"));
            return;
        }
        // ★★ 2026-10-10（P17-1，审计 03 F-01 Critical）：**主线程先把控件读成值**
        //   （`prepare`），worker 只拿这份冻结值与引擎打交道；算完由**主线程收尾回调**
        //   画到控件上（`apply_result`）。修前 worker 直接调 `build()` ⇒ 跨线程读下拉框、
        //   写 `result_label_`/`state_label_`、裸 `++refused_count_`（数据竞争），
        //   并且页析构时 `result_label_` 先于 `task_` 被删 ⇒ 访问违例。
        // ★ IX-Ray：参照包在**主线程**快照（worker 不许读控件）
        ixr_pack_at_start_ = pack_edit_ != nullptr ? pack_edit_->text().trimmed() : QString();
        // ★★ 2026-10-27（用户：「生成结果是俄语包而非汉化包」）：**字符集来自「文本 XML」**——
        //   那一栏才是"要汉化的文本"。参照包只提供字体与 fonts.ltx（结构），
        //   字符集若只读参照包的 `configs/text/<lang>`，出来的永远是俄语字形集。
        ixr_text_at_start_ = xml_edit_ != nullptr ? xml_edit_->text().trimmed() : QString();
        if (game_combo_->currentData().toString() == QStringLiteral("IX-Ray") &&
            ixr_pack_at_start_.isEmpty()) {
            result_label_->setText(QStringLiteral("IX-Ray 需要「参照包」（能当模组打的整包目录）"));
            return;
        }
        const engine::font::PackageOptions options = prepare(xml_dir, font, out);
        auto outcome = std::make_shared<BuildOutcome>();
        const bool started = task_->run(QStringLiteral("生成汉化包"),
                                        [this, options, outcome, out](const ProgressSink& progress,
                                                                      const MessageSink& message) {
            progress(0);
            // ★ worker 里**只调引擎**：`run_build` 不碰任何控件（碰了会被 UI 线程守卫拒）。
            *outcome = run_build(options);
            if (outcome->refused) {
                // 11.9：拒绝式检查 —— 这里**没有**任何目录被建出来
                message(outcome->reason.toUtf8().toStdString());
                progress(100);
                publish_outcome(outcome);
                return;
            }
            if (!outcome->ok) {
                message(outcome->reason.toUtf8().toStdString());
                progress(100);
                publish_outcome(outcome);
                return;
            }
            message(QStringLiteral("产物写到 %1/gamedata").arg(out).toUtf8().toStdString());
            progress(100);
            publish_outcome(outcome);
        });
        if (!started) {
            // 13.1 重入闸门拒绝（已有任务在跑）：什么都不许改（结果也还没算）
            result_label_->setText(QStringLiteral("已有任务在跑，请等它结束"));
        }
    });

    // ★ D2 批：默认字体（1.x `:129-149` 的三级优先级）—— 上次用过的 → 系统雅黑 → 仓库根。
    const QString saved = utf8(xrkit::core::load_user_value(kFontSection, kFontKey));
    const QString initial = default_font_path(saved);
    if (!initial.isEmpty()) font_edit_->setText(initial);
    refresh_state();
}

// ★★ P17-1（03 F-01）：**先把 worker 收干净，再让子控件被删**。
//   `result_label_`/`state_label_` 是页的子控件，Qt 按登记顺序删 ⇒ 它们**先于** `task_`
//   被删除（`task_` 是后造的）；`~TaskShell` 的 join 发生在 `task_` 被删的那一刻 ——
//   修前 worker 还在往那些已经被删的 QLabel 上 setText（访问违例）。现在 worker 一个字
//   都不碰控件（见 prepare/run_build/apply_result 三步），这里再把"析构期还有线程在跑"
//   这个窗口也关掉：析构函数体里就置取消位 + join。
FontPackPage::~FontPackPage() {
    if (task_ != nullptr) task_->stop_and_wait();
}

void FontPackPage::sync_lang_edit() {
    if (lang_edit_ == nullptr || lang_combo_ == nullptr) return;
    const bool custom = lang_combo_->currentText() == QStringLiteral("自定义");
    lang_edit_->setVisible(custom);
    if (!custom) lang_edit_->clear();
}

void FontPackPage::sync_suffix_edit() {
    if (suffix_edit_ == nullptr || suffix_combo_ == nullptr) return;
    const bool custom = suffix_combo_->currentText() == QStringLiteral("自定义");
    suffix_edit_->setVisible(custom);
    if (!custom) suffix_edit_->clear();
}

int FontPackPage::size_offset() const {
    if (size_combo_ == nullptr) return 0;
    return size_combo_->currentData().toInt();
}

QDialog* FontPackPage::open_font_picker() {
    // ★★ P8a：选择器是**宿主唯一一份**（`font_picker.cpp`）—— 页面与 `api.ui.pick_font` 共用它。
    //   "选中之后怎么办"由这里回调决定（页面：写进输入框 + 记住到 user.ltx + 报账）。
    //   返回对话框**是为了自检能驱动它**（双击＝选择那条链要验的就是页面这份接线）。
    const QString current = font_edit_ != nullptr ? font_edit_->text().trimmed() : QString();
    if (QDialog* dialog = make_font_picker(
            this, current, [this]() { return font_dialog_start(); },
            [this](const QString& path) {
                QString problem;
                apply_font_choice(path, &problem);
                if (!problem.isEmpty() && result_label_ != nullptr) result_label_->setText(problem);
            },
            [this](const QString& problem) {
                if (!problem.isEmpty() && result_label_ != nullptr) result_label_->setText(problem);
            })) {
        dialog->show();
        return dialog;
    }
    return nullptr;
}

bool FontPackPage::remember_font(const QString& path, QString* problem) {
    // 1.x `:598-607`：记住这次选的字体（写 user.ltx 的 `[font] path`，**保留其它段**）。
    // 失败**必须明确告警**（"下次启动需重新选择字体"），绝不静默。
    if (path.isEmpty()) return true;
    // ★★ 2026-10-25：`path` + `name` **一次写**（保留其它段；失败整条回滚不落半份）。
    const QString family = xrkit::shell::font_family_name(path);
    const std::vector<std::pair<std::string, std::string>> values = {
        {kFontKey, path.toUtf8().toStdString()},
        {kFontNameKey, family.toUtf8().toStdString()},
    };
    if (!xrkit::core::save_user_values(kFontSection, values)) {
        if (problem != nullptr) {
            *problem = QStringLiteral("警告: 无法写入 user.ltx，下次启动需重新选择字体");
        }
        log_to_host(QStringLiteral("警告: 无法写入 user.ltx，下次启动需重新选择字体"));
        return false;
    }
    if (problem != nullptr) problem->clear();
    return true;
}

bool FontPackPage::apply_font_choice(const QString& path, QString* problem) {
    if (problem != nullptr) problem->clear();
    if (path.isEmpty()) return false;
    if (font_edit_ != nullptr) font_edit_->setText(QDir::toNativeSeparators(path));
    refresh_state();
    return remember_font(path, problem);
}

bool FontPackPage::open_output(QString* problem) {
    const QString dir = out_edit_ != nullptr ? out_edit_->text().trimmed() : QString();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        // 1.x `:649-654`：目录无效要明确说，不是点了没反应
        if (problem != nullptr) *problem = QStringLiteral("输出目录无效：") + dir;
        return false;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        // 1.x `:656-659`：`open_in_explorer` 失败原先忽略返回值 ⇒ 点了没反馈
        if (problem != nullptr) {
            *problem = QStringLiteral("打开失败：%1（可能缺少文件管理器关联或权限不足，请手动打开）")
                           .arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }
    if (problem != nullptr) problem->clear();
    return true;
}

engine::font::CjkFontCheck FontPackPage::check_font(const QString& font_path) const {
    // 11.8 的判据**只有引擎那一份**（列表标记与预览标题共用它）
    if (font_path.isEmpty()) return engine::font::CjkFontCheck{};
    return engine::font::classify_cjk_font(font_path.toUtf8().toStdString(), 40);
}

QString FontPackPage::font_state_text(const QString& font_path) const {
    const engine::font::CjkFontCheck check = check_font(font_path);
    QString text = utf8(engine::font::cjk_state_mark(check.state));
    if (!check.missing.empty()) {
        text += QStringLiteral("　缺 %1 个字").arg(check.missing.size());
        if (!check.fallbacks.empty()) {
            text += QStringLiteral("，其中 %1 个可用繁体顶").arg(check.fallbacks.size());
        }
    }
    if (font_path.isEmpty()) text = QStringLiteral("未选择字体");
    return text;
}

void FontPackPage::refresh_state() {
    if (state_label_ == nullptr) return;
    state_label_->setText(QStringLiteral("字体状态：") + font_state_text(font_edit_->text().trimmed()));
}

engine::font::PackageOptions FontPackPage::options_for(const QString& xml_dir,
                                                       const QString& font_path,
                                                       const QString& out_dir) const {
    // ★★ P17-1：这是"读控件"的一条路 ⇒ 必须过 UI 线程守卫（worker 里调 = 拒绝 + 记账）。
    if (!ui_thread_guard("FontPackPage::options_for")) return engine::font::PackageOptions{};
    engine::font::PackageOptions options;
    options.game = game_combo_->currentData().toString().toUtf8().toStdString();
    // 语言：选"自定义"时用输入框里的值（1.x `_generate` `:667-672` 的口径）
    if (lang_combo_->currentText() == QStringLiteral("自定义")) {
        options.lang = lang_edit_->text().trimmed().toUtf8().toStdString();
    } else {
        options.lang = lang_combo_->currentData().toString().toUtf8().toStdString();
    }
    options.xml_dir = xml_dir.toUtf8().toStdString();
    options.font_path = font_path.toUtf8().toStdString();
    options.out_dir = out_dir.toUtf8().toStdString();
    // 后缀：无 ⇒ 空串；_chs ⇒ `_chs`；自定义 ⇒ 输入框（1.x `:674-679`）
    if (suffix_combo_->currentText() == QStringLiteral("自定义")) {
        options.suffix = suffix_edit_->text().trimmed().toUtf8().toStdString();
    } else {
        options.suffix = suffix_combo_->currentData().toString().toUtf8().toStdString();
    }
    // 1.x `:692`：`offset = int(off_txt[1:]) if off_txt.startswith("+") else 0`
    options.offset = size_offset();
    options.full_cyrillic = cyrillic_box_->isChecked();
    return options;
}

// ★★ P17-1（03 F-01）：主线程冻结控件值 —— worker 只拿这一份（见 fontpack_page.h）。
engine::font::PackageOptions FontPackPage::prepare(const QString& xml_dir, const QString& font_path,
                                                   const QString& out_dir) const {
    return options_for(xml_dir, font_path, out_dir);
}

// ★★ P17-1：**worker 可调**的那一步 —— 只做引擎的事（判字形 + 出包），一个字都不碰控件。
//   因此它可以在工作线程里跑（重活就该在这儿跑），而"画到控件上"留给主线程的 apply_result。
FontPackPage::BuildOutcome FontPackPage::run_build(
    const engine::font::PackageOptions& options) const {
    BuildOutcome outcome;
    // ★★ 2026-10-27：IX-Ray（矢量）—— 走外部 fonttools 助手（`engine::font::ixr_build`）。
    //   与位图出包**并列**的一条路：不判字形（助手自己会报缺字数），不做"先判后建"的目录守卫
    //   （助手先 copy 整包再改字体；失败时它自己删临时产物）。
    if (options.game == "IX-Ray") {
        outcome.ixr = true;
        xrkit::engine::font::IxrBuildOptions ixr;
        ixr.pack = ixr_pack_at_start_.toStdString();
        ixr.font = options.font_path;
        ixr.out = options.out_dir;
        ixr.text_root = ixr_text_at_start_.toStdString();
        const xrkit::engine::font::IxrBuildResult result =
            xrkit::engine::font::ixr_build(ixr);
        outcome.ok = result.ok;
        outcome.result.ok = result.ok;
        if (!result.ok) {
            outcome.result.error = result.error;
            outcome.reason = utf8(result.error);
            outcome.ixr_log = utf8(result.stdout_tail) + utf8(result.stderr_tail);
        } else {
            // 助手报告在 stdout 的 `IXRGEN {...}` 里 —— **按 JSON 解析**（不是数子串：
            //   子串统计既脆、又会被线程契约的静态判据误当成"碰控件"）。
            const QString all = utf8(result.stdout_tail);
            const int marker = all.indexOf(QStringLiteral("IXRGEN {"));
            if (marker >= 0) {
                const int line_end = all.indexOf(QChar('\n'), marker);
                const QString json_text = all.mid(
                    marker + 7, line_end < 0 ? -1 : line_end - marker - 7).trimmed();
                QJsonParseError parse_error{};
                const QJsonDocument doc = QJsonDocument::fromJson(json_text.toUtf8(), &parse_error);
                if (parse_error.error == QJsonParseError::NoError && doc.isObject()) {
                    const QJsonObject obj = doc.object();
                    outcome.ixr_added = obj.value(QStringLiteral("added")).toInt();
                    outcome.ixr_missing = obj.value(QStringLiteral("missing")).toInt();
                    for (const QJsonValue& value : obj.value(QStringLiteral("reports")).toArray()) {
                        const QJsonObject one = value.toObject();
                        outcome.ixr_fonts +=
                            one.value(QStringLiteral("fonts")).toArray().size();
                        outcome.ixr_texts += one.value(QStringLiteral("texts")).toInt();
                    }
                }
            }
        }
        return outcome;
    }
    // 11.9：**先判后建** —— 缺字形/检测失败直接拒绝，且**不建任何目录**。
    // （`font_unrenderable` 只告警不拒绝：那条路在引擎的 build_package 里以 warning 体现。）
    // 判据**只有** `check_font` 那一份（引擎的 `classify_cjk_font`），页面不自己再判一遍。
    outcome.check = check_font(utf8(options.font_path));
    if (outcome.check.state == engine::font::CjkFontState::Missing ||
        outcome.check.state == engine::font::CjkFontState::Unknown) {
        outcome.refused = true;
        // 原子量：worker 里递增（修前是裸 `int` ⇒ 数据竞争）
        refused_count_.fetch_add(1);
        outcome.reason = QStringLiteral("拒绝生成：%1（缺字形或字体不可用；没有创建任何目录）")
                             .arg(utf8(engine::font::cjk_state_mark(outcome.check.state)));
        return outcome;
    }
    outcome.result = engine::font::build_package(options);
    outcome.ok = outcome.result.ok;
    if (!outcome.ok) {
        outcome.reason = utf8(outcome.result.error);
        return outcome;
    }
    outcome.fallback_used = !outcome.check.fallbacks.empty();
    return outcome;
}

// ★★ P17-1：**主线程**把结果画到控件上（守卫保证它不在 worker 里被调）。
void FontPackPage::apply_result(const BuildOutcome& outcome) {
    if (!ui_thread_guard("FontPackPage::apply_result")) return;
    if (outcome.refused) {
        if (result_label_ != nullptr) result_label_->setText(outcome.reason);
        return;
    }
    if (outcome.ixr) {
        if (result_label_ != nullptr) {
            result_label_->setText(
                outcome.ok
                    ? QStringLiteral("完成（IX-Ray 矢量）：字体 %1 个、文本 %2 个、"
                                     "新增字形 %3、缺字 %4（产物可直接当模组用）")
                          .arg(outcome.ixr_fonts)
                          .arg(outcome.ixr_texts)
                          .arg(outcome.ixr_added)
                          .arg(outcome.ixr_missing)
                    : QStringLiteral("失败：") + outcome.reason);
        }
        return;
    }
    if (result_label_ != nullptr) {
        result_label_->setText(
            outcome.result.ok
                ? QStringLiteral("完成：字体 %1 个、XML %2 个%3")
                      .arg(outcome.result.fonts_written)
                      .arg(outcome.result.xml_written)
                      .arg(outcome.result.warnings.empty()
                               ? QString()
                               : QStringLiteral("（%1 条告警）").arg(outcome.result.warnings.size()))
                : QStringLiteral("失败：") + utf8(outcome.result.error));
    }
    if (outcome.fallback_used && state_label_ != nullptr) {
        state_label_->setText(state_label_->text() + QStringLiteral("（已按繁体回退生成）"));
    }
}

// ★★ P17-1：worker 算完 ⇒ 交给主线程收尾（`TaskShell::set_finish_handler`，见构造函数）。
void FontPackPage::publish_outcome(std::shared_ptr<const BuildOutcome> outcome) {
    std::lock_guard<std::mutex> lock(outcome_mutex_);
    pending_outcome_ = std::move(outcome);
}

std::shared_ptr<const FontPackPage::BuildOutcome> FontPackPage::take_pending_outcome() {
    std::lock_guard<std::mutex> lock(outcome_mutex_);
    std::shared_ptr<const BuildOutcome> outcome = pending_outcome_;
    pending_outcome_.reset();
    return outcome;
}

bool FontPackPage::build(const QString& xml_dir, const QString& font_path, const QString& out_dir,
                         QString* reason) {
    // ★★ P17-1：本方法＝三步合一，**主线程专用**（自检与单线程调用走它）。
    //   worker 里调它 ⇒ 守卫在第一句就拒绝并记账，控件一个字都不许变（smoke 有这条判据）。
    if (!ui_thread_guard("FontPackPage::build")) {
        if (reason != nullptr) {
            *reason = QStringLiteral("线程纪律：本方法只许在主线程调用");
        }
        return false;
    }
    const BuildOutcome outcome = run_build(prepare(xml_dir, font_path, out_dir));
    apply_result(outcome);
    if (reason != nullptr) *reason = outcome.reason;
    return outcome.ok;
}

// ★ 2026-10-04 用户第 2 条（"目录定向"）：这三个函数是各"选择"调用点的判据唯一实现 ——
//   按钮 lambda 绑它们（见构造期的 `*_judge`），自检也断言它们。
DialogStart FontPackPage::xml_dialog_start() const {
    // 文本 XML 是这一页的**第一输入** ⇒ 空时落到"工具自身所在文件夹"
    return dialog_start_for(xml_edit_->text(), QStringList(), false);
}

DialogStart FontPackPage::font_dialog_start() const {
    // 字体 ← 文本 XML 所在目录（唯一才用；XML 是目录输入）
    return dialog_start_for(font_edit_->text(), QStringList{xml_edit_->text()}, false);
}

DialogStart FontPackPage::out_dialog_start() const {
    // 输出目录 ← 文本 XML 目录（唯一才用）
    return dialog_start_for(out_edit_->text(), QStringList{xml_edit_->text()}, false);
}

}  // namespace xrkit::shell
