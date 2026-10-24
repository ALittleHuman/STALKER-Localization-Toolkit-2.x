# -*- coding: utf-8 -*-
"""**CLI 写盘点**静态判据（C1 的锁）：`run_cli()` 区间内不许出现 `std::ofstream`。

为什么按函数区间切，而不按 `operands[` 匹配
------------------------------------------
11 个写盘点的路径表达式**各不相同**：`operands[2]`、`operands[out_arg]`（动态下标）、
`tgt.path`（引擎补过扩展名）、`args[1]`（`--extract-chars` 用 args 不是 operands）。
按 `ofstream <id>(operands[` 匹配会漏掉后三种。
而产品写盘点**100% 落在 `run_cli()` 里**、夹具写盘点**100% 在它之前** ⇒
"区间内 `std::ofstream` 计数 == 0"这条一刀切最干净，也不误伤 smoke 夹具。

防假绿（照 `require_repo_file` 的教训）
--------------------------------------
· 区间行数必须 **> 2000**（函数被改名/挪走 ⇒ 扫到 0 行 ⇒ 静默绿）；
· 被扫描的文件里必须能找到 `write_cli_output(`（helper 没被删/没被改名）；
· 注释行不算（`//` 开头的行里提到 `std::ofstream` 是说明文字）。

退出码：0=绿 / 1=红 / 2=缺源文件。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "cli" / "main.cpp"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

problems: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            print("      " + detail)
        problems.append(label)


def main() -> int:
    print("== CLI 写盘点静态锁：run_cli() 区间内 std::ofstream 必须为 0 ==")
    if not SRC.is_file():
        print("CLI-WRITE skip 缺 %s" % SRC.relative_to(ROOT).as_posix())
        return 2
    lines = SRC.read_text(encoding="utf-8", errors="replace").splitlines()
    starts = [i for i, l in enumerate(lines) if l.startswith("int run_cli(")]
    check(len(starts) == 1, "找得到唯一的 `int run_cli(` 起点", "找到 %d 处" % len(starts))
    if len(starts) != 1:
        return 1
    start = starts[0]
    span = len(lines) - start
    check(span > 2000, "被扫描区间 > 2000 行（防函数被改名/挪走 ⇒ 扫到 0 行 ⇒ 静默绿）",
          "实得 %d 行" % span)

    hits = [(i + 1, lines[i].strip()) for i in range(start, len(lines))
            if "std::ofstream" in lines[i] and not lines[i].lstrip().startswith("//")]
    check(not hits, "区间内没有 `std::ofstream`（写盘一律走 `write_cli_output()`）",
          "；".join("%d: %s" % (n, s) for n, s in hits[:6]))

    helper = sum(1 for l in lines if "write_cli_output(" in l and not l.lstrip().startswith("//"))
    check(helper >= 2, "`write_cli_output(` 存在且被调用（helper 没被删/改名）", "实得 %d 处" % helper)
    check(any("enum class CliWriteStatus" in l for l in lines),
          "`CliWriteStatus` 状态枚举存在（helper 只返回状态、不打印）")

    if problems:
        print("CLI-WRITE fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("CLI-WRITE ok run_cli 区间 %d 行内零 ofstream；写盘唯一入口是 write_cli_output()" % span)
    return 0


if __name__ == "__main__":
    sys.exit(main())
