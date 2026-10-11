# -*- coding: utf-8 -*-
"""P17-97 判据：`when` 的**值**不许让类型互相冒充（审计 `05 PLG-06`）。

背景（事实）
------------
`when` 是「注册时给的条件」：`when={"键": 值, ...}` 或 `when=callable`。
宿主侧的比较键类型是 `ContextValue = std::optional<std::string>`（`registry.h`），
即**值按字符串比**；而宿主喂进 context 的值本来就全是字符串（例：`fs_page.cpp` 的
`context["loaded"] = "1"/"0"`）。

修前的洞：Python 侧用 `py::str(item.second)` 把值转字 ⇒

| 插件写 | `str()` 之后 | 后果 |
| --- | --- | --- |
| `when={"n": 1}` | `"1"` | 与下面那行**不可区分** |
| `when={"n": "1"}` | `"1"` | 同上 |
| `when={"n": True}` | `"True"` | 被误当成字符串 `"True"` |

⇒ `when={"n": 1}` 会被宿主喂的**字符串** `"1"` 命中 —— 那不是插件写的意图（类型冒充）。
改法：`py::repr(item.second)` ⇒ `1`→`"1"`、`"1"`→`"'1'"`、`True`→`"True"`，**类型写进那串字**。

判据（两组，都要有牙）
----------------------
① `py_host.cpp` 的 `when_from_python` 里，**非 None 的值必须走 `py::repr`**，
   且**不许**再出现 `py::str(item.second)`（`item.first` 是键，用 `str` 没问题）。
② **口径必须落在字据上**：`registry.h` 里 `ContextValue` 附近要写明
   "值按字符串比较 + 宿主喂的是字符串 + None 表示键不存在"这三件事
   （否则下一个人只能靠猜 —— 本仓"字段/口径先运到、判据跟着落"的同一条纪律）。

输出：`WHEN-VALUE ok …` / `WHEN-VALUE fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
PY_HOST = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
REGISTRY_H = ROOT / "include" / "xrkit" / "engine" / "plugin" / "registry.h"


def audit_when_repr(py_host: str) -> list:
    """① 值必须走 `repr`，且不许再用 `str` 转值。"""
    problems = []
    at = py_host.find("WhenSpec when_from_python")
    if at < 0:
        return ["py_host.cpp：找不到 `when_from_python`（判据与源码脱节？）"]
    # 只看这个函数体（到下一个顶层花括号为空 — 用 "spec.match.emplace_back" 之后 200 字符做窗口）
    tail = py_host[at:at + 4000]
    if "py::repr(item.second)" not in tail:
        problems.append("py_host.cpp：`when` 的值没用 `py::repr` 转（类型会互相冒充）")
    if "py::str(item.second)" in tail:
        problems.append("py_host.cpp：`when` 的值又用回 `py::str` 了（`1`/`True`/`\"1\"` 会撞成一个）")
    # 键用 str 是对的：若连键也换了 repr，反而与宿主 context 的键形状对不上
    if "py::str(item.first)" not in tail:
        problems.append("py_host.cpp：`when` 的**键**不该改（宿主 context 的键是普通字符串）")
    return problems


def audit_contract_written(registry_h: str) -> list:
    """② 口径必须写在 `ContextValue` 附近（三件事：按字符串比 / 宿主喂字符串 / None 的语义）。"""
    problems = []
    at = registry_h.find("using ContextValue")
    if at < 0:
        return ["registry.h：找不到 `using ContextValue`"]
    window = registry_h[at:at + 1400]
    for needle, why in (
            ("按\"字符串\"比较", "没写「值按**字符串**比较」"),
            ("本来就是字符串", "没写「宿主喂进 context 的值本来就是字符串」"),
            ("None", "没写 `None` 的语义（＝期望这个键不存在）"),
    ):
        if needle not in window:
            problems.append("registry.h：`ContextValue` 附近%s" % why)
    return problems


def selfcheck() -> tuple:
    good_host = ('WhenSpec when_from_python(const py::object& when) {\n'
                 '  const std::string key = py::str(item.first).cast<std::string>();\n'
                 '  value = py::repr(item.second).cast<std::string>();\n'
                 '  spec.match.emplace_back(key, value);\n}\n')
    bad_host = good_host.replace("py::repr(item.second)", "py::str(item.second)")
    good_h = ('using ContextValue = std::optional<std::string>;\n'
              '// 值按"字符串"比较；宿主喂进 context 的值本来就是字符串；None 表示键不存在。\n')
    cases = [
        ("好形状", 0, audit_when_repr(good_host) + audit_contract_written(good_h)),
        ("值又用回 str（修前形状）", 1, audit_when_repr(bad_host)),
        ("口径没落字据", 1, audit_contract_written("using ContextValue = std::optional<std::string>;\n")),
        ("键被误改成 repr", 1, audit_when_repr(good_host.replace("py::str(item.first)", "py::repr(item.first)"))),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good = (len(problems) > 0) if want else (not problems)
        ok &= good
        results.append((label, want, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-97：`when` 的值不许让类型互相冒充（审计 `05 PLG-06`）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-26s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                "有错" if want else "无错", got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("WHEN-VALUE fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (PY_HOST, REGISTRY_H):
        if not path.is_file():
            print("WHEN-VALUE fail 仓库自带文件缺失：%s" % path)
            return 1
    problems = audit_when_repr(PY_HOST.read_text(encoding="utf-8", errors="replace"))
    problems += audit_contract_written(REGISTRY_H.read_text(encoding="utf-8", errors="replace"))
    print("-- 实测 --")
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("WHEN-VALUE fail %d 项不成立" % len(problems))
        return 1
    print("   ✓ `when` 的值走 `repr`（类型不互相冒充）")
    print("   ✓ 口径已落在 `registry.h` 的 `ContextValue` 旁边（按字符串比 / 宿主喂字符串 / None 语义）")
    print()
    print("WHEN-VALUE ok `when` 的值类型不再冒充；比较口径有字据（审计 05 PLG-06）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
