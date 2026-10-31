# -*- coding: utf-8 -*-
"""P-684 / P-767 / P-768 的静态红锁（注入审计 session-ac61aa8a）。

这三条的修复都是"小到没有行为差异"的类型，**没有可靠的外部可观测量**：
  · P-684 需要"目录读不到"的权限夹具（Windows 上要改 ACL，太重）；
  · P-767 是删死参数，行为不变；
  · P-768 是"先累加再清零"改成"源头不计数"，`hits()` 最终值一样。
所以这里用**源码锚点**做红锁（红线 = 锚点在/不在），并在文件里明写这是静态锚点、
不是行为证据 —— 注入审计自己骂过"判据只读源码文本"这个盲区，这里如实标注。

红线（修前必须失败）：
  1. `xml_page.cpp` 必须**读** `report.errors`（P-684）；
  2. `run_extract` 里必须把遍历错误当失败返回（`walk_errors`）；
  3. `apply_filter_to` 不许再有 `parent_visible` 形参（P-767）；
  4. `++hits_` 必须带 `!needle.isEmpty()` 守卫（P-768）。
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBLEMS = []


def ok(msg):
    print("  \u2713 " + msg)


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def main() -> int:
    print("== 树/提取：P-684 / P-767 / P-768 红锁 ==")
    xml_page = (ROOT / "src" / "shell" / "pages" / "xml_page.cpp").read_text(encoding="utf-8", errors="replace")
    facade_cpp = (ROOT / "src" / "shell" / "facade_tree.cpp").read_text(encoding="utf-8", errors="replace")
    facade_h = (ROOT / "src" / "shell" / "facade_tree.h").read_text(encoding="utf-8", errors="replace")

    if "report.errors" in xml_page:
        ok("P-684：xml_page 读了 WalkReport.errors")
    else:
        fail("P-684：`WalkReport` 仍构造完就扔（遍历错误静默）")
    if "walk_errors" in xml_page and "无法判定空侧" in xml_page:
        ok("P-684：run_extract 把遍历错误当失败（不再误判空侧）")
    else:
        fail("P-684：run_extract 没把遍历错误当失败")
    if "parent_visible" not in facade_cpp and "parent_visible" not in facade_h:
        ok("P-767：死参数 parent_visible 已清掉")
    else:
        fail("P-767：`parent_visible` 死参数还在")
    if "self_match && !needle.isEmpty()" in facade_cpp:
        ok("P-768：命中计数在源头排除空串（不再先累加再清零）")
    else:
        fail("P-768：`++hits_` 仍无空串守卫")

    if PROBLEMS:
        print("TREE-FILTER: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("TREE-FILTER: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
