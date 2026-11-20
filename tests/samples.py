# -*- coding: utf-8 -*-
"""**真机样本的唯一定位器**（2026-11-09：样本信息全部移出源码；2026-11-20 增补跳过/流程）。

口径（用户 2026-11-09 / 2026-11-20）
------------------------------------
· **源码里只允许出现通用别名**（如 `unpack.vessel_ai`），不许出现真机模组名/绝对路径；
· 真实样本名 → 相对片段/绝对根的映射放在**工作区**配置文件：
  `tests/samples.local.json`（已 gitignore）或 `XRKIT_SAMPLES_CONFIG` 指向的 JSON；
· 根优先级：`XRKIT_SAMPLES`（环境变量）→ 配置文件 `roots`；都没有时用占位根
  （`find()` 返回该占位路径，判据会明确报"样本缺失"，不是空字符串）。
· ★ 2026-11-20：门禁**不与本机样本绑定**——样本要么在配置里给路径，要么在
  `skip` 里**显式声明跳过**；两者都没有才是"缺样本"（WARN）。
· 配置里还可以写 `flows`：自定义测试链，由门禁的 `samples-flows` 步骤执行。

文件格式（全部可选，缺省见括号）
--------------------------------
{
  "roots":   ["D:/samples", "E:/..."],                  // 相对路径的基准根
  "aliases": {"unpack.vessel_ai": "VESSEL/<真机名>/..."},// 别名 → 相对片段或绝对路径
  "skip":    {"golden.real_corpus": "本机没有这套语料"},   // 别名 → 跳过理由（也可写列表）
  "flows":   [                                          // 自定义测试链（按顺序跑）
    {"id": "cop-track", "cmd": ["{python}", "tests/check_xxx.py", "{alias:mod.cop_root}"]}
  ]
}

token 替换：`{python}` = 当前解释器；`{root}` = 第一个可用根；`{alias:名字}` = 该别名解析出的路径。
"""
from __future__ import annotations

import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = Path(os.environ.get("XRKIT_SAMPLES_CONFIG", str(HERE / "samples.local.json")))

ROOTS: list[Path] = []
ALIASES: dict[str, str] = {}
SKIP: dict[str, str] = {}
FLOWS: list[dict] = []

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
    raw_skip = data.get("skip") or {}
    if isinstance(raw_skip, list):
        SKIP = {str(k): "" for k in raw_skip}
    elif isinstance(raw_skip, dict):
        SKIP = {str(k): str(v) for k, v in raw_skip.items()}
    for flow in data.get("flows") or []:
        if isinstance(flow, dict) and flow.get("cmd"):
            FLOWS.append(flow)

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


# ── ★ 2026-11-20：跳过声明与自定义流程 ───────────────────────────────────
def skip_reason(name: str) -> str | None:
    """该别名是否被配置声明跳过；返回理由（可能为空串），未声明返回 None。"""
    return SKIP.get(name)


def skipped_names() -> list[str]:
    return sorted(SKIP)


def has_alias(name: str) -> bool:
    return name in ALIASES


def flows() -> list[dict]:
    """配置里声明的自定义测试链（每项含 id + cmd）。"""
    return list(FLOWS)


def expand(tokens: list[str]) -> list[tuple[str, bool]]:
    """把 `{python}` / `{root}` / `{alias:名字}` 换掉。

    返回 [(值, 是否解析成功)] —— 别名缺失时**不猜路径**，调用方据此跳过该流程。
    """
    out: list[tuple[str, bool]] = []
    for token in tokens:
        text = str(token)
        if text == "{python}":
            import sys

            out.append((sys.executable, True))
            continue
        if text == "{root}":
            out.append((str(ROOTS[0]), True))
            continue
        if text.startswith("{alias:"):
            name = text[len("{alias:"):].rstrip("}")
            if not has_alias(name) or skip_reason(name) is not None:
                out.append(("", False))
                continue
            out.append((str(find_alias(name)), True))
            continue
        out.append((text, True))
    return out
