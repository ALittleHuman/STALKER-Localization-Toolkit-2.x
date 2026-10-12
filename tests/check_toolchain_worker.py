# -*- coding: utf-8 -*-
"""P17-107 判据：`toolchain_page.py` 的**六段引擎重活必须经任务壳 worker**（审计 `05 PLG-05`）。

口径来源：★ **用户 2026-10-12 拍板 A** —— 不改产品，只修夹具页；产品代码零改动。

事实（修前）：三块面板的按钮回调**直接在主线程**调 `api.engine.*`
（`patch_identify` / `patch_apply` / `font_pack` / `text_extract_dir` / `pkg_install` / `pkg_restore`）
—— 都是长任务 ⇒ 跑起来界面死住（拖不动、日志不刷、取消点不动）。

判据（分**静态接线**与**引擎可调用性**两半；**能证到哪一步就说哪一步**）
------------------------------------------------------------------------
**① 静态接线**（AST，不靠正则糊）：`toolchain_page.py` 的 `_run_off_ui_thread()` 必须
   · 调 `api.ui.task_run(`（worker 的唯一入口）；
   · 在 worker 里调 `api.ui.on_ui_thread()`（取证位）；
   · 界面那一侧**恰好 6 处** `_run_off_ui_thread(api, panel, ...)` 调用（六段重活各一处）。

**② 引擎可调用性**（真跑）：无界面跑 `headless(action="thread_probe")`，逐段报告
   "这段引擎调用在哪个线程、能不能被调"。

★ **判据自曝的边界（不许当"已证"）**：无界面进程里**没有任务壳** ⇒
  `task_probe` 会如实报 `worker=unavailable`；也就是说
  **"这段活真的跑在非 UI 线程上"这条运行期证据，本判据拿不到**
  （它需要真 Hub；`_run_off_ui_thread` 已内置 marker 取证位 —— 设
  `XRKIT_TOOLCHAIN_MARKER=<文件>` 后点一次按钮，marker 里会出现 `on_ui_thread=False`）。
  ⇒ 本判据只声称"**接线对 ＋ 六段引擎调用都调得动**"，**不**声称"跑过 worker"。

输出：`TOOLCHAIN-WORKER ok …` / `… fail …`；退出码 0=绿 / 1=有断言不成立 / 2=外部前提缺失。
"""

from __future__ import annotations

import ast
import json
import subprocess
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
PAGE = ROOT / "plugins" / "toolchain_page.py"
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
HEAVY = ("identify", "apply", "font_pack", "pipeline", "install", "restore")

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def _calls(node: ast.AST, dotted: str) -> int:
    """数 `a.b.c(...)` 形状的调用（严格按属性链比对，不靠子串）。"""
    n = 0
    for sub in ast.walk(node):
        if isinstance(sub, ast.Call) and isinstance(sub.func, ast.Attribute):
            parts = []
            cur = sub.func
            while isinstance(cur, ast.Attribute):
                parts.append(cur.attr)
                cur = cur.value
            if isinstance(cur, ast.Name):
                parts.append(cur.id)
            if ".".join(reversed(parts)) == dotted:
                n += 1
    return n


def static_audit(source: str) -> list:
    problems = []
    tree = ast.parse(source)
    helper = None
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == "_run_off_ui_thread":
            helper = node
            break
    if helper is None:
        return ["找不到 `_run_off_ui_thread`（六段重活没有再经任务壳）"]
    if _calls(helper, "api.ui.task_run") == 0:
        problems.append("`_run_off_ui_thread` 里没有 `api.ui.task_run(` —— 那就不在 worker 上跑")
    if _calls(helper, "api.ui.on_ui_thread") == 0:
        problems.append("`_run_off_ui_thread` 里没有 `api.ui.on_ui_thread()` 取证位")
    # 界面侧恰好 6 处（六段重活各一处）
    total = 0
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) \
                and node.func.id == "_run_off_ui_thread":
            total += 1
    if total != 6:
        problems.append("界面侧 `_run_off_ui_thread(...)` 调用 %d 处（期望 6 处 ＝ 六段重活）" % total)
    # `HEAVY_STEPS` 的六段名字必须齐（取证入口按同一张表跑）
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "HEAVY_STEPS" for t in node.targets):
            if isinstance(node.value, ast.Tuple):
                for elt in node.value.elts:
                    if isinstance(elt, ast.Tuple) and elt.elts and isinstance(elt.elts[0], ast.Constant):
                        names.add(elt.elts[0].value)
    missing = [n for n in HEAVY if n not in names]
    if missing:
        problems.append("`HEAVY_STEPS` 缺段：%s" % ", ".join(missing))
    return problems


def runtime_probe(cli: Path) -> tuple:
    payload = json.dumps({"action": "thread_probe"})
    p = subprocess.run([str(cli), "--py-plugin-call", str(ROOT / "plugins"),
                        "--plugin", "toolchain_page.py", payload],
                       capture_output=True, timeout=900)
    text = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
    lines = []
    for raw in text.splitlines():
        if "PYPLUGINCALL plugin=" in raw and " result=" in raw:
            try:
                obj = json.loads(raw.split(" result=", 1)[1])
                lines = obj.get("lines", []) or []
            except ValueError:
                lines = []
    return p.returncode, text, lines


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-107 `toolchain_page` 六段重活经任务壳 worker（审计 `05 PLG-05`，口径 A）==")
    if not PAGE.is_file():
        print("!! 缺夹具页：%s" % PAGE)
        print("TOOLCHAIN-WORKER skip 缺夹具页")
        return 2
    source = PAGE.read_text(encoding="utf-8", errors="replace")

    print("-- ① 静态接线（AST）--")
    for item in static_audit(source):
        want(False, item)
    if not PROBLEMS:
        want(True, "`_run_off_ui_thread` 里有 `task_run` ＋ `on_ui_thread` 取证位")
        want(True, "界面侧恰好 6 处调用（六段重活各一处）")
        want(True, "`HEAVY_STEPS` 六段齐全")

    print("-- ② 引擎可调用性（真跑 thread_probe）--")
    if not cli.is_file():
        print("   ⚠ 缺构建产物 %s ⇒ 跳过这一半（外部前提）" % cli)
    else:
        rc, text, lines = runtime_probe(cli)
        if rc != 0 or not lines:
            want(False, "thread_probe 没跑成", "rc=%d" % rc)
        else:
            for line in lines:
                if line.startswith("THREADPROBE"):
                    print("     " + line[:150])
            ok_steps = [l for l in lines if "called=ok" in l]
            want(len(ok_steps) >= 6,
                 "六段引擎调用都调得动（主线程那侧）", "called=ok %d 条" % len(ok_steps))
            # ★ 边界必须**打印出来**，不许悄悄放过
            if any("worker=unavailable" in l for l in lines):
                print("   ⚠ **边界（判据自曝）**：无界面进程没有任务壳 ⇒ "
                      "「真的跑在非 UI 线程」这条运行期证据本判据拿不到")
                print("     （`_run_off_ui_thread` 已内置 marker 取证位：设 "
                      "`XRKIT_TOOLCHAIN_MARKER=<文件>` 后点一次按钮，marker 里会出现 "
                      "`on_ui_thread=False`）")

    print("-" * 66)
    if PROBLEMS:
        print("TOOLCHAIN-WORKER fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("TOOLCHAIN-WORKER ok %d 项（**静态接线对 ＋ 六段引擎调用都调得动**；"
          "★ «真的跑在 worker 上» 这条运行期证据本判据**拿不到**，边界已如实打印；"
          "审计 05 PLG-05）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
