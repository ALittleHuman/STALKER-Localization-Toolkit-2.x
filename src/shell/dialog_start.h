#pragma once
// xrkit::shell —— "选择"对话框的**默认起点**（2026-10-04 用户第 2 条，本轮重点）。
//
// 用户原话（勿改写）：
//   「然后，就是**目录定向**的问题。比如**解包的目标目录**，点击"选择"，**默认定向到工具所在
//     的目录**。**改成定向到输入的目录**。**如果输入目录多元或没有输入，那才定向到本身所在的
//     文件夹**。然后**其他定向保持现有逻辑**，即**继承上次的选择**。」
//
// ⇒ 三条规则、一份实现（用户红线：「同类东西只维护一套组件/一份实现」）：
//   ① **继承上次的选择**：字段里已经有值（用户上次选的/上次拖进来的）⇒ 就用它。
//      这就是"其他字段保持现有逻辑"，所以它**排在第一位**（现有行为不许被改掉）。
//   ② **对应的输入目录且唯一** ⇒ 起点＝那个输入目录。例：解包目标 ← 解包来源包所在目录；
//      编码转换的输出目录 ← 源路径；XML 校对 B ← 文件夹 A；汉化包输出 ← 文本 XML 目录。
//      "唯一"= 该输入**只有一个**（"输入目录多元"= 多个 ⇒ 落到 ③）；文件输入取它所在目录。
//   ③ 输入为空 / 有多个 ⇒ 起点＝**工具自身所在文件夹**。
//      ★ 用 `QCoreApplication::applicationDirPath()`（exe 自身位置）求，**不相信 CWD**：
//        用户从别处双击/快捷方式启动时，CWD 与工具目录能差出十万八千里 —— 上一版的
//        "默认落到工具所在目录"正是靠 CWD 偶然成立的，换个启动方式就飘。
//
// 不在这里做的事：**不做**目录存在性检查（起点只是个"预填进去的建议"，用户还能自己在
// 对话框里改；文件对话框对不存在的起点会自行落到最近的有效祖辈）。

#include <QString>
#include <QStringList>

namespace xrkit::shell {

// 一个"选择"调用点的起点判据。页面**声明**判据，起点由 `resolve_dialog_start` 统一算。
struct DialogStart {
    // ① 上次的选择 / 已有值（页面负责给：通常就是该字段当前的文本）
    QString inherited;
    // ② 对应的**唯一**输入目录（没有这种语义的字段给空串）
    QString input_dir;
};

// 单个目录路径的规范化：空/无效 ⇒ 空串；否则统一成 '/' 分隔的本地路径。
QString canonical_dialog_dir(const QString& path);

// **唯一一份**"求默认起点"。空返回值不会出现（最差也有 exe 目录）。
QString resolve_dialog_start(const DialogStart& spec);

// 自检用记录器（判"页面到底调没调这一份"，见 smoke.cpp 的 ② 组）。空串 = 没记过。
void reset_last_dialog_start();
QString last_dialog_start_dir();
int last_dialog_start_calls();

// 常用判据构造：某个**输入目录**字段（目录，多值 ⇒ 不算唯一）
DialogStart dialog_start_for_input_dir(const QString& inherited, const QStringList& input_dirs);
// 常用判据构造：某个**文件**输入（取它所在目录；唯一才用）
DialogStart dialog_start_for_input_file(const QString& inherited, const QStringList& input_files);

// 页面侧的统一入口：`inputs` = 该字段**对应的输入**（0 个 ⇒ 没有输入；≥2 个 ⇒ "多元"）。
// `input_is_file` = 这份输入是**包/视频这类文件**（起点取它所在目录）还是**目录本身**。
// 唯一性判据只有这一个函数，页面**不许自己数**（数法一分叉，"多元"那条规则就会漏）。
DialogStart dialog_start_for(const QString& inherited, const QStringList& inputs,
                             bool input_is_file = true);

}  // namespace xrkit::shell
