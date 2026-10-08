#pragma once
// xrkit::shell —— **字库选择器**（宿主唯一一份；2026-10-08 P8a 从 `fontpack_page.cpp` 抽出来）。
//
// 为什么要抽（三条理由，都是本仓的既定纪律）：
//   ① 用户口径的下限是「**对于控件的操控，必须至少支持现有的所有功能和观感**」——
//      插件版的汉化包生成页要能弹出**同一个**选择器（系统字体列表 + 四态标记 + 预览 + 缩放 +
//      「浏览其他目录…」），而不是退化成系统的文件选择框；
//   ② 页面里再写一份、或门面里再写一份，就是第二份实现（本仓已因"旁边再写一份差不多的"
//      被门禁拦下四次）；
//   ③ 这个选择器踩过一个真缺陷（`QTimer::singleShot` 自续期链没绑生命期 ⇒ `--smoke` 崩
//      0xC0000005），**把修法与那段注释一起搬过来**才不会又踩一次。
//
// 口径（逐条照原实现，别改）：
//   · 列表行文本：`basename  [目录]  标记`；标记**先留空**，检测**按批（每 24 个）**回填；
//   · 缓存**跨对话框复用**（第二次打开几乎是立刻的）；
//   · 双击列表项 ＝ 点「选择」；「选择」/「浏览其他目录…」都算用户的选择（回调给调用方）；
//   · 预览：字号 `max(12, int(40*zoom))`，图**随内容增长**（外层滚动条随即可用）；
//   · 自续期链必须绑在**对话框**的生命期上（本文件里 `QTimer::singleShot(25, dialog, …)`）。

#include "dialog_start.h"

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace xrkit::shell {

// 系统字体候选（唯一一份：页面与 `api.ui.pick_font` 共用；`keyword` 为空 = 全部）。
// `current_font`：**当前已选字体所在目录**也算候选（1.x `:489-493`：用户自带的字体就在那儿），
// 且**存在的目录排前面**（目录不存在时列表为空，而不是"看起来没找到字体"）。
QStringList font_candidates(const QString& keyword, const QString& current_font = QString());

// 四态标记（带缓存；判据**只有引擎那一份** —— `cjk_state_mark`）
QString font_mark_of(const QString& font_path);

// 预览样张两行（第二行带西里尔与数字）；日语字体按引擎给的替换表换成繁体字形
QString font_preview_line(const QString& font_path, int which);

// 默认字体路径（唯一一份）：上次用过的（仍存在）→ 系统雅黑 → exe 旁 msyh.ttf
QString default_font_path(const QString& saved);

// **宿主唯一的字库选择器**（非模态）。`current` 用于预选；用户确认 ⇒ `on_picked(路径)`；
// 需要报账的问题（写 user.ltx 失败等）由调用方在 `on_picked` 里处理，`on_problem` 可空。
// `start_judge`＝「浏览其他目录…」的起点判据：页面传自己那份（`font_dialog_start`）；
// 插件那条路没有页面输入框 ⇒ 传 nullptr（这里按"当前字体路径"算，口径同 `dialog_start.h`）。
QDialog* make_font_picker(QWidget* parent, const QString& current,
                          std::function<DialogStart()> start_judge,
                          std::function<void(const QString&)> on_picked,
                          std::function<void(const QString&)> on_problem = nullptr);

}  // namespace xrkit::shell
