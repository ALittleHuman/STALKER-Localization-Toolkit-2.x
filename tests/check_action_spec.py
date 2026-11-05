# -*- coding: utf-8 -*-
"""帮助的结构化判据（DSH 2026-11-05）：spec 表是单一真源，recipes 任务视图，--actions 机器可读。

判据（当前覆盖先做追更链路；未列出的动作只记 TODO，不红）：
  ① `--actions` 输出可解析 JSON，每个 spec 的 args/reads/writes/exit/tech/caps/example 都非空；
  ② 每个 spec 的名字出现在 `--help` 里，且 `--help <动作>` 能输出该动作的参数行（rc=0）；
  ③ recipes 必须提到追更链路的两个核心动作（--snapshot / --track）；
  ④ spec 里的动作必须在 `main.cpp` 的派发里真实存在（防止"文档写了没实现"）；
  ⑤ 记 TODO：派发里有、spec 里还没有的动作数量（只报数，不判红 —— 逐步扩）。
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
MAIN = ROOT / "tools" / "cli" / "main.cpp"
PROBLEMS = []
FIELDS = ("args", "reads", "writes", "exit", "tech", "caps", "example")


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== 帮助结构化：spec 表 / recipes / --actions ==")
    if not CLI.is_file():
        print("ACTION-SPEC skip 缺 CLI")
        return 2
    rc, out = run("--actions")
    try:
        data = json.loads(out.strip().splitlines()[0])
        specs = data["actions"]
    except Exception as exc:
        fail("--actions 不是合法 JSON：%s" % exc)
        specs = []
    (ok if specs else fail)("--actions 有 %d 个动作 spec" % len(specs))

    for s in specs:
        missing = [f for f in FIELDS if not s.get(f)]
        (ok if not missing else fail)("%s 的字段齐全（缺 %s）" % (s.get("name"), missing or "无"))

    help_out = run("--help")[1]
    for s in specs:
        name = s["name"]
        (ok if name in help_out else fail)("--help 里出现 %s" % name)
        rc1, det = run("--help", name)
        (ok if rc1 == 0 and name in det and s["args"] in det else fail)(
            "--help %s 输出参数签名" % name)

    for need in ("--snapshot", "--track"):
        (ok if need in help_out else fail)("recipes 覆盖 %s" % need)

    main_src = MAIN.read_text(encoding="utf-8", errors="replace")
    for s in specs:
        # 派发形态： action == "--x"
        pat = re.compile(r'action\s*==\s*"' + re.escape(s["name"]) + r'"')
        (ok if pat.search(main_src) else fail)("%s 在派发里存在" % s["name"])

    dispatched = set(re.findall(r'action\s*==\s*"(--[a-z0-9-]+)"', main_src))
    known = {s["name"] for s in specs}
    todo = sorted(dispatched - known)
    print("  · TODO：还有 %d 个动作没有结构元数据（%s）"
          % (len(todo), ", ".join(todo[:8]) + ("…" if len(todo) > 8 else "")))

    if PROBLEMS:
        print("ACTION-SPEC: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("ACTION-SPEC: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
