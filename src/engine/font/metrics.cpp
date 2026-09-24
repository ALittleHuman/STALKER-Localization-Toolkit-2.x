#include "xrkit/engine/font/metrics.h"

#include "xrkit/core/path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

namespace xrkit::engine::font {
namespace {

// 生成物（tools/gen_font_tables.py，门禁第 [13] 步 --check 锁）
#include "adv_tables.inc"   // NOLINT  kFontSlotSizes / kFontAdvCodepoints / kFontAdvWidths
#include "eaw_tables.inc"   // NOLINT  kEawWideRanges / kForcedFullwidth
#include "st_fallback.inc"  // NOLINT  kStFallback

constexpr int kSlotCount = static_cast<int>(sizeof(kFontSlotSizes) / sizeof(kFontSlotSizes[0]));
constexpr int kAdvCount =
    static_cast<int>(sizeof(kFontAdvCodepoints) / sizeof(kFontAdvCodepoints[0]));
constexpr int kEawCount = static_cast<int>(sizeof(kEawWideRanges) / sizeof(kEawWideRanges[0]));
constexpr int kStCount = static_cast<int>(sizeof(kStFallback) / sizeof(kStFallback[0]));

// ── 1.x font_pack.py 里的两张**策略表**（不是 JSON 数据，所以直接写在这儿）──
//
// CELL_HEIGHTS：各槽位的 cell 高，也就是写进 .ini 的 `height=`。
// ★ 已用**原版**成品交叉核对（tests 的取证脚本）：`汉化包生成工具2024` 的
//   ui_font_NN_chs.ini 的 height= 与本表 10/10 逐一相等，且它的槽位网格
//   `(x1-(cell-w)//2) % cell == 0` 用**本表**当 cell 时对 3428 行 × 10 档**全部成立**
//   （用 BLOCK_WIDTHS 当 cell 则 32360 行全不符）⇒ 原版的横向格就是 cell 高（方格）。
constexpr struct {
    int size;
    int cell;
} kCellHeights[] = {
    {11, 20}, {13, 22}, {15, 23}, {16, 25}, {17, 26},
    {18, 29}, {20, 31}, {21, 32}, {23, 33}, {25, 35},
};

// BLOCK_WIDTHS：方块字符画多宽；原版成品方块字实测宽度 3276/3277 命中本表
// （唯一例外 U+2014「—」在原版是 +1~+3px，1.x 按本表走，2.x 复刻 1.x）。
constexpr struct {
    int size;
    int width;
} kBlockWidths[] = {
    {11, 17}, {13, 18}, {15, 19}, {16, 21}, {17, 22},
    {18, 24}, {20, 26}, {21, 27}, {23, 29}, {25, 31},
};

// 表外字号的兜底：1.x `CELL_HEIGHT_RATIO`（= 本表自身的最大比例 35/25，且 ≥ 实测所需 1.364）。
constexpr double kCellHeightRatio = 1.40;
// 表外字号的方块宽兜底：1.x `BLOCK_WIDTHS.get(size, size + 2)`。
constexpr int kBlockWidthBias = 2;
// 1.x `PUNCT_PAD_PX`：四类统一 3px（两侧各自收敛到 min(原留白, PAD)）。
constexpr int kPunctPadPx = 3;

// 1.x 的四张标点集合（码位升序；顺序即 punct_class 的判定顺序 Open→Close→Single→Wide）。
constexpr uint32_t kPunctOpen[] = {0x2018, 0x201C, 0x3008, 0x300A, 0x300C,
                                   0x300E, 0x3010, 0x3014, 0xFF08, 0xFF3B, 0xFF5B};
constexpr uint32_t kPunctClose[] = {0x2019, 0x201D, 0x3009, 0x300B, 0x300D,
                                    0x300F, 0x3011, 0x3015, 0xFF09, 0xFF3D, 0xFF5D};
constexpr uint32_t kPunctSingle[] = {0x3001, 0x3002, 0xFF01, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF1F};
constexpr uint32_t kPunctWide[] = {0x00B7, 0x2014, 0x2026, 0xFF5E};

template <size_t N>
bool in_sorted(const uint32_t (&table)[N], uint32_t cp) {
    return std::binary_search(table, table + N, cp);
}

}  // namespace

// ── 槽位几何 ───────────────────────────────────────────────────────────

int slot_count() { return kSlotCount; }

int slot_size(int index) {
    if (index < 0 || index >= kSlotCount) return 0;
    return kFontSlotSizes[index];
}

bool is_slot_size(int size) {
    for (int i = 0; i < kSlotCount; ++i) {
        if (kFontSlotSizes[i] == size) return true;
    }
    return false;
}

int cell_height_for(int size) {
    for (const auto& row : kCellHeights) {
        if (row.size == size) return row.cell;
    }
    // 1.x：`CELL_HEIGHTS.get(rsize) or int(math.ceil(rsize * CELL_HEIGHT_RATIO))`
    // ★ 必须与 Python 的双精度算法逐位同式：`ceil(double(size) * 1.40)`。
    return static_cast<int>(std::ceil(static_cast<double>(size) * kCellHeightRatio));
}

int block_width_for(int size) {
    for (const auto& row : kBlockWidths) {
        if (row.size == size) return row.width;
    }
    return size + kBlockWidthBias;
}

// ── 字符分类 ───────────────────────────────────────────────────────────

bool is_block_char(uint32_t cp) {
    if (cp > 0x10FFFF) return false;
    if (in_sorted(kForcedFullwidth, cp)) return true;
    // 区间表按 from 升序且互不相邻 ⇒ 找到最后一个 from ≤ cp 的段再看 to。
    int lo = 0;
    int hi = kEawCount - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (kEawWideRanges[mid].from > cp) {
            hi = mid - 1;
        } else if (kEawWideRanges[mid].to < cp) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

PunctClass punct_class(uint32_t cp) {
    if (in_sorted(kPunctOpen, cp)) return PunctClass::Open;
    if (in_sorted(kPunctClose, cp)) return PunctClass::Close;
    if (in_sorted(kPunctSingle, cp)) return PunctClass::Single;
    if (in_sorted(kPunctWide, cp)) return PunctClass::Wide;
    return PunctClass::None;
}

// ── 宽度裁决 ───────────────────────────────────────────────────────────

int rect_w_for(int size, uint32_t cp) {
    // adv 表：10 个档位共用同一批升序码位；行 = 档位下标。
    if (cp <= 0xFFFF) {
        const uint16_t key = static_cast<uint16_t>(cp);
        const uint16_t* end = kFontAdvCodepoints + kAdvCount;
        const uint16_t* hit = std::lower_bound(kFontAdvCodepoints, end, key);
        if (hit != end && *hit == key) {
            for (int i = 0; i < kSlotCount; ++i) {
                if (kFontSlotSizes[i] == size) {
                    return kFontAdvWidths[i][hit - kFontAdvCodepoints];
                }
            }
            // 档位不在表内 ⇒ 落到方块判定（1.x 对未知档位的 ADV_TABLES.get(size, {}) 同义）
        }
    }
    if (is_block_char(cp)) return block_width_for(size);
    return -1;
}

BoxResult punct_box_px(uint32_t cp, int rw0, int left, int gw) {
    BoxResult out;
    out.pl = left;
    out.rw = rw0;
    const PunctClass cls = punct_class(cp);
    if (cls == PunctClass::None || gw < 1) return out;
    // 1.x：pr0 = rw0 - left - gw; m = max(0, min(left, pr0, pad)); 返回 (m, max(1, m+gw+m))
    const int pr0 = rw0 - left - gw;
    const int m = std::max(0, std::min({left, pr0, kPunctPadPx}));
    out.pl = m;
    out.rw = std::max(1, m + gw + m);
    return out;
}

int clamp_box_to_ink(int pl, int rw, int gw) {
    if (gw >= 1 && rw < pl + gw) return pl + gw;
    return rw;
}

int ink_draw_w(int gw, int rw, int pl) {
    const int avail = std::max(1, rw - pl);
    return gw <= avail ? gw : avail;
}

BoxResult adjudicate_box(uint32_t cp, int raw_rw, int left0, int gw, int cell_h) {
    BoxResult box = punct_box_px(cp, raw_rw, left0, gw);
    box.rw = clamp_box_to_ink(box.pl, box.rw, gw);
    if (box.rw > cell_h) box.rw = cell_h;
    box.gw_draw = ink_draw_w(gw, box.rw, box.pl);
    return box;
}

// ── 简→繁单字表 ───────────────────────────────────────────────────────

uint32_t st_fallback(uint32_t cp) {
    if (cp > 0xFFFF) return 0;
    const uint16_t key = static_cast<uint16_t>(cp);
    int lo = 0;
    int hi = kStCount - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint16_t from = kStFallback[mid].from;
        if (from < key) {
            lo = mid + 1;
        } else if (from > key) {
            hi = mid - 1;
        } else {
            return kStFallback[mid].to;
        }
    }
    return 0;
}

// ── 探针 ───────────────────────────────────────────────────────────────
//
// 小端定长，供 Python 侧按 1.x 的 font_pack 逐字节对拍。格式（顺序即写出顺序）：
//   u32 magic 'FMET'
//   u32 slot_count                    每槽：u32 size, u32 cell_height, u32 block_width
//   u32 rsize_n                       每档：u32 cell_height_for(1..rsize_n)
//   u32 block_space (= 0x110000)      每码位：u8 is_block_char
//   u32 bmp        (= 0x10000)        每槽 × 每码位：u8 punct_class
//   u32 rect_slots (= slot_count)     每槽 × 每码位(0x10000)：i32 rect_w_for
//   u32 st_bmp     (= 0x10000)        每码位：u32 st_fallback
//   u32 case_n                        每例：u32 cp, i32 raw_rw, i32 left0, i32 gw,
//                                            i32 cell_h, i32 pl, i32 rw, i32 gw_draw
//
// 裁决对拍的**输入**由本侧生成、连输入一起写进 dump，Python 只做"按 1.x 的函数重算"，
// 于是两端没有第二份需要同步的用例表。
namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void put8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }

// 确定性 LCG（两端不需要共享：输入随 dump 一起给 Python）。
struct Lcg {
    uint32_t s = 0x2545F491u;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int pick(int lo, int hi) {  // 闭区间
        return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1));
    }
};

constexpr uint32_t kCaseCps[] = {
    // 四类标点各取样 + 强制全角 + 表内 + 表外非方块
    0x3002, 0xFF0C, 0xFF01, 0x3001, 0xFF1A, 0xFF1B, 0xFF1F,       // single
    0xFF08, 0x300C, 0x3010, 0x2018, 0x201C, 0xFF3B, 0xFF5B,       // open
    0xFF09, 0x300D, 0x3011, 0x2019, 0x201D, 0xFF3D, 0xFF5D,       // close
    0x2026, 0x00B7, 0x2014, 0xFF5E,                               // wide
    0x0041, 0x4E2D, 0x2116, 0x00AD, 0x0483, 0x0020, 0x002D,       // 非标点
};

constexpr int kCasesPerSlot = 2000;

void append_cases(std::vector<uint8_t>& out, int& case_n) {
    Lcg rng;
    for (int i = 0; i < kSlotCount; ++i) {
        const int size = kFontSlotSizes[i];
        const int cell_h = cell_height_for(size);
        for (int k = 0; k < kCasesPerSlot; ++k) {
            const uint32_t cp = kCaseCps[rng.next() % (sizeof(kCaseCps) / sizeof(uint32_t))];
            const int raw_rw = rng.pick(0, cell_h);  // widths_of 已封顶一次 cell 的值域
            const int left0 = rng.pick(0, 16);
            const int gw = rng.pick(0, cell_h + 2);
            const BoxResult box = adjudicate_box(cp, raw_rw, left0, gw, cell_h);
            put32(out, cp);
            put32(out, static_cast<uint32_t>(raw_rw));
            put32(out, static_cast<uint32_t>(left0));
            put32(out, static_cast<uint32_t>(gw));
            put32(out, static_cast<uint32_t>(cell_h));
            put32(out, static_cast<uint32_t>(box.pl));
            put32(out, static_cast<uint32_t>(box.rw));
            put32(out, static_cast<uint32_t>(box.gw_draw));
            ++case_n;
        }
    }
}

}  // namespace

bool dump_metrics(const std::string& out_path) {
    std::vector<uint8_t> out;
    out.reserve(8u << 20);

    put32(out, 0x54454D46u);  // "FMET"
    put32(out, static_cast<uint32_t>(kSlotCount));
    for (int i = 0; i < kSlotCount; ++i) {
        const int size = kFontSlotSizes[i];
        put32(out, static_cast<uint32_t>(size));
        put32(out, static_cast<uint32_t>(cell_height_for(size)));
        put32(out, static_cast<uint32_t>(block_width_for(size)));
    }

    constexpr int kRsizeN = 64;
    put32(out, static_cast<uint32_t>(kRsizeN));
    for (int rsize = 1; rsize <= kRsizeN; ++rsize) {
        put32(out, static_cast<uint32_t>(cell_height_for(rsize)));
    }

    constexpr uint32_t kBlockSpace = 0x110000;
    put32(out, kBlockSpace);
    for (uint32_t cp = 0; cp < kBlockSpace; ++cp) {
        put8(out, is_block_char(cp) ? 1 : 0);
    }

    constexpr uint32_t kBmp = 0x10000;
    put32(out, kBmp);
    for (int i = 0; i < kSlotCount; ++i) {
        for (uint32_t cp = 0; cp < kBmp; ++cp) {
            put8(out, static_cast<uint8_t>(punct_class(cp)));
        }
    }

    put32(out, static_cast<uint32_t>(kSlotCount));
    for (int i = 0; i < kSlotCount; ++i) {
        const int size = kFontSlotSizes[i];
        for (uint32_t cp = 0; cp < kBmp; ++cp) {
            put32(out, static_cast<uint32_t>(rect_w_for(size, cp)));
        }
    }

    put32(out, kBmp);
    for (uint32_t cp = 0; cp < kBmp; ++cp) {
        put32(out, st_fallback(cp));
    }

    const size_t case_count_pos = out.size();
    put32(out, 0);
    int case_n = 0;
    append_cases(out, case_n);
    out[case_count_pos + 0] = static_cast<uint8_t>(case_n & 0xFF);
    out[case_count_pos + 1] = static_cast<uint8_t>((case_n >> 8) & 0xFF);
    out[case_count_pos + 2] = static_cast<uint8_t>((case_n >> 16) & 0xFF);
    out[case_count_pos + 3] = static_cast<uint8_t>((case_n >> 24) & 0xFF);

    std::ofstream f(core::path_from_utf8(out_path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

}  // namespace xrkit::engine::font
