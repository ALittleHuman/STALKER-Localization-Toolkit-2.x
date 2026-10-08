# -*- coding: utf-8 -*-
"""插件**不许丢弃"成没成"型门面调用的返回值**（P16-0 第二腿的判据）。

为什么要它（P16-0，2026-10-08）
--------------------------------
`plugins/xml_page.py` 原来写的是：

    ui.copy_text(ids[0])
    ui.set_text(stats, "已复制 ID：" + ids[0])

而那时 `copy_text` 恒返回 False（门面登记缺口，见 `check_facade_registry.py`）
⇒ **界面对用户谎报成功**（点了没反应还说"已复制"）。修法是看返回值走两支；
本锁保证"下一个人不能把它写回去"。

判据（唯一事实来源 = 门面头文件）
--------------------------------
从 `include/xrkit/engine/plugin/widget_facade.h` 的 `UiFacade` 逐字段取**返回类型**，
扫 `plugins/*.py`：凡 `ui.<name>(...)` 的**裸语句**（整行只有这个调用、返回值被丢掉），

  · 返回类型 `void`      ⇒ 正常（`set_text` / `pack` / `log` …）；
  · 返回类型 `UiHandle`  ⇒ 正常（`label` / `button` / `tree` … 是**控件构造**，
                            句柄丢给宿主管，插件不留也不该留）；
  · **其余任何返回类型 ⇒ 判红**（`bool` 的"成没成"、`int` / `std::string` 的取值…）。

★ 为什么判据长在头文件上而不是手写名单：手写名单会与门面分叉（本仓已因"同一个判定
  写两份"栽过）；返回类型是**可以机器读的唯一真值**。
★ 名字别名表只有一条（实测）：Python 侧 `log` ↔ 字段 `log_append`
  （`py_host.cpp:2891` 的 `.def("log", …)` → `:2011` 的 `available("log_append")`）。
  除它之外，`dir(xrkit.Ui)` 的 93 个名字与结构体 93 个字段**逐名相同**。
★ **本锁不设"允许丢弃"白名单** —— 白名单一旦开了口子就会变成橡皮章
  （"多一个即红"的纪律同理）。真要放开某个调用，就把失败路径写出来。

输出与退出码
------------
    `PLUGIN-RET ok 丢弃的只有 void/UiHandle（n 处）`
    `PLUGIN-RET fail <n> 处丢弃了非 void 返回值（见上）`
退出码：`0`=绿；`1`=有断言不成立。

自带反例自证（`selfcheck()`）
-----------------------------
合成五种片段：void 裸调用（绿）/ UiHandle 裸调用（绿）/ bool 裸调用（红）/
bool 放进 `if`（绿）/ 名字不在门面里（红）。
"""

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

sys.path.insert(0, str(Path(__file__).resolve().parent))

import _facade_tables as ft  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

# 裸语句形式：整行（可有缩进）就是一个 `ui.xxx(` / `self.ui.xxx(` 调用
BARE_CALL = re.compile(r"^\s*(?:self\.)?ui\.(\w+)\(")

# 唯一一条名字别名（见文件头 ★）：Python 可见名 → 结构体字段名
_ALIAS = {"log": "log_append"}

# 丢掉返回值属于正常的返回类型（控件构造句柄 / 纯动作）
_DISCARDABLE = {"void", "UiHandle"}


def bare_calls(source: str):
    """返回 [(行号, 方法名)] —— 返回值被丢掉的裸调用。"""
    out = []
    for index, line in enumerate(source.splitlines(), start=1):
        code = line.split("#", 1)[0]
        match = BARE_CALL.match(code)
        if match:
            out.append((index, match.group(1)))
    return out


def audit(source: str, returns: dict[str, str]):
    """返回 (问题列表, 可丢弃的裸调用数)。"""
    problems = []
    ok_count = 0
    for line_no, name in bare_calls(source):
        field = _ALIAS.get(name, name)
        ret = returns.get(field)
        if ret is None:
            problems.append("第 %d 行：`ui.%s(` 在 `UiFacade` 里找不到对应字段"
                            "（名字对不上？别名表也查过了）" % (line_no, name))
            continue
        if ret.replace(" ", "") in _DISCARDABLE:
            ok_count += 1
            continue
        problems.append("第 %d 行：`ui.%s(` 丢弃了返回类型 `%s` 的返回值 —— "
                        "这次调用成没成会**静默**（失败必须可见）"
                        % (line_no, name, ret))
    return problems, ok_count


# ── 反例自证 ────────────────────────────────────────────────────────────
_SYN_RETURNS = {"set_text": "void", "label": "UiHandle", "copy_text": "bool",
                "get_text": "std::string"}


def selfcheck():
    cases = [
        ("void 裸调用", "def f(ui):\n    ui.set_text(w, 'x')\n", False),
        ("UiHandle 裸调用（控件构造）", "def f(ui):\n    ui.label(None, 'x')\n", False),
        ("bool 裸调用（丢成败位）", "def f(ui):\n    ui.copy_text('x')\n", True),
        ("bool 放进 if", "def f(ui):\n    if ui.copy_text('x'):\n        pass\n", False),
        ("名字不在门面里", "def f(ui):\n    ui.not_a_facade_method(1)\n", True),
    ]
    results, ok = [], True
    for label, source, want_red in cases:
        problems, _ok = audit(source, _SYN_RETURNS)
        got_red = bool(problems)
        good = got_red == want_red
        ok &= good
        results.append((label, want_red, got_red, good, problems))
    return ok, results


def main() -> int:
    print("== 插件不得丢弃「成没成」型门面返回值的核对器 ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_red, got_red, good, problems in results:
        print("   %s %-28s 期望=%s 实得=%s"
              % ("✓" if good else "✗", label, "红" if want_red else "绿",
                 "红" if got_red else "绿"))
        if not good:
            for p in problems:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("PLUGIN-RET fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 5/5")
    print()

    header = ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h"
    if not header.exists():
        print("PLUGIN-RET fail 仓库自带文件缺失：%s" % header)
        return 1
    returns = ft.struct_function_returns(header.read_text(encoding="utf-8"), "UiFacade")
    if not returns:
        print("PLUGIN-RET fail 一个字段都没解析到（解析器与源码脱节）")
        return 1

    plugin_dir = ROOT / "plugins"
    files = sorted(plugin_dir.glob("*.py"))
    if not files:
        print("PLUGIN-RET fail %s 下没有 .py（集合为空不算通过）" % plugin_dir)
        return 1

    discardable = sum(1 for v in returns.values()
                      if v.replace(" ", "") in _DISCARDABLE)
    print("-- 逐插件核对（门面字段 %d 个：可丢弃 %d / 必须看返回值 %d；别名 %d 条）--"
          % (len(returns), discardable, len(returns) - discardable, len(_ALIAS)))
    all_problems = 0
    total_ok = 0
    for path in files:
        problems, ok_count = audit(path.read_text(encoding="utf-8"), returns)
        total_ok += ok_count
        if problems:
            print("   ✗ %s" % path.name)
            for p in problems:
                print("        %s" % p)
            all_problems += len(problems)
        else:
            print("   ✓ %-20s 丢弃的只有 void/UiHandle（%d 处）" % (path.name, ok_count))
    print()
    if all_problems:
        print("PLUGIN-RET fail %d 处丢弃了「成没成」型返回值" % all_problems)
        return 1
    print("PLUGIN-RET ok 丢弃的只有 void/UiHandle（%d 处，%d 个插件）" % (total_ok, len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
