# -*- coding: utf-8 -*-
"""**包选择与版本身份的唯一实现**（2026-10-23 收敛）。

为什么要有它
------------
修前有三个脚本各自实现"选哪个包"：
  · `check_package_fixture_pages.py::newest_package()`（2026-10-23 已改成 current 优先）
  · `check_package_platform.py::newest_package()`（**跨标记取最大序号**）
  · `check_version_identity.py::newest_package()`（**跨标记取最大序号**）
后果实测（`current` = BETA/1 时）：
    fixture_pages → dist/2.0.0/**BETA/1**
    platform      → dist/2.0.0/**BETA/258**   ← 跑的是"另一个包"
    version_ident → dist/2.0.0/**BETA/258**   ← 同上
⇒ 门禁的 `pkg-platform` / `version-identity` 两步**一直在验一个不是 current 的包**。
   这比"判错结论"更糟：**判错了对象**。

口径（本模块是唯一实现）
------------------------
1. **首选 `current`**（`version.json` 的 `prerelease` + `index`）—— 它就是"这一轮要验的那一个"；
2. 兜底 A：同一标记下 `tree` 里最大序号（`current` 的目录不存在时）；
3. 兜底 B：全 `tree` 最大序号（跨标记，**只应急**，会打印警告）；
4. 每一步都**回报走了哪条路**（`reason`），调用方必须打印它 —— 选包路径可读，不许静默。

★ 为什么现在可以依赖 `current`：2026-10-23 抓到并修掉了唯一一个"诊断写盘"的写入者
  （`xrkit_build_gui --layout-report` 构造期 `--version-store set`），门禁里已**没有任何步骤**
  改 `version.json`（逐步骤 sha 取证）；`run_ci.py` 收尾还会逐字节恢复三件套。
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
VERSION_JSON = ROOT / "version.json"


def load_version_json() -> dict:
    try:
        return json.loads(VERSION_JSON.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def current_identity() -> tuple:
    """`version.json` 的 `current` → (version, marker, index)。读不到就返回空串/0。"""
    cur = (load_version_json().get("current") or {})
    try:
        index = int(cur.get("index") or 0)
    except (TypeError, ValueError):
        index = 0
    return (str(cur.get("version") or ""), str(cur.get("prerelease") or ""), index)


def _dirs_for(marker: str, index: str) -> list:
    if not DIST.is_dir():
        return []
    return [p for p in DIST.glob("*/*/*")
            if p.is_dir() and p.name == index and (not marker or p.parent.name == marker)]


def resolve_current_package() -> tuple:
    """→ `(path | None, marker, index, reason)`；`reason` 说明走了哪条路（永远可打印）。"""
    data = load_version_json()
    tree = data.get("tree") or {}
    version, marker, index = current_identity()

    # ① current（标记 + 序号）
    if marker and index > 0:
        cands = _dirs_for(marker, str(index))
        if len(cands) == 1:
            return (cands[0], marker, index, "current（%s/%d）" % (marker, index))
        if len(cands) > 1:
            return (None, marker, index,
                    "歧义：current=%s/%d 命中 %d 个目录" % (marker, index, len(cands)))
        reason_prefix = "current=%s/%d 的目录不存在" % (marker, index)
    else:
        reason_prefix = "version.json 没有可用的 current"

    # ② 兜底 A：同标记下 tree 最大序号
    if marker:
        best = -1
        for num in (tree.get(version, {}).get(marker) or {}):
            if str(num).isdigit():
                best = max(best, int(num))
        if best > 0:
            cands = _dirs_for(marker, str(best))
            if len(cands) == 1:
                return (cands[0], marker, best,
                        "%s ⇒ 兜底A：标记 %s 下最大序号 %d" % (reason_prefix, marker, best))

    # ③ 兜底 B：全 tree 最大序号（跨标记，只应急）
    best = -1
    best_marker = ""
    for _ver, marks in tree.items():
        for mark, builds in (marks or {}).items():
            for num in (builds or {}):
                if str(num).isdigit() and int(num) > best:
                    best, best_marker = int(num), mark
    if best > 0:
        cands = _dirs_for(best_marker, str(best))
        if len(cands) == 1:
            return (cands[0], best_marker, best,
                    "%s ⇒ 兜底B（跨标记，仅应急）：%s/%d" % (reason_prefix, best_marker, best))
    return (None, marker, index, "%s ⇒ 没有任何可用的包目录" % reason_prefix)


def tree_last_release() -> tuple:
    """`tree` 里"最后一次成功发包"（跨标记取最大序号）→ (version, marker, index)。"""
    tree = load_version_json().get("tree") or {}
    best = (-1, "", "")
    for ver, marks in tree.items():
        for mark, builds in (marks or {}).items():
            for num in (builds or {}):
                if str(num).isdigit() and int(num) > best[0]:
                    best = (int(num), mark, ver)
    return (best[2], best[1], best[0]) if best[0] > 0 else ("", "", 0)
