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

#include "xrkit/engine/video/ffmpeg.h"

#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;
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

private:
    void refresh_status();

    engine::video::FfmpegLocator locator_;
    QLineEdit* source_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* detect_button_ = nullptr;
    QWidget* dialog_ = nullptr;
};

}  // namespace xrkit::shell
