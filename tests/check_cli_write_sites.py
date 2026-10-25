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
    text = SRC.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # ★★ C2 修正（2026-10-24）：**不再用"区间 > 2000 行"这种尺寸守卫** ——
    #   那是"按距离猜结构"，函数变长/变短都可能误判（同一类坑在 check_when_value_types 的
    #   `[:4000]`、check_cli_exit_codes 的 LOOKBACK 上都出现过）。改用仓里现成的
    #   `tests/_cpp_source.py::function_body()` 精确切函数体。
    sys.path.insert(0, str(ROOT / "tests"))
    from _cpp_source import function_body  # noqa: PLC0415
    body = function_body(text, "int run_cli(")
    check(bool(body) and len(body) > 200, "切得到 `run_cli()` 的函数体", "长度 %d" % len(body))

    body_lines = body.splitlines()
    hits = [(i + 1, l.strip()) for i, l in enumerate(body_lines)
            if "std::ofstream" in l and not l.lstrip().startswith("//")]
    check(not hits, "函数体内没有 `std::ofstream`（写盘一律走 `write_cli_output()`）",
          "；".join("%d: %s" % (n, s) for n, s in hits[:6]))

    helper = sum(1 for l in lines if "write_cli_output(" in l and not l.lstrip().startswith("//"))
    check(helper >= 2, "`write_cli_output(` 存在且被调用（helper 没被删/改名）", "实得 %d 处" % helper)
    check(any("enum class CliWriteStatus" in l for l in lines),
          "`CliWriteStatus` 状态枚举存在（helper 只返回状态、不打印）")

    # ★★ H6c 的静态锁（**不需要造磁盘满**）：每一次 `write_cli_output(...)` 的返回值都必须被判断。
    #   做法：把 `const CliWriteStatus X = write_cli_output(` 形式取出变量名，要求后面出现 `X ==`/`X !=`
    #   （或调用点本身就带比较）。这样"写失败静默打印 ok"这类回归会当场变红。
    import re  # noqa: PLC0415
    unchecked = []
    for idx, line in enumerate(lines):
        if "write_cli_output(" not in line or line.lstrip().startswith("//"):
            continue
        if "CliWriteStatus write_cli_output" in line:      # 声明
            continue
        m = re.search(r"const\s+CliWriteStatus\s+(\w+)\s*=\s*write_cli_output\(", line)
        if m:
            var = m.group(1)
            tail = "\n".join(lines[idx:idx + 12])
            if (var + " ==") not in tail and (var + " !=") not in tail:
                unchecked.append("%d: %s" % (idx + 1, var))
            continue
        if "==" in line or "!=" in line:                    # 内联比较
            continue
        unchecked.append("%d: %s" % (idx + 1, line.strip()[:60]))
    check(not unchecked, "每一次 `write_cli_output(...)` 的返回值都被判断（H6c 的静态锁）",
          "；".join(unchecked[:6]))

    # ★★ C2 的静态锁：`--font-render` 的删除分支必须被"本次新建"包住（覆盖场景不许删）
    fr = function_body(text, 'if (action == "--font-render")')
    if fr:
        has_created = "created_dds" in fr
        has_remove = "std::filesystem::remove(path_from_utf8(dds_path)" in fr
        guarded = has_created and has_remove and ("if (created_dds)" in fr)
        check(guarded,
              "--font-render 删 dds 的分支被 `if (created_dds)` 包住（覆盖场景一个字节都不动）",
              "created_dds=%s remove=%s guarded=%s" % (has_created, has_remove, guarded))
        check("dds 已写出（覆盖了原有文件）" in fr,
              "--font-render 覆盖场景**说清谁在盘上**（零静默）")

    if problems:
        print("CLI-WRITE fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("CLI-WRITE ok run_cli 函数体内零 ofstream；写盘唯一入口 write_cli_output()；"
          "返回值全部被判断；font-render 半成品处置有守卫")
    return 0


if __name__ == "__main__":
    sys.exit(main())
