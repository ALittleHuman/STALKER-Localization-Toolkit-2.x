#pragma once
// xrkit::shell —— `--smoke` 自检（M2-1）。
//
// 为什么外壳也要自检：M2 的判据是"行为口径对照表 15 组逐条重验 + 拖动性能不低于试点"，
// 而界面行为**在离屏（offscreen）下也能量**（Qt 的 `-platform offscreen` 能构造真实的
// 控件树）。所以外壳从一开始就带一个**能在门禁里跑**的 ASCII 自检入口，而不是靠人眼截图
// （用户说过"这个不好截屏"—— 那就把结论做成可判定的最小验证）。

#include <QString>
#include <QStringList>

namespace xrkit::shell {

struct SmokeResult {
    int checks = 0;
    int failed = 0;
    // ★★ 2026-10-23（用户口径）：**未覆盖必须单独计数** —— 不许混进 PASS，也不冒充 FAIL。
    //   "这条判据本轮没条件跑"（页没建出来 / 该页本来就没这个形状 / 判据自检前提不成立）
    //   记在这里；收尾行打印 `not_covered=K`，消费侧自己决定容忍度（不许静默算通过）。
    int not_covered = 0;
    QStringList failures;
    QStringList notes;  // 实测值等证据（无论成败都打印，供人核对；如拖动一步的 ms）
};

// 跑全部自检项。调用方负责先建 QApplication（用 offscreen 平台）。
SmokeResult run_smoke();

}  // namespace xrkit::shell
