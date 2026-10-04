#include "dialog_start.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace xrkit::shell {
namespace {

// 自检用记录器（见 dialog_start.h）
QString last_dir;
int call_count = 0;

}  // namespace

QString canonical_dialog_dir(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return QString();
    // 统一成 '/'（起点是给 QFileDialog 的，两种分隔符都认；统一之后自检能逐字比）
    const QString native = QDir::fromNativeSeparators(trimmed);
    const QString cleaned = QDir::cleanPath(native);
    if (cleaned.isEmpty() || cleaned == QStringLiteral(".")) return QString();
    return cleaned;
}

QString resolve_dialog_start(const DialogStart& spec) {
    QString start = canonical_dialog_dir(spec.inherited);   // ① 继承上次的选择（现有逻辑）
    if (start.isEmpty()) start = canonical_dialog_dir(spec.input_dir);   // ② 唯一输入目录
    if (start.isEmpty()) {
        // ③ 工具自身所在文件夹（exe 位置；**不是 CWD**）
        start = canonical_dialog_dir(QCoreApplication::applicationDirPath());
    }
    last_dir = start;
    ++call_count;
    return start;
}

void reset_last_dialog_start() {
    last_dir.clear();
    call_count = 0;
}

QString last_dialog_start_dir() { return last_dir; }

int last_dialog_start_calls() { return call_count; }

DialogStart dialog_start_for_input_dir(const QString& inherited, const QStringList& input_dirs) {
    DialogStart spec;
    spec.inherited = inherited;
    // **唯一**才认：0 个（没有输入）或 ≥2 个（"输入目录多元"）都留空 ⇒ 落到 exe 目录
    if (input_dirs.size() == 1) spec.input_dir = input_dirs.front();
    return spec;
}

DialogStart dialog_start_for_input_file(const QString& inherited, const QStringList& input_files) {
    DialogStart spec = dialog_start_for_input_dir(inherited, QStringList());
    if (input_files.size() == 1) {
        // 文件输入 ⇒ 用**它所在目录**当起点（例：视频输出 ← 源视频所在目录）
        spec.input_dir = QFileInfo(input_files.front()).absoluteDir().absolutePath();
    }
    return spec;
}

DialogStart dialog_start_for(const QString& inherited, const QStringList& inputs,
                             bool input_is_file) {
    return input_is_file ? dialog_start_for_input_file(inherited, inputs)
                         : dialog_start_for_input_dir(inherited, inputs);
}

}  // namespace xrkit::shell
