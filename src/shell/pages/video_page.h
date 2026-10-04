#pragma once
// xrkit::shell —— 视频转换页（M2-6f）。
//
// 本页只做"按 ⑧ 的口径问引擎要结论、并把结论如实画出来"，定位规则全在
// `engine::video`（只认两处、不搜 PATH、惰性探测、8.x 拒用、只有手动指定才写盘）。
//
// 对照表里属本页的口径：
//   · 8.2 **构造期不探测**：进页面只看得到「未检测」，探测发生在第一次需要它的时候；
//   · 8.3 读不到就弹**带三按钮**的小窗（自动安装 / 手动选择 / 稍后），**非阻塞**
//     （`show()` 而不是 `exec()`，不 `setModal(true)`）—— 探针驱动页面时不能卡住进程；
//   · 8.5 状态栏三态：**未检测 / ✓ / ✗**，**探测之前不显示 ✗**；
//   · 8.4 自动安装**只能挂在按钮上**（这里不偷偷装），缺件如实报。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/video/ffmpeg.h"
#include "xrkit/engine/video/transcode.h"

#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
class QProcess;
class QWidget;

namespace xrkit::shell {

class VideoPage : public PageBase {
public:
    explicit VideoPage(QWidget* parent = nullptr);

    // 8.5：三态文案。探测之前是「未检测 ffmpeg」（**不是 ✗**）
    QString status_text() const;
    engine::video::ProbeState probe_state() const { return locator_.state(); }
    // 8.2：惰性探测（界面点「检测」或第一次真要跑时调）；返回是否找到
    bool detect();
    // 8.7：手动指定（写 user.ltx，手动优先）
    bool set_manual(const QString& ffmpeg, const QString& ffprobe, QString* error);
    // 8.3：三按钮小窗（**非阻塞**）。返回对话框指针，便于自检核对按钮数与非阻塞性
    QWidget* request_setup();
    // 8.4：自动安装只能挂在按钮上；缺 download_ffmpeg.py 就如实报
    QString auto_install();
    int probe_count() const { return locator_.probe_count(); }
    QLineEdit* source_edit() const { return source_edit_; }
    QLineEdit* output_edit() const { return output_edit_; }
    QPushButton* detect_button() const { return detect_button_; }

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 两个"选择"调用点的起点判据
    //   （输出目录 ← 源视频**所在目录**）
    DialogStart source_dialog_start() const;
    DialogStart output_dialog_start() const;

    // 转码（用户 2026-09-26 决策：完整移植）。argv 一律由 `plan_transcode` 算，
    // 页面只负责起进程、把 stderr 喂给 `progress_from_line`、以及把失败如实报出来。
    // 取消照 1.x：置令牌 → kill → **删掉半成品输出**（只有取消才删；失败保留供排障）。
    bool start_transcode(const engine::video::TranscodeRequest& request, QString* error);
    void cancel_transcode();
    bool transcode_running() const;
    int progress_percent() const { return progress_percent_; }
    QString last_error() const { return last_error_; }
    engine::video::CancelToken& cancel_token() { return cancel_token_; }

private:
    void refresh_status();

    engine::video::FfmpegLocator locator_;
    QLineEdit* source_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* detect_button_ = nullptr;
    QWidget* dialog_ = nullptr;
    QProcess* process_ = nullptr;
    engine::video::CancelToken cancel_token_;
    double duration_sec_ = 0.0;
    int progress_percent_ = 0;
    QString last_error_;
};

}  // namespace xrkit::shell
