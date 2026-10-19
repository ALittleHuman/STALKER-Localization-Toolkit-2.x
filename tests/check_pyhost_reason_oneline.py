# -*- coding: utf-8 -*-
"""判据 C-04：`PYHOST` 报告的 `reason` 必须先**折成一行**（第三方只读审计 C-04）。

修前事实（审计原话）
--------------------
「**C-04** `python_host_report` 的 `reason` 未折行：`PyStatus.err_msg` 带换行时破坏
"一行一条"协议。」

代码核：`py_host.cpp` 里 `" reason=" + status.reason` —— 解释器错误文本（多帧 traceback）
直接把 `\n` 带进了那条机器记录。

修法
----
`py_host_one_line(text)`：`\r`/`\n`/`\t` → 空格、合并连续空白、吃行首空白；
`python_host_report()` 只对 `reason` 用它（**只洗这一个字段** —— 路径里的空格不许动）。

断言
----
A 源码里 `python_host_report()` 用的是 `py_host_one_line(status.reason)`（不是裸 `status.reason`）；
B `py_host_one_line` 的实现真的在分类空白（出现 `'\\r'`、`'\\n'`、`'\\t'` 三个字符字面量）；
C 头文件有声明，且注释写明"一行一条记录"的理由；
D 行为那半在 `--selftest`（多行输入 ⇒ 单行输出、内容不丢、连续空白压掉、报告整行无换行）。

退出码：0=绿 / 1=红 / 2=缺输入。`--cpp`/`--hdr` 只为取证。
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
CPP = ROOT / "src" / "engine" / "plugin" / "py_host.cpp"
HDR = ROOT / "include" / "xrkit" / "engine" / "plugin" / "py_host.h"

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
    ap.add_argument("--cpp", default=str(CPP))
    ap.add_argument("--hdr", default=str(HDR))
    args = ap.parse_args()
    cpp_p, hdr_p = Path(args.cpp), Path(args.hdr)

    print("== C-04：PYHOST 的 reason 必须折成一行 ==")
    for p in (cpp_p, hdr_p):
        if not p.is_file():
            print("PYHOST-ONELINE skip 缺 %s" % p)
            return 2
    cpp = cpp_p.read_text(encoding="utf-8", errors="replace")
    hdr = hdr_p.read_text(encoding="utf-8", errors="replace")

    i = cpp.find("std::string python_host_report() {")
    want(i >= 0, "A0 找到 `python_host_report()` 的实现")
    body = cpp[i:cpp.find("\n}", i) + 2] if i >= 0 else ""
    want("py_host_one_line(status.reason)" in body,
         "A1 `reason` 走 `py_host_one_line(...)`（不是裸拼接）",
         [l.strip() for l in body.splitlines() if "reason=" in l][:1])
    want('" reason=" + status.reason' not in body,
         "A2 已经没有裸的 `\" reason=\" + status.reason`")

    j = cpp.find("std::string py_host_one_line(const std::string& text) {")
    want(j >= 0, "B1 `py_host_one_line` 有实现")
    impl = cpp[j:cpp.find("\n}\n", j) + 3] if j >= 0 else ""
    for lit, label in (("'\\r'", "\\r"), ("'\\n'", "\\n"), ("'\\t'", "\\t")):
        want(lit in impl, "B2 实现里分类了 `%s`" % label)
    want("pending_space" in impl or "push_back(' ')" in impl,
         "B3 连续空白被压成一个空格（不是逐个换空白字符）")

    want("std::string py_host_one_line(const std::string& text);" in hdr,
         "C1 头文件有 `py_host_one_line` 声明（自检要能直接调它）")
    k = hdr.find("std::string py_host_one_line")
    ctx = hdr[max(0, k - 700):k] if k > 0 else ""
    want("一行一条" in ctx, "C2 声明旁边写清了理由（「一行一条记录」是机器协议）")

    print("-" * 66)
    if PROBLEMS:
        print("PYHOST-ONELINE fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PYHOST-ONELINE ok %d 项（`reason` 过 `py_host_one_line`；实现真分类空白；"
          "头文件有声明与理由；行为那半在 `--selftest`）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
