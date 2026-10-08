# -*- coding: utf-8 -*-
"""`struct stk_ui` 的 16 个方法 ↔ 契约表 `kUiMethods[]` 的**闭合**判据（P16-0b）。

为什么要有它：另一个会话 2026-10-09 的只读核对指出 —— 「`stk_ui`(16 方法)**不受任何判据管**」。
我复核属实：全仓 `*.py` 里与 `stk_ui` 有关的只有生成器的一句 `EXPECTED_UI_METHODS = 16`
（`tools/gen_plugin_contract.py:29`），它管的是**契约表自身的条数**，
**没有任何判据拿契约表与 `include/xrkit/plugin/abi.h` 的 `struct stk_ui` 对账**。

后果（这正是 P16-0 那一类）：契约表说"有 16 个方法、参数名是什么"，而 C ABI 结构体里
真正被宿主调用的 16 个函数指针**可以悄悄改名/换序/少一个**，第三方照契约写出来的插件
就会在运行期拿到空指针或喂错参数 —— 而门禁一声不响。

判据
----
从 `include/xrkit/plugin/abi.h` 的 `struct stk_ui` 取函数指针字段名（按声明顺序），
与 `src/engine/plugin/contract_tables.inc` 的 `kUiMethods[]` 名字逐项比：
  ① 条数相等（且 == 契约表自报的 `kUiMethodCount`，且 == 16）；
  ② **逐名同序**（顺序也锁：C ABI 的调用端按位置取，换序 = 换语义）；
  ③ 空集不算通过。

输出：`ABI-CLOSURE ok …` / `ABI-CLOSURE fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证：匹配/换序/少一个/条数不符/空集 五种合成输入。
"""

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent

ABI_H = ROOT / "include" / "xrkit" / "plugin" / "abi.h"
CONTRACT_INC = ROOT / "src" / "engine" / "plugin" / "contract_tables.inc"

_FIELD_RE = re.compile(r"\(\*\s*(\w+)\s*\)")
_UI_NAME_RE = re.compile(r'\{\s*"(\w+)"\s*,')
_COUNT_RE = re.compile(r"kUiMethodCount\s*=\s*(\d+)")
EXPECTED = 16


def abi_ui_fields(source: str) -> list[str]:
    """`struct stk_ui { ... }` 里的函数指针字段名（按声明顺序）。"""
    start = source.find("typedef struct stk_ui {")
    if start < 0:
        return []
    seg = source[start:]
    end = seg.find("} stk_ui;")
    if end >= 0:
        seg = seg[:end]
    return _FIELD_RE.findall(seg)


def contract_ui_names(source: str):
    """`kUiMethods[]` 的名字（按声明顺序）+ 自报条数。"""
    start = source.find("kUiMethods[]")
    names = []
    if start >= 0:
        seg = source[start:]
        end = seg.find("};")
        if end >= 0:
            seg = seg[:end]
        names = _UI_NAME_RE.findall(seg)
    match = _COUNT_RE.search(source)
    return names, (int(match.group(1)) if match else -1)


def audit(abi_source: str, contract_source: str):
    """返回问题清单。"""
    problems = []
    fields = abi_ui_fields(abi_source)
    names, declared = contract_ui_names(contract_source)

    if not fields:
        problems.append("`abi.h` 里没解析到 `struct stk_ui` 的方法字段（解析器与源码脱节？）")
    if not names:
        problems.append("`contract_tables.inc` 里没解析到 `kUiMethods[]`（解析器与源码脱节？）")
    if problems:
        return problems

    if len(fields) != EXPECTED:
        problems.append("`struct stk_ui` 有 %d 个方法，契约冻结值是 %d" % (len(fields), EXPECTED))
    if declared != len(names):
        problems.append("契约表自报 `kUiMethodCount = %d`，实际列出 %d 条" % (declared, len(names)))
    if len(fields) != len(names):
        problems.append("条数不等：`struct stk_ui` %d 个 / 契约表 %d 条 —— 契约与 ABI 不是同一件事"
                        % (len(fields), len(names)))

    shown = 0
    for index, (field, name) in enumerate(zip(fields, names)):
        if field != name:
            if shown < 6:
                problems.append("第 %d 项：`struct stk_ui` 里是 `%s`，契约表里是 `%s`"
                                % (index + 1, field, name))
            shown += 1
    if shown > 6:
        problems.append("……另有 %d 项不同名/不同序" % (shown - 6))
    return problems


# ── 反例自证 ────────────────────────────────────────────────────────────
def _syn(names):
    fields = "".join("    void (*%s)(void);\n" % n for n in names)
    abi = "typedef struct stk_ui {\n%s} stk_ui;\n" % fields
    rows = "".join('    {"%s", "p"},\n' % n for n in names)
    table = ("inline constexpr UiMethodSpec kUiMethods[] = {\n%s};\n"
             "inline constexpr int kUiMethodCount = %d;\n" % (rows, len(names)))
    return abi, table


_OK = ["alpha", "beta", "gamma"]


def selfcheck():
    cases = [
        ("匹配", _syn(_OK), 0),
        ("换序", (_syn(["beta", "alpha", "gamma"])[0], _syn(_OK)[1]), 1),
        ("少一个", _syn(["alpha", "beta"]), 1),
        ("条数自报不符", (_syn(_OK)[0], _syn(_OK)[1].replace("kUiMethodCount = 3",
                                                             "kUiMethodCount = 4")), 1),
        ("空集", ("typedef struct stk_ui {\n} stk_ui;\n", _syn([])[1]), 1),
    ]
    results, ok = [], True
    for label, (abi, table), want_min in cases:
        problems = audit(abi, table)
        good = len(problems) >= want_min if want_min else not problems
        ok &= good
        results.append((label, want_min, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== struct stk_ui ↔ kUiMethods[] 闭合判据 ==")
    print()
    print("-- 反例自证 --")
    # 合成用例用 3 项，所以这里临时把 EXPECTED 放宽：只验"逐名同序 + 条数自洽"
    global EXPECTED
    saved = EXPECTED
    EXPECTED = 3
    self_ok, results = selfcheck()
    EXPECTED = saved
    for label, want_min, got, good, problems in results:
        print("   %s %-14s 期望≥%d 实得=%d" % ("✓" if good else "✗", label, want_min, got))
        if not good:
            for p in problems[:2]:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("ABI-CLOSURE fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    if not ABI_H.exists() or not CONTRACT_INC.exists():
        print("ABI-CLOSURE fail 仓库自带文件缺失：%s / %s" % (ABI_H, CONTRACT_INC))
        return 1
    problems = audit(ABI_H.read_text(encoding="utf-8"), CONTRACT_INC.read_text(encoding="utf-8"))

    fields = abi_ui_fields(ABI_H.read_text(encoding="utf-8"))
    names, declared = contract_ui_names(CONTRACT_INC.read_text(encoding="utf-8"))
    print("-- 实测 --")
    print("   struct stk_ui：%d 个方法" % len(fields))
    print("   契约表：%d 条（自报 kUiMethodCount = %d）" % (len(names), declared))
    print("   名字：%s" % ", ".join(names))
    print()
    if problems:
        for p in problems:
            print("   ✗ %s" % p)
        print()
        print("ABI-CLOSURE fail %d 项不成立" % len(problems))
        return 1
    print("ABI-CLOSURE ok 16 个方法与契约表**逐名同序**（%s）" % ", ".join(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
