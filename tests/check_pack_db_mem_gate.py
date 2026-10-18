# -*- coding: utf-8 -*-
"""判据 T-05：**DB 封包不许无上限地吃内存**（第三方只读审计 T-05）。

修前事实（审计原话 + 我的逐行核）
--------------------------------
`run_pack_job()` 把每个文件读进 `DbPackItem.data`，`pack_db()` 再拼一份连续的 `data`，
最后 `write_file_atomic` 又拷一份 `vector` ⇒ **峰值约 2–3 倍总大小，且没有任何内存闸**。
只有"头字段 4 GiB"的检查。1.9 GB 级包会直接 OOM。**SquashFS 分支是流式的**（逐块读逐块写），
两条路不对称。

修法（两条闸，共享同一条上限）
----------------------------
* 上限：`untrusted_limits.h` 的 `XRKIT_DB_MAX_PACK_BYTES`（2 GiB，与既有上限同族）；
  `db_pack_max_bytes()` 返回生效值，环境变量 `XRKIT_DB_PACK_LIMIT` **只能往小改**
  （与 `01 F-09` 的整包读闸门同一套解析规则）。
* ① **调用方在读文件之前**量总量（CLI / 官方 fs 页）⇒ 超限时**一个字节都不读**；
* ② **引擎 `pack_db` 再量一次**（公开 API 的兜底；那一条由 `--smoke` 的 T-05 三条锁覆盖，
  见 `src/shell/smoke.cpp`）。

断言
----
A 超上限（env 压到 1 MiB、树 2 MiB）⇒ `--pack-db` **rc=1**、理由同时写出**待封字节数**与**上限**
  （零静默：两个数都要能读出来），且**不产出任何文件**（目标与 `.part` 都不许留）；
B **没被读进去**：给一个**逻辑 4 GiB 的稀疏文件**（物理≈0）＋ 1 MiB 上限 ⇒ 必须在**短超时内**
  返回拒绝（修前会把 4 GiB 啃进内存）；这条是"读之前就挡住了"的间接证据；
C **没误伤**：上限 8 MiB 时同一棵树必须正常出包，且 `--dump` 能回读条目；
D **只能往小改**（静态）：`db_pack_max_bytes()` 的解析必须带 `parsed <= XRKIT_DB_MAX_PACK_BYTES`
  这一条上界（写坏/想放宽都保持默认），且 `pack_db` 里那道闸必须存在。

退出码：0=绿 / 1=红 / 2=缺前提（CLI 产物或源码）。
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
DB_CPP = ROOT / "src" / "engine" / "fs" / "db.cpp"
LIMITS_H = ROOT / "include" / "xrkit" / "engine" / "fs" / "untrusted_limits.h"

TINY = 1024 * 1024             # 1 MiB
ROOMY = 8 * 1024 * 1024        # 8 MiB
TREE_BYTES = 2 * 1024 * 1024   # 树的真实字节数 2 MiB
HUGE_LOGICAL = 4 * 1024 ** 3   # 稀疏文件的逻辑大小 4 GiB
FAST_S = 45                    # "没被读进去"的间接证据：必须比读 4 GiB 快得多

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


def run_cli(args: list, limit: int | None, timeout: int = 300) -> tuple:
    env = dict(os.environ)
    if limit is None:
        env.pop("XRKIT_DB_PACK_LIMIT", None)
    else:
        env["XRKIT_DB_PACK_LIMIT"] = str(limit)
    t0 = time.time()
    try:
        proc = subprocess.run([str(CLI)] + args, capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None, "TIMEOUT", time.time() - t0
    text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    return proc.returncode, text, time.time() - t0


def main() -> int:
    print("== T-05 DB 封包必须有内存闸（超限拒绝 ＋ 读之前就挡 ＋ 不误伤）==")
    if not CLI.is_file():
        print("PACK-MEM-GATE skip 缺 CLI 构建产物")
        return 2
    if not DB_CPP.is_file() or not LIMITS_H.is_file():
        print("PACK-MEM-GATE skip 缺源码（db.cpp / untrusted_limits.h）")
        return 2

    with tempfile.TemporaryDirectory(prefix="dbpackmem_") as tmp:
        base = Path(tmp)
        tree = base / "src"
        tree.mkdir()
        # 2 MiB 真数据（3 个文件，避开整 KiB 边界对齐的巧合）
        chunk = b"\xA5" * (TREE_BYTES // 3)
        for i, name in enumerate(("a.bin", "b.bin", "c.bin")):
            (tree / name).write_bytes(chunk + bytes([i]))

        # ── A：超上限 ⇒ 明确拒绝、不产出任何文件 ──────────────────────
        out = base / "packed.out.db0"
        tree_total = sum(p.stat().st_size for p in tree.iterdir())
        rc, text, _ = run_cli(["--pack-db", str(out), "xdb", str(tree)], TINY)
        want(rc == 1, "A1 超上限时 rc=1（失败，不是 3「什么都没做」）", "rc=%s" % rc)
        want("超过 DB 封包上限" in text, "A2 理由点名「超过 DB 封包上限」",
             text.strip().splitlines()[0][:110] if text.strip() else "(无输出)")
        want(str(tree_total) in text, "A3 理由写出**待封入的字节数**", "期望含 %d" % tree_total)
        want(str(TINY) in text, "A4 理由写出**上限多少**", "期望含 %d" % TINY)
        want(not out.exists(), "A5 不产出半成品（目标文件不存在）")
        want(not list(base.glob("*.part")), "A6 不留 `.part`")

        # ── B：读之前就挡住（4 GiB 稀疏文件 ⇒ 必须很快返回拒绝）─────────
        huge_dir = base / "huge"
        huge_dir.mkdir()
        huge = huge_dir / "huge.bin"
        with open(huge, "wb") as fh:
            fh.write(b"X")
            fh.seek(HUGE_LOGICAL)      # ★ 稀疏：逻辑 4 GiB、物理几乎为 0
            fh.write(b"\x00")
        huge_out = base / "huge.out.db0"
        rc2, text2, dt2 = run_cli(["--pack-db", str(huge_out), "xdb", str(huge_dir)], TINY,
                                  timeout=FAST_S)
        # ★ 只判 `rc==1` 不够：修前 4 GiB 会**分配失败/崩掉**，rc 也 ≠0 —— 那是"崩了"不是"拒了"。
        #   所以这条必须同时要求**理由行**（说明是闸门拒绝，不是崩溃）。
        want(rc2 == 1 and "超过 DB 封包上限" in text2,
             "B1 4 GiB 稀疏文件 ⇒ 被**闸门明确拒绝**（rc=1 且有理由行，不是崩溃）",
             "rc=%s 输出=%s" % (rc2, text2.strip().splitlines()[0][:80] if text2.strip() else "(无输出)"))
        want(dt2 < FAST_S, "B2 **在 %.0fs 内返回**（修前会把 4 GiB 啃进内存）" % FAST_S,
             "实测 %.1fs" % dt2)
        want(not huge_out.exists(), "B3 不产出半成品")
        print("     原始行：%s" % (text2.strip().splitlines()[0][:120] if text2.strip() else "(无输出)"))

        # ── C：没误伤（上限给足 ⇒ 正常出包 ＋ 能回读）─────────────────
        ok_out = base / "ok.out.db0"
        rc3, text3, _ = run_cli(["--pack-db", str(ok_out), "xdb", str(tree)], ROOMY)
        want(rc3 == 0 and ok_out.is_file(), "C1 上限 8 MiB 时同一棵树正常出包",
             "rc=%s" % rc3)
        rc4, text4, _ = run_cli(["--dump", str(ok_out), "xdb"], None)
        names = set()
        for line in text4.splitlines():
            f = line.split("\t")
            if len(f) >= 2:
                names.add(f[1].strip())
        want(rc4 == 0 and {"a.bin", "b.bin", "c.bin"} <= names,
             "C2 产物能回读三个条目（改了没把正常路径弄坏）", "回读：" + ", ".join(sorted(names))[:90])

    # ── D：只能往小改（静态）────────────────────────────────────────
    db = DB_CPP.read_text(encoding="utf-8", errors="replace")
    hdr = LIMITS_H.read_text(encoding="utf-8", errors="replace")
    want("XRKIT_DB_MAX_PACK_BYTES" in hdr, "D1 上限定义在 `untrusted_limits.h`（与既有上限同族）")
    want("parsed <= XRKIT_DB_MAX_PACK_BYTES" in db,
         "D2 `db_pack_max_bytes()` 的解析带**上界**（环境变量只能往小改，不许关闸门）")
    want("db_pack_max_bytes()" in db and "超过 DB 封包上限" in db,
         "D3 `pack_db` 里那道引擎级兜底闸在（公开 API 不许绕过）")

    print("-" * 66)
    if PROBLEMS:
        print("PACK-MEM-GATE fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PACK-MEM-GATE ok %d 项（超限拒绝、读之前就挡、正常路径不受影响；引擎兜底在 --smoke 的 T-05 四条）"
          % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
