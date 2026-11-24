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
#include <QHash>
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

const efont::CjkFontCheck& font_check_of(const QString& font_path) {
    static std::map<QString, efont::CjkFontCheck> cache;
    const auto it = cache.find(font_path);
    if (it != cache.end()) return it->second;
    return cache.emplace(font_path, efont::classify_cjk_font(
                                        font_path.toUtf8().toStdString()))
        .first->second;
}

QString font_mark_of(const QString& font_path) {
    if (font_path.isEmpty()) return QString();
    auto& cache = mark_cache();
    const auto it = cache.find(font_path);
    if (it != cache.end()) return it->second;
    // 判据**只有引擎那一份**（列表标记与预览标题共用它 ⇒ 两处结论不可能矛盾）
    const QString mark = utf8(efont::cjk_state_mark(font_check_of(font_path).state));
    cache.emplace(font_path, mark);
    return mark;
}

QString font_mark_cached(const QString& font_path) {
    if (font_path.isEmpty()) return QString();
    auto& cache = mark_cache();
    const auto it = cache.find(font_path);
    return it == cache.end() ? QString() : it->second;
}

int preview_font_id(const QString& path) {
    static QHash<QString, int> cache;
    const auto it = cache.find(path);
    if (it != cache.end()) return it.value();
    const int id = QFontDatabase::addApplicationFont(path);
    cache.insert(path, id);
    return id;
}


QPixmap font_preview_pixmap(const QString& font_path, const QString& line1,
                            const QString& line2, int pixel_size, int weight) {
    const int id = preview_font_id(font_path);
    const QStringList families =
        id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
    QFont font = families.isEmpty() ? QApplication::font() : QFont(families.first());
    if (weight > 0) font.setVariableAxis(QFont::Tag("wght"), static_cast<float>(weight));
    font.setPixelSize(std::max(6, pixel_size));
    const QFontMetrics metrics(font);
    const int pad = xrkit::core::px(8);
    const bool two_lines = !line2.isEmpty();
    const int width = std::max(metrics.horizontalAdvance(line1),
                               two_lines ? metrics.horizontalAdvance(line2) : 0) + 2 * pad;
    const int height = (two_lines ? 2 : 1) * metrics.height() + (two_lines ? 3 : 2) * pad;
    QPixmap pixmap(std::max(1, width), std::max(1, height));
    pixmap.fill(QColor(QString::fromUtf8(xrkit::core::theme_color_current("surface"))));
    QPainter painter(&pixmap);
    painter.setFont(font);
    painter.setPen(QColor(QString::fromUtf8(xrkit::core::theme_color_current("text"))));
    painter.drawText(pad, pad + metrics.ascent(), line1);
    if (two_lines) {
        painter.setPen(QColor(QString::fromUtf8(xrkit::core::theme_color_current("text_dim"))));
        painter.drawText(pad, pad + metrics.height() + pad / 2 + metrics.ascent(), line2);
    }
    return pixmap;
}

QString font_preview_line(const QString& font_path, int which) {
    // 1.x `:570`：两行样本（第二行带西里尔与数字）
    QString line = which == 0 ? QStringLiteral("汉化测试你好ABC") : QStringLiteral("Привет 世界 123");
    // 用户口径 2026-09-14：「日语的，标明是日语字体，然后那些缺简体的就上繁体」——
    // 预览里直接把缺的简体字换成繁体字形（否则那几格是空白，用户看到的还是"缺字"）。
    const efont::CjkFontCheck& check = font_check_of(font_path);
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
    // ★★ 2026-10-25（用户裁定）：**默认字体从微软雅黑改成 NotoSansSC**。
    //   雅黑**保留为兜底**（用户明确要"留作兜底"，不是删掉）。
    //   候选名取 Noto Sans SC 官方发行与 Windows 自带可变体的常见文件名。
    static const char* kNotoNames[] = {"NotoSansSC-Regular.otf", "NotoSansSC-Regular.ttf",
                                       "NotoSansSC-VF.otf", "NotoSansSC-VF.ttf",
                                       "NotoSansSC.ttf", "NotoSansSC.otf"};
    const auto first_in = [](const QString& dir, const char* const* names, size_t n) -> QString {
        for (size_t i = 0; i < n; ++i) {
            const QString cand = QDir(dir).filePath(QString::fromLatin1(names[i]));
            if (QFileInfo::exists(cand)) return cand;
        }
        return QString();
    };
    // ② 系统目录：先 NotoSansSC，再微软雅黑（按名字优先级逐个目录找）
    for (const std::string& raw_dir : efont::system_font_dirs()) {
        const QString dir = utf8(raw_dir);
        const QString noto = first_in(dir, kNotoNames, std::size(kNotoNames));
        if (!noto.isEmpty()) return QDir::toNativeSeparators(noto);
        for (const char* name : {"msyh.ttc", "msyh.ttf", "msyhl.ttc"}) {
            const QString cand = QDir(dir).filePath(QString::fromLatin1(name));
            if (QFileInfo::exists(cand)) return QDir::toNativeSeparators(cand);
        }
    }
    // ③ exe 旁：先 NotoSansSC.*，再 msyh.ttf（1.x 的预留分支；源码版通常没有）
    const QString local_dir = QString::fromStdString(xrkit::core::app_dir());
    const QString local_noto = first_in(local_dir, kNotoNames, std::size(kNotoNames));
    if (!local_noto.isEmpty()) return QDir::toNativeSeparators(local_noto);
    const QString local = QDir(local_dir).filePath(QStringLiteral("msyh.ttf"));
    if (QFileInfo::exists(local)) return QDir::toNativeSeparators(local);
    return QString();
}

QString font_family_name(const QString& font_path) {
    const QString trimmed = font_path.trimmed();
    if (trimmed.isEmpty()) return QString();
    const int id = QFontDatabase::addApplicationFont(trimmed);
    QString family;
    if (id >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) family = families.first();
        QFontDatabase::removeApplicationFont(id);   // 只是"问一下名字"，别把它留在应用字体表里
    }
    if (!family.isEmpty()) return family;
    // 回落：文件名主干（**绝不返回空串** —— `[font] name` 的意义就是"可读的身份"）
    const QString stem = QFileInfo(trimmed).completeBaseName();
    return stem.isEmpty() ? trimmed : stem;
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
    // 图随内容定高；不要让 QLabel 被拉满视口（否则预览区高度与图片/缩放无关）。
    preview_scroll->setWidgetResizable(false);
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
            const QString mark = font_mark_cached(path);   // 只取缓存；后台分批算
            auto* item = new QListWidgetItem(
                QStringLiteral("%1  [%2]  %3")
                    .arg(QFileInfo(path).fileName(), QFileInfo(path).absolutePath(), mark));
            item->setData(Qt::UserRole, path);
            list->addItem(item);
            if (path == current) list->setCurrentItem(item);
        }
    };
    refill(items);

    // 不在打开时批量识别全部字体：一次四态判定要读整份字体（实测 ~64ms/字体），
    // 几百个字体在 UI 线程排队就是"打开选择器即卡死"。列表先只显示缓存标记；
    // 当前选中项的标记/预览在下面按需算（同一份引擎判据，不另写）。
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
        int font_id = preview_font_id(path);
        QStringList families;
        QString family = QStringLiteral("(解析失败)");
        if (font_id >= 0) {
            families = QFontDatabase::applicationFontFamilies(font_id);
            if (!families.isEmpty()) family = families.first();
        }
        const QString line1 = font_preview_line(path, 0);
        const QString line2 = font_preview_line(path, 1);
        const QPixmap pixmap =
            font_preview_pixmap(path, line1, line2, preview_pixel_size(), 0);
        preview->setPixmap(pixmap);
        preview->adjustSize();
        // 标题：`basename  [字体名]` + 四态标记（1.x `:593`）
        QString text =
            QStringLiteral("%1  [%2]  %3").arg(QFileInfo(path).fileName(), family,
                                               font_mark_of(path));
        const efont::CjkFontCheck& check = font_check_of(path);
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
                     [list, refill, current](const QString& text) {
                         refill(font_candidates(text, current));
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

    // 预览与分类都排到事件循环：先让对话框出来，避免构造期同步解析字体。
    QTimer::singleShot(0, dialog, render_preview);
    return dialog;
}

}  // namespace xrkit::shell
