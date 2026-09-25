#pragma once
// xrkit::engine::font::metrics —— 字库的槽位几何与宽度裁决（M1-7 第③步）。
//
// 这里装的是 1.x `font_pack.py` 里那几张**标定表**与那四条**宽度裁决函数**的 C++ 版。
// 判据是"同输入产物逐字节一致"，所以语义逐条照抄，不做任何"优化"：
//   · 字号 → 槽位高 cell：表内查 `CELL_HEIGHTS`，表外 `ceil(rsize * 1.40)`
//     （比例兜底是实测值，见 1.x `CELL_HEIGHT_RATIO`）；
//   · 字符 → 推进宽：adv 表 → 方块宽（EAW∈{W,F} 或强制全角）→ 交给调用方按字体 advance 现算；
//   · 标点框裁决：`punct_box_px` → `clamp_box_to_ink` → **封顶 cell** → `ink_draw_w`。
//
// 三张表由 `tools/gen_font_tables.py` 生成到 `src/engine/font/*.inc`，门禁第 [13] 步
// 用 `--check` 把它们与 1.x 的 JSON / Python 的 unicodedata 逐字节锁住。
//
// 本模块**不依赖 FreeType**：需要墨迹的地方（`left0` / `gw`）由调用方量好传进来。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xrkit::engine::font {

// ── 槽位几何 ───────────────────────────────────────────────────────────

// 槽位档位数（1.x `SIZES` 的前 10 项），槽位名见 slot_size()。
int slot_count();

// 第 index 个槽位的名字（11/13/15/16/17/18/20/21/23/25）；越界返回 0。
int slot_size(int index);

// 该槽位名是否在表内（10 个之一）。
bool is_slot_size(int size);

// 该槽位对应的 cell 高（也写进 .ini 的 `height=`，引擎据此算行高）。
// 表内走表；表外（GUI「尺寸 +N」那条路）按 1.40 比例放大 —— 与 1.x cell_height_for 同式。
int cell_height_for(int size);

// 方块字符（汉字/全角标点）在 `size` 这一档画多宽。表外退回 `size + 2`（1.x 口径）。
int block_width_for(int size);

// ── 字符分类 ───────────────────────────────────────────────────────────

// "方块字符"：EAW∈{W,F} 或 1.x 的 FORCED_FULLWIDTH（“ ” ‘ ’ — ‖）。
// 代理区 (0xD800..0xDFFF) 与 U+10FFFF 以上返回 false —— 与 Python 侧一致（前者 EAW 为 N）。
bool is_block_char(uint32_t cp);

// 标点类别：1.x 的 PUNCT_OPEN / PUNCT_CLOSE / PUNCT_SINGLE / PUNCT_WIDE 四张集合。
enum class PunctClass : int {
    None = 0,
    Open = 1,
    Close = 2,
    Single = 3,
    Wide = 4,
};

PunctClass punct_class(uint32_t cp);

// ── 宽度裁决 ───────────────────────────────────────────────────────────

// 1.x `rect_w_for`：表里没有、也不是方块字符时返回 -1（由调用方按字体 advance 现算）。
// 返回的是**未封顶**的框宽（封顶 cell 由 widths_of / adjudicate_box 负责）。
int rect_w_for(int size, uint32_t cp);

// 框宽裁决的输入/输出（1.x 的 `(pl, rw)` 与压完之后的绘制宽）。
struct BoxResult {
    int pl = 0;       // 墨迹在框内的左偏移
    int rw = 0;       // 写进 .ini 的框宽（x2 - x1）
    int gw_draw = 0;  // 墨迹实际绘制宽（横压后）
};

// 框宽裁决**全流程**（= 1.x `render_font` 里那四句，顺序不许动）：
//     (pl, rw) = punct_box_px(cp, raw_rw, left0, gw)
//     rw       = clamp_box_to_ink(pl, rw, gw)
//     rw       = min(rw, cell_h)          # 框宽硬上限
//     gw_draw  = ink_draw_w(gw, rw, pl)
// `raw_rw` 必须是调用方已按 `widths_of` 封过一次 cell 的值（与 1.x 一致）。
BoxResult adjudicate_box(uint32_t cp, int raw_rw, int left0, int gw, int cell_h);

// 下面三条是上面那条链的分解，公开出来是为了让探针能逐句对拍（1.x 也把探针挂在同一处）。
// punct_box_px：标点两侧留白各收敛到 min(留白, PAD=3)；非标点/无墨迹原样返回。
BoxResult punct_box_px(uint32_t cp, int rw0, int left, int gw);
// 框不得比墨迹窄（宽度表个别码位比墨迹还窄）。
int clamp_box_to_ink(int pl, int rw, int gw);
// 墨迹放不下就横向压进框（绝不右裁）。
int ink_draw_w(int gw, int rw, int pl);

// ── 简→繁单字表 ───────────────────────────────────────────────────────

// 该字没有字形时用来顶的繁体字（1.x `ST_FALLBACK`）；没有则返回 0。
uint32_t st_fallback(uint32_t cp);

// ── ⑪11.8 CJK 字形四态判定 ──────────────────────────────────────────
//
// 1.x `apps/font_pack_app.glyph_status`：**唯一**的判据，列表标记与预览标题共用它
//（两处各判一次就会出现"列表 ✓ 中文、预览却缺常用字"的自相矛盾）。
//   · ok       —— 样本里每个字都画得出墨迹，且**不同 bbox 数 > 1**
//                 （全同 = 该字体对什么字都画同一个方块）；
//   · japanese —— 有简体字画不出，但缺的那些里**有能**用繁体字形顶上的
//                 （日文字体按 JIS 收字，没有简体专有形却有繁体形）；
//   · missing  —— 画不出的字连繁体也补不出来；或 bbox 全同（fail-closed）；
//   · unknown  —— **检测失败**（字体打不开等），与"缺字形"不是一回事，界面上要分开。
//
// ★ 与对照表措辞的差异（**照代码不照文档**）：11.8 把 japanese 写成"缺的简体字
//   **全都能**用繁体顶上"，而 1.x 的代码是 `if sample_fallback(path):` ——
//   **非空即可**（只要有一个缺字能补就判 japanese）。这里按代码实现，差异记账。
enum class CjkFontState { Ok, Japanese, Missing, Unknown };

struct CjkFontCheck {
    CjkFontState state = CjkFontState::Unknown;
    std::vector<std::string> missing;              // 样本里画不出墨迹的字（UTF-8 单字，按样本顺序）
    std::map<std::string, std::string> fallbacks;  // 缺字里能用繁体顶上的：{简体: 繁体}
};

// 判定样本（1.x `CJK_SAMPLE`）——**判据的一部分，不许随手改**：
// 老样本"永你好汉化测"漏了"简体特有"的字，日文字体照样判 ✓ 中文，而预览里那几个字
// 是整字宽的空洞。现在混入简体特有的字，缺一个就 fail-closed。
const char* cjk_sample();

CjkFontCheck classify_cjk_font(const std::string& font_path, int pixel_size = 40);
const char* cjk_state_mark(CjkFontState state);  // "✓ 中文" / "⚠ 日语字体（缺简体→用繁体）" 等

// ── 探针 ───────────────────────────────────────────────────────────────

// 把上面所有查询按**固定扫描顺序**导出成一个小端二进制（供 Python 侧按 1.x 的
// font_pack 逐字节对拍）。格式见 metrics.cpp 顶部。写不出文件返回 false。
bool dump_metrics(const std::string& out_path);

}  // namespace xrkit::engine::font
