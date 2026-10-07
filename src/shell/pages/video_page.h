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
//
// ★★ 本批（2.x 视频页补全）新增的接线，全部照 1.x `apps/video_ogm_app.py`：
//   · **两个输入行都是单文件**：源文件 + 参考 OGM（1.x `:516-526` 两个 drop_zone）；
//     1.x 的转换按钮在**未选源**时置灰、参考未落地时也置灰（`:574-577`、`:708`）；
//   · **输出是文件**（1.x `asksaveasfilename` + `_auto_output`，`:544-555/715-745`），
//     引擎 `TranscodeRequest::output` 本来就是文件路径 ⇒ 默认名按 1.x 三条规则推导；
//   · **进度条 + 文案**（1.x `:561-562`、`:889-901`，百分数上限 99.5 来自引擎）；
//   · **取消**＝终止进程 + **删掉半成品**（1.x `:344-352/360-366`），删的结果要如实报；
//   · **「视频参数」面板**＝1.x `_refresh_display` 的字段（源/参考/转换参数对比，`:749-787`）；
//   · **不能静默失败**（用户 2026-10-04 红线：「不能静默报错，不然出问题了还不知道。」）
//     ⇒ 失败/取消/降级/跳过一律带原因可见：找不到 ffmpeg 报**试过哪些路径**；取消报
//     半成品删没删掉；转码失败带 **ffmpeg stderr 原文**（并落日志）；探测不到报原因；
//     有跳过的分支要**计数 + 逐条原因**。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/video/ffmpeg.h"
#include "xrkit/engine/video/transcode.h"

#include <QString>
#include <QStringList>

#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QProcess;
class QWidget;

namespace xrkit::shell {

class VideoPage : public PageBase {
public:
    // 一次探测的目标：源 / 参考 / **转换产物**（产物那一屏 1.x 也是 ffprobe 解析出来的）
    enum class ProbeTarget { Source, Reference, Result };

    explicit VideoPage(QWidget* parent = nullptr);
    // 析构期**先断开再杀**：异步回调（ffprobe / `ffmpeg -encoders` / 转码）绝不能回到一个正在
    // 析构的页面上（那会碰已经析构的子控件 —— 实测崩在 `QPlainTextEdit::setPlainText` 里）。
    ~VideoPage() override;

    // ── ⑧ 8.2/8.3/8.4/8.5/8.7（原有）────────────────────────────────
    // 8.5：三态文案。探测之前是「未检测 ffmpeg」（**不是 ✗**）；找不到时要**报出试过的路径**
    QString status_text() const;
    engine::video::ProbeState probe_state() const { return locator_.state(); }
    // 8.2：惰性探测（界面点「检测」或第一次真要跑时调）；返回是否找到
    bool detect();
    // 8.7：手动指定（写 user.ltx，手动优先）
    bool set_manual(const QString& ffmpeg, const QString& ffprobe, QString* error);
    // 8.3：三按钮小窗（**非阻塞**）。返回对话框指针，便于自检核对按钮数与非阻塞性
    QWidget* request_setup();
    // 8.4：自动安装只能挂在按钮上（**按钮**走 run_auto_install；本函数给"结论"文本）
    QString auto_install();
    void run_auto_install();
    int probe_count() const { return locator_.probe_count(); }
    QPushButton* detect_button() const { return detect_button_; }
    QString auto_install_message() const { return last_auto_install_; }

    // ── 用户 2026-10-04（第 1/2 条）：三个输入行各自的"选择"起点判据 ──
    DialogStart source_dialog_start() const;
    DialogStart reference_dialog_start() const;
    DialogStart output_dialog_start() const;

    // ── 三个输入行（**都是单文件**；拖放区域＝整行，见 drop_zone.h）──
    QLineEdit* source_edit() const { return source_edit_; }
    QLineEdit* reference_edit() const { return reference_edit_; }
    QLineEdit* output_edit() const { return output_edit_; }
    QPushButton* convert_button() const { return convert_button_; }
    QPushButton* cancel_button() const { return cancel_button_; }
    QProgressBar* progress_bar() const { return progress_; }
    QLabel* progress_label() const { return progress_text_; }
    QPlainTextEdit* info_view() const { return info_; }
    QLabel* status_label() const { return status_; }

    // 源/参考文件落地（拖入与"选择…"走**同一份**实现）：起异步 ffprobe
    void apply_file(const QString& path, bool is_reference);
    // 探测结果落地（异步 ffprobe 的回调与自检走**同一份**实现）
    void apply_probe_result(const QString& path, ProbeTarget target,
                            const engine::video::MediaInfo& info, const QString& error);
    bool source_ready() const { return source_probed_; }
    bool reference_ready() const { return reference_probed_; }
    // 编码器表扫描是否已有结论（含"扫不到"——降级理由在面板上可见）
    bool encoders_scanned() const { return encoders_scanned_; }

    // 输出：默认名按 1.x `_auto_output` 推导；「选择…」是**保存文件**对话框
    QString default_output_path() const;
    void auto_output();
    void browse_output();
    // 保存文件对话框的**唯一**调用点（默认 = `QFileDialog::getSaveFileName`；自检覆盖它
    // 就能判"点「选择…」走的是保存文件对话框、默认名与起点对不对"）
    virtual QString ask_save_output(const QString& start_dir, const QString& initial_name);
    // 打开**文件**对话框的唯一调用点（源/参考两行共用；1.x 两行都是选文件）
    virtual QString ask_open_file(const QString& title, const QString& filter,
                                  const QString& start_path);

    // ★★ 2026-10-07（审计补充 F1-2）：**输出已存在**时的唯一确认点。
    //   默认＝`QMessageBox::question`（用户可见、不静默；与 convert_page 的取消确认同款）。
    //   自检覆盖它 ⇒ 能判"问没问、答否时到底跑没跑"。
    //   注意：只有 `OutputConflict::Exists` 会走到这里 —— "输出＝源/参考"是**直接拒**，
    //   那不是"要不要覆盖"的选择题，是把输入毁掉。
    virtual bool confirm_overwrite(const QString& reason);

    // 视频参数面板文本（1.x `_refresh_display` 的字段与"OK/将缩放"标记）
    QString info_panel_text() const;

    // ── 转换 ─────────────────────────────────────────────
    // 未选源 / 参考未落地 / 正在跑 ⇒ false（1.x：按钮灰着且不额外解释，是有意为之）
    bool can_transcode() const;
    void request_transcode();                       // 「转换为 OGM」按钮的动作
    void handle_stderr_text(const QString& text);   // 1.x 每行 stderr 的落地（进度）
    QString last_stderr_tail() const;

    // 转码（原有 API；argv 一律由 `plan_transcode` 算）
    bool start_transcode(const engine::video::TranscodeRequest& request, QString* error);
    void cancel_transcode();
    bool transcode_running() const { return process_ != nullptr; }
    int progress_percent() const { return progress_percent_; }
    QString last_error() const { return last_error_; }
    QString last_cancel_message() const { return last_cancel_message_; }
    engine::video::CancelToken& cancel_token() { return cancel_token_; }

    // ── 跳过/降级的口径（用户 2026-10-04 红线：不能静默）──
    int skipped_count() const { return skip_reasons_.size(); }
    QStringList skip_reasons() const { return skip_reasons_; }
    void note_skip(const QString& reason);

protected:
    // ffmpeg/ffprobe 路径的**唯一**来源（默认＝locator 的惰性探测）。自检覆盖它，就能把
    // 夹具 exe 当 ffmpeg/ffprobe 用 ⇒ "按钮 → argv → 进程 → 进度 → 取消"这条链能真跑。
    virtual engine::video::FfmpegPaths resolve_ffmpeg();
    // 「找不到 ffmpeg」时要如实报出**试过哪些路径**（用户红线：不能只打一行泛泛的话）
    virtual QString ffmpeg_search_summary() const;

private:
    void refresh_status();
    void refresh_display();
    void refresh_buttons();
    void set_running_ui(bool running);
    void start_probe(const QString& path, ProbeTarget target);
    void start_encoder_scan();
    void complete_encoder_scan(bool ok, const QString& reason);
    engine::video::TranscodeRequest build_request(const engine::video::FfmpegPaths& paths) const;
    QProcess* make_child_process();
    // ★★ 2026-10-07（F1-1）：放弃一个子进程对象的**唯一**实现（先置空 → disconnect →
    //   deleteLater）。析构 / 取消超时 / `waitForStarted` 失败 / 回调收尾全走它 ⇒
    //   "置空却不 disconnect"这一族缺陷在结构上不可能再出现（静态判据只盯这一个函数）。
    void discard_process(QProcess*& slot);

    engine::video::FfmpegLocator locator_;
    QLineEdit* source_edit_ = nullptr;
    QLineEdit* reference_edit_ = nullptr;
    QLineEdit* output_edit_ = nullptr;
    QLabel* status_ = nullptr;
    QPlainTextEdit* info_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* detect_button_ = nullptr;
    QPushButton* setup_button_ = nullptr;
    QPushButton* convert_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QWidget* dialog_ = nullptr;

    // 源 / 参考 / 产物 的探测结论（`parsed=true` 才算落地）
    engine::video::MediaInfo source_info_;
    engine::video::MediaInfo reference_info_;
    engine::video::MediaInfo result_info_;
    bool source_probed_ = false;
    bool reference_probed_ = false;
    QString source_probe_error_;
    QString reference_probe_error_;
    QString result_probe_error_;
    QString source_path_;
    QString reference_path_;
    QString result_path_;

    QProcess* probe_process_ = nullptr;   // 1.x：同一时刻只允许一次探测（连点/连拖不堆线程）
    ProbeTarget probe_target_ = ProbeTarget::Source;
    QString probe_path_;
    QString last_source_request_;      // 同一份文本只落一次地（textChanged 一次改动可能来两趟）
    QString last_reference_request_;

    QProcess* encoder_process_ = nullptr;
    bool encoders_scanned_ = false;
    std::vector<std::string> available_encoders_;
    QString encoder_note_;                // 编码器扫描失败/降级的可见原因

    // ★ 2026-10-07（审计补充 F3）：`download_ffmpeg.py` 那条自动安装子进程。
    //   修前它是**局部** `new_child_process(this)`：析构不停（`~VideoPage` 只收
    //   probe/encoder/transcode 三个）、回调也没有 `shutting_down_` 守卫 ⇒
    //   关窗后回调仍会碰已析构的 `status_`。现在它是成员、走析构里同一个 `stop()`。
    QProcess* install_process_ = nullptr;

    bool pending_transcode_ = false;
    bool shutting_down_ = false;
    QString last_auto_install_;

    QProcess* process_ = nullptr;
    QString output_path_;                 // **真在写**的那个输出（取消时删的就是它）
    std::vector<char> stderr_tail_;       // ffmpeg stderr 的尾部（失败时原文带出来）
    engine::video::CancelToken cancel_token_;
    double duration_sec_ = 0.0;
    int progress_percent_ = 0;
    QString last_error_;
    QString last_cancel_message_;
    QStringList skip_reasons_;
};

}  // namespace xrkit::shell
