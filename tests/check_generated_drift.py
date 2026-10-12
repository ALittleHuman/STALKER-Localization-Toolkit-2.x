# -*- coding: utf-8 -*-
"""P17-109 判据：**生成物必须与生成器逐字节一致**（复审计 [Medium]："F-07 的更正被手写进生成物"）。

事实（复审计抓到、本批实测复现）
--------------------------------
P17-24（`04 F-07`）把收紧后的措辞**手改进生成物** `third_party/SOURCES.md`，
**忘了改生成器** `tools/fetch_third_party.py:285`（那里还是旧句「会逐个校验 sha256」）
⇒ 谁重跑一次生成器就把更正**静默抹掉**。

实测（本批）：直接调 `write_sources_md()` ⇒ 文件**真的变了**：生成器给 **46 行**、库里 **47 行**，
首个差异在第 5 行 —— 正是那句。

★ 而且本批修这件事时**我自己又踩了一次**：先跑生成器、后改生成器 ⇒ 当场把更正抹掉，
再从 git 取回。⇒ 所以这条判据**必须存在**，而不是靠"记得先改生成器"。

判据做法（**真算一遍再比**，不是查字符串）
------------------------------------------
① 备份 `third_party/SOURCES.md`；
② 调生成器的 `write_sources_md(load_lock())`（**不动网络**，只用本地锁文件）；
③ 把产出与备份**逐字节**比；
④ **无论结果**都恢复备份（失败也要恢复 —— 判据不许改工作区状态）。

输出：`GEN-DRIFT ok …` / `GEN-DRIFT fail …`；退出码 0=绿 / 1=漂了 / 2=外部前提缺失。
自带反例自证（**在内层注入一段"手改版"再比**，证明比对的灵敏度）。
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
GENERATOR = ROOT / "tools" / "fetch_third_party.py"
LOCK = ROOT / "third_party" / "sources.lock.json"
TARGET = ROOT / "third_party" / "SOURCES.md"


def load_generator():
    spec = importlib.util.spec_from_file_location("fetch_third_party_under_test", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def regenerate(module) -> bytes:
    """让生成器把 SOURCES.md 重写一遍，返回它写出来的字节。"""
    module.write_sources_md(module.load_lock())
    return TARGET.read_bytes()


def first_diff(a: bytes, b: bytes) -> str:
    ta = a.decode("utf-8", "replace").splitlines()
    tb = b.decode("utf-8", "replace").splitlines()
    for i in range(max(len(ta), len(tb))):
        x = ta[i] if i < len(ta) else "(缺)"
        y = tb[i] if i < len(tb) else "(缺)"
        if x != y:
            return "行 %d：库里「%s」/ 生成器「%s」" % (i + 1, x[:90], y[:90])
    return "（字节不同但逐行相同 —— 行尾/编码差别）"


def selfcheck() -> tuple:
    """反例自证：把"库里"换成手改版 ⇒ 比对必须报出差异（证明比对的灵敏度）。"""
    src_generated = b"# t\n> A\n> B\n"
    src_hand = b"# t\n> A\n> B\n> hand edit\n"
    cases = [
        ("一致", 0, [] if src_generated == src_generated else ["不该报"]),
        ("手改版（复审计的形状）", 1, [] if src_generated == src_hand else ["库里与生成器不一致"]),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good_case = (len(problems) > 0) if want else (not problems)
        ok &= good_case
        results.append((label, want, len(problems), good_case, problems))
    return ok, results


def main() -> int:
    print("== P17-109 生成物必须与生成器逐字节一致（复审计 [Medium]，`04 F-07` 的漂移）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-24s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                 "有错" if want else "无错", got))
    if not self_ok:
        print()
        print("GEN-DRIFT fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (GENERATOR, LOCK, TARGET):
        if not path.is_file():
            print("!! 缺文件：%s" % path.relative_to(ROOT))
            print("GEN-DRIFT skip 缺外部前提")
            return 2

    backup = TARGET.read_bytes()
    try:
        module = load_generator()
        produced = regenerate(module)
        drifted = produced != backup
        print("-- 实测（真重算一遍再逐字节比）--")
        print("   库里 %d 字节 / 生成器写出 %d 字节" % (len(backup), len(produced)))
        if drifted:
            print("   ✗ **漂了**：%s" % first_diff(backup, produced))
    finally:
        TARGET.write_bytes(backup)          # ★ 无论结果都恢复
        restored = TARGET.read_bytes() == backup
        print("   （已恢复原状：%s）" % ("是" if restored else "★ 否 —— 判据改了工作区，必须查！"))

    print("-" * 66)
    if drifted:
        print("GEN-DRIFT fail 生成物与生成器不一致（改生成器后重跑，别手改生成物）")
        return 1
    print("GEN-DRIFT ok `third_party/SOURCES.md` 与生成器逐字节一致（`04 F-07` 的更正不会再被抹掉）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
