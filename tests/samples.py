# -*- coding: utf-8 -*-
"""**真机样本的唯一定位器**（2026-11-09：样本信息全部移出源码）。

口径（用户 2026-11-09）
----------------------
· **源码里只允许出现通用别名**（如 `unpack.vessel_ai`），不许出现真机模组名/绝对路径；
· 真实样本名 → 相对片段/绝对根的映射放在**工作区**配置文件：
  `tests/samples.local.json`（已 gitignore）或 `XRKIT_SAMPLES_CONFIG` 指向的 JSON；
· 根优先级：`XRKIT_SAMPLES`（环境变量）→ 配置文件 `roots`；都没有时用一个占位根
  （`find()` 返回该占位路径，判据会明确报"样本缺失"，不是空字符串）。
· 配置文件格式：
  {
    "roots": ["D:/samples", "E:/..."],
    "aliases": {"unpack.vessel_ai": "VESSEL/<真机名>/database/configs/ai.db",
                "text.real_scripts": "Golden Sphere/gamedata/scripts"}
  }
"""
from __future__ import annotations

import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = Path(os.environ.get("XRKIT_SAMPLES_CONFIG", str(HERE / "samples.local.json")))

ROOTS: list[Path] = []
ALIASES: dict[str, str] = {}

if os.environ.get("XRKIT_SAMPLES"):
    ROOTS.append(Path(os.environ["XRKIT_SAMPLES"]))

if CONFIG.is_file():
    try:
        data = json.loads(CONFIG.read_text(encoding="utf-8-sig"))
    except Exception:  # noqa: BLE001 - 坏配置按"没配"处理，判据自己会报样本缺失
        data = {}
    for root in data.get("roots", []) or []:
        if str(root).strip():
            ROOTS.append(Path(str(root)))
    ALIASES = {str(k): str(v) for k, v in (data.get("aliases") or {}).items()}

if not ROOTS:
    ROOTS = [Path("samples")]


def find(*parts: str) -> Path:
    """在各根下依次拼 `parts`；返回第一个存在的，找不到返回第一个根下的路径。"""
    for root in ROOTS:
        cand = root.joinpath(*parts)
        if cand.exists():
            return cand
    return ROOTS[0].joinpath(*parts)


def find_alias(name: str) -> Path:
    """按**通用别名**取样本（映射在 samples.local.json；源码不出现真机名/路径）。"""
    rel = ALIASES.get(name)
    if not rel:
        return ROOTS[0] / name
    path = Path(rel)
    if path.is_absolute():
        return path
    return find(*path.parts)


def find_any(*candidates: tuple) -> Path:
    """按**候选列表**逐个试（每个候选是相对片段元组）；都没有则返回第一个候选。"""
    for parts in candidates:
        got = find(*parts)
        if got.exists():
            return got
    return find(*candidates[0])


def exists(*parts: str) -> bool:
    return find(*parts).exists()
