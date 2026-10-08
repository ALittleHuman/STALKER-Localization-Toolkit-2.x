// xrkit::shell —— **字库选择器**的唯一实现（2026-10-08 P8a 从 `fontpack_page.cpp` 抽出来）。
// 口径与"为什么抽"见 `font_picker.h`。本文件的代码**逐条照搬**原实现（含那条真缺陷的修法与注释），
// 改动只有三处：
//   ① 原来靠页面成员（`font_mark_cache_` / `preview_zoom_`）的地方改成**文件内静态**/对话框局部；
//   ② "选中的落点"从页面的 `apply_font_choice` 改成**注入的回调** `on_picked`；
//   ③ 浏览起点用共用的 `resolve_dialog_start` / `font_dialog_start`（原来在页面里也是走它）。

#include "font_picker.h"

#include "dialog_start.h"
#include "theme.h"

#include "xrkit/core/theme.h"
#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/system_dirs.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace xrkit::shell {
namespace {

namespace efont = xrkit::engine::font;

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// 字体后缀白名单（1.x `:501-503`：**小写比较**，逐个 fnmatch）
const char* kFontExts[] = {".ttf", ".ttc", ".otf"};

bool font_ext_ok(const QString& name) {
    const QString lower = name.toLower();
    for (const char* ext : kFontExts) {
        if (lower.endsWith(QString::fromLatin1(ext))) return true;
    }
    return false;
}

// 四态标记的缓存：**跨对话框复用**（第二次打开几乎是立刻的）
std::map<QString, QString>& mark_cache() {
    static std::map<QString, QString> instance;
    return instance;
}

}  // namespace

QStringList font_candidates(const QString& keyword, const QString& current_font) {
    QStringList dirs;
    // ★ 系统字体目录**从引擎拿**（唯一一份：与 `api.host.system_font_dirs()` 共用）
    for (const std::string& raw_dir : efont::system_font_dirs()) {
        const QString qdir = utf8(raw_dir);
        if (!dirs.contains(qdir)) dirs << qdir;
    }
    // 1.x `:489-493`：**当前已选字体所在目录**也加进候选（用户自带的字体就在那儿）
    if (!current_font.trimmed().isEmpty()) {
        const QString parent = QFileInfo(current_font.trimmed()).absolutePath();
        if (!parent.isEmpty() && !dirs.contains(parent)) dirs << parent;
    }
    // 存在的排前面（1.x 只返回存在的目录；这里把存在的提前 —— 目录不存在时列表为空而不是
    // "看起来没找到字体"，"为什么不列出来"这件事仍然可解释）
    QStringList existing;
    QStringList missing;
    for (const QString& dir : dirs) (QFileInfo(dir).isDir() ? existing : missing) << dir;

    QStringList out;
    QSet<QString> seen;   // **完整路径**去重（1.x `seen` 集合，`:494`/`:507`）
    const QString word = keyword.trimmed().toLower();
    for (const QString& dir : existing + missing) {
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

QString font_mark_of(const QString& font_path) {
    if (font_path.isEmpty()) return QString();
    auto& cache = mark_cache();
    const auto it = cache.find(font_path);
    if (it != cache.end()) return it->second;
    // 判据**只有引擎那一份**（列表标记与预览标题共用它 ⇒ 两处结论不可能矛盾）
    const QString mark = utf8(efont::cjk_state_mark(efont::classify_cjk_font(
        font_path.toUtf8().toStdString()).state));
    cache.emplace(font_path, mark);
    return mark;
}

QString font_preview_line(const QString& font_path, int which) {
    // 1.x `:570`：两行样本（第二行带西里尔与数字）
    QString line = which == 0 ? QStringLiteral("汉化测试你好ABC") : QStringLiteral("Привет 世界 123");
    // 用户口径 2026-09-14：「日语的，标明是日语字体，然后那些缺简体的就上繁体」——
    // 预览里直接把缺的简体字换成繁体字形（否则那几格是空白，用户看到的还是"缺字"）。
    const efont::CjkFontCheck check =
        efont::classify_cjk_font(font_path.toUtf8().toStdString());
    if (check.state == efont::CjkFontState::Japanese) {
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

QString default_font_path(const QString& saved) {
    // ① 上次用过的（user.ltx 的 `[font] path`，**文件仍存在**才算）
    if (!saved.isEmpty() && QFileInfo::exists(saved)) return QDir::toNativeSeparators(saved);
    // ② 系统微软雅黑（按名字优先级逐个目录找）
    for (const std::string& raw_dir : efont::system_font_dirs()) {
        const QString dir = utf8(raw_dir);
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

QDialog* make_font_picker(QWidget* parent, const QString& current,
                          std::function<DialogStart()> start_judge,
                          std::function<void(const QString&)> on_picked,
                          std::function<void(const QString&)> on_problem) {
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("font_picker"));
    dialog->setWindowTitle(QStringLiteral("选择字体文件"));
    dialog->resize(xrkit::core::px(900), xrkit::core::px(520));
    auto* outer = new QVBoxLayout(dialog);

    // 预览缩放（原来是页成员；现在**只属于这个对话框**）
    auto zoom = std::make_shared<double>(1.0);
    const auto preview_zoom = [zoom] {
        return *zoom;
    };
    const auto zoom_preview = [zoom](double delta) {
        double next = *zoom + delta;
        next = std::round(next * 10.0) / 10.0;   // 1.x `set_zoom`
        *zoom = next < 0.5 ? 0.5 : (next > 4.0 ? 4.0 : next);
    };
    const auto preview_pixel_size = [preview_zoom] {
        // 1.x `:568`：`size = max(12, int(40 * zoom))`
        const int size = static_cast<int>(40.0 * preview_zoom());
        return size < 12 ? 12 : size;
    };

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
                        .arg(utf8(efont::cjk_state_mark(efont::CjkFontState::Ok)),
                             utf8(efont::cjk_state_mark(efont::CjkFontState::Missing)),
                             utf8(efont::cjk_state_mark(efont::CjkFontState::Unknown))));
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
    const QStringList items = font_candidates(keyword->text(), current);
    const auto refill = [list, current](const QStringList& paths) {
        list->clear();
        for (const QString& path : paths) {
            const QString mark = font_mark_of(path);   // 有缓存就带出来，没算过留空
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
    // 缓存跨对话框复用，所以第二次打开几乎是立刻的。
    const auto detect_batch = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weak = detect_batch;
    *detect_batch = [list, items, weak, dialog](int start) {
        const int end = std::min(start + 24, static_cast<int>(items.size()));
        for (int i = start; i < end; ++i) font_mark_of(items[i]);
        // 回填标记（列表项文本：`basename [目录] 标记`）
        for (int i = 0; i < list->count() && i < static_cast<int>(items.size()); ++i) {
            QListWidgetItem* item = list->item(i);
            if (item == nullptr || item->data(Qt::UserRole).toString() != items[i]) continue;
            item->setText(QStringLiteral("%1  [%2]  %3")
                              .arg(QFileInfo(items[i]).fileName(),
                                   QFileInfo(items[i]).absolutePath(), font_mark_of(items[i])));
        }
        if (end < static_cast<int>(items.size())) {
            // ★★ 2026-10-05 真缺陷修复（血账：`--smoke` 里 0xC0000005 的**真因**就在这里）：
            //   这条自续期链**必须绑在对话框的生命期上**。修前写的是
            //   `QTimer::singleShot(25, [keep, end] { (*keep)(end); })` —— **没有 context 对象**，
            //   于是它既不属于 dialog、也不属于页面，谁析构都拦不住它；而 `detect_batch` 捕获的是
            //   裸 `this`（当时是页面），下一跳里 `cached_mark(items[i])` 直接摸页面的
            //   `font_mark_cache_`（`std::map<QString,QString>` 成员）。
            //   现场（门禁 [23] 实崩）：页夹具析构后链还在 25 ms 一跳地跑 ⇒ 下一个
            //   `processEvents()` 里 `_Find_lower_bound` 解引用已释放的树节点 ⇒ 访问违例
            //   （崩溃栈：`run_smoke` ← `make_font_picker 的 lambda` ← `cached_mark`
            //    ← `std::_Tree<...>::_Find_lower_bound`）。
            //   ★ 修法：把 **dialog** 交给 `singleShot` 当 context —— 对话框一析构，Qt 就把这一跳
            //     取消，链自然终止（`keep` 随之释放）。抽到本文件后，"缓存的宿主"从页面变成了
            //     **文件内静态**（不再有页面生命周期问题），context 仍是 dialog。
            if (auto keep = weak.lock()) QTimer::singleShot(25, dialog, [keep, end] { (*keep)(end); });
        }
    };

    // 预览（1.x `show_preview` `:532-596`）：字号 = max(12, int(40*zoom))，图随内容增长。
    const auto render_preview = [list, preview, caption, zoom_label, dialog, preview_zoom,
                                 preview_pixel_size] {
        QListWidgetItem* item = list->currentItem();
        zoom_label->setText(QStringLiteral("%1%").arg(static_cast<int>(preview_zoom() * 100)));
        if (item == nullptr) {
            preview->clear();
            caption->clear();
            return;
        }
        const QString path = item->data(Qt::UserRole).toString();
        // 字体名 + 四态标记（**与列表标记同源**：`font_mark_of` → 引擎 `cjk_state_mark`）
        int font_id = QFontDatabase::addApplicationFont(path);
        QStringList families;
        QString family = QStringLiteral("(解析失败)");
        if (font_id >= 0) {
            families = QFontDatabase::applicationFontFamilies(font_id);
            if (!families.isEmpty()) family = families.first();
        }
        const QString line1 = font_preview_line(path, 0);
        const QString line2 = font_preview_line(path, 1);
        QFont font;
        if (!families.isEmpty()) {
            font = QFont(families.first());
        } else {
            font = dialog->font();
        }
        font.setPixelSize(preview_pixel_size());
        const QFontMetrics metrics(font);
        const int pad = xrkit::core::px(8);
        const int width =
            std::max(metrics.horizontalAdvance(line1), metrics.horizontalAdvance(line2)) + 2 * pad;
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
        QString text =
            QStringLiteral("%1  [%2]  %3").arg(QFileInfo(path).fileName(), family,
                                               font_mark_of(path));
        const efont::CjkFontCheck check = efont::classify_cjk_font(path.toUtf8().toStdString());
        if (check.state == efont::CjkFontState::Japanese && !check.fallbacks.empty()) {
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

    const auto accept_path = [dialog, on_picked, on_problem](const QString& path) {
        if (path.isEmpty()) return;
        if (on_picked != nullptr) on_picked(path);
        if (on_problem != nullptr) on_problem(QString());
        dialog->accept();
    };

    QObject::connect(keyword, &QLineEdit::textChanged, dialog,
                     [list, refill, detect_batch, current](const QString& text) {
                         const QStringList paths = font_candidates(text, current);
                         refill(paths);
                         // 旧检测链作废（1.x 的"代次"）：这里用 items 快照 + 匹配校验代替，
                         // 列表项与 items 对不上就跳过 ⇒ 不会把过期结果写进新列表。
                         if (!paths.isEmpty()) (*detect_batch)(0);
                     });
    QObject::connect(list, &QListWidget::currentItemChanged, dialog,
                     [render_preview](QListWidgetItem*, QListWidgetItem*) { render_preview(); });
    // ★ **双击＝选择**（1.x `:641`）
    QObject::connect(list, &QListWidget::itemDoubleClicked, dialog,
                     [pick](QListWidgetItem*) { pick->click(); });
    QObject::connect(zoom_out, &QPushButton::clicked, dialog, [zoom_preview, render_preview] {
        zoom_preview(-0.5);
        render_preview();
    });
    QObject::connect(zoom_in, &QPushButton::clicked, dialog, [zoom_preview, render_preview] {
        zoom_preview(0.5);
        render_preview();
    });
    QObject::connect(pick, &QPushButton::clicked, dialog, [list, accept_path] {
        QListWidgetItem* item = list->currentItem();
        if (item == nullptr) return;
        accept_path(item->data(Qt::UserRole).toString());
    });
    QObject::connect(browse_other, &QPushButton::clicked, dialog,
                     [dialog, accept_path, start_judge, current] {
                         // 1.x `:619-632`：起点取自"当前字体/XML 目录/工具目录"——
                         // **不写死 C:\Windows\Fonts**（走共用的起点判据）。
                         // 页面传自己那份判据；没有页面（插件那条路）⇒ 按当前字体路径算一份。
                         const DialogStart spec = start_judge != nullptr
                                                      ? start_judge()
                                                      : dialog_start_for_input_file(
                                                            current, QStringList());
                         const QString start = resolve_dialog_start(spec);
                         const QString picked = QFileDialog::getOpenFileName(
                             dialog, QStringLiteral("选择字体文件"), start,
                             QStringLiteral("字体文件 (*.ttf *.ttc *.otf)"));
                         accept_path(picked);   // 「浏览其他目录」也是用户的选择
                     });
    QObject::connect(cancel, &QPushButton::clicked, dialog, [dialog] { dialog->reject(); });

    render_preview();
    (*detect_batch)(0);
    return dialog;
}

}  // namespace xrkit::shell
