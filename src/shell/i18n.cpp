#include "i18n.h"

#include <algorithm>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGroupBox>
#include <QHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QRegularExpression>
#include <QTimer>
#include <QTreeWidget>
#include <QVector>
#include <QWidget>

#include "xrkit/core/theme.h"

namespace xrkit::shell {
namespace {

UiLanguage g_language = UiLanguage::Chinese;
QHash<QString, QString> g_ru;

struct TextTemplate {
    QRegularExpression pattern;
    QString russian;
    int placeholders = 0;
};
QVector<TextTemplate> g_templates;

QString language_name(UiLanguage language) {
    return language == UiLanguage::Russian ? QStringLiteral("ru") : QStringLiteral("zh");
}

QString read_language_override(const QStringList& args) {
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--lang") && i + 1 < args.size()) return args[i + 1];
        if (args[i].startsWith(QStringLiteral("--lang="))) return args[i].mid(7);
    }
    const QByteArray env = qgetenv("XRKIT_LANG");
    if (!env.isEmpty()) return QString::fromUtf8(env);
    const std::string saved = core::load_user_value("ui", "lang", "");
    if (!saved.empty()) return QString::fromStdString(saved);
    return {};
}

void rebuild_templates();

bool load_ru(const QString& file) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) return false;
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(input.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject strings = doc.object().value(QStringLiteral("strings")).toObject();
    if (strings.isEmpty()) return false;
    g_ru.clear();
    for (auto it = strings.begin(); it != strings.end(); ++it) {
        if (it.value().isString()) g_ru.insert(it.key(), it.value().toString());
    }
    rebuild_templates();
    return !g_ru.isEmpty();
}

void rebuild_templates() {
    g_templates.clear();
    for (auto it = g_ru.constBegin(); it != g_ru.constEnd(); ++it) {
        const QString source = it.key();
        int max_placeholder = 0;
        QString pattern = QStringLiteral("^");
        bool has_placeholder = false;
        for (int i = 0; i < source.size(); ++i) {
            const QChar ch = source[i];
            if (ch == QLatin1Char('%') && i + 1 < source.size() && source[i + 1].isDigit()) {
                const int number = source[i + 1].digitValue();
                if (number <= 0) {
                    has_placeholder = false;
                    break;
                }
                max_placeholder = std::max(max_placeholder, number);
                pattern += QStringLiteral("(.+?)");
                has_placeholder = true;
                ++i;
            } else {
                pattern += QRegularExpression::escape(QString(ch));
            }
        }
        pattern += QStringLiteral("$");
        if (!has_placeholder) continue;
        QRegularExpression re(pattern);
        re.setPatternOptions(QRegularExpression::DotMatchesEverythingOption);
        if (!re.isValid()) continue;
        g_templates.push_back(TextTemplate{re, it.value(), max_placeholder});
    }
}

void translate_text(QString& text) {
    if (g_language != UiLanguage::Russian || text.isEmpty()) return;
    const auto it = g_ru.constFind(text);
    if (it != g_ru.constEnd()) {
        text = it.value();
        return;
    }
    for (const TextTemplate& candidate : g_templates) {
        const QRegularExpressionMatch match = candidate.pattern.match(text);
        if (!match.hasMatch()) continue;
        QString out;
        const QString& format = candidate.russian;
        for (int i = 0; i < format.size(); ++i) {
            if (format[i] == QLatin1Char('%') && i + 1 < format.size() &&
                format[i + 1].isDigit()) {
                const int number = format[i + 1].digitValue();
                if (number > 0 && number <= match.lastCapturedIndex()) {
                    out += match.captured(number);
                    ++i;
                    continue;
                }
            }
            out += format[i];
        }
        text = out;
        return;
    }
}

void translate_object(QObject* object);

void translate_children(QObject* object) {
    if (object == nullptr) return;
    for (QObject* child : object->children()) translate_object(child);
}

void translate_object(QObject* object) {
    if (object == nullptr) return;

    if (auto* widget = qobject_cast<QWidget*>(object)) {
        QString title = widget->windowTitle();
        translate_text(title);
        if (title != widget->windowTitle()) widget->setWindowTitle(title);
    }
    if (auto* label = qobject_cast<QLabel*>(object)) {
        QString text = label->text();
        translate_text(text);
        if (text != label->text()) label->setText(text);
    }
    if (auto* button = qobject_cast<QAbstractButton*>(object)) {
        QString text = button->text();
        translate_text(text);
        if (text != button->text()) button->setText(text);
    }
    if (auto* group = qobject_cast<QGroupBox*>(object)) {
        QString title = group->title();
        translate_text(title);
        if (title != group->title()) group->setTitle(title);
    }
    if (auto* action = qobject_cast<QAction*>(object)) {
        QString text = action->text();
        translate_text(text);
        if (text != action->text()) action->setText(text);
        QString tip = action->toolTip();
        translate_text(tip);
        if (tip != action->toolTip()) action->setToolTip(tip);
        QString status = action->statusTip();
        translate_text(status);
        if (status != action->statusTip()) action->setStatusTip(status);
    }
    if (auto* menu = qobject_cast<QMenu*>(object)) {
        QString title = menu->title();
        translate_text(title);
        if (title != menu->title()) menu->setTitle(title);
    }
    if (auto* tab_bar = qobject_cast<QTabBar*>(object)) {
        for (int i = 0; i < tab_bar->count(); ++i) {
            QString text = tab_bar->tabText(i);
            translate_text(text);
            if (text != tab_bar->tabText(i)) tab_bar->setTabText(i, text);
        }
    }
    if (auto* tabs = qobject_cast<QTabWidget*>(object)) {
        for (int i = 0; i < tabs->count(); ++i) {
            QString text = tabs->tabText(i);
            translate_text(text);
            if (text != tabs->tabText(i)) tabs->setTabText(i, text);
        }
    }
    if (auto* combo = qobject_cast<QComboBox*>(object)) {
        const QSignalBlocker blocker(combo);
        for (int i = 0; i < combo->count(); ++i) {
            QString text = combo->itemText(i);
            translate_text(text);
            if (text != combo->itemText(i)) combo->setItemText(i, text);
        }
    }
    if (auto* line = qobject_cast<QLineEdit*>(object)) {
        QString text = line->placeholderText();
        translate_text(text);
        if (text != line->placeholderText()) line->setPlaceholderText(text);
    }
    if (auto* table = qobject_cast<QTableWidget*>(object)) {
        for (int i = 0; i < table->columnCount(); ++i) {
            if (auto* item = table->horizontalHeaderItem(i)) {
                QString text = item->text();
                translate_text(text);
                if (text != item->text()) item->setText(text);
            }
        }
        for (int i = 0; i < table->rowCount(); ++i) {
            if (auto* item = table->verticalHeaderItem(i)) {
                QString text = item->text();
                translate_text(text);
                if (text != item->text()) item->setText(text);
            }
        }
    }
    if (auto* tree = qobject_cast<QTreeWidget*>(object)) {
        for (int i = 0; i < tree->columnCount(); ++i) {
            if (auto* item = tree->headerItem()) {
                QString text = item->text(i);
                translate_text(text);
                if (text != item->text(i)) item->setText(i, text);
            }
        }
    }

    translate_children(object);
}

class LanguageWatcher final : public QObject {
public:
    explicit LanguageWatcher(QObject* parent = nullptr) : QObject(parent) {
        auto* timer = new QTimer(this);
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, this, [] {
            for (QWidget* widget : QApplication::allWidgets()) {
                if (widget != nullptr && widget->isWindow() && widget->isVisible()) {
                    apply_ui_language(widget);
                }
            }
        });
        timer->start();
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event != nullptr && event->type() == QEvent::Show) {
            if (auto* widget = qobject_cast<QWidget*>(watched); widget != nullptr &&
                                                               widget->isWindow()) {
                QTimer::singleShot(0, widget, [widget] { apply_ui_language(widget); });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

}  // namespace

void initialize_ui_language(const QStringList& args) {
    QString code = read_language_override(args).trimmed().toLower();
    if (code.isEmpty()) {
        code = QLocale::system().language() == QLocale::Russian ? QStringLiteral("ru")
                                                                 : QStringLiteral("zh");
    }
    g_language = code.startsWith(QStringLiteral("ru")) ? UiLanguage::Russian : UiLanguage::Chinese;
    g_ru.clear();
    if (g_language != UiLanguage::Russian) return;

    const QDir app(QCoreApplication::applicationDirPath());
    QStringList candidates;
    const QByteArray env_file = qgetenv("XRKIT_LANG_FILE");
    if (!env_file.isEmpty()) candidates << QString::fromUtf8(env_file);
    candidates << app.filePath(QStringLiteral("lang/ru.json"))
               << app.filePath(QStringLiteral("../lang/ru.json"))
               << app.filePath(QStringLiteral("../../assets/i18n/ru.json"))
               << app.filePath(QStringLiteral("../../../../assets/i18n/ru.json"));
    for (const QString& candidate : candidates) {
        if (load_ru(candidate)) return;
    }
    qWarning("XRKIT i18n: 俄语已选中，但找不到 lang/ru.json；界面回退中文");
}

UiLanguage ui_language() { return g_language; }

QString tr_zh(const char* chinese) {
    if (chinese == nullptr) return {};
    const QString source = QString::fromUtf8(chinese);
    if (g_language == UiLanguage::Russian) {
        const auto it = g_ru.constFind(source);
        if (it != g_ru.constEnd()) return it.value();
    }
    return source;
}

void apply_ui_language(QObject* root) {
    if (root == nullptr || g_language != UiLanguage::Russian) return;
    translate_object(root);
}

void install_ui_language_watcher() {
    if (g_language != UiLanguage::Russian) return;
    static LanguageWatcher* watcher = nullptr;
    if (watcher == nullptr) {
        watcher = new LanguageWatcher(qApp);
        qApp->installEventFilter(watcher);
    }
}

}  // namespace xrkit::shell
