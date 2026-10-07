#include "fontpack_page.h"

#include "xrkit/core/theme.h"
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

    game_combo_ = new QComboBox;
    game_combo_->setObjectName(QStringLiteral("font_game"));
    for (const char* game : {"SoC", "ClS", "CoP"}) game_combo_->addItem(utf8(game), utf8(game));
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
        task_->run(QStringLiteral("生成汉化包"), [this, xml_dir, font, out](
                                                     const ProgressSink& progress,
                                                     const MessageSink& message) {
            progress(0);
            // ★★ 2026-10-06（② 用户拍板）：**本页不再有取消** —— 修前这里有两处轮询
            //   （"开始之前"的空转取消 + "生成中收到请求 ⇒ 如实说明不可中断"），
            //   连同任务壳的取消按钮一起整块删掉了（用户：「本质是完全没有的，故直接删除即可」）。
            QString reason;
            if (!build(xml_dir, font, out, &reason)) {
                // 11.9：拒绝式检查 —— 这里**没有**任何目录被建出来
                message(reason.toUtf8().toStdString());
                progress(100);
                return;
            }
            message(QStringLiteral("产物写到 %1/gamedata").arg(out).toUtf8().toStdString());
            progress(100);
        });
    });

    // ★ D2 批：默认字体（1.x `:129-149` 的三级优先级）—— 上次用过的 → 系统雅黑 → 仓库根。
    const QString saved = utf8(xrkit::core::load_user_value(kFontSection, kFontKey));
    const QString initial = default_font_path(saved);
    if (!initial.isEmpty()) font_edit_->setText(initial);
    refresh_state();
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

// ── 系统字体（1.x `system_font_dirs` `:222-234` + `_browse_font` 的目录补充）──────
QStringList FontPackPage::system_font_dirs() const {
    QStringList dirs;
    // ★★ P7b：系统字体目录**从引擎拿**（唯一一份：页面与 `api.host.system_font_dirs()` 共用）
    for (const std::string& dir : engine::font::system_font_dirs()) {
        const QString qdir = utf8(dir);
        if (!dirs.contains(qdir)) dirs << qdir;
    }
    // 1.x `:489-493`：**当前已选字体所在目录**也加进候选（用户自带的字体就在那儿）
    const QString current = font_edit_ != nullptr ? font_edit_->text().trimmed() : QString();
    if (!current.isEmpty()) {
        const QString parent = QFileInfo(current).absolutePath();
        if (!parent.isEmpty() && !dirs.contains(parent)) dirs << parent;
    }
    // 存在的排前面（1.x 只把"存在的目录"返回；这里保留全部但把存在的提前，好让
    // "为什么不列出来"这件事可解释 —— 目录不存在时列表为空而不是"看起来没找到字体"）
    QStringList existing;
    QStringList missing;
    for (const QString& dir : dirs) {
        (QFileInfo(dir).isDir() ? existing : missing) << dir;
    }
    return existing + missing;
}

QStringList FontPackPage::font_candidates(const QString& keyword) const {
    QStringList out;
    QSet<QString> seen;   // **完整路径**去重（1.x `seen` 集合，`:494`/`:507`）
    const QString word = keyword.trimmed().toLower();
    for (const QString& dir : system_font_dirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        const QStringList names = d.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& name : names) {
            if (!font_ext_ok(name)) continue;                       // 白名单后缀
            if (!word.isEmpty() && !name.toLower().contains(word)) continue;  // 子串过滤
            const QString full = QDir::toNativeSeparators(d.filePath(name));
            if (seen.contains(full)) continue;
            seen.insert(full);
            out << full;
        }
    }
    return out;
}

QString FontPackPage::default_font_path(const QString& saved) const {
    // ① 上次用过的（user.ltx 的 `[font] path`，**文件仍存在**才算）
    if (!saved.isEmpty() && QFileInfo::exists(saved)) return QDir::toNativeSeparators(saved);
    // ② 系统微软雅黑（按名字优先级逐个目录找）
    for (const QString& dir : system_font_dirs()) {
        for (const char* name : {"msyh.ttc", "msyh.ttf", "msyhl.ttc"}) {
            const QString cand = QDir(dir).filePath(QString::fromLatin1(name));
            if (QFileInfo::exists(cand)) return QDir::toNativeSeparators(cand);
        }
    }
    // ③ 仓库根 / exe 所在目录的 msyh.ttf（1.x 的预留分支；源码版没有这个文件）
    const QString local =
        QDir(QString::fromStdString(xrkit::core::app_dir())).filePath(QStringLiteral("msyh.ttf"));
    if (QFileInfo::exists(local)) return QDir::toNativeSeparators(local);
    return QString();
}

QString FontPackPage::cached_mark(const QString& font_path) {
    if (font_path.isEmpty()) return QString();
    const auto it = font_mark_cache_.find(font_path);
    if (it != font_mark_cache_.end()) return it->second;
    // 判据**只有引擎那一份**（列表标记与预览标题共用它 ⇒ 两处结论不可能矛盾）
    const QString mark = utf8(engine::font::cjk_state_mark(check_font(font_path).state));
    font_mark_cache_.emplace(font_path, mark);
    return mark;
}

void FontPackPage::zoom_preview(double delta) {
    // 1.x `set_zoom`：`max(0.5, min(4.0, round(v + delta, 1)))`
    double next = preview_zoom_ + delta;
    next = std::round(next * 10.0) / 10.0;
    preview_zoom_ = next < 0.5 ? 0.5 : (next > 4.0 ? 4.0 : next);
}

int FontPackPage::preview_pixel_size() const {
    // 1.x `:568`：`size = max(12, int(40 * zoom))`
    const int size = static_cast<int>(40.0 * preview_zoom_);
    return size < 12 ? 12 : size;
}

QString FontPackPage::preview_text_line(const QString& font_path, int which) const {
    // 1.x `:570`：两行样本（第二行带西里尔与数字）
    QString line = which == 0 ? QStringLiteral("汉化测试你好ABC") : QStringLiteral("Привет 世界 123");
    // 用户口径 2026-09-14：「日语的，标明是日语字体，然后那些缺简体的就上繁体」——
    // 预览里直接把缺的简体字换成繁体字形（否则那几格是空白，用户看到的还是"缺字"）。
    const engine::font::CjkFontCheck check = check_font(font_path);
    if (check.state == engine::font::CjkFontState::Japanese) {
        QString replaced;
        for (const QChar ch : line) {
            const std::string key = QString(ch).toUtf8().toStdString();
            const auto it = check.fallbacks.find(key);
            replaced += it == check.fallbacks.end() ? QString(ch) : utf8(it->second);
        }
        line = replaced;
    }
    return line;
}

void FontPackPage::open_font_picker() {
    if (QDialog* dialog = make_font_picker(this)) dialog->show();
}

QDialog* FontPackPage::make_font_picker(QWidget* parent) {
    auto* dialog = new QDialog(parent != nullptr ? parent : this);
    dialog->setObjectName(QStringLiteral("font_picker"));
    dialog->setWindowTitle(QStringLiteral("选择字体文件"));
    dialog->resize(xrkit::core::px(900), xrkit::core::px(520));
    auto* outer = new QVBoxLayout(dialog);

    // 过滤框（1.x `:293-296`：`kw_var.trace_add("write", refresh)`）
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("过滤："), dialog), 0);
    auto* keyword = new QLineEdit(dialog);
    keyword->setObjectName(QStringLiteral("font_picker_filter"));
    bar->addWidget(keyword, 1);
    outer->addLayout(bar);

    auto* body = new QHBoxLayout;
    auto* list = new QListWidget(dialog);
    list->setObjectName(QStringLiteral("font_picker_list"));
    body->addWidget(list, 3);
    auto* right = new QVBoxLayout;
    auto* zoom_row = new QHBoxLayout;
    zoom_row->addWidget(new QLabel(QStringLiteral("预览"), dialog), 0);
    zoom_row->addStretch(1);
    auto* zoom_out = new QPushButton(QStringLiteral("−"), dialog);
    zoom_out->setObjectName(QStringLiteral("font_picker_zoom_out"));
    zoom_out->setMaximumWidth(xrkit::core::px(40));
    auto* zoom_label = new QLabel(QStringLiteral("100%"), dialog);
    zoom_label->setObjectName(QStringLiteral("font_picker_zoom"));
    auto* zoom_in = new QPushButton(QStringLiteral("+"), dialog);
    zoom_in->setObjectName(QStringLiteral("font_picker_zoom_in"));
    zoom_in->setMaximumWidth(xrkit::core::px(40));
    zoom_row->addWidget(zoom_out, 0);
    zoom_row->addWidget(zoom_label, 0);
    zoom_row->addWidget(zoom_in, 0);
    right->addLayout(zoom_row);
    // ★ 1.x `:532-545` 那次修正（用户实测 2026-09-14）：预览是**图随内容增长**的，
    //   缩放得能真的看全 —— 放大后整张图变大，画布**滚动条随之可用**。
    //   所以预览标签放进 QScrollArea（`setWidgetResizable(true)` ⇒ 图比视口大就出条）；
    //   原来只是"画进一个固定框"，字一大就被裁掉，看起来"缩放没什么用"。
    auto* preview_scroll = new QScrollArea(dialog);
    preview_scroll->setObjectName(QStringLiteral("font_picker_preview_scroll"));
    preview_scroll->setWidgetResizable(true);
    auto* preview = new QLabel;
    preview->setObjectName(QStringLiteral("font_picker_preview"));
    preview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    preview_scroll->setWidget(preview);
    right->addWidget(preview_scroll, 1);
    auto* caption = new QLabel(dialog);
    caption->setObjectName(QStringLiteral("font_picker_caption"));
    caption->setWordWrap(true);
    right->addWidget(caption, 0);
    body->addLayout(right, 2);
    outer->addLayout(body, 1);

    // 图例（1.x `:301-302`：只留"符号 = 什么意思"，实现细节不摆在界面上）
    auto* legend = new QLabel(dialog);
    legend->setObjectName(QStringLiteral("font_picker_legend"));
    legend->setText(QStringLiteral("图例: %1 · %2 · %3")
                        .arg(utf8(engine::font::cjk_state_mark(engine::font::CjkFontState::Ok)),
                             utf8(engine::font::cjk_state_mark(engine::font::CjkFontState::Missing)),
                             utf8(engine::font::cjk_state_mark(engine::font::CjkFontState::Unknown))));
    legend->setWordWrap(true);
    outer->addWidget(legend, 0);

    auto* buttons = new QHBoxLayout;
    auto* pick = new QPushButton(QStringLiteral("选择"), dialog);
    pick->setObjectName(QStringLiteral("font_picker_pick"));
    auto* browse_other = new QPushButton(QStringLiteral("浏览其他目录…"), dialog);
    browse_other->setObjectName(QStringLiteral("font_picker_browse"));
    auto* cancel = new QPushButton(QStringLiteral("取消"), dialog);
    cancel->setObjectName(QStringLiteral("font_picker_cancel"));
    buttons->addWidget(pick, 0);
    buttons->addWidget(browse_other, 0);
    buttons->addStretch(1);
    buttons->addWidget(cancel, 0);
    outer->addLayout(buttons);

    // 行文本：`basename [目录] 标记`（1.x `:476`）—— 标记先留空，检测按批回填。
    const QStringList items = font_candidates(keyword->text());
    const auto refill = [this, list](const QStringList& paths) {
        const QString current = font_edit_ != nullptr ? font_edit_->text().trimmed() : QString();
        list->clear();
        for (const QString& path : paths) {
            const QString mark = font_mark_cache_.count(path) != 0 ? font_mark_cache_[path]
                                                                  : QString();
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  [%2]  %3")
                    .arg(QFileInfo(path).fileName(), QFileInfo(path).absolutePath(), mark));
            item->setData(Qt::UserRole, path);
            list->addItem(item);
            if (path == current) list->setCurrentItem(item);
        }
    };
    refill(items);

    // 后台分片识别中文字形（1.x `detect_all`）：一屏几百个字体，一次性全解析会卡住界面。
    // 缓存跨对话框复用（`font_mark_cache_`），所以第二次打开几乎是立刻的。
    const auto detect_batch = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = detect_batch;
    *detect_batch = [this, list, items, weak](int start) {
        const int end = std::min(start + 24, static_cast<int>(items.size()));
        for (int i = start; i < end; ++i) cached_mark(items[i]);
        // 回填标记（列表项文本：`basename [目录] 标记`）
        for (int i = 0; i < list->count() && i < static_cast<int>(items.size()); ++i) {
            QListWidgetItem* item = list->item(i);
            if (item == nullptr || item->data(Qt::UserRole).toString() != items[i]) continue;
            const QString mark = font_mark_cache_.count(items[i]) != 0 ? font_mark_cache_[items[i]]
                                                                      : QString();
            item->setText(QStringLiteral("%1  [%2]  %3")
                              .arg(QFileInfo(items[i]).fileName(),
                                   QFileInfo(items[i]).absolutePath(), mark));
        }
        if (end < static_cast<int>(items.size())) {
            // ★★ 2026-10-05 真缺陷修复（血账：`--smoke` 里 0xC0000005 的**真因**就在这里）：
            //   这条自续期链**必须绑在页的生命期上**。修前这里写的是
            //   `QTimer::singleShot(25, [keep, end] { (*keep)(end); })` —— **没有 context 对象**，
            //   于是它既不属于 dialog、也不属于 page，谁析构都拦不住它；而 `detect_batch`
            //   捕获的是**裸 `this`**，下一跳里 `cached_mark(items[i])` 直接摸
            //   `font_mark_cache_`（`std::map<QString,QString>`，页的成员）。
            //   现场（门禁 [23] 实崩）：`FontPackPage` 夹具析构后链还在 25 ms 一跳地跑
            //  ⇒ 下一个 `processEvents()` 里 `_Find_lower_bound` 解引用已释放的树节点
            //  ⇒ 访问违例。实测栈（VEH + `/MAP` 符号化，与 WER 记录一致）：
            //     `run_smoke` ← `make_font_picker 的 lambda_2` ← `FontPackPage::cached_mark`
            //     ← `std::_Tree<...>::_Find_lower_bound` ← 崩在 `Qt6Core.dll+0x16B260`。
            //   ★ 修法：把 `this`（**页**）交给 `singleShot` 当 context —— 页一析构，Qt 就把
            //     这一跳取消，链**自然终止**（`keep` 那个 shared_ptr 随之释放）。
            //     为什么不写 `QPointer` 自己判：context 对象是 Qt 给这件事的**原生机制**，
            //     少一层手写判空，也不会漏掉"下一跳已经排上队但页刚死"的窗口。
            //   ★ 反证锁：把 context 参数去掉（还原成修前那行）⇒ 门禁 [23] 外壳自检
            //     以 3221225477（0xC0000005）退出。
            if (auto keep = weak.lock()) QTimer::singleShot(25, this, [keep, end] { (*keep)(end); });
        }
    };

    // 预览（1.x `show_preview` `:532-596`）：字号 = max(12, int(40*zoom))，图随内容增长。
    const auto render_preview = [this, list, preview, caption, zoom_label, dialog] {
        QListWidgetItem* item = list->currentItem();
        zoom_label->setText(QStringLiteral("%1%").arg(static_cast<int>(preview_zoom() * 100)));
        if (item == nullptr) {
            preview->clear();
            caption->clear();
            return;
        }
        const QString path = item->data(Qt::UserRole).toString();
        // 字体名 + 四态标记（**与列表标记同源**：`cached_mark` → 引擎 `cjk_state_mark`）
        int font_id = QFontDatabase::addApplicationFont(path);
        QStringList families;
        QString family = QStringLiteral("(解析失败)");
        if (font_id >= 0) {
            families = QFontDatabase::applicationFontFamilies(font_id);
            if (!families.isEmpty()) family = families.first();
        }
        const QString line1 = preview_text_line(path, 0);
        const QString line2 = preview_text_line(path, 1);
        QFont font;
        if (!families.isEmpty()) {
            font = QFont(families.first());
        } else {
            font = dialog->font();
        }
        font.setPixelSize(preview_pixel_size());
        const QFontMetrics metrics(font);
        const int pad = xrkit::core::px(8);
        const int width = std::max(metrics.horizontalAdvance(line1), metrics.horizontalAdvance(line2)) +
                          2 * pad;
        const int height = 2 * metrics.height() + 3 * pad;
        QPixmap pixmap(width, height);
        pixmap.fill(QColor(QString::fromUtf8(xrkit::core::theme_color_current("surface"))));
        {
            QPainter painter(&pixmap);
            painter.setFont(font);
            painter.setPen(QColor(QString::fromUtf8(xrkit::core::theme_color_current("text"))));
            painter.drawText(pad, pad + metrics.ascent(), line1);
            painter.setPen(
                QColor(QString::fromUtf8(xrkit::core::theme_color_current("text_dim"))));
            painter.drawText(pad, pad + metrics.height() + pad / 2 + metrics.ascent(), line2);
        }
        preview->setPixmap(pixmap);
        // 标题：`basename  [字体名]` + 四态标记（1.x `:593`）
        QString text = QStringLiteral("%1  [%2]  %3")
                           .arg(QFileInfo(path).fileName(), family, cached_mark(path));
        const engine::font::CjkFontCheck check = check_font(path);
        if (check.state == engine::font::CjkFontState::Japanese && !check.fallbacks.empty()) {
            QStringList pairs;
            for (const auto& [from, to] : check.fallbacks) {
                pairs << QStringLiteral("%1→%2").arg(utf8(from), utf8(to));
            }
            text += QStringLiteral("（缺 %1 个简体字，预览已用繁体字形显示：%2）")
                        .arg(check.fallbacks.size())
                        .arg(pairs.join(QStringLiteral("、")));
        }
        caption->setText(text);
    };

    QObject::connect(keyword, &QLineEdit::textChanged, dialog,
                     [this, list, refill, detect_batch](const QString& text) {
                         const QStringList paths = font_candidates(text);
                         refill(paths);
                         // 旧检测链作废（1.x 的"代次"）：这里用 items 快照 + 匹配校验代替，
                         // 列表项与 items 对不上就跳过 ⇒ 不会把过期结果写进新列表。
                         if (!paths.isEmpty()) (*detect_batch)(0);
                     });
    QObject::connect(list, &QListWidget::currentItemChanged, dialog,
                     [render_preview](QListWidgetItem*, QListWidgetItem*) { render_preview(); });
    // ★ **双击＝选择**（1.x `:641`）
    QObject::connect(list, &QListWidget::itemDoubleClicked, dialog,
                     [this, pick](QListWidgetItem*) { pick->click(); });
    QObject::connect(zoom_out, &QPushButton::clicked, dialog, [this, render_preview] {
        zoom_preview(-0.5);
        render_preview();
    });
    QObject::connect(zoom_in, &QPushButton::clicked, dialog, [this, render_preview] {
        zoom_preview(0.5);
        render_preview();
    });
    QObject::connect(pick, &QPushButton::clicked, dialog, [this, list, dialog] {
        QListWidgetItem* item = list->currentItem();
        if (item == nullptr) return;
        QString problem;
        apply_font_choice(item->data(Qt::UserRole).toString(), &problem);
        if (!problem.isEmpty() && result_label_ != nullptr) result_label_->setText(problem);
        dialog->accept();
    });
    QObject::connect(browse_other, &QPushButton::clicked, dialog, [this, dialog] {
        // 1.x `:619-632`：起点取自"当前字体/XML 目录/工具目录"—— **不写死 C:\Windows\Fonts**。
        // 走**共用**的起点判据（`resolve_dialog_start` + `font_dialog_start`，一份实现）：
        // ① 继承上次的选择（＝框里那个字体，Qt 的文件对话框会落到它所在目录并预选它）
        // → ② 唯一输入目录（文本 XML 目录）→ ③ 工具自身所在文件夹。
        const QString start = resolve_dialog_start(font_dialog_start());
        const QString picked = QFileDialog::getOpenFileName(
            dialog, QStringLiteral("选择字体文件"), start,
            QStringLiteral("字体文件 (*.ttf *.ttc *.otf)"));
        if (picked.isEmpty()) return;
        QString problem;
        apply_font_choice(picked, &problem);   // 「浏览其他目录」也是用户的选择 ⇒ 同样记住
        if (!problem.isEmpty() && result_label_ != nullptr) result_label_->setText(problem);
        dialog->accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, dialog, [dialog] { dialog->reject(); });

    render_preview();
    (*detect_batch)(0);
    return dialog;
}

bool FontPackPage::remember_font(const QString& path, QString* problem) {
    // 1.x `:598-607`：记住这次选的字体（写 user.ltx 的 `[font] path`，**保留其它段**）。
    // 失败**必须明确告警**（"下次启动需重新选择字体"），绝不静默。
    if (path.isEmpty()) return true;
    if (!xrkit::core::save_user_value(kFontSection, kFontKey, path.toUtf8().toStdString())) {
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

bool FontPackPage::build(const QString& xml_dir, const QString& font_path, const QString& out_dir,
                         QString* reason) {
    // 11.9：**先判后建** —— 缺字形/检测失败直接拒绝，且**不建任何目录**。
    // （`font_unrenderable` 只告警不拒绝：那条路在引擎的 build_package 里以 warning 体现。）
    const engine::font::CjkFontCheck check = check_font(font_path);
    if (check.state == engine::font::CjkFontState::Missing ||
        check.state == engine::font::CjkFontState::Unknown) {
        ++refused_count_;
        if (reason != nullptr) {
            *reason = QStringLiteral("拒绝生成：%1（缺字形或字体不可用；没有创建任何目录）")
                          .arg(utf8(engine::font::cjk_state_mark(check.state)));
        }
        if (result_label_ != nullptr) result_label_->setText(reason != nullptr ? *reason : QString());
        return false;
    }
    const engine::font::PackageResult result = engine::font::build_package(
        options_for(xml_dir, font_path, out_dir));
    if (result_label_ != nullptr) {
        result_label_->setText(
            result.ok ? QStringLiteral("完成：字体 %1 个、XML %2 个%3")
                            .arg(result.fonts_written)
                            .arg(result.xml_written)
                            .arg(result.warnings.empty()
                                     ? QString()
                                     : QStringLiteral("（%1 条告警）").arg(result.warnings.size()))
                      : QStringLiteral("失败：") + utf8(result.error));
    }
    if (!result.ok) {
        if (reason != nullptr) *reason = utf8(result.error);
        return false;
    }
    if (!check.fallbacks.empty() && state_label_ != nullptr) {
        state_label_->setText(state_label_->text() +
                              QStringLiteral("（已按繁体回退生成）"));
    }
    return true;
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
