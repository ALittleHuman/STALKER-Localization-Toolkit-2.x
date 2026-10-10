#include "video_page.h"

#include "xrkit/core/path.h"
#include "xrkit/core/theme.h"

#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>   // ★ P17-7（03 F-06）：取消改成**定时器兜底**（不再 waitForFinished 阻塞主线程）
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <functional>

#if defined(_WIN32)
#include <windows.h>  // CREATE_NO_WINDOW（子进程无控制台窗口）
#endif

namespace xrkit::shell {
namespace {

QString utf8(const std::string& text) { return QString::fromStdString(text); }

// 子进程一律走这里：**同一份**实现（`CREATE_NO_WINDOW` 缺了就会弹 cmd 窗口，
// 门禁 `tests/check_no_console.py` 按文件判它）。ffprobe / `ffmpeg -encoders` / 转码都用它。
QProcess* new_child_process(QObject* parent) {
    auto* process = new QProcess(parent);
#ifdef _WIN32
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#else
    (void)parent;
#endif
    return process;
}

QString native(const QString& path) { return QDir::toNativeSeparators(path); }

// 1.x `_fmt`：面板里"一条媒体"的那几行（顺序、单位、小数位照 1.x）
QStringList media_lines(const QString& path, const engine::video::MediaInfo& info) {
    QStringList lines;
    lines << QStringLiteral("  文件: %1").arg(QFileInfo(path).fileName());
    lines << QStringLiteral("  大小: %1 MB").arg(info.size_mb, 0, 'f', 1);
    if (info.duration > 0) {
        const int total = static_cast<int>(info.duration);
        lines << QStringLiteral("  时长: %1:%2")
                     .arg(total / 60)
                     .arg(total % 60, 2, 10, QLatin1Char('0'));
    }
    if (info.has_video) {
        QString line = QStringLiteral("  视频: %1  %2x%3  %4fps")
                           .arg(utf8(info.video.codec))
                           .arg(info.video.width)
                           .arg(info.video.height)
                           .arg(info.video.fps, 0, 'f', 2);
        if (info.video.bitrate > 0) {
            line += QStringLiteral("  %1kbps").arg(info.video.bitrate / 1000);
        }
        lines << line;
    }
    if (info.has_audio) {
        QString line = QStringLiteral("  音频: %1  %2ch  %3Hz")
                           .arg(utf8(info.audio.codec))
                           .arg(info.audio.channels)
                           .arg(info.audio.sample_rate);
        if (info.audio.bitrate > 0) {
            line += QStringLiteral("  %1kbps").arg(info.audio.bitrate / 1000);
        }
        lines << line;
    }
    return lines;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 转码进程（引擎只算 argv；起进程 / 读 stderr / 取消都在这里）
// ═══════════════════════════════════════════════════════════════════════════


// ★★ 2026-10-07（审计补充 F1-1）：**放弃一个子进程对象**的**唯一**实现。
//
// 事实（修前）：`cancel_transcode` 的取消超时支走 `deleteLater(); process_ = nullptr;`
// 却**没有断开**，而 `finished` 回调读的是**成员** `process_`（此刻已是 nullptr）⇒
//   ① 对 nullptr 调 `deleteLater()`；② 取消超时后**立刻再点转换** ⇒ 旧对象的回调把
//      **新**的进程对象删掉。同形还有 `probe_process_` / `encoder_process_` 的
//      `waitForStarted` 失败支、以及 python 自动安装的 `install_process_`。
//
// 口径（同类只维护一套）：**先把成员置空**（回调里的守卫立刻生效）→ **再 disconnect**
// （`kill`/`waitForFinished`/`deleteLater` 期间绝不把 `finished` 派回来）→ 最后 `deleteLater()`。
// 析构、取消超时、起不来、回调自身收尾**全部**走这里 ⇒ 静态判据只需要盯住这一个函数。
void VideoPage::discard_process(QProcess*& slot) {
    QProcess* process = slot;
    slot = nullptr;
    if (process == nullptr) return;
    process->disconnect();
    process->deleteLater();
}

VideoPage::~VideoPage() {
    shutting_down_ = true;
    const auto stop = [](QProcess* process) {
        if (process == nullptr) return;
        process->disconnect();  // 先断开：kill/wait 期间绝不把 finished 派回来
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(2000);
        }
    };
    stop(probe_process_);
    stop(encoder_process_);
    stop(install_process_);
    if (process_ != nullptr) {
        const bool running = process_->state() != QProcess::NotRunning;
        stop(process_);
        if (running) {
            // 与「取消」同口径：不留半成品（回调已断开，这里自己删）
            cancel_token_.cancel();
            if (!output_path_.isEmpty() && QFileInfo::exists(output_path_)) {
                QFile::remove(output_path_);
            }
        }
    }
    discard_process(probe_process_);
    discard_process(encoder_process_);
    discard_process(install_process_);
    discard_process(process_);
}

bool VideoPage::start_transcode(const engine::video::TranscodeRequest& request, QString* error) {
    if (process_ != nullptr) {
        if (error != nullptr) *error = QStringLiteral("已经在转码中");
        return false;
    }
    cancel_token_.reset();  // 1.x：新一轮要显式 reset（convert() 自己不清零）
    progress_percent_ = 0;
    last_error_.clear();
    last_cancel_message_.clear();
    stderr_tail_.clear();
    output_path_ = native(utf8(request.output));

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
    process_ = new_child_process(this);
    process_->setProgram(QString::fromStdString(plan.argv.front()));
    process_->setArguments(args);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        if (shutting_down_) return;
        handle_stderr_text(QString::fromUtf8(process_->readAllStandardError()));
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (shutting_down_) return;
        // 先把进程对象收干净（口径：跑完 / 取消 / 失败都不能留孤儿进程）
        discard_process(process_);
        if (cancel_token_.cancelled()) {
            // 1.x：取消就**删掉半成品**（不留"看着像成功"的输出）；删没删掉都要**说出来**
            QString detail;
            if (!output_path_.isEmpty() && QFileInfo::exists(output_path_)) {
                detail = QFile::remove(output_path_)
                             ? QStringLiteral("，已删除半成品文件 %1").arg(output_path_)
                             : QStringLiteral("，但半成品文件删不掉 %1（请手动删除）")
                                   .arg(output_path_);
            } else {
                detail = QStringLiteral("，没有半成品文件可删");
            }
            last_error_ = QStringLiteral("已取消") + detail;
            last_cancel_message_ = last_error_;
            progress_percent_ = 0;
            if (progress_ != nullptr) progress_->setValue(0);
            if (progress_text_ != nullptr) progress_text_->setText(last_cancel_message_);
            log_to_host(QStringLiteral("视频转换：") + last_cancel_message_);
        } else if (status != QProcess::NormalExit || code != 0) {
            const QString tail = last_stderr_tail();
            last_error_ = QStringLiteral("ffmpeg 退出码 %1（%2）")
                              .arg(code)
                              .arg(status == QProcess::NormalExit ? QStringLiteral("正常退出")
                                                                  : QStringLiteral("异常终止"));
            if (!tail.isEmpty()) {
                last_error_ += QStringLiteral("；stderr 末尾：\n") + tail;
            }
            progress_percent_ = 0;
            if (progress_ != nullptr) progress_->setValue(0);
            if (progress_text_ != nullptr) {
                // 界面上留下**第一手理由**（原文在面板与日志里；不静默）
                progress_text_->setText(QStringLiteral("失败：ffmpeg 退出码 %1").arg(code));
            }
            log_to_host(QStringLiteral("视频转换失败：") + last_error_);
            // 1.x：失败时输出文件**保留**供排障（只有取消才删）
        } else {
            progress_percent_ = 100;
            if (progress_ != nullptr) progress_->setValue(100);
            if (progress_text_ != nullptr) {
                progress_text_->setText(
                    QStringLiteral("完成: %1").arg(QFileInfo(output_path_).fileName()));
            }
            log_to_host(QStringLiteral("视频转换完成：") + output_path_);
            // 1.x：转换完成后**再解析一次产物**，并把结果贴到面板（不是抹掉原对比）
            start_probe(output_path_, ProbeTarget::Result);
        }
        set_running_ui(false);
        refresh_display();  // 失败原文 / 产物那一屏都要在面板上可见（不静默）
    });
    process_->start();
    // ★★ 2026-10-10（P17-7，审计 03 F-06 Medium）：**主线程不再等 10 秒**（见
    //   `wait_for_start_probed` 的说明）。同步只判"确定起不来"，晚一拍的失败由下面的
    //   `errorOccurred` 兜底（两半缺一不可：只缩短等待会把失败变成静默）。
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (shutting_down_ || err != QProcess::FailedToStart) return;
        last_error_ = QStringLiteral("启动失败：%1（进程随后才报）")
                          .arg(process_ != nullptr ? process_->errorString() : QString());
        discard_process(process_);
        if (progress_text_ != nullptr) progress_text_->setText(QStringLiteral("失败：") + last_error_);
        log_to_host(QStringLiteral("视频转换启动失败：") + last_error_);
        set_running_ui(false);
        refresh_display();
    });
    if (!wait_for_start_probed(process_)) {
        last_error_ = QStringLiteral("启动失败：%1（程序 %2）")
                          .arg(process_->errorString())
                          .arg(QString::fromStdString(plan.argv.front()));
        // ★ 2026-10-07（F1-1）：与 cancel_transcode 同一口径 —— **先置空再断开**
        //   （`waitForStarted` 超时返回 false 时进程仍可能随后真的起来并派 `finished`）。
        discard_process(process_);
        if (error != nullptr) *error = last_error_;
        if (progress_text_ != nullptr) progress_text_->setText(QStringLiteral("失败：") + last_error_);
        log_to_host(QStringLiteral("视频转换启动失败：") + last_error_);
        return false;
    }
    set_running_ui(true);
    if (progress_ != nullptr) progress_->setValue(0);
    if (progress_text_ != nullptr) progress_text_->setText(QStringLiteral("转换中… 0%"));
    return true;
}

void VideoPage::cancel_transcode() {
    cancel_token_.cancel();  // 令牌先置位（1.x `Converter.cancel()` 的第一件事）
    if (process_ == nullptr) {
        // 没在跑也要说出来（"点了取消但什么都没发生"最容易被当成静默失败）
        last_cancel_message_ = QStringLiteral("已取消：当前没有在跑的转换");
        if (progress_text_ != nullptr) progress_text_->setText(last_cancel_message_);
        return;
    }
    log_to_host(QStringLiteral("视频转换：用户取消"));
    process_->kill();
    // ★★ 2026-10-10（P17-7，审计 03 F-06 Medium）：**取消不再在主线程上等 5 秒**。
    //   事实（修前）：`kill()` 之后紧跟 `waitForFinished(5000)` —— 它跑在按钮回调这条
    //   **主线程**路径上，ffmpeg 不响应 kill 时界面就冻结 5 秒（Windows 上还会标"无响应"）。
    //   改法：改成**定时器兜底**（`QTimer::singleShot`，非阻塞）—— 到点还没收干净才走
    //   "取消超时"那一支；正常情况下 `finished` 回调会先把 `process_` 置空 ⇒ 定时器空转。
    //   判据：run_ci 的 shell-smoke 静态锁要求 `waitForFinished` 不再出现在取消路径上
    //   （只允许析构里那个 2 秒的收尾等待）。
    QTimer::singleShot(5000, this, [this] {
        if (process_ == nullptr) return;   // `finished` 已经收干净（正常取消）
        // 5 秒没死透 ⇒ 如实报（不假装停了），并把对象收干净。
        // ★★ 2026-10-07（审计补充 F1-1）：**必须走 `discard_process`**（先置空、再
        //   `disconnect()`、最后 `deleteLater()`）。修前这一支只 `deleteLater(); process_ = nullptr;`
        //   而**没有断开** ⇒ 这个对象还在事件循环里排着队、进程也可能随后才退出 ⇒
        //   `finished` 会派回来，回调读的**成员** `process_` 已是 nullptr：
        //     ① 对 nullptr 调 `deleteLater()`；② 取消超时后立刻再点转换 ⇒ 旧对象的回调
        //        把**新**的进程对象删掉。析构里早就有正确写法可对照（`process->disconnect()`）。
        discard_process(process_);
        last_error_ = QStringLiteral("取消超时：ffmpeg 没在 5 秒内退出（进程已放弃等待）");
        // ★ 同 F3：超时支不许**留半成品**（正常取消那条路会删，这里原来漏了）——
        //   否则用户看到的是一个"看着像成功"的输出文件。删没删掉都要说出来。
        if (!output_path_.isEmpty() && QFileInfo::exists(output_path_)) {
            last_error_ += QFile::remove(output_path_)
                               ? QStringLiteral("；已删除半成品文件 %1").arg(output_path_)
                               : QStringLiteral("；但半成品文件删不掉 %1（请手动删除）")
                                     .arg(output_path_);
        }
        if (progress_text_ != nullptr) progress_text_->setText(QStringLiteral("失败：") + last_error_);
        log_to_host(QStringLiteral("视频转换：") + last_error_);
        set_running_ui(false);
    });
}

// ★★ 2026-10-10（P17-7，审计 03 F-06 Medium）：**起进程时的"短窗口探测"（唯一一份）**。
//   修前四个调用点都是 `waitForStarted(10000)` —— 跑在按钮回调 / `apply_file` /
//   `request_transcode` 这条**主线程**路径上，进程启动慢（exe 缺依赖、杀软拦截、
//   网络盘上的 python）时界面冻结，Windows 上被标成"无响应"。审计原文：
//   「对 10s 的 `waitForStarted` 尤其代价高」。
//   语义（**保住既有同步契约，同时不再长阻塞**）：
//     · 返回 true  ⇔ "这一刻没有确凿的启动失败"（可能仍在 Starting）—— 调用方照常往下走；
//     · 返回 false ⇔ 进程**已经确定没起来**（`NotRunning`，Windows 上 exe 不存在/无权执行
//       几乎立刻就是这一态）—— 调用方走既有的"失败 + 给理由"那一支。
//   剩下的"晚一拍才失败"由各调用点接的 `&QProcess::errorOccurred` 兜底（**零静默**：
//   失败理由一样要写到状态标签 + 宿主日志里）。两半缺一不可 —— 只缩短等待会让失败变静默。
bool VideoPage::wait_for_start_probed(QProcess* process) {
    static constexpr int kStartProbeMs = 500;   // 修前 10 000
    if (process == nullptr) return false;
    if (process->waitForStarted(kStartProbeMs)) return true;
    return process->state() != QProcess::NotRunning;
}

void VideoPage::handle_stderr_text(const QString& text) {
    // 1.x：**每一行** stderr 上先看取消、再抓 `time=`（顺序不能颠倒）；进度口径在引擎里。
    const std::string raw = text.toUtf8().toStdString();
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t nl = raw.find('\n', start);
        const std::string line =
            raw.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty()) {
            stderr_tail_.insert(stderr_tail_.end(), line.begin(), line.end());
            stderr_tail_.push_back('\n');
            const size_t limit = 4096;
            if (stderr_tail_.size() > limit) {
                stderr_tail_.erase(stderr_tail_.begin(), stderr_tail_.begin() +
                                                              (stderr_tail_.size() - limit));
            }
        }
        const engine::video::ProgressSample sample =
            engine::video::progress_from_line(line, duration_sec_);
        if (sample.matched) {
            progress_percent_ = static_cast<int>(sample.percent);
            if (progress_ != nullptr) progress_->setValue(progress_percent_);
            if (progress_text_ != nullptr) {
                progress_text_->setText(
                    QStringLiteral("转换中… %1%").arg(progress_percent_));
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ffprobe / ffmpeg -encoders（异步；主线程只落地结论）
// ═══════════════════════════════════════════════════════════════════════════

engine::video::FfmpegPaths VideoPage::resolve_ffmpeg() {
    locator_.ensure();
    return locator_.paths();
}

QString VideoPage::ffmpeg_search_summary() const {
    // 8.1：只认两处。用户 2026-10-04 红线：「不能静默报错」⇒ 找不着时要**报出试过哪些路径**。
    const std::string config = xrkit::core::load_user_value("ffmpeg", "path");
    const QString app_dir = utf8(xrkit::core::app_dir());
    const QString local = QDir(app_dir).filePath(QStringLiteral("ffmpeg.exe"));
    return QStringLiteral("试过两处：① user.ltx [ffmpeg] path = %1；② 程序目录 %2（%3）。"
                          "按口径**不搜 PATH**。")
        .arg(config.empty() ? QStringLiteral("（未设置）") : utf8(config))
        .arg(native(local))
        .arg(QFileInfo::exists(local) ? QStringLiteral("文件在，但探测没认它")
                                      : QStringLiteral("不存在"));
}

QProcess* VideoPage::make_child_process() { return new_child_process(this); }

QString VideoPage::last_stderr_tail() const {
    if (stderr_tail_.empty()) return QString();
    return QString::fromUtf8(stderr_tail_.data(), static_cast<int>(stderr_tail_.size())).trimmed();
}

void VideoPage::start_probe(const QString& path, ProbeTarget target) {
    if (path.isEmpty()) return;
    if (probe_process_ != nullptr) {
        // 1.x 的并发保护（同一时刻只允许一次探测）；但**跳过要留痕**（用户红线：不能静默）
        note_skip(QStringLiteral("上一份文件的参数还在解析中 ⇒ 暂不解析 %1").arg(native(path)));
        return;
    }
    const engine::video::FfmpegPaths paths = resolve_ffmpeg();
    if (paths.ffprobe.empty()) {
        apply_probe_result(path, target, engine::video::MediaInfo(),
                           QStringLiteral("没找到 ffprobe（") + ffmpeg_search_summary() +
                               QStringLiteral("；ffprobe 与 ffmpeg 可以分开放，但都要在程序目录"
                                              "或 user.ltx 里）"));
        return;
    }
    probe_target_ = target;
    probe_path_ = path;
    probe_process_ = make_child_process();
    connect(probe_process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (shutting_down_) return;
        const QString out = QString::fromUtf8(probe_process_->readAllStandardOutput());
        const QString err = QString::fromUtf8(probe_process_->readAllStandardError());
        const QString path = probe_path_;
        const ProbeTarget target = probe_target_;
        discard_process(probe_process_);
        if (status != QProcess::NormalExit || code != 0) {
            apply_probe_result(path, target, engine::video::MediaInfo(),
                               QStringLiteral("ffprobe 返回码 %1：%2")
                                   .arg(code)
                                   .arg(err.trimmed().left(200)));
            return;
        }
        const engine::video::MediaInfo info =
            engine::video::parse_ffprobe_json(out.toUtf8().toStdString());
        if (!info.parsed) {
            apply_probe_result(path, target, engine::video::MediaInfo(),
                               QStringLiteral("ffprobe 的输出不是合法 JSON（%1 字节）")
                                   .arg(out.toUtf8().size()));
            return;
        }
        apply_probe_result(path, target, info, QString());
    });
    probe_process_->start(
        QString::fromStdString(paths.ffprobe),
        QStringList{QStringLiteral("-v"), QStringLiteral("quiet"), QStringLiteral("-print_format"),
                    QStringLiteral("json"), QStringLiteral("-show_format"),
                    QStringLiteral("-show_streams"), native(path)});
    // ★★ P17-7（03 F-06）：晚一拍的启动失败也要有理由（见 `wait_for_start_probed`）
    connect(probe_process_, &QProcess::errorOccurred, this,
            [this, path, target](QProcess::ProcessError err) {
                if (shutting_down_ || err != QProcess::FailedToStart) return;
                const QString reason =
                    QStringLiteral("起不了 ffprobe（随后才报）：%1")
                        .arg(probe_process_ != nullptr ? probe_process_->errorString() : QString());
                discard_process(probe_process_);
                apply_probe_result(path, target, engine::video::MediaInfo(), reason);
            });
    if (!wait_for_start_probed(probe_process_)) {
        const QString reason = QStringLiteral("起不了 ffprobe：%1").arg(probe_process_->errorString());
        // ★ 2026-10-07（F1-1）：**先置空再断开** —— 否则这个对象随后派 `finished` 时，
        //   回调读的是成员 `probe_process_`（已经是 nullptr）⇒ 对 nullptr 解引用。
        discard_process(probe_process_);
        apply_probe_result(path, target, engine::video::MediaInfo(), reason);
    }
}

void VideoPage::start_encoder_scan() {
    if (encoder_process_ != nullptr || encoders_scanned_) return;
    const engine::video::FfmpegPaths paths = resolve_ffmpeg();
    if (!paths.found) {
        // 找不到 ffmpeg ⇒ 编码器集合只能空着；**原因要可见**（用户红线）
        complete_encoder_scan(false, QStringLiteral("没找到 ffmpeg，编码器表没扫：") +
                                        ffmpeg_search_summary());
        return;
    }
    encoder_process_ = make_child_process();
    connect(encoder_process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (shutting_down_) return;
        const QString out = QString::fromUtf8(encoder_process_->readAllStandardOutput());
        const QString err = QString::fromUtf8(encoder_process_->readAllStandardError());
        discard_process(encoder_process_);
        if (status != QProcess::NormalExit || code != 0) {
            complete_encoder_scan(false, QStringLiteral("`ffmpeg -encoders` 返回码 %1：%2")
                                            .arg(code)
                                            .arg(err.trimmed().left(200)));
            return;
        }
        available_encoders_ =
            engine::video::parse_encoder_list(out.toUtf8().toStdString());
        if (available_encoders_.empty()) {
            complete_encoder_scan(false, QStringLiteral("`ffmpeg -encoders` 里一个编码器都没解析出来"
                                                        "（输出 %1 字节）")
                                            .arg(out.toUtf8().size()));
            return;
        }
        complete_encoder_scan(true, QString());
    });
    encoder_process_->start(QString::fromStdString(paths.ffmpeg),
                            QStringList{QStringLiteral("-encoders")});
    // ★★ P17-7（03 F-06）：晚一拍的启动失败也要有理由（见 `wait_for_start_probed`）
    connect(encoder_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (shutting_down_ || err != QProcess::FailedToStart) return;
        const QString reason = QStringLiteral("起不了 ffmpeg（随后才报）：%1")
                                   .arg(encoder_process_ != nullptr ? encoder_process_->errorString()
                                                                    : QString());
        discard_process(encoder_process_);
        complete_encoder_scan(false, reason);
    });
    if (!wait_for_start_probed(encoder_process_)) {
        const QString reason = QStringLiteral("起不了 ffmpeg：%1").arg(encoder_process_->errorString());
        // ★ 2026-10-07（F1-1）：同 `probe_process_` —— 先置空再断开。
        discard_process(encoder_process_);
        complete_encoder_scan(false, reason);
    }
}

void VideoPage::complete_encoder_scan(bool ok, const QString& reason) {
    encoders_scanned_ = true;
    encoder_note_ = ok ? QString() : reason;
    if (!ok) log_to_host(QStringLiteral("视频转换：编码器表不可用 —— ") + reason);
    refresh_display();
    if (pending_transcode_) {
        pending_transcode_ = false;
        request_transcode();  // 扫描完成（或确定扫不到）之后再按引擎的结论开跑
    }
}

void VideoPage::apply_file(const QString& path, bool is_reference) {
    if (path.trimmed().isEmpty()) return;
    // 同一份文件**同一个目标**已经在解析 ⇒ 不重复起进程（`QLineEdit::textChanged` 一次改动
    // 可能来两趟；重复那趟不该被记成"跳过"，那是同一件事，不是被丢掉的信息）
    const ProbeTarget wanted = is_reference ? ProbeTarget::Reference : ProbeTarget::Source;
    if (probe_process_ != nullptr && probe_path_ == path.trimmed() && probe_target_ == wanted) {
        return;
    }
    if (is_reference) {
        reference_probed_ = false;
        reference_probe_error_.clear();
    } else {
        source_probed_ = false;
        source_probe_error_.clear();
        auto_output();  // 1.x：只有**源**落地时才重推默认输出名（`_on_source` → `_auto_output`）
    }
    start_probe(path, is_reference ? ProbeTarget::Reference : ProbeTarget::Source);
    if (!encoders_scanned_ && encoder_process_ == nullptr) start_encoder_scan();
    refresh_display();
    refresh_buttons();
}

void VideoPage::apply_probe_result(const QString& path, ProbeTarget target,
                                   const engine::video::MediaInfo& info, const QString& error) {
    const bool bad = !error.isEmpty() || !info.parsed;
    switch (target) {
        case ProbeTarget::Source:
            source_probed_ = !bad;
            if (bad) {
                source_path_.clear();
                source_probe_error_ = error.isEmpty() ? QStringLiteral("输出不完整") : error;
            } else {
                source_info_ = info;
                source_path_ = path;
                source_probe_error_.clear();
            }
            break;
        case ProbeTarget::Reference:
            reference_probed_ = !bad;
            if (bad) {
                reference_path_.clear();
                reference_probe_error_ = error.isEmpty() ? QStringLiteral("输出不完整") : error;
            } else {
                reference_info_ = info;
                reference_path_ = path;
                reference_probe_error_.clear();
            }
            break;
        case ProbeTarget::Result:
            result_path_ = path;
            result_info_ = info;
            result_probe_error_ = bad ? (error.isEmpty() ? QStringLiteral("输出不完整") : error)
                                      : QString();
            if (bad) {
                log_to_host(QStringLiteral("视频转换：产物参数没解析出来 —— ") + result_probe_error_);
            }
            break;
    }
    if (bad && target != ProbeTarget::Result) {
        // 失败**不静默**：进面板 + 进日志（1.x 是弹窗，本项目口径是不阻塞）
        const QString what = target == ProbeTarget::Source ? QStringLiteral("源文件")
                                                           : QStringLiteral("参考 OGM");
        log_to_host(QStringLiteral("视频转换：%1 参数解析失败 —— %2").arg(what).arg(error));
    }
    refresh_display();
    refresh_buttons();
}

// ═══════════════════════════════════════════════════════════════════════════
// 输出（**文件**）与默认名（1.x `_auto_output` / `_browse_output`）
// ═══════════════════════════════════════════════════════════════════════════

QString VideoPage::default_output_path() const {
    const QString source = source_edit_->text().trimmed();
    if (source.isEmpty()) return QString();
    const QFileInfo src(source);
    const QDir source_dir = src.absoluteDir();
    const QString stem = src.completeBaseName();  // 1.x `Path.with_suffix` 保留主名
    QString out = source_dir.filePath(stem + QStringLiteral(".ogm"));
    const QString reference = reference_edit_->text().trimmed();
    if (!reference.isEmpty()) {
        // 1.x：有参考 ⇒ 输出落到**参考所在目录**（同主名）
        out = QFileInfo(reference).absoluteDir().filePath(stem + QStringLiteral(".ogm"));
        // 源与参考同目录同主名时（X.mp4 + X.ogm），上面算出来的输出就是**参考文件本身**。
        // 1.x 原实现只靠一次"覆盖?"确认挡着 —— 这里直接改到源文件目录，避免默认值指向输入。
        if (QDir::cleanPath(out).compare(QDir::cleanPath(QFileInfo(reference).absoluteFilePath()),
                                         Qt::CaseInsensitive) == 0) {
            out = source_dir.filePath(stem + QStringLiteral(".new.ogm"));
        }
    }
    return native(QDir::cleanPath(out));
}

void VideoPage::auto_output() {
    if (source_edit_->text().trimmed().isEmpty()) return;  // 1.x：没有源就不动输出框
    const QString out = default_output_path();
    if (!out.isEmpty() && output_edit_->text() != out) output_edit_->setText(out);
}

void VideoPage::browse_output() {
    // ① 起点 = `resolve_dialog_start`（继承上次的选择 → 唯一源所在目录 → 工具目录）
    const QString start = resolve_dialog_start(output_dialog_start());
    // ② 默认文件名 = 1.x 规则推出来的那个（`_auto_output`）
    const QString initial = QFileInfo(default_output_path()).fileName();
    const QString picked = ask_save_output(start, initial);
    if (!picked.isEmpty()) output_edit_->setText(native(picked));
}

QString VideoPage::ask_save_output(const QString& start_dir, const QString& initial_name) {
    // 1.x `asksaveasfilename(defaultextension=".ogm", filetypes=[("OGM 视频","*.ogm")])`
    QString initial = start_dir;
    if (!initial_name.isEmpty()) initial = QDir(start_dir).filePath(initial_name);
    return QFileDialog::getSaveFileName(this, QStringLiteral("保存为 OGM"), initial,
                                        QStringLiteral("OGM 视频 (*.ogm)"));
}

QString VideoPage::ask_open_file(const QString& title, const QString& filter,
                                 const QString& start_path) {
    return QFileDialog::getOpenFileName(this, title, start_path, filter);
}

bool VideoPage::confirm_overwrite(const QString& reason) {
    // ★ 2026-10-07（审计补充 F1-2）：`-y` 恒在 argv 里 ⇒ 覆盖前必须**先说清楚再问**。
    //   文案里带引擎给的第一手理由（哪个文件、为什么会覆盖）。答否 ⇒ 一个字都不写。
    return QMessageBox::question(this, QStringLiteral("确认覆盖"),
                                 reason + QStringLiteral("\n\n要覆盖它吗？")) == QMessageBox::Yes;
}

// ═══════════════════════════════════════════════════════════════════════════
// 参数面板（1.x `_refresh_display` 的字段）
// ═══════════════════════════════════════════════════════════════════════════

QString VideoPage::info_panel_text() const {
    QStringList lines;
    const QString source_text = source_edit_->text().trimmed();
    const QString reference_text = reference_edit_->text().trimmed();

    const auto block = [&lines](const QString& title, const QString& path,
                                const engine::video::MediaInfo& info, const QString& error) {
        lines << QStringLiteral("══ %1 ══").arg(title);
        if (!error.isEmpty()) {
            // 「探测不到」必须**带原因**（用户 2026-10-04 红线）；文件行由这里自己给
            if (!path.isEmpty()) {
                lines << QStringLiteral("  文件: %1").arg(QFileInfo(path).fileName());
            }
            lines << QStringLiteral("  ⚠ 参数未探测到：%1").arg(error);
        } else {
            lines << media_lines(path, info);  // 里面已经含"文件/大小/时长/视频/音频"那几行
        }
        lines << QString();
    };

    if (!reference_text.isEmpty()) {
        if (reference_probed_) {
            block(QStringLiteral("参考 OGM"), reference_path_, reference_info_, QString());
        } else {
            block(QStringLiteral("参考 OGM"), reference_text, engine::video::MediaInfo(),
                  reference_probe_error_.isEmpty() ? QStringLiteral("正在解析（还没出结论）")
                                                   : reference_probe_error_);
        }
    }
    if (!source_text.isEmpty()) {
        if (source_probed_) {
            block(QStringLiteral("源文件"), source_path_, source_info_, QString());
        } else {
            block(QStringLiteral("源文件"), source_text, engine::video::MediaInfo(),
                  source_probe_error_.isEmpty() ? QStringLiteral("正在解析（还没出结论）")
                                                : source_probe_error_);
        }
    }

    if (source_probed_ && reference_probed_) {
        // 转换参数：**全部读引擎的结论**（`plan_transcode`），页面不再判一遍阈值/默认值
        engine::video::TranscodeRequest request;
        request.has_reference = true;
        request.reference = reference_info_;
        request.source = source_info_;
        request.available_encoders = available_encoders_;
        const engine::video::TranscodePlan plan = engine::video::plan_transcode(request);
        const engine::video::VideoStream& sv = source_info_.video;
        const engine::video::VideoStream& rv = reference_info_.video;
        const engine::video::AudioStream& sa = source_info_.audio;
        const engine::video::AudioStream& ra = reference_info_.audio;

        lines << QStringLiteral("══ 转换参数（匹配参考 OGM） ══");
        if (source_info_.has_video && reference_info_.has_video) {
            lines << QStringLiteral("  视频编码: %1 -> %2")
                         .arg(utf8(rv.codec), utf8(plan.video_encoder));
            lines << QStringLiteral("  分辨率:   %1x%2 -> %3x%4  [%5]")
                         .arg(sv.width)
                         .arg(sv.height)
                         .arg(rv.width)
                         .arg(rv.height)
                         .arg(plan.scale_applied ? QStringLiteral("将缩放") : QStringLiteral("OK"));
            lines << QStringLiteral("  帧率:     %1 -> %2  [%3]")
                         .arg(sv.fps, 0, 'f', 2)
                         .arg(rv.fps, 0, 'f', 2)
                         .arg(plan.fps_applied ? QStringLiteral("将调整") : QStringLiteral("OK"));
            if (rv.bitrate > 0) {
                lines << QStringLiteral("  视频码率: %1 kbps（匹配参考）").arg(rv.bitrate / 1000);
            }
        }
        if (source_info_.has_audio && reference_info_.has_audio) {
            // 显示**实际会传给 ffmpeg 的值**（引擎的结论；参考为 0 时它已经回退成 48000/2/160k）
            lines << QStringLiteral("  音频:     采样率 %1Hz  %2ch  %3kbps")
                         .arg(plan.audio_sample_rate)
                         .arg(plan.audio_channels)
                         .arg(plan.audio_bitrate / 1000);
            lines << QStringLiteral("            源 %1ch -> %2ch  [%3]")
                         .arg(sa.channels)
                         .arg(plan.audio_channels)
                         .arg(sa.channels == ra.channels ? QStringLiteral("OK")
                                                         : QStringLiteral("将调整"));
        }
        if (plan.encoder_warning) {
            // 降级必须可见（1.x 会告警：输出编码与参考不一致）
            lines << QStringLiteral("  ⚠ 没有可用编码器表 ⇒ 回退内置 mpeg4（输出编码可能与参考不一致）");
        }
        lines << QString();
    }

    if (result_probe_error_.isEmpty() && result_info_.parsed) {
        lines << QStringLiteral("══ 输出文件（本次转换结果） ══");
        lines << media_lines(result_path_, result_info_);
        lines << QString();
    } else if (!result_probe_error_.isEmpty()) {
        lines << QStringLiteral("══ 输出文件（本次转换结果） ══");
        lines << QStringLiteral("  ⚠ 产物参数未探测到：%1").arg(result_probe_error_);
        lines << QString();
    }

    if (!encoder_note_.isEmpty()) {
        lines << QStringLiteral("  ⚠ 编码器表：%1").arg(encoder_note_);
    }
    if (!skip_reasons_.isEmpty()) {
        lines << QStringLiteral("══ 跳过 %1 项（逐条原因） ══").arg(skip_reasons_.size());
        for (const QString& reason : skip_reasons_) {
            lines << QStringLiteral("  · %1").arg(reason);
        }
    }
    if (!last_error_.isEmpty()) {
        lines << QStringLiteral("══ 最近一次转换的错误原文 ══");
        lines << QStringLiteral("  %1").arg(QString(last_error_).replace(QLatin1Char('\n'),
                                                                        QStringLiteral("\n  ")));
    }

    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();
    if (lines.isEmpty()) return QStringLiteral("拖入视频文件查看参数");  // 1.x 的空状态文案
    return lines.join(QLatin1Char('\n'));
}

void VideoPage::note_skip(const QString& reason) {
    skip_reasons_ << reason;
    log_to_host(QStringLiteral("视频转换：跳过 —— ") + reason);
}

// ═══════════════════════════════════════════════════════════════════════════
// 转换入口（「转换为 OGM」主按钮）
// ═══════════════════════════════════════════════════════════════════════════

bool VideoPage::can_transcode() const {
    // 1.x：未选源 ⇒ 灰；参考没落地也 ⇒ 灰（参考必需，且"灰着且不解释"是有意为之）
    return source_probed_ && reference_probed_ && process_ == nullptr;
}

engine::video::TranscodeRequest VideoPage::build_request(
    const engine::video::FfmpegPaths& paths) const {
    engine::video::TranscodeRequest request;
    request.ffmpeg = paths.ffmpeg;
    request.input = source_path_.toUtf8().toStdString();
    request.output = output_edit_->text().trimmed().toUtf8().toStdString();
    request.has_reference = true;
    request.reference = reference_info_;
    request.source = source_info_;
    request.available_encoders = available_encoders_;
    return request;
}

void VideoPage::request_transcode() {
    if (process_ != nullptr) return;
    if (!source_probed_) {
        note_skip(QStringLiteral("没有可用的源文件（参数没落地）⇒ 不转换"));
        refresh_display();
        return;
    }
    if (!reference_probed_) {
        note_skip(QStringLiteral("参考 OGM 没落地（1.x 口径：参考必需）⇒ 不转换"));
        refresh_display();
        return;
    }
    const engine::video::FfmpegPaths paths = resolve_ffmpeg();
    if (!paths.found) {
        // 用户红线：找不到 ffmpeg 要**明确报**（含试过的路径），不能只灰着
        last_error_ = QStringLiteral("找不到 ffmpeg，无法转换。") + ffmpeg_search_summary();
        if (progress_text_ != nullptr) progress_text_->setText(QStringLiteral("失败：") + last_error_);
        log_to_host(QStringLiteral("视频转换：") + last_error_);
        refresh_display();
        return;
    }
    if (!encoders_scanned_) {
        // 编码器表还没扫完 ⇒ 等它（1.x 把它放在工作线程里；这里异步做完再开跑）
        pending_transcode_ = true;
        start_encoder_scan();
        if (progress_text_ != nullptr) {
            progress_text_->setText(QStringLiteral("正在扫描可用编码器（ffmpeg -encoders）…"));
        }
        return;
    }
    // 1.x：输出没有 .ogm 后缀就补上
    QString out = output_edit_->text().trimmed();
    if (out.isEmpty()) {
        out = default_output_path();
        output_edit_->setText(out);
    }
    if (!out.toLower().endsWith(QStringLiteral(".ogm"))) {
        out += QStringLiteral(".ogm");
        output_edit_->setText(out);
    }
    // ── ★★ 2026-10-07（审计补充 F1-2 / F1-3）：**起跑前的输出目标守卫** ────────────────
    //   argv 里恒有 `-y`（1.x 原样）⇒ 覆盖是**静默的**。修前这里零存在性检查，而唯一的
    //   "防自毁"只作用于**默认名**、且只跟**参考**比 ⇒ 源本身是 `.ogm` 时默认输出＝源文件，
    //   叠上 `-y` 就是把**输入截断**；手打的输出框同理没有任何校验。
    //   口径（与"零静默"一致）：
    //     · 输出 == 源 / == 参考 ⇒ **直接拒**（那是输入，改输出名是唯一出路）；
    //     · 输出是别的已存在文件 ⇒ **问一次**（答否 ⇒ 不跑，且理由可见）；
    //     · 其余 ⇒ 照跑。
    engine::video::TranscodeRequest request = build_request(paths);
    request.reference_path = reference_path_.toUtf8().toStdString();
    const engine::video::OutputConflict conflict = engine::video::check_output_conflict(request);
    if (conflict != engine::video::OutputConflict::None) {
        const QString reason = utf8(engine::video::output_conflict_reason(conflict, request));
        if (conflict == engine::video::OutputConflict::Exists && confirm_overwrite(reason)) {
            log_to_host(QStringLiteral("视频转换：用户确认覆盖 ") + native(out));
        } else {
            last_error_ = (conflict == engine::video::OutputConflict::Exists)
                              ? QStringLiteral("已取消：不覆盖已存在的输出文件 ") + native(out)
                              : reason;
            if (progress_text_ != nullptr) {
                progress_text_->setText(QStringLiteral("失败：") + last_error_);
            }
            log_to_host(QStringLiteral("视频转换：") + last_error_);
            refresh_display();
            return;
        }
    }
    log_to_host(QStringLiteral("视频转换：开始 %1 -> %2").arg(native(source_path_), native(out)));
    QString error;
    if (!start_transcode(request, &error)) {
        last_error_ = error;
        refresh_display();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 界面
// ═══════════════════════════════════════════════════════════════════════════

VideoPage::VideoPage(QWidget* parent)
    : PageBase(QStringLiteral("video"), QStringLiteral("视频转换"), parent) {
    // ★ 用户 2026-09-27：「先把我截图的文字全删了」—— 页内说明整段删（六页统一）
    // ★ 用户 2026-10-04：「视频，一般都是一对一的转换，所以基本上都是**单文件形式**」
    //   ⇒ 三个输入行都是**单文件**（源 / 参考 OGM / 输出 OGM），每行整行可拖（drop_zone.h）。
    const auto make_edit = [this](const QString& name, const QString& placeholder) {
        auto* edit = new QLineEdit;
        edit->setObjectName(name);
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    source_edit_ = make_edit(QStringLiteral("video_source"), QStringLiteral("源视频文件"));
    reference_edit_ = make_edit(QStringLiteral("video_reference"), QStringLiteral("参考 OGM 文件"));
    output_edit_ = make_edit(QStringLiteral("video_output"), QStringLiteral("输出 OGM 文件"));

    const auto row = [this](const QString& label, QLineEdit* edit, std::function<void()> on_browse) {
        // ★ 2026-10-04 用户第 1 条：整行成容器 ⇒ "拖到相应区域"
        auto* container = new QWidget;
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(new QLabel(label), 0);
        layout->addWidget(edit, 1);
        auto* browse = new QPushButton(QStringLiteral("选择…"));
        layout->addWidget(browse, 0);
        QObject::connect(browse, &QPushButton::clicked, this, std::move(on_browse));
        drop_zones_.attach_row(container, this);
        return container;
    };

    body_layout()->addWidget(row(QStringLiteral("源文件："), source_edit_, [this] {
        const QString picked = ask_open_file(
            QStringLiteral("选择源视频文件"),
            QStringLiteral("视频文件 (*.mp4 *.mov *.avi *.mkv *.webm *.m4v);;所有文件 (*.*)"),
            resolve_dialog_start(source_dialog_start()));
        if (!picked.isEmpty()) source_edit_->setText(native(picked));
    }));
    body_layout()->addWidget(row(QStringLiteral("参考 OGM："), reference_edit_, [this] {
        const QString picked = ask_open_file(QStringLiteral("选择参考 OGM"),
                                             QStringLiteral("OGM 视频 (*.ogm);;所有文件 (*.*)"),
                                             resolve_dialog_start(reference_dialog_start()));
        if (!picked.isEmpty()) reference_edit_->setText(native(picked));
    }));
    body_layout()->addWidget(row(QStringLiteral("输出 OGM："), output_edit_,
                                 [this] { browse_output(); }));

    // 拖进来的路径与"选择…"走**同一份**落地实现：只在**真存在的文件**上起探测
    //（打字打到一半的路径不存在 ⇒ 不探，免得把半截路径解析失败当成结论）。
    // ★ 同一个文本只处理一次：`QLineEdit::textChanged` 对一次改动**可能来两趟**
    //   （实测：不挡住的话第二趟会在第一趟的探测刚结束时又起一次探测，把随后那份文件的
    //   探测挤成"跳过"——面板上就多出一条莫名其妙的跳过记录）。挡住它＝同一件事不重复做。
    QObject::connect(source_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString trimmed = text.trimmed();
        if (trimmed == last_source_request_) return;
        last_source_request_ = trimmed;
        const QFileInfo info(trimmed);
        if (info.exists() && info.isFile() && trimmed != source_path_) {
            apply_file(trimmed, false);
        }
    });
    QObject::connect(reference_edit_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString trimmed = text.trimmed();
        if (trimmed == last_reference_request_) return;
        last_reference_request_ = trimmed;
        const QFileInfo info(trimmed);
        if (info.exists() && info.isFile() && trimmed != reference_path_) {
            apply_file(trimmed, true);
        }
    });

    // ── 「视频参数」面板（1.x `:531-542`）：只读、可选中、等宽 ──
    info_ = new QPlainTextEdit;
    info_->setObjectName(QStringLiteral("video_info"));
    info_->setReadOnly(true);
    info_->setMinimumHeight(xrkit::core::px(200));
    info_->setLineWrapMode(QPlainTextEdit::NoWrap);
    // 等宽：**在主题给的字上要 style hint**，不去另找「系统等宽字体」那类字体——
    // 离屏自检机器上 `QFontDatabase::systemFont(FixedFont)` 很容易落到没有字形的字体上
    // （实测：面板整块空白方块，甚至 QTextDocument 布局里直接崩）。
    {
        QFont mono = info_->font();
        mono.setStyleHint(QFont::Monospace);
        info_->setFont(mono);
    }
    info_->setPlainText(info_panel_text());
    body_layout()->addWidget(info_, 1);

    // ── 进度（1.x `:561-562`）──
    progress_ = new QProgressBar;
    progress_->setObjectName(QStringLiteral("video_progress"));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    body_layout()->addWidget(progress_, 0);
    progress_text_ = new QLabel(QStringLiteral("就绪"));
    progress_text_->setObjectName(QStringLiteral("video_progress_text"));
    progress_text_->setWordWrap(true);
    body_layout()->addWidget(progress_text_, 0);

    // 8.5：三态状态栏。构造期**不探测**（8.2）⇒ 这里显示"未检测"，不是 ✗
    status_ = new QLabel;
    status_->setObjectName(QStringLiteral("video_status"));
    status_->setWordWrap(true);
    body_layout()->addWidget(status_, 0);
    body_layout()->addStretch(1);

    detect_button_ = add_action(QStringLiteral("检测 ffmpeg"));
    detect_button_->setObjectName(QStringLiteral("video_detect"));
    QObject::connect(detect_button_, &QPushButton::clicked, this, [this] {
        if (detect()) return;
        request_setup();  // 8.3：读不到就弹三按钮小窗（非阻塞）
    });
    setup_button_ = add_action(QStringLiteral("设置 ffmpeg"));
    setup_button_->setObjectName(QStringLiteral("video_setup"));
    QObject::connect(setup_button_, &QPushButton::clicked, this, [this] { request_setup(); });

    // 1.x：取消按钮平时**不出现**，跑起来才出现在主按钮左边（`:579-583`）
    cancel_button_ = add_action(QStringLiteral("取消"));
    cancel_button_->setObjectName(QStringLiteral("video_cancel"));
    cancel_button_->setVisible(false);
    QObject::connect(cancel_button_, &QPushButton::clicked, this, [this] { cancel_transcode(); });

    // 1.x 主按钮（`:574-577`）：未选源时置灰，点击走 start_transcode
    convert_button_ = add_action(QStringLiteral("转换为 OGM"));
    convert_button_->setObjectName(QStringLiteral("video_convert"));
    convert_button_->setEnabled(false);
    QObject::connect(convert_button_, &QPushButton::clicked, this, [this] { request_transcode(); });

    refresh_status();
    refresh_buttons();
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
            // 用户 2026-10-04 红线：找不到要**报出试过哪些路径**，不能只给一行泛泛的话
            return QStringLiteral("✗ 没找到 ffmpeg（只认 user.ltx 与程序目录）—— ") +
                   ffmpeg_search_summary();
    }
    return QString();
}

void VideoPage::refresh_status() {
    if (status_ != nullptr) status_->setText(status_text());
}

void VideoPage::refresh_display() {
    if (info_ != nullptr) info_->setPlainText(info_panel_text());
}

void VideoPage::refresh_buttons() {
    if (convert_button_ != nullptr) convert_button_->setEnabled(can_transcode());
    if (cancel_button_ != nullptr) cancel_button_->setVisible(process_ != nullptr);
}

void VideoPage::set_running_ui(bool running) {
    // 1.x `_set_ui_state`：跑起来就禁主按钮、亮取消；同时禁掉"设置 ffmpeg"
    //（否则转换途中换掉 ffmpeg，正在跑的进程就成孤儿）
    if (convert_button_ != nullptr) convert_button_->setEnabled(!running && can_transcode());
    if (cancel_button_ != nullptr) cancel_button_->setVisible(running);
    if (detect_button_ != nullptr) detect_button_->setEnabled(!running);
    if (setup_button_ != nullptr) setup_button_->setEnabled(!running);
    if (!running) refresh_buttons();
}

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
        QStringLiteral("没找到 ffmpeg。可以自动安装（项目自带的 download_ffmpeg.py / 随包自带的 "
                       "ffmpeg.exe），也可以手动指定 ffmpeg.exe / ffprobe.exe 的位置。")));
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
    // 8.4：自动安装**只能挂在按钮上**（绝不在构造期/后台偷偷装）；按钮要**真做事**
    //（随包自带 ⇒ 指到它并重新探测；只有脚本 ⇒ 真跑脚本；都没有 ⇒ 如实报不可用）
    QObject::connect(install, &QPushButton::clicked, this, [this] { run_auto_install(); });
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
    return dialog_;
}

QString VideoPage::auto_install() {
    // 8.4 + 用户口径"依赖自带"：**先看随包自带的那一份**，再看项目自带的下载脚本；
    // 两者都没有就如实报（不联网、不静默自救）。
    const QString app_dir = utf8(xrkit::core::app_dir());
    const QString bundled = QDir(app_dir).filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo::exists(bundled)) {
        return QStringLiteral("随包自带的 ffmpeg 已在程序目录：") + native(bundled) +
               QStringLiteral("（点击本按钮会重新探测并直接用它）");
    }
    const QString script = QDir(app_dir).filePath(QStringLiteral("download_ffmpeg.py"));
    if (QFileInfo::exists(script)) {
        return QStringLiteral("程序目录里有 ") + native(script) +
               QStringLiteral("（点击本按钮会运行它下载 ffmpeg 7.x 到程序目录，它会拒绝 8.x）");
    }
    return QStringLiteral("自动安装不可用：程序目录里既没有自带的 ffmpeg.exe，也没有 "
                          "download_ffmpeg.py（不联网、不静默自救）。") +
           ffmpeg_search_summary();
}

void VideoPage::run_auto_install() {
    // 8.4：自动安装**只由这个按钮**触发。三条路都**真做事**、且结论可见。
    if (install_process_ != nullptr) {
        // ★ 2026-10-07（F3）：上一趟还在跑 ⇒ 不许再起一个（否则成员被覆盖、前一个没人收）。
        if (status_ != nullptr) status_->setText(QStringLiteral("自动安装已经在跑，等它结束再看结论"));
        return;
    }
    const QString message = auto_install();
    last_auto_install_ = message;
    const QString app_dir = utf8(xrkit::core::app_dir());
    const QString script = QDir(app_dir).filePath(QStringLiteral("download_ffmpeg.py"));
    if (QFileInfo::exists(QDir(app_dir).filePath(QStringLiteral("ffmpeg.exe")))) {
        locator_.invalidate();
        const bool found = detect();
        if (status_ != nullptr) {
            status_->setText(message + (found ? QStringLiteral("（已重新探测：✓）")
                                              : QStringLiteral("（已重新探测：还是没认它）")));
        }
        last_auto_install_ = status_ != nullptr ? status_->text() : message;
        return;
    }
    if (QFileInfo::exists(script)) {
        // 真跑脚本（Python 缺失 / 启动失败都如实报）。脚本自己把 exe 下到**它所在目录**
        //（= 程序目录），跑完我们再重新探测一次。
        // ★ 2026-10-07（审计补充 F3）：它是**成员**（不再是局部对象）⇒ 走 `~VideoPage`
        //   里同一个 `stop()`（先 disconnect 再 kill/wait），回调也有 `shutting_down_` 守卫
        //   —— 修前这个子进程析构不停、回调还会碰已经析构的 `status_`。
        QProcess* downloader = new_child_process(this);
        install_process_ = downloader;
        if (status_ != nullptr) status_->setText(message + QStringLiteral("（正在下载…）"));
        connect(downloader, &QProcess::finished, this,
                [this, downloader](int code, QProcess::ExitStatus) {
                    if (shutting_down_) return;  // 窗口/页面已在收尾：绝不碰控件
                    const QString out = QString::fromUtf8(downloader->readAllStandardOutput());
                    const QString err = QString::fromUtf8(downloader->readAllStandardError());
                    // ★ F1-1：走同一个"放弃对象"实现（先置空再断开再 deleteLater）。
                    if (install_process_ == downloader) discard_process(install_process_);
                    else downloader->deleteLater();
                    locator_.invalidate();
                    const bool found = detect();
                    const QString tail = (out + err).trimmed().right(300);
                    if (status_ != nullptr) {
                        status_->setText(QStringLiteral("download_ffmpeg.py 退出码 %1；重新探测：%2"
                                                        "；输出末尾：%3")
                                             .arg(code)
                                             .arg(found ? QStringLiteral("✓") : QStringLiteral("✗"))
                                             .arg(tail.isEmpty() ? QStringLiteral("(无输出)") : tail));
                    }
                    log_to_host(QStringLiteral("视频转换：自动安装结束 —— 退出码 %1").arg(code));
                });
        downloader->start(QStringLiteral("python"), QStringList{native(script)});
        // ★★ P17-7（03 F-06）：晚一拍的启动失败也要有理由（见 `wait_for_start_probed`）
        connect(downloader, &QProcess::errorOccurred, this,
                [this, downloader](QProcess::ProcessError err) {
                    if (shutting_down_ || err != QProcess::FailedToStart) return;
                    const QString reason = downloader->errorString();
                    if (install_process_ == downloader) discard_process(install_process_);
                    else downloader->deleteLater();
                    if (status_ != nullptr) {
                        status_->setText(
                            QStringLiteral("自动安装失败：起不了 python（%1）。可以手动指定 "
                                           "ffmpeg.exe，或把 ffmpeg.exe 放进程序目录。")
                                .arg(reason));
                    }
                    log_to_host(QStringLiteral("视频转换：自动安装失败 —— ") + reason);
                });
        if (!wait_for_start_probed(downloader)) {
            const QString reason = downloader->errorString();
            // 同 F1-1：走同一个"放弃对象"实现（先置空再断开）。
            if (install_process_ == downloader) discard_process(install_process_);
            else downloader->deleteLater();
            if (status_ != nullptr) {
                status_->setText(QStringLiteral("自动安装失败：起不了 python（%1）。可以手动指定 "
                                                "ffmpeg.exe，或把 ffmpeg.exe 放进程序目录。")
                                     .arg(reason));
            }
            log_to_host(QStringLiteral("视频转换：自动安装失败 —— ") + reason);
        }
        return;
    }
    if (status_ != nullptr) status_->setText(message);
    log_to_host(QStringLiteral("视频转换：自动安装不可用 —— ") + message.left(120));
}

// ★ 2026-10-04 用户第 2 条：三个"选择"调用点的起点判据（与按钮 lambda 里那一行同源）
DialogStart VideoPage::source_dialog_start() const {
    return dialog_start_for(source_edit_->text(), QStringList(), false);
}

DialogStart VideoPage::reference_dialog_start() const {
    return dialog_start_for(reference_edit_->text(), QStringList(), false);
}

DialogStart VideoPage::output_dialog_start() const {
    // 源是**文件** ⇒ 起点＝它所在目录（唯一才用）；输出框里已有值 ⇒ **继承优先**
    DialogStart spec = dialog_start_for(QString(), QStringList{source_edit_->text()}, true);
    const QString current = output_edit_->text().trimmed();
    if (!current.isEmpty()) spec.inherited = QFileInfo(current).absolutePath();
    return spec;
}

}  // namespace xrkit::shell
