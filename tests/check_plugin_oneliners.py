# -*- coding: utf-8 -*-
"""P-739 / P-745 / P-770 的红锁（注入审计 session-ac61aa8a）。

这三条都是"一行级"缺陷，**修前必须红**：
  · P-739 `plugins/video_page.py`：版本闸门 `if not rejected(...): pass`（空壳 ⇒ 8.x 放行）；
  · P-745 `plugins/fontpack_page.py`：`index == len(SIZES) + 1` 永假 ⇒ `SUFFIXES[2]` IndexError；
  · P-770 `tools/cli/build_cmd.cpp`：回退失败后 `goto fail_archive` 回到失败块 ⇒ 自循环。

P-739/P-745 同时做**可导入性**检查：`python -m py_compile` 通过，防止"修好了但语法坏"。
诚实标注：这三条是**源码锚点 + 语法编译**，不是运行行为（要驱动插件 UI 需整套 facade 桩）。
"""
import py_compile
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBLEMS = []


def ok(msg):
    print("  \u2713 " + msg)


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def main() -> int:
    print("== 一行级缺陷红锁：P-739 / P-745 / P-770 ==")
    video = (ROOT / "plugins" / "video_page.py").read_text(encoding="utf-8", errors="replace")
    fontpack = (ROOT / "plugins" / "fontpack_page.py").read_text(encoding="utf-8", errors="replace")
    build = (ROOT / "tools" / "cli" / "build_cmd.cpp").read_text(encoding="utf-8", errors="replace")

    if "if not engine.video_version_rejected(" not in video and "video_version_rejected(version_text)" in video:
        ok("P-739：版本闸门不再是空壳（rejected 决定是否继续）")
    else:
        fail("P-739：版本闸门仍是 `if not rejected(...): pass`")
    if "len(SIZES) + 1" not in fontpack:
        ok("P-745：suffix_value 不再用 len(SIZES)+1 判自定义")
    else:
        fail("P-745：`len(SIZES) + 1` 还在（自定义永远走错分支）")
    # 只许"阶段失败 ⇒ 归档"的 goto；回退失败那条必须消失。
    # 注意：注释里会提到 `goto fail_archive`（说明历史），所以只认**代码行**。
    import re as _re
    rollback_region = build[build.find("回退失败") : build.find("回退失败") + 600]
    code_goto = _re.search(r"(?m)^\s*goto fail_archive;", rollback_region)
    if code_goto is None:
        ok("P-770：回退失败后不再 goto（自循环消除；注释里的历史说明不算）")
    else:
        fail("P-770：回退失败后仍有 goto fail_archive")

    for rel in ("plugins/video_page.py", "plugins/fontpack_page.py"):
        try:
            py_compile.compile(str(ROOT / rel), doraise=True)
            ok("%s 语法编译通过" % rel)
        except Exception as exc:  # noqa: BLE001
            fail("%s 编译失败：%r" % (rel, exc))

    if PROBLEMS:
        print("PLUGIN-ONELINERS: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("PLUGIN-ONELINERS: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
