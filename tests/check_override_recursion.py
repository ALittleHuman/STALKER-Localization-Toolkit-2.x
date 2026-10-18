# -*- coding: utf-8 -*-
"""判据 T-09：**"用户改了源编码"那条重跑不许自递归**（第三方只读审计 T-09）。

修前事实（审计原话 + 代码核）
----------------------------
`src/engine/text/convert.cpp` 的 `Overridden` 分支里直接
`return convert_bytes(raw_in, rel_path, …source_encoding = chosen…)` —— **没有重入保护**。
只要确认回调把 `chosen` 回成 `"auto"`（或任何会被判成"还没决定"的值），递归调用就会
**原样**再进同一段 ⇒ 无限递归 ⇒ 爆栈（进程直接死掉、没有任何错误信息）。

修法（护栏三件）
----------------
1. `g_override_rerun_depth()`：**thread_local** 的重跑深度（转换跑在工作线程，各线程互不相干）；
2. 回到这一段时深度 `!= 0` ⇒ **明确报错返回**（理由里带 `chosen` 与 `T-09`），不写盘、不递归；
3. 递归调用包在一段 RAII（`DepthScope`）里 ⇒ 深度一定还原（异常安全）。

★ 为什么是静态锁：行为锁需要一份**会被判成"待确认"**的样本来真的走进 `Overridden` 分支
（我第一版用 `"hello\\n"` 当样本，它被判成**高置信 utf-8**、根本不做确认 ⇒ 那三条自检
修前修后都绿 ⇒ 是**假锁**，已删掉）。真样本 / 或把确认门限做成可注入，是下一步。

退出码：0=绿 / 1=红 / 2=缺输入。`--cpp` 只为取证（拿 `git show <修前>:…convert.cpp` 跑红证）。
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
CPP = ROOT / "src" / "engine" / "text" / "convert.cpp"

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
    args = ap.parse_args()
    src_p = Path(args.cpp)

    print("== T-09：编码覆盖重跑不许自递归（静态锁）==")
    if not src_p.is_file():
        print("OVERRIDE-RECURSION skip 缺 %s" % src_p)
        return 2
    src = src_p.read_text(encoding="utf-8", errors="replace")

    want("int& g_override_rerun_depth()" in src, "A1 有线程局部的重跑深度 `g_override_rerun_depth()`")
    want("thread_local" in src, "A2 那个深度是 `thread_local`（工作线程之间互不影响）")

    i = src.find("g_override_rerun_depth() != 0")
    want(i > 0, "B1 回到这一段时**先查深度**（`!= 0` ⇒ 拒绝）")
    j = src.find("return convert_bytes(raw_in, rel_path", i if i > 0 else 0)
    rec = src.find("return convert_bytes(raw_in, rel_path")
    want(rec > 0, "B2 递归调用点还在（不是把功能删了了事）")
    want(i > 0 and rec > i, "B3 守卫在递归**之前**（先查深度、再重跑）")

    if i > 0:
        seg = src[i:i + 900]
        want("拒绝再重跑" in seg or "T-09" in seg,
             "C1 拒绝理由写明「拒绝再重跑」/点名 T-09（零静默，别只返回空结果）")
        want("out.error" in seg, "C2 拒绝走的是 `out.error`（调用方一定看得见）")

    d = src.find("DepthScope")
    want(d > 0, "D1 递归调用包在 RAII `DepthScope` 里（深度一定还原，异常安全）")
    want(src.count("++g_override_rerun_depth()") >= 1 and
         src.count("--g_override_rerun_depth()") >= 1,
         "D2 深度真的加减（不是只声明）")

    print("-" * 66)
    if PROBLEMS:
        print("OVERRIDE-RECURSION fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("OVERRIDE-RECURSION ok %d 项（重跑前查深度、拒绝时写明理由、递归包在 RAII 里；"
          "★ 行为锁待「会被判成待确认」的样本）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
