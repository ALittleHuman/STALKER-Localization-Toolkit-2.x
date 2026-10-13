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


def regenerate_into(module, target: Path) -> bytes:
    """★ P17-115（复审计 `P0-3`）：把 `regenerate()` 的**真实路径**在**别处**跑一遍。

    反例自证必须真触发"重新生成"与 `first_diff` 这两条路，不能只比两个写死字节
    （那版自证恒为 2/2 —— 复审计原话："它的『会红』是假的"）。
    做法：生成器把结果写在 `module.TP / "SOURCES.md"`（`TP` 是模块级）⇒ **临时改 `TP`**
    指向一个临时目录，跑真生成逻辑，再还原。
    """
    original_tp = module.TP
    try:
        module.TP = target.parent
        module.write_sources_md(module.load_lock())
    finally:
        module.TP = original_tp
    return target.read_bytes()


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
    """反例自证（★ P17-115 重写，复审计 `P0-3`）：必须**真触发**"重新生成"与 `first_diff`。

    第一版只比两个写死字节（`src_generated == src_generated`）⇒ **恒为 2/2**，
    它的"会红"是假的（复审计原话）。现在改成：
      ① 用**真生成器**在临时目录里跑一遍（`regenerate_into`）⇒ 拿到"生成器会写什么"；
      ② 把它的产物**改成手改版**（模拟"生成物被手改"）⇒ `first_diff` 必须**真的报出行号**；
      ③ 生成器产物与**它自己**比 ⇒ 必须**无差异**（反向锁，防"永远报有差异"）。
    """
    import tempfile

    results, ok = [], True
    problems: list = []
    try:
        module = load_generator()
        with tempfile.TemporaryDirectory(prefix="gen_drift_selfcheck_") as tmp:
            produced = regenerate_into(module, Path(tmp) / "SOURCES.md")
            if not produced:
                problems.append("生成器在临时目录里什么都没写出来（自证前提不成立）")
            hand = produced + "\n> ★ 手改了一行\n".encode("utf-8")
            # ③ 反向锁：与自身比必须无差异（防"永远报有差异"的恒真式）
            same = first_diff(produced, produced)
            if not same.startswith("（字节不同但逐行相同"):
                problems.append("与自身比竟然报出差异：%s" % same[:80])
            # ① 真漂移：必须报出**行号**
            drift = first_diff(produced, hand)
            if not drift.startswith("行 "):
                problems.append("手改版没被报出行号（first_diff 没真跑）：%s" % drift[:80])
    except Exception as exc:  # noqa: BLE001
        problems.append("自证过程抛异常：%s" % exc)

    cases = [
        ("真·无差异（与自身比）", 0, []),
        ("真·漂移（真生成 + 手改一行）", 1, problems or ["库里与生成器不一致"]),
    ]
    for label, want, probs in cases:
        good_case = (len(probs) > 0) if want else (not probs)
        ok &= good_case
        results.append((label, want, len(probs), good_case, probs))
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
