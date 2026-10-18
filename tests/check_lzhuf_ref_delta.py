# -*- coding: utf-8 -*-
r"""判据：`lzhuf_ref.c` 与参照源 `docs/reference/lzhuf_dll.c` 的**分叉必须是已知的那三条**。

★ 为什么要有这条（复审计 09 分册 ⑤）：
  文件头原写「逐字复制自 `docs/reference/lzhuf_dll.c`，**仅**把 EXPORT 置空」——
  而实际有三条改动（EXPORT、P17-59 端序/对齐、P17-115 共享宏）⇒ **自述与代码脱同源**，
  且**没有任何判据比对两份文件** ⇒ 将来谁再悄悄改一处，没人会发现。

★ 口径：比的是**代码**（去注释、压空白），不是注释 —— 本仓的历史就是"注释说 A、代码是 B"。
★ 判红形状：差异集合 ≠ 冻结的白名单 ⇒ 红，并**把实际差异打出来**（可诊断，不是干巴巴一句不符）。
★ 收敛条件：若哪天真的要与参照源**重新同步**（连这三条也回退），就把白名单清零 ——
  那时"空差异"本身就是正确的期望值。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REF = ROOT / "docs" / "reference" / "lzhuf_dll.c"
IMPL = ROOT / "src" / "engine" / "fs" / "lzhuf_ref.c"

# ── 冻结的差异白名单（2026-10-16 取证：8 行删 / 12 行增，逐行逐字）──────────
#  来源：① EXPORT 置空（原设计）② P17-59（01 F-19）端序/对齐 ③ P17-115（N-01）共享宏
ONLY_IN_REF = [
    "#ifdef _WIN32",
    "#define EXPORT __declspec(dllexport)",
    "#else",
    "#endif",
    "return (int)(*(unsigned*)src);",
    "textsize = (int)(*(unsigned*)src);",
    "if (textsize <= 0 || textsize > dest_cap || textsize > 256*1024*1024) return -1;",
    "*(unsigned*)dest = (unsigned)textsize;",
]
ONLY_IN_IMPL = [
    '#include "xrkit/engine/fs/untrusted_limits.h"',
    "static unsigned lzhuf_read_u32_le(const unsigned char *p) {",
    "return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |",
    "((unsigned)p[3] << 24);",
    "return (int)lzhuf_read_u32_le(src);",
    "textsize = (int)lzhuf_read_u32_le(src);",
    "if (textsize <= 0 || textsize > dest_cap ||",
    "(unsigned long long)textsize > (unsigned long long)XRKIT_LZHUF_MAX_DECLARED_BYTES) {",
    "dest[0] = (unsigned char)(textsize & 0xFF);",
    "dest[1] = (unsigned char)((textsize >> 8) & 0xFF);",
    "dest[2] = (unsigned char)((textsize >> 16) & 0xFF);",
    "dest[3] = (unsigned char)((textsize >> 24) & 0xFF);",
]
PROBLEMS: list = []


def want(cond: bool, label: str, detail: str = "") -> bool:
    if cond:
        print("   ✓ " + label + (("（%s）" % detail) if detail else ""))
        return True
    print("   ✗ " + label + (("（%s）" % detail) if detail else ""))
    PROBLEMS.append(label)
    return False


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def code_lines(path: Path) -> list:
    out = []
    for line in strip_comments(path.read_text(encoding="utf-8", errors="replace")).splitlines():
        s = " ".join(line.split())
        if s:
            out.append(s)
    return out


def main() -> int:
    print("== lzhuf_ref.c ↔ 参照源：分叉必须只有已知的三条（复审计 09 分册 ⑤）==")
    for p in (REF, IMPL):
        if not p.is_file():
            print("LZHUF-DELTA skip 缺文件：%s（判据没跑成，不是通过）" % p.relative_to(ROOT))
            return 2
    a = code_lines(REF)
    b = code_lines(IMPL)
    sa, sb = set(a), set(b)
    only_ref = [l for l in a if l not in sb]
    only_impl = [l for l in b if l not in sa]

    want(len(a) > 300, "参照源读到了（%d 行代码）" % len(a))
    want(len(b) > 300, "lzhuf_ref.c 读到了（%d 行代码）" % len(b))

    # 除白名单外**不许有别的差异**
    extra_ref = [l for l in only_ref if l not in ONLY_IN_REF]
    extra_impl = [l for l in only_impl if l not in ONLY_IN_IMPL]
    if extra_ref or extra_impl:
        print("   ⇒ 出现**白名单之外**的分叉：")
        for l in extra_ref[:8]:
            print("      只在参照源：" + l[:120])
        for l in extra_impl[:8]:
            print("      只在 ref：" + l[:120])
    want(not extra_ref, "没有**白名单之外**的『只在参照源』行",
         ("多出 %d 行" % len(extra_ref)) if extra_ref else "")
    want(not extra_impl, "没有**白名单之外**的『只在 ref』行",
         ("多出 %d 行" % len(extra_impl)) if extra_impl else "")

    # 白名单里每一条都必须**仍然真实存在**（否则白名单本身过期了，会掩盖新差异）
    missing = [l for l in ONLY_IN_REF if l not in only_ref]
    missing += [l for l in ONLY_IN_IMPL if l not in only_impl]
    want(not missing, "冻结的白名单**逐条仍然成立**（过期白名单会掩盖新差异）",
         ("失效 %d 条：%s" % (len(missing), missing[:2])) if missing else "")

    # 文件头自述必须与这份白名单**同源**（防"注释说 A、代码是 B"）
    head = IMPL.read_text(encoding="utf-8", errors="replace")[:2000]
    for token, why in (("P17-59", "端序/对齐那条改动"), ("P17-115", "共享宏那条改动"),
                       ("EXPORT", "EXPORT 置空那条改动")):
        want(token in head, "文件头自述里点名了 %s（%s）" % (token, why))
    want("仅把 EXPORT 置空" not in head,
         "文件头**不再**声称「仅把 EXPORT 置空」——那与三处实际分叉矛盾")

    print("-" * 66)
    if PROBLEMS:
        print("LZHUF-DELTA fail %d 项不成立" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("LZHUF-DELTA ok 分叉＝已知三条（删 %d 行 / 增 %d 行，逐行冻结；白名单有效；自述同源）"
          % (len(ONLY_IN_REF), len(ONLY_IN_IMPL)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
