#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""静态判据的共用小工具：C++ 源码 → "活代码"（去注释，**保留行号**）。

★ 为什么要有它（2026-10-06 审计 §D1-8 的教训）：按行首 `*` 判断注释的启发式会把
**活代码**（解引用赋值、乘法续行）一起丢掉，判据就能被排版绕过；而**不去注释**又会被
"修复说明里引用了修前那一行"骗成假红（本批的 PR 注释里就有原样引用的反例）。
所以两个方向都要：注释不算数，活代码一个字都不许少。
"""

from __future__ import annotations


def strip_comments(source: str) -> str:
    """去掉 `//` 与 `/* */` 注释；换行照数（行号可与原文件对齐）。"""
    out: list[str] = []
    i = 0
    n = len(source)
    while i < n:
        c = source[i]
        if c == "/" and i + 1 < n and source[i + 1] == "/":
            while i < n and source[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and source[i + 1] == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                if source[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        if c in ('"', "'"):
            quote = c
            out.append(c)
            i += 1
            while i < n and source[i] != quote:
                if source[i] == "\\" and i + 1 < n:
                    out.append(source[i])
                    i += 1
                out.append(source[i])
                i += 1
            if i < n:
                out.append(source[i])
                i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def function_body(source: str, name: str) -> str:
    """粗切一个函数体：从 `name` 起，按花括号配对到闭合（找不到返回空串）。"""
    start = source.find(name)
    if start < 0:
        return ""
    open_at = source.find("{", start)
    if open_at < 0:
        return ""
    depth = 0
    for i in range(open_at, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[open_at:i + 1]
    return ""
