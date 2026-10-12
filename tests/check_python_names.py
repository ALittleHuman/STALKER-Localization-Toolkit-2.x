# -*- coding: utf-8 -*-
"""P17-108 判据：**夹具页/判据脚本的 Python 名字必须先解析得住**（复审计 [Medium] #5）。

来源（复审计原文的意思）：判据不看 Python 名字解析 ⇒ `plugins/toolchain_page.py` 里
`_run_off_ui_thread` 用了**没定义的** `show_holder`（签名里只有 `show`）**一直没被发现**，
直到**重复点长任务按钮**才 `NameError` 崩（[High]）。我自己的 `check_toolchain_worker.py`
只查"`task_run` 这串在不在"—— 那种"某串在不在"的判据**抓不到"名字根本不存在"**。

判据（三层，都是**名字解析**而不是字符串匹配）
--------------------------------------------
① **语法**：`compile(src, path, "exec")` 必须过（语法错直接红）。
② **符号表**：`symtable` 逐作用域走一遍 —— 任何"被当作**全局**引用、但既不在模块全局、
   也不是内建"的名字 ⇒ 红（这正是 `show_holder` 的形状）。
③ **自由变量**：函数体里"既不是参数、也没有局部绑定、也不在模块全局、也不是内建"的名字 ⇒ 红
   （`symtable` 的 `is_free()` 覆盖闭包引用；配合 ② 把"写错变量名"这一类兜住）。

★ 为什么不直接"真跑每个页"：那要起真 Hub（重、且页里全是界面调用）；
  名字解析是**静态可达的最强近似**，而这次的 bug 恰好**只**能被它抓住。
  ★ 它不是万能的（动态 `setattr`/`getattr` 绕得过去）—— 这一条如实写在下面的输出里。

输出：`PY-NAMES ok …` / `PY-NAMES fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
"""

from __future__ import annotations

import builtins
import symtable
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
GLOBS = ("plugins/*.py", "tests/python_plugins/*.py", "tests/*.py")
BUILTINS = set(dir(builtins))


def _module_globals(src: str, label: str) -> set:
    table = symtable.symtable(src, label, "exec")
    names = {s.get_name() for s in table.get_symbols()}
    # 再收一遍 import 进来的与赋值出来的（symtable 顶层已经覆盖，这里只是保险）
    return names


def audit_names(src: str, label: str) -> list:
    """返回**问题清单**（空 = 名字都解析得住）。"""
    problems = []
    try:
        compile(src, label, "exec")
    except SyntaxError as exc:
        return ["%s：语法错 —— %s" % (label, exc)]
    table = symtable.symtable(src, label, "exec")
    globals_ = {s.get_name() for s in table.get_symbols()}

    def walk(node, path, bound_by_ancestors):
        # ★ 本作用域**绑定**的名字（参数 / 赋值 / import / def …）
        local = {s.get_name() for s in node.get_symbols()
                 if s.is_parameter() or s.is_assigned() or s.is_imported()
                 or s.is_namespace() or s.is_local()}
        inherited = bound_by_ancestors | local
        for sym in node.get_symbols():
            name = sym.get_name()
            if name in BUILTINS or name in globals_:
                continue
            # ★ 解释器**自己造**的名字（PEP 649 的注解求值作用域里会出现 `__classdict__`）
            #   —— 那不是用户写错的名字 ⇒ 跳过（否则判据会误伤，第一版实测就误伤了
            #   `tests/check_sqfs_dos.py` 的类注解）。
            if name.startswith("__") and name.endswith("__"):
                continue
            # ② 被当全局引用、却哪儿都没定义（`show_holder` 就是这个形状）
            if sym.is_global():
                problems.append("%s：`%s` 被当全局用，但模块里没有它（%s）" % (label, name, path))
            # ③ 自由变量：闭包引用了，但**祖先作用域里也没绑**（`kept` 那种正常闭包不算错）
            elif sym.is_free() and name not in bound_by_ancestors:
                problems.append("%s：`%s` 是未绑定的自由变量（%s）" % (label, name, path))
        for child in node.get_children():
            walk(child, path + ">" + child.get_name(), inherited)

    walk(table, "<module>", set())
    return problems


def selfcheck() -> tuple:
    cases = [
        ("好代码", 0, audit_names("def f(a, show=None):\n    if show is not None:\n        show(a)\n", "t.py")),
        # ★ 复审计 [High] 的**最小复现**（正是它抓到的那个形状）
        ("写错变量名（复审计 High 的形状）", 1,
         audit_names("def f(a, show=None):\n    if show_holder['fn'] is not None:\n"
                     "        show_holder['fn'](a)\n", "t.py")),
        ("闭包自由变量没绑", 1,
         audit_names("def outer():\n    def inner():\n        return missing_name\n    return inner\n", "t.py")),
        ("正常闭包引用不算错", 0,
         audit_names("def outer():\n    kept = 1\n    def inner():\n        return kept\n    return inner\n",
                     "t.py")),
        ("语法错", 1, audit_names("def f(:\n", "t.py")),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good_case = (len(problems) > 0) if want else (not problems)
        ok &= good_case
        results.append((label, want, len(problems), good_case, problems))
    return ok, results


def main() -> int:
    print("== P17-108 夹具页/判据脚本的 Python 名字必须解析得住（复审计 [Medium] #5）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-30s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                 "有错" if want else "无错", got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("PY-NAMES fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    files = []
    for pattern in GLOBS:
        files.extend(sorted(ROOT.glob(pattern)))
    files = [p for p in files if p.is_file()]
    if not files:
        print("!! 一个 .py 都没扫到（%s）" % ", ".join(GLOBS))
        print("PY-NAMES skip 没有可扫的脚本")
        return 2

    problems = []
    for path in files:
        src = path.read_text(encoding="utf-8", errors="replace")
        problems += audit_names(src, path.relative_to(ROOT).as_posix())

    print("-- 实测（%d 个脚本）--" % len(files))
    if problems:
        for item in problems:
            print("   ✗ %s" % item)
        print()
        print("PY-NAMES fail %d 项不成立" % len(problems))
        return 1
    print("   ✓ %d 个脚本的语法与名字引用都解析得住（含 `plugins/*.py`、`tests/*.py`）" % len(files))
    print("   ★ 边界（如实）：动态 `setattr/getattr`、`eval`、跨文件注入的名字**扫不到** ——"
          " 它只保证「写错变量名／名字不存在」这一类不再漏过去")
    print()
    print("PY-NAMES ok 名字解析体检通过（复审计 [Medium] #5 的锁）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
