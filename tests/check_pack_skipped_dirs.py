# -*- coding: utf-8 -*-
"""判据 B-1：封包的**空目录**不许静默消失（第三方只读审计 B-1，P3）。

事实（审计原话 + 我的核对）
--------------------------
`PackFileItem` 只有 `rel/source_path`（全部 `is_dir=false`）⇒ 收集侧只收文件，**空目录无法进包**；
DB 包内的目录信息只能靠路径推导。对 SquashFS 影响小（有文件的目录可由路径推导），
对 DB 是**信息丢失**（`pack_db` 的写侧其实支持目录条目，问题在**收集侧**）。

本批口径（**不做产品变更**，只做"如实记账"）
------------------------------------------
★★ 本批判据第一次跑就**更正了审计的一处前提**：`--pack-db` 这条路**不缺目录条目** ——
CLI 的收集带着 `is_dir`（`pack_db` 的写侧有专门的目录分支），空目录**真的进包、也解得出**。
真正会丢的是：① `--pack-sqfs` 不带 `--pack-dirs`；② **官方 fs 页**那条收集
（`PackFileItem` 只有文件）。
所以本批做的是：
  · `PackCollectResult::skipped_empty_dirs`（引擎收集侧算：**子树里一个文件都没有**的目录）；
  · CLI `--pack-sqfs` 打 `PACK skipped_dirs=N`；`--pack-db` 如实报 `0`（那条不丢）；
  · 官方 fs 页把同一笔账打进日志（GUI 侧那条收集确实会丢）。

断言
----
A `--pack-db` ⇒ `PACKDB skipped_dirs=0`，且解包后 `empty/` **真的在**（证明这条路不丢）；
B `--pack-db` 对"没有空目录"的树 ⇒ 也是 `0`（防恒为 N 的假账）；
C `--pack-sqfs`（不带 `--pack-dirs`）⇒ `PACK skipped_dirs=2`；
D `--pack-sqfs --pack-dirs`（目录真的进包）⇒ `PACK skipped_dirs=0`（别把"进包"也算成丢）；
E 产物仍然可用：DB 包能解出 `a.txt` / `sub/b.txt`；
F 静态锁：`overlay.h` 有 `skipped_empty_dirs`、`overlay.cpp` 真算它、`fs_page.cpp` 真把它打出来。

退出码：0=绿 / 1=红 / 2=缺前提（CLI 产物或源码）。
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
OVERLAY_H = ROOT / "include" / "xrkit" / "engine" / "fs" / "overlay.h"
OVERLAY_CPP = ROOT / "src" / "engine" / "fs" / "overlay.cpp"
FS_PAGE = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"

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


def cli(args: list, timeout: int = 600) -> tuple:
    proc = subprocess.run([str(CLI)] + args, capture_output=True, timeout=timeout)
    text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    return proc.returncode, text


def make_tree(base: Path, with_empty: bool) -> Path:
    tree = base / ("tree_empty" if with_empty else "tree_plain")
    (tree / "sub" / "deeper").mkdir(parents=True)
    (tree / "a.txt").write_bytes(b"alpha\n")
    (tree / "sub" / "b.txt").write_bytes(b"beta\n")
    if with_empty:
        (tree / "empty").mkdir()
    else:
        (tree / "sub" / "deeper").rmdir()
    return tree


def main() -> int:
    print("== B-1：封包的空目录不许静默消失（口径：如实记账，不改变包内语义）==")
    if not CLI.is_file():
        print("PACK-SKIPPED-DIRS skip 缺 CLI 构建产物")
        return 2
    for p in (OVERLAY_H, OVERLAY_CPP, FS_PAGE):
        if not p.is_file():
            print("PACK-SKIPPED-DIRS skip 缺源码 %s" % p)
            return 2

    with tempfile.TemporaryDirectory(prefix="packdirs_") as tmp:
        base = Path(tmp)
        with_empty = make_tree(base, True)
        plain = make_tree(base, False)

        rc, text = cli(["--pack-db", str(base / "e.db0"), "xdb", str(with_empty)])
        want(rc == 0 and "PACKDB skipped_dirs=0" in text,
             "A1 `--pack-db` 如实报 `skipped_dirs=0`（这条路一直带目录条目，不丢）",
             [l for l in text.splitlines() if "skipped_dirs" in l][:1])
        rc, text = cli(["--pack-db", str(base / "p.db0"), "xdb", str(plain)])
        want(rc == 0 and "PACKDB skipped_dirs=0" in text,
             "B 没有空目录的树 ⇒ 也是 `skipped_dirs=0`（防恒为 N 的假账）",
             [l for l in text.splitlines() if "skipped_dirs" in l][:1])

        rc, text = cli(["--pack-sqfs", str(base / "e.sqfs"), str(with_empty)])
        want(rc == 0 and "PACK skipped_dirs=2" in text,
             "C `--pack-sqfs`（不带 --pack-dirs）⇒ `PACK skipped_dirs=2`（**这条才真的丢**）",
             [l for l in text.splitlines() if "skipped_dirs" in l][:1])
        rc, text = cli(["--pack-sqfs", str(base / "d.sqfs"), str(with_empty), "--pack-dirs"])
        want(rc == 0 and "PACK skipped_dirs=0" in text,
             "D `--pack-sqfs --pack-dirs`（目录真的进包）⇒ 损失为 0",
             [l for l in text.splitlines() if "skipped_dirs" in l][:1])

        # E：DB 包**真的带着空目录**（本批判据第一次跑就推翻了审计的那条前提）
        out_dir = base / "out"
        rc, text = cli(["--unpack-package", str(base / "e.db0"), str(out_dir)])
        files = [p for p in out_dir.rglob("*") if p.is_file()] if out_dir.is_dir() else []
        names = {p.name for p in files}
        want(rc == 0 and "b.txt" in names and "a.txt" in names,
             "E1 产物仍可用：DB 包能解出 a.txt / sub/b.txt", "rc=%s 解出 %s" % (rc, sorted(names)))
        empty_left = any(p.is_dir() and p.name == "empty" for p in out_dir.rglob("*")) \
            if out_dir.is_dir() else False
        want(empty_left, "A2（决定性）解包后 `empty/` **真的在** —— DB 这条路不丢空目录")

    h = OVERLAY_H.read_text(encoding="utf-8", errors="replace")
    cpp = OVERLAY_CPP.read_text(encoding="utf-8", errors="replace")
    page = FS_PAGE.read_text(encoding="utf-8", errors="replace")
    want("skipped_empty_dirs" in h, "F1 `PackCollectResult` 有 `skipped_empty_dirs`")
    want("with_files" in cpp and "skipped_empty_dirs" in cpp,
         "F2 `collect_pack_files` 真的在算它（不是只声明）")
    want("skipped_empty_dirs" in page, "F3 官方页把同一笔账打进日志（GUI 侧也不静默）")

    print("-" * 66)
    if PROBLEMS:
        print("PACK-SKIPPED-DIRS fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PACK-SKIPPED-DIRS ok %d 项（空目录的损失**如实报数**；带 --pack-dirs 时不算损失）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
