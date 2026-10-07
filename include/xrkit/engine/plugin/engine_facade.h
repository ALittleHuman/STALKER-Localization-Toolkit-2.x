#pragma once
// xrkit::plugin —— **引擎调用门面**（插件改装的"引擎面"；用户 2026-10-07 拍板走**方案 3**）。
//
// 为什么是这一套（用户问"哪个方案最终插件能力最强"，答完他选了 3）：
//   · 方案 1（`api.engine.*` 把引擎函数直绑）与方案 3 **运行时完全一样**（进程内直调、字节
//     与内置页一致），差别只在**治理**：方案 1 一旦开了口，"顺手多暴露一个内部函数"是免费的
//     ⇒ 必然长成影子 API、且没法版本化；
//   · 方案 3 把同一件事写成**声明式表**（本文件）+ **能力表判据**（多一个＝泄漏、少一个＝削能力）
//     ⇒ 加能力是**有意为之的一次提交**，宿主重构内部时插件一行不用改；
//   · 方案 2（`xrkit_cli.exe` 子进程）达不到同样能力：富结构过不去、编排逻辑要重写一份，
//     而且**支持不了"插件给引擎加扩展点"**（规格 §四.2 要的正是它）⇒ 只留作诊断/兜底。
//
// 三条口径：
//   ① 门面由**外壳**注入（`set_engine_facade`）：引擎层（Qt-free）只持指针并转发；
//   ② ★**表外无门**：Python 侧能看到的只有本文件里的方法（判据＝`dir(xrkit.Engine)` 与预期表对账）；
//   ③ **值类型是 Python 友好的**（dict / list / str / int），**不把 C++ 结构体、指针、
//      生命周期漏给插件** —— 那是方案 1 的病根。
//
// 本批（P7a）只开**样板页 fontpack 真正用到的那几件**（反向定义，不是猜）：
//   `font_check` / `font_pack` / `font_state_marks` / `font_size_slots`。
// 下一批随样板页迁移补齐（页面侧还要：偏好读写、系统字体目录、目录选择 —— 那些属**宿主服务**，
// 进 UI/宿主门面，不进这里）。

#include <functional>
#include <string>
#include <vector>

namespace xrkit::plugin {

// ── 值类型（Python 侧对应 dict / list）──────────────────────────────────────
struct FontCheckResult {
    std::string state;                    // "ok" / "japanese" / "missing" / "unknown"
    std::string mark;                     // 引擎给的短标记（"✓ 中文" / "⚠ 日语字体…" 等）
    std::vector<std::string> missing;     // 画不出墨迹的字（UTF-8 单字，按样本顺序）
};

struct FontStateMarks {
    std::string ok;
    std::string japanese;
    std::string missing;
    std::string unknown;
};

struct FontPackOptions {
    std::string game = "SoC";             // SoC / ClS / CoP
    std::string lang = "chs";
    std::string xml_dir;
    std::string font_path;
    std::string out_dir;
    int offset = 0;
    std::string suffix;
    bool full_cyrillic = true;
    std::vector<int> only_sizes;          // 空 = 全部槽位
};

struct FontPackResult {
    bool ok = false;
    std::string error;
    std::string out_gamedata;
    int fonts_written = 0;
    int xml_written = 0;
    std::vector<std::string> warnings;
};

struct EngineFacade {
    std::function<FontCheckResult(const std::string& font_path)> font_check;
    std::function<FontPackResult(const FontPackOptions& options)> font_pack;
    std::function<FontStateMarks()> font_state_marks;
    std::function<std::vector<int>()> font_size_slots;
};

// 外壳注入实现（`src/shell/engine_facade_impl.cpp`）。传 nullptr = 撤销。
void set_engine_facade(const EngineFacade* facade);
const EngineFacade* engine_facade();

// 不可用时 `reason` 里是**为什么**（零静默：插件作者能立刻看出为什么调用没生效）
bool engine_facade_available(std::string* reason = nullptr);
bool engine_facade_has(const std::string& name);
std::vector<std::string> engine_facade_method_names();

}  // namespace xrkit::plugin
