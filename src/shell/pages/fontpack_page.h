#pragma once
// xrkit::shell —— 汉化包生成页（M2-6g + 2026-10-06 D2 批"系统字体选择器"）。
//
// 出包本身早已完成（`engine::font::build_package`，门禁 [22] 与 1.x 整棵产物树逐字节一致），
// 本页只补 ⑪ 的两条**界面口径**：
//   · 11.8 字体四态（`✓ 中文 / ⚠ 日语字体 / ⚠ 疑似缺字形 / ? 检测失败`）——
//     列表标记与预览标题**共用同一个判据**（`classify_cjk_font`），页面不许自己再判一遍；
//   · 11.9 缺字形是**拒绝式检查、且在建目录之前**（引擎负责，页面只把拒绝理由显示出来，
//     并且**不留下半成品 gamedata**）；`font_unrenderable` 只告警不拒绝 —— 两者语义不同。
//
// ★★ 2026-10-06（D2 批）：用户点名「**字体默认从系统字体里找，对应逻辑看 1.x**」
//   ⇒ 逐条照抄 1.x `apps/font_pack_app.py._browse_font`（`:285-643`）与
//   `toolkit_platform.system_font_dirs`（`:222-234`）：
//     · 去哪找：`%SystemRoot%\Fonts`（**不写死 C:\Windows**）＋ 当前已选字体所在目录；
//     · 过滤：小写白名单 `*.ttf/*.ttc/*.otf` ＋ 文件名**子串**过滤 ＋ **完整路径**去重
//       ＋ 显示 `basename [目录]`（`:484-511`）；
//     · 默认选哪个（优先级，`:129-149`）：`user.ltx` 的 `[font] path`（**文件仍存在**）
//       → `msyh.ttc / msyh.ttf / msyhl.ttc` → 仓库根 `msyh.ttf`；
//     · 形态＝**自建对话框**（左：过滤框＋列表；右：预览＋±缩放 0.5~4.0＋文件名/字体名/警告；
//       底部：选择/浏览其他目录/取消；**双击＝选择**，`:290`/`:641`）；
//     · **记住上次选择**：任何选择路径都写 `user.ltx`（`:598-607`），失败要**明确告警、不静默**。
//   另四条（1.x `:84-89` / `:155` / `:202-220`）：尺寸档位下拉、打开输出目录、
//   加载语言"自定义＋输入框"、后缀预设 无/`_chs`/自定义。

#include "page_base.h"

#include "../dialog_start.h"

#include "xrkit/engine/font/metrics.h"
#include "xrkit/engine/font/package.h"

#include <QString>
#include <QStringList>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

class QCheckBox;
class QComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace xrkit::shell {

class TaskShell;

class FontPackPage : public PageBase {
public:
    explicit FontPackPage(QWidget* parent = nullptr);
    // ★★ P17-1（03 F-01）：析构函数体里先 `task_->stop_and_wait()`（见 .cpp 的理由）
    ~FontPackPage() override;

    // 11.8：字体状态（**唯一判据**在引擎里；页面只是显示）
    engine::font::CjkFontCheck check_font(const QString& font_path) const;
    QString font_state_text(const QString& font_path) const;
    // 11.9：先判后建 —— 缺字形直接拒绝（返回 false + 理由），**不建目录**
    //
    // ★★ 2026-10-10（P17-1，审计 03 F-01 Critical）：本方法是**主线程专用**的便捷路径
    //   （＝ `prepare` + `run_build` + `apply_result`），自检与单线程调用走它。
    //   worker 里**不许**调它（它读控件、写控件）—— 守卫会拒绝并记账（见 page_base.h）。
    bool build(const QString& xml_dir, const QString& font_path, const QString& out_dir,
               QString* reason);

    // ── ★★ P17-1：把"碰控件的那份"与"给 worker 的那份"**分开**（03 F-01 的修法）────
    //   · `prepare()`（**主线程**）：读控件 ⇒ 冻结成 `PackageOptions` 值；
    //   · `run_build()`（**worker 可调，纯引擎**）：一个字都不碰控件，结果用出参回传；
    //   · `apply_result()`（**主线程**）：把结果画到 `result_label_`/`state_label_`。
    //   为什么必须分成三步：修前 worker 直接调 `build()` ⇒ 跨线程读下拉框、写 QLabel；
    //   页析构时控件先被删、worker 还在写 ⇒ 访问违例（UAF）。
    struct BuildOutcome {
        bool refused = false;                 // 11.9：缺字形/检测失败 ⇒ 拒绝（没建任何目录）
        bool ok = false;                      // 引擎出包成功
        bool fallback_used = false;           // 走了繁体回退（界面要加一句"已按繁体回退生成"）
        engine::font::CjkFontCheck check;
        engine::font::PackageResult result;
        QString reason;                       // 拒绝/失败的理由（原样来自引擎与状态标记）
    };
    engine::font::PackageOptions prepare(const QString& xml_dir, const QString& font_path,
                                         const QString& out_dir) const;
    BuildOutcome run_build(const engine::font::PackageOptions& options) const;
    void apply_result(const BuildOutcome& outcome);

    // worker 里算完的结果 ⇒ 交给主线程收尾回调（见 TaskShell::set_finish_handler）
    void publish_outcome(std::shared_ptr<const BuildOutcome> outcome);
    std::shared_ptr<const BuildOutcome> take_pending_outcome();

    QLineEdit* xml_edit() const { return xml_edit_; }
    QLineEdit* font_edit() const { return font_edit_; }
    QLineEdit* out_edit() const { return out_edit_; }
    QComboBox* game_combo() const { return game_combo_; }
    QComboBox* lang_combo() const { return lang_combo_; }
    QLineEdit* lang_edit() const { return lang_edit_; }
    QComboBox* suffix_combo() const { return suffix_combo_; }
    QLineEdit* suffix_edit() const { return suffix_edit_; }
    QComboBox* size_combo() const { return size_combo_; }
    QLabel* state_label() const { return state_label_; }
    QLabel* result_label() const { return result_label_; }
    TaskShell* task() const { return task_; }
    int refused_count() const { return refused_count_.load(); }

    // ── ★ 2026-10-08 P8a：字体选择器抽成**宿主唯一一份**（`font_picker.h`）─────
    //   `make_font_picker`（自建对话框，**不 exec**）/ `font_candidates` / `default_font_path` /
    //   `font_mark_of`（列表标记缓存，判据在引擎）全在那儿 —— 页面与 `api.ui.pick_font` **共用**。
    //   页面只留"选中之后怎么办"：
    // 选中一个字体：写输入框 + 刷新状态 + **记住到 user.ltx**（1.x：任何选择路径都要记）。
    // 失败**明确告警、不静默**（返回 false + 理由）。
    bool apply_font_choice(const QString& path, QString* problem);
    bool remember_font(const QString& path, QString* problem);
    // 「打开输出目录」（1.x `:649-659`：目录无效 / 打开失败都要有反馈）
    bool open_output(QString* problem);
    // 尺寸档位（1.x `off_var`：标准/+3/+5/+7/+9 ⇒ `PackageOptions::offset`）
    int size_offset() const;

    // ★ 2026-10-04（用户第 1/2 条）：拖放区域清单 + 三个"选择"调用点的起点判据
    //   （字体 ← 文本 XML 所在的上一级；输出目录 ← 文本 XML 目录）
    DialogStart xml_dialog_start() const;
    DialogStart font_dialog_start() const;
    DialogStart out_dialog_start() const;

    engine::font::PackageOptions options_for(const QString& xml_dir, const QString& font_path,
                                             const QString& out_dir) const;

private:
    void refresh_state();
    void sync_lang_edit();      // 语言选"自定义"才显示输入框（1.x `_lang_changed`）
    void sync_suffix_edit();    // 后缀选"自定义"才显示输入框（1.x `_suf_changed`）
    // 弹出字库选择器（宿主唯一一份）并返回它 —— 自检要驱动它（双击＝选择）
    QDialog* open_font_picker();
    // 预览用的两行样本（1.x `:570`）+ 日语字体时按繁体回退替换（用户口径 2026-09-14）

    QLineEdit* xml_edit_ = nullptr;
    QLineEdit* font_edit_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
    QComboBox* game_combo_ = nullptr;
    QComboBox* lang_combo_ = nullptr;
    QLineEdit* lang_edit_ = nullptr;
    QComboBox* suffix_combo_ = nullptr;
    QLineEdit* suffix_edit_ = nullptr;
    QComboBox* size_combo_ = nullptr;
    QCheckBox* cyrillic_box_ = nullptr;
    QPushButton* open_out_button_ = nullptr;
    QLabel* state_label_ = nullptr;
    QLabel* result_label_ = nullptr;
    TaskShell* task_ = nullptr;
    // ★★ P17-1：拒绝计数**由 worker 递增**（`run_build` 在 worker 里跑）⇒ 必须是原子量
    //   （修前是裸 `int`，跨线程 `++` 就是数据竞争 —— 审计 03 F-01 的同一条）。
    //   `mutable`：`run_build` 是 const 成员（它只读页面、只动这个计数器）。
    mutable std::atomic<int> refused_count_{0};
    // worker 算完的结果 ⇒ 主线程收尾回调取走（互斥保护；只在 worker 与主线程各一处访问）
    mutable std::mutex outcome_mutex_;
    std::shared_ptr<const BuildOutcome> pending_outcome_;
    // 字体四态的**缓存**（1.x `self._font_cjk`：跨对话框复用，避免重复解析同一个字体文件）
};

}  // namespace xrkit::shell
