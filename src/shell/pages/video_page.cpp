#include "video_page.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFile>
#include <QProcess>

#if defined(_WIN32)
#include <windows.h>  // CREATE_NO_WINDOW（子进程无控制台窗口）
#endif
#include <QPushButton>
#include <QVBoxLayout>
#include <filesystem>

namespace xrkit::shell {
namespace {

QString utf8(const std::string& text) { return QString::fromStdString(text); }

}  // namespace

bool VideoPage::start_transcode(const engine::video::TranscodeRequest& request,
                                QString* error) {
    if (process_ != nullptr) {
        if (error != nullptr) *error = QStringLiteral("已经在转码中");
        return false;
    }
    cancel_token_.reset();  // 1.x：新一轮要显式 reset（convert() 自己不清零）
    progress_percent_ = 0;
    last_error_.clear();

    const engine::video::TranscodePlan plan = engine::video::plan_transcode(request);
    duration_sec_ = plan.duration;
    if (plan.argv.empty()) {
        if (error != nullptr) *error = QStringLiteral("空命令行");
        return false;
    }
    QStringList args;
    for (size_t i = 1; i < plan.argv.size(); ++i) {
        args << QString::fromStdString(plan.argv[i]);
    }
    process_ = new QProcess(this);
#ifdef _WIN32
    // ★ 用户 2026-09-27：「不要打开cmd」—— 子进程是控制台程序（ffmpeg / CLI），
    //   Qt 默认会**新建一个控制台窗口**；这里加 CREATE_NO_WINDOW 让它无窗口运行。
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    process_->setProgram(QString::fromStdString(plan.argv.front()));
    process_->setArguments(args);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(process_->readAllStandardError());
        const std::string raw = text.toUtf8().toStdString();
        size_t start = 0;
        while (start <= raw.size()) {
            const size_t nl = raw.find('\n', start);
            const std::string line =
                raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            const engine::video::ProgressSample sample =
                engine::video::progress_from_line(line, duration_sec_);
            if (sample.matched) progress_percent_ = static_cast<int>(sample.percent);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    });
    connect(process_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                if (cancel_token_.cancelled()) {
                    // 1.x：取消就**删掉半成品**（不留"看着像成功"的输出）
                    if (output_edit_ != nullptr) QFile::remove(output_edit_->text());
                    last_error_ = QStringLiteral("已取消");
                } else if (status != QProcess::NormalExit || code != 0) {
                    last_error_ = QStringLiteral("ffmpeg 退出码 %1").arg(code);
                }
                if (process_ != nullptr) {
                    process_->deleteLater();
                    process_ = nullptr;
                }
            });
    process_->start();
    if (!process_->waitForStarted(10000)) {
        last_error_ = QStringLiteral("启动失败：%1").arg(process_->errorString());
        process_->deleteLater();
        process_ = nullptr;
        if (error != nullptr) *error = last_error_;
        return false;
    }
    return true;
}

void VideoPage::cancel_transcode() {
    cancel_token_.cancel();
    if (process_ != nullptr) {
        process_->kill();
        process_->waitForFinished(5000);
    }
}

bool VideoPage::transcode_running() const { return process_ != nullptr; }

VideoPage::VideoPage(QWidget* parent)
    : PageBase(QStringLiteral("video"), QStringLiteral("视频转换"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    source_edit_ = new QLineEdit;
    source_edit_->setObjectName(QStringLiteral("video_source"));
    source_edit_->setAcceptDrops(true);
    source_edit_->setPlaceholderText(QStringLiteral("源视频文件"));
    output_edit_ = new QLineEdit;
    output_edit_->setObjectName(QStringLiteral("video_output"));
    output_edit_->setAcceptDrops(true);
    output_edit_->setPlaceholderText(QStringLiteral("输出目录"));

    const auto row = [this](const QString& label, QLineEdit* edit, const QString& title) {
        auto* layout = new QHBoxLayout;
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, [this, edit, title] {
            const QString picked = QFileDialog::getExistingDirectory(this, title, edit->text());
            if (!picked.isEmpty()) edit->setText(QDir::toNativeSeparators(picked));
        });
        return layout;
    };
    body_layout()->addLayout(row(QStringLiteral("源文件："), source_edit_,
                                 QStringLiteral("选择输出目录")));
    body_layout()->addLayout(row(QStringLiteral("输出目录："), output_edit_,
                                 QStringLiteral("选择输出目录")));

    // 8.5：三态状态栏。构造期**不探测**（8.2）⇒ 这里显示"未检测"，不是 ✗
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("video_status"));
    body_layout()->addWidget(status_, 0);
    body_layout()->addStretch(1);

    detect_button_ = add_action(QStringLiteral("检测 ffmpeg"));
    detect_button_->setObjectName(QStringLiteral("video_detect"));
    QObject::connect(detect_button_, &QPushButton::clicked, this, [this] {
        if (detect()) return;
        request_setup();  // 8.3：读不到就弹三按钮小窗（非阻塞）
    });
    auto* setup = add_action(QStringLiteral("设置 ffmpeg"));
    setup->setObjectName(QStringLiteral("video_setup"));
    QObject::connect(setup, &QPushButton::clicked, this, [this] { request_setup(); });

    refresh_status();
}

QString VideoPage::status_text() const {
    switch (locator_.state()) {
        case engine::video::ProbeState::NotProbed:
            // ★ 探测之前**不显示 ✗**（那就是在断言"没有"，而我们还没查）
            return QStringLiteral("未检测 ffmpeg");
        case engine::video::ProbeState::Found: {
            const engine::video::FfmpegPaths& paths = locator_.paths();
            QString text = QStringLiteral("✓ ffmpeg: ") + utf8(paths.ffmpeg);
            if (!paths.has_probe) text += QStringLiteral("（缺 ffprobe —— 视频信息读不出来）");
            if (paths.ffmpeg_from_config) text += QStringLiteral("（user.ltx 手动指定）");
            return text;
        }
        case engine::video::ProbeState::Missing:
            return QStringLiteral("✗ 没找到 ffmpeg（只认 user.ltx 与程序目录，不搜 PATH）");
    }
    return QString();
}

void VideoPage::refresh_status() { status_->setText(status_text()); }

bool VideoPage::detect() {
    locator_.ensure();  // 惰性：探过一次（含"没找到"）就不再探
    refresh_status();
    return locator_.state() == engine::video::ProbeState::Found;
}

bool VideoPage::set_manual(const QString& ffmpeg, const QString& ffprobe, QString* error) {
    std::string reason;
    if (!locator_.set_manual(ffmpeg.toUtf8().toStdString(), ffprobe.toUtf8().toStdString(),
                             reason)) {
        if (error != nullptr) *error = utf8(reason);
        return false;
    }
    refresh_status();
    return true;
}

QWidget* VideoPage::request_setup() {
    if (dialog_ != nullptr) {
        dialog_->show();
        dialog_->raise();
        return dialog_;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("video_setup_dialog"));
    dialog->setWindowTitle(QStringLiteral("需要 ffmpeg"));
    dialog->setModal(false);  // 8.3：**非阻塞**（不 grab、不等窗口）
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(
        QStringLiteral("没找到 ffmpeg。可以自动安装（项目自带的 download_ffmpeg.py），"
                       "也可以手动指定 ffmpeg.exe / ffprobe.exe 的位置。")));
    auto* buttons = new QHBoxLayout;
    // 8.3：**三个**按钮 —— 自动安装 / 手动选择 / 稍后
    auto* install = new QPushButton(QStringLiteral("自动安装"));
    auto* choose = new QPushButton(QStringLiteral("手动选择"));
    auto* later = new QPushButton(QStringLiteral("稍后"));
    buttons->addWidget(install);
    buttons->addWidget(choose);
    buttons->addWidget(later);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    QObject::connect(later, &QPushButton::clicked, dialog, &QWidget::hide);
    // 8.4：自动安装**只能挂在按钮上**（绝不在构造期/后台偷偷装）
    QObject::connect(install, &QPushButton::clicked, this, [this] {
        const QString message = auto_install();
        if (status_ != nullptr) status_->setText(message);
    });
    QObject::connect(choose, &QPushButton::clicked, this, [this, dialog] {
        const QString picked = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 ffmpeg.exe"), QString(), QStringLiteral("ffmpeg (ffmpeg*.exe)"));
        if (picked.isEmpty()) return;
        const QDir dir = QFileInfo(picked).absoluteDir();
        QString probe = dir.filePath(QStringLiteral("ffprobe.exe"));
        if (!QFileInfo::exists(probe)) probe.clear();  // 8.4：缺 ffprobe 如实报，不假装有
        QString error;
        if (!set_manual(picked, probe, &error) && status_ != nullptr) status_->setText(error);
        dialog->hide();
    });
    dialog_ = dialog;
    dialog->show();  // show 而不是 exec ⇒ 不阻塞
    return dialog;
}

QString VideoPage::auto_install() {
    // 8.4：先试**项目自带**的 download_ffmpeg.py（在程序目录里）；没有就如实报。
    // 这里刻意不去 import imageio-ffmpeg / 不去联网 —— 静默自救是明令禁止的。
    const std::filesystem::path script =
        xrkit::core::path_from_utf8(xrkit::core::app_dir()) / "download_ffmpeg.py";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(script, ec)) {
        return QStringLiteral("自动安装不可用：程序目录里没有 download_ffmpeg.py（不联网、不静默自救）");
    }
    return QStringLiteral("请运行 ") + utf8(xrkit::core::path_to_utf8(script)) +
           QStringLiteral(" 完成下载（它会校验并拒绝 ffmpeg 8.x）");
}

}  // namespace xrkit::shell
