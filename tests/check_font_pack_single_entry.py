# -*- coding: utf-8 -*-
"""汉化包生成：**调用路径单一入口**（HA 打回点 ①）。

规则：页面层（src/shell/pages/*.cpp）不许直接调 `build_package(` / `ixr_build(`，
      只能调统一入口 `build_font_pack(`；游戏名只决定默认来源，不决定调用路径。
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = ROOT / "src" / "shell" / "pages"
FORBIDDEN = ("build_package(", "ixr_build(")
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 汉化包生成：调用路径单一入口 ==")
    page = PAGES / "fontpack_page.cpp"
    if not page.is_file():
        fail("找不到 fontpack_page.cpp")
    else:
        text = page.read_text(encoding="utf-8", errors="replace")
        bad = [t for t in FORBIDDEN if t in text]
        (ok if not bad else fail)("fontpack_page.cpp 不直接调 writer（发现：%s）" % (bad or "无"))
        (ok if "build_font_pack(" in text else fail)(
            "fontpack_page.cpp 走统一入口 build_font_pack()")
        # 游戏名可以参与"默认来源"（IX-Ray ⇒ 参照包快照），但不许参与"调用哪条 writer"
        (ok if "ixr_pack_at_start_" in text else fail)(
            "默认来源来自 IX-Ray 参照包快照（游戏名只决定默认值）")
    # 所有页面都不许绕
    for f in sorted(PAGES.glob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        bad = [t for t in FORBIDDEN if t in text]
        if bad:
            fail("%s 直接调 writer：%s" % (f.name, bad))
    if PROBLEMS:
        print("FONT-PACK-SINGLE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("FONT-PACK-SINGLE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
