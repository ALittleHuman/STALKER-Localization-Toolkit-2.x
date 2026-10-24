# -*- coding: utf-8 -*-
"""**真机样本的唯一定位器**（2026-10-24 收敛）。

为什么要有它
------------
修前每个判据脚本各自写死绝对路径（`E:\\Software\\Games\\STALKER\\Games\\…`）。
2026-10-24 真机样本被整理到 `E:\\Software\\Tools\\Transition\\DB双向验证套件\\samples\\原件\\`
⇒ 四个门禁步骤（语料 / DB 语料 / 真机解包 / 真语料文本）全部退化成"样本缺失"，
而**样本其实都在**，只是路径不同 —— 判据"看起来红"，红的却是"位置变了"。

口径
----
· 根按优先级：`XRKIT_SAMPLES`（环境变量，一台机器一个根）→ 旧 `Games` → `Transition…\\原件`；
· 脚本只写**相对片段**（`VESSEL/S.T.A.L.K.E.R. VESSEL/…`），由 `find()` 逐根拼；
· 找不到时返回"第一个根 + 片段"的路径（让报错**点名**到底在找什么，而不是空字符串）。
"""
from __future__ import annotations

import os
from pathlib import Path

ROOTS: list[Path] = []
if os.environ.get("XRKIT_SAMPLES"):
    ROOTS.append(Path(os.environ["XRKIT_SAMPLES"]))
ROOTS.extend([
    Path(r"E:\Software\Games\STALKER\Games"),
    Path(r"E:\Software\Tools\Transition\DB双向验证套件\samples\原件"),
])


def find(*parts: str) -> Path:
    """在各根下依次拼 `parts`；返回第一个存在的，找不到返回第一个根下的路径。"""
    for root in ROOTS:
        cand = root.joinpath(*parts)
        if cand.exists():
            return cand
    return ROOTS[0].joinpath(*parts)


def find_any(*candidates: tuple) -> Path:
    """按**候选列表**逐个试（每个候选是相对片段元组）；都没有则返回第一个候选。"""
    for parts in candidates:
        got = find(*parts)
        if got.exists():
            return got
    return find(*candidates[0])


def exists(*parts: str) -> bool:
    return find(*parts).exists()
