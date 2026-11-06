# -*- coding: utf-8 -*-
"""段表解析单一来源（最小统一 ⑤）。

规则：C++ 侧读 `fonts.ltx` / 解析 `[section]` 只许走 `parse_segments()`（package.cpp）
或 `scan_lang_pack()`（font/langpack.cpp）。其它 engine/font 文件出现 `fonts.ltx`
字样或自己扫 `[` 段落 ⇒ 红（防"核心有了，writer 又抄一份"）。
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "src" / "engine" / "font"
ALLOW = {"package.cpp", "langpack.cpp"}   # 段表解析的两个合法落点
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def main() -> int:
    print("== 段表解析单一来源 ==")
    scanned = 0
    for f in FONT_DIR.glob("*.cpp"):
        scanned += 1
        if f.name in ALLOW:
            continue
        text = f.read_text(encoding="utf-8", errors="replace")
        if "fonts.ltx" in text:
            fail("%s 直接提到 fonts.ltx（段表解析应走 parse_segments）" % f.name)
    (ok if scanned > 5 else fail)("扫描了 %d 个 font/*.cpp" % scanned)
    pkg = (FONT_DIR / "package.cpp").read_text(encoding="utf-8", errors="replace")
    (ok if "parse_segments" in pkg else fail)("package.cpp 里存在 parse_segments（单一实现）")
    if PROBLEMS:
        print("SEGMENTS-SINGLE: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("SEGMENTS-SINGLE: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
