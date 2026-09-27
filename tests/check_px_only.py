#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""像素口径门禁（屏幕自适应，2026-09-27 用户：「我电脑是2.5K屏，所以做一个自适应」）。

判据：**设计稿像素一律过 `px()`** —— shell 侧任何"尺寸/间距"调用里出现**非零裸数字**即判红，
因为那类字面量按 96 DPI 定死，在 2.5K/高 DPI 屏上不会跟着缩放（表现就是"字体变大了、间距没动"）。

放行：
  · 数字 0（不需要缩放）；
  · ALLOW（白名单，逐条写理由）；
  · ALLOW_FILES（整文件豁免：测试夹具）；
  · KNOWN_DEBT（登记在案的待改项，命中时**打印可见提示**）。

输出（供门禁抓取）：`PX-ONLY ok` / `PX-ONLY fail <详情>`；退出码 0/1。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CALLS = (
    "setFixedHeight|setMinimumHeight|setMaximumHeight|setFixedWidth|setMinimumWidth"
    "|setMaximumWidth|setSpacing|setContentsMargins|resize"
)
BARE = re.compile(r"\b(?:%s)\s*\(\s*(?!xrkit::core::px\()([1-9])" % CALLS)

# 白名单（每条都必须写明理由；**降级要可见**，不许静默跳过）
ALLOW = {
    # 1 逻辑像素的布局技巧（宽度不参与布局撑开），不是设计稿像素
    ("src/shell/log_box.cpp", "setMinimumWidth(1)"),
}
# 整文件豁免：**测试夹具** —— 里面的画布尺寸是"刻意的固定逻辑像素"（测不变量，不测外观），
# 过 px() 反而会让"测什么"随屏幕变。
ALLOW_FILES = {"src/shell/smoke.cpp"}
# 登记在案、**待改**：产品代码里写死的尺寸。命中时**打印可见提示**（不是静默放过），
# 提示里写清"该改成什么"，避免这条锁变成一句空话。
KNOWN_DEBT = {
    ("src/shell/hub.cpp", "resize(1180, 760)"):
        "默认窗口尺寸写死 ⇒ 应改为 adaptive_window_size()（或至少 px()）；文件当前归 Hana 改，"
        "登记在案、待他收口时一并改",
}


def active_code(text: str) -> str:
    """只留活代码：去掉整行注释与行尾 `//` 注释（与 run_ci 的静态锁同一口径）。"""
    out = []
    for line in text.splitlines():
        s = line.split("//", 1)[0]
        out.append(s)
    return "\n".join(out)


def scan() -> list:
    offenders = []
    files = sorted((ROOT / "src" / "shell").rglob("*.cpp")) + [ROOT / "tools" / "build_gui" / "main.cpp"]
    for src in files:
        text = active_code(src.read_text(encoding="utf-8", errors="replace"))
        for m in BARE.finditer(text):
            snippet = text[m.start():m.start() + 70].splitlines()[0].strip()
            rel = str(src.relative_to(ROOT)).replace("\\", "/")
            if rel in ALLOW_FILES:
                continue
            if (rel, "setMinimumWidth(1)") in ALLOW and "setMinimumWidth(1)" in snippet:
                continue
            debt = next((k for k in KNOWN_DEBT if k[0] == rel and k[1] in snippet), None)
            if debt is not None:
                print("PX-ONLY debt %s: %s —— %s" % (rel, snippet, KNOWN_DEBT[debt]))
                continue
            offenders.append("%s: %s" % (rel, snippet))
    return offenders


def main() -> int:
    offenders = scan()
    if offenders:
        print("PX-ONLY fail %d 处尺寸/间距没过 px()：" % len(offenders))
        for o in offenders[:8]:
            print("           " + o)
        return 1
    print("PX-ONLY ok 尺寸/间距一律过 px()（白名单 %d 条：log_box 的 1px 布局技巧）" % len(ALLOW))
    return 0


if __name__ == "__main__":
    sys.exit(main())
