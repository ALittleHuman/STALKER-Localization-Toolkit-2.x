#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门面"登记表"三份事实的**共用读取器**（唯一一份实现）。

一个门面的能力，在源码里有三份必须同名同数的"事实"：

  ① **声明**：头文件 `struct XxxFacade` 里的 `std::function<...>` 字段；
  ② **登记**：实现文件里的 `kMethodNames[]`（门面自查"哪些方法算实现了"用的名单）；
  ③ **判定**：实现文件里的 `has_impl()` 分支（某个字段到底是不是非空）。

★ 为什么必须共用一份读取器：`check_facade_order.py`（管**顺序**）与
  `check_facade_registry.py`（管**集合**）看的是同一批文本；各写一份解析器，
  两份语义迟早分叉 —— 本仓已经因为"同一个判定写了两份"栽过
  （`is_plugin_dll_name`：两处各抄一份 ⇒ `NLC.DLL` 静默漏载）。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _cpp_source import function_body, strip_comments  # noqa: E402


def struct_function_fields(header_text: str, struct_name: str) -> list[str]:
    """`struct XxxFacade` 里的 `std::function<...>` 字段名（按声明顺序）。

    逐条照原 `check_facade_order.py` 的解析方式（切 `;` → 去行注释 → 取末尾标识符），
    只把"哪一段文本"参数化出来，语义一字未改。
    """
    marker = "struct %s {" % struct_name
    start = header_text.find(marker)
    if start < 0:
        return []
    body = header_text[start:]
    end = body.find("\n};")
    if end >= 0:
        body = body[:end]
    fields: list[str] = []
    for decl in body.split(";"):
        if "std::function<" not in decl:
            continue
        cleaned = re.sub(r"//[^\n]*", "", decl).strip()
        match = re.search(r"(\w+)\s*$", cleaned)
        if match:
            fields.append(match.group(1))
    return fields


def aggregate_markers(impl_text: str, marker: str) -> list[str]:
    """聚合初始化里的 `/* 字段名 */` 标记（按出现顺序；两种注释形态都认）。"""
    start = impl_text.find(marker)
    if start < 0:
        return []
    seg = impl_text[start:]
    end = seg.find("\n};")
    if end >= 0:
        seg = seg[:end]
    return re.findall(r"/\*\s*(\w+)", seg)


def struct_function_returns(header_text: str, struct_name: str) -> dict[str, str]:
    """`struct XxxFacade` 里每个字段的**返回类型**（名字 → 返回类型文本）。

    用途：判"插件能不能把这个调用的返回值丢掉"。返回类型为 `void` 的调用丢弃返回值是
    正常的；**非 void 的返回值被丢弃**，就丢掉了"这次操作到底成没成"——`copy_text`
    那条静默链正是这么来的（`plugins/xml_page.py` 不看返回值、照样显示"已复制 ID"）。
    """
    marker = "struct %s {" % struct_name
    start = header_text.find(marker)
    if start < 0:
        return {}
    body = header_text[start:]
    end = body.find("\n};")
    if end >= 0:
        body = body[:end]
    out: dict[str, str] = {}
    for decl in body.split(";"):
        if "std::function<" not in decl:
            continue
        cleaned = re.sub(r"//[^\n]*", "", decl).strip()
        name = re.search(r"(\w+)\s*$", cleaned)
        ret = re.search(r"std::function<\s*(.*?)\s*\(", cleaned)
        if name and ret:
            out[name.group(1)] = ret.group(1)
    return out


def k_method_names(impl_text: str) -> list[str]:
    """`kMethodNames[] = { ... }` 里的名字（按出现顺序）。"""
    start = impl_text.find("kMethodNames[]")
    if start < 0:
        return []
    seg = impl_text[start:]
    end = seg.find("};")
    if end >= 0:
        seg = seg[:end]
    return re.findall(r'"(\w+)"', seg)


def has_impl_names(impl_text: str) -> list[str]:
    """`has_impl()` 里 `name == "..."` 的分支名（按出现顺序；先去掉注释再取）。"""
    body = function_body(strip_comments(impl_text), "bool has_impl")
    if not body:
        return []
    return re.findall(r'name\s*==\s*"(\w+)"', body)
