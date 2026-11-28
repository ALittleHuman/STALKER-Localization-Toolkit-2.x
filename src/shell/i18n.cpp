#include "i18n.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QVector>

#include "xrkit/core/theme.h"

namespace xrkit::shell {
namespace {

UiLanguage g_language = UiLanguage::Chinese;
QHash<QString, QString> g_ru;

struct TextTemplate {
    QRegularExpression pattern;
    QString russian;
};

QVector<TextTemplate> g_templates;

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
        g_templates.push_back(TextTemplate{re, it.value()});
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
                    QString captured = match.captured(number);
                    translate_text(captured);
                    out += captured;
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

}  // namespace

void initialize_ui_language(const QStringList& args) {
    QString code = read_language_override(args).trimmed().toLower();
    if (code.isEmpty()) {
        code = QLocale::system().language() == QLocale::Russian ? QStringLiteral("ru")
                                                                 : QStringLiteral("zh");
    }
    g_language = code.startsWith(QStringLiteral("ru")) ? UiLanguage::Russian : UiLanguage::Chinese;
    g_ru.clear();
    g_templates.clear();
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
    QString source = QString::fromUtf8(chinese);
    translate_text(source);
    return source;
}

}  // namespace xrkit::shell
