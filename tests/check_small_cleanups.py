# -*- coding: utf-8 -*-
r"""判据 C-10 / C-11 / C-12：三处**小清理**（第三方只读审计 C 组，P3/P4）。

三条都是"同一件事写了两遍 / 死东西留在现场 / 两条语句挤一行"，每条都能用**静态锁**判红：

* **C-10**（`src/engine/text/sniff.cpp`）：`decode_bytes` 里 `first` 与 `declared` 修前是
  **两份字面相同的表达式**——改一处漏一处，"判定结论 ≠ 实际使用的编码"又会被复算掩盖。
  锁：那个表达式在本函数里**只许出现一次**，`first` 必须是 `declared` 的**别名**（引用）。
* **C-11**（`src/engine/fs/extract_tree.cpp`）：`failure_note()` 里 `if (failures.empty()) return …;`
  后面**同一行**还跟着一条声明。锁：那一行不许"一眼两语句"（`;` 后面还有代码）。
* **C-12**（`src/shell/hub.cpp`）：`load_plugins` 里 `int loaded = 0;` 是**死变量**
  （`++loaded` 之后再也不读）。锁：不许再有 `int loaded` / `++loaded`。

退出码：0=绿 / 1=红 / 2=缺输入。三个 `--*-src` 只为取证（拿 `git show <修前>:…` 跑红证）。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
SNIFF = ROOT / "src" / "engine" / "text" / "sniff.cpp"
EXTRACT = ROOT / "src" / "engine" / "fs" / "extract_tree.cpp"
HUB = ROOT / "src" / "shell" / "hub.cpp"

EXPR = "had_utf8_bom ? std::string(kUtf8) : decided.encoding"

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--sniff-src", default=str(SNIFF))
    ap.add_argument("--extract-src", default=str(EXTRACT))
    ap.add_argument("--hub-src", default=str(HUB))
    args = ap.parse_args()
    paths = {"sniff": Path(args.sniff_src), "extract": Path(args.extract_src),
             "hub": Path(args.hub_src)}

    print("== C-10 / C-11 / C-12：三处小清理 ==")
    for name, p in paths.items():
        if not p.is_file():
            print("SMALL-CLEANUP skip 缺 %s（%s）" % (p, name))
            return 2
    sniff = paths["sniff"].read_text(encoding="utf-8", errors="replace")
    extract = paths["extract"].read_text(encoding="utf-8", errors="replace")
    hub = paths["hub"].read_text(encoding="utf-8", errors="replace")

    # ── C-10 ──────────────────────────────────────────────────────────
    n = sniff.count(EXPR)
    want(n == 1, "C-10.1 `decode_bytes` 的判定表达式**只出现一次**（修前是两份）",
         "实测 %d 处" % n)
    want("const std::string& first = declared;" in sniff,
         "C-10.2 `first` 是 `declared` 的**别名**（引用），不再是第二份拷贝")

    # ── C-11 ──────────────────────────────────────────────────────────
    bad = [l.strip() for l in extract.splitlines()
           if "if (failures.empty()) return std::string();" in l and l.strip().count(";") > 1]
    want(not bad, "C-11 `failure_note()` 里不再有「一条语句半」（`;` 后面还跟着代码）",
         bad[:1])
    want(extract.count("const uint64_t total = files_written + failures.size();") == 1,
         "C-11.2 那条声明还在（只是拆成了独立的一行）")

    # ── C-12 ──────────────────────────────────────────────────────────
    # ★ 注释里"引用被删掉的代码"不算残留（H-01/H-04/S-02 同族教训：文档引用代码不该锁住自己）
    code_lines = [l for l in hub.splitlines() if not l.lstrip().startswith("//")]
    hub_code = "\n".join(code_lines)
    want("int loaded = 0;" not in hub_code, "C-12.1 死变量 `int loaded = 0;` 已删（注释里引用不算）")
    want("++loaded;" not in hub_code, "C-12.2 只增不读的 `++loaded;` 也已删")
    want("plugin_errors_ = registry_.registration_errors();" in hub,
         "C-12.3 那段真正的活还在（删的是死代码，不是功能）")

    print("-" * 66)
    if PROBLEMS:
        print("SMALL-CLEANUP fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SMALL-CLEANUP ok %d 项（C-10 一份表达式／C-11 一眼一语句／C-12 死代码已清）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
