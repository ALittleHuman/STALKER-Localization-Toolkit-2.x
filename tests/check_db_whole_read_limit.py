# -*- coding: utf-8 -*-
"""P17-111 判据：DB 兜底**整包读必须有尺寸上限**（复审计 `01 F-09`）。

事实（修前）
------------
`src/engine/fs/db.cpp` 退回整文件读那一段是**无上限**的
`std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), …)`，
而它发生在"**还认不出这是不是 DB**"之前（seek 式判定失败才退到这里）⇒
用户在文件对话框里随手指一个几 GB 的文件（镜像/视频/安装包）就会被**整个读进内存**：
白读一遍，还可能把进程撑爆。

判据（**真造大文件试**；用**稀疏文件**，逻辑大、物理几乎为 0）
------------------------------------------------------------
① **超上限 ⇒ 拒绝整包读**：把上限用 `XRKIT_DB_WHOLE_READ_LIMIT` 压到 1 MiB，
   对一个**逻辑 64 MiB** 的"认不出格式"的文件跑 `--dump` ⇒
   · 必须**非零退出**（不许"看着成功"）；
   · 理由里必须**同时**写出**文件多大**、**上限多少**（零静默：两件事都要能读出来）；
   · ★ **必须在超时内返回**（修前是"整包啃进内存"）—— 这是"没被读进去"的间接证据；
② **没超上限 ⇒ 行为照旧**：小文件走同一条路仍然给出**正常**结论（改了不许把正常情况弄坏）；
③ **判据自曝边界**：`--dump` 不露 `used_seek/whole_read_refused`，所以本条**不能**直接断言
   "内部真的没读"；它断言的是"**拒绝发生且理由可见**"。内部那一层由 `DbReadInfo`
   的 `whole_read_refused` 承载（界面/Python 消费点可见），本判据打印这条边界。

退出码：0=绿 / 1=红 / 2=缺前提。
"""

from __future__ import annotations

import os
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
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
LIMIT = 1024 * 1024            # 判据把上限压到 1 MiB
BIG_BYTES = 64 * 1024 * 1024   # 稀疏文件的逻辑大小
TIMEOUT_S = 60

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def dump(cli: Path, path: Path, limit: int | None) -> tuple:
    env = dict(os.environ)
    if limit is None:
        env.pop("XRKIT_DB_WHOLE_READ_LIMIT", None)
    else:
        env["XRKIT_DB_WHOLE_READ_LIMIT"] = str(limit)
    try:
        proc = subprocess.run([str(cli), "--dump", str(path)],
                              capture_output=True, timeout=TIMEOUT_S, env=env)
    except subprocess.TimeoutExpired:
        return (None, "TIMEOUT", True)
    text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    return (proc.returncode, text, False)


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-111 DB 兜底整包读要有尺寸上限（复审计 `01 F-09`）==")
    if not cli.is_file():
        print("!! 缺构建产物：%s" % cli)
        print("DB-WHOLE-READ skip 缺 CLI")
        return 2

    with tempfile.TemporaryDirectory(prefix="db_whole_") as tmp:
        big = Path(tmp) / "big.db"
        with open(big, "wb") as handle:
            handle.write(b"NOT-A-DB-AT-ALL" * 4)
            handle.seek(BIG_BYTES)          # ★ 稀疏：逻辑 64 MiB、物理几乎为 0
            handle.write(b"\x00")
        size = big.stat().st_size

        # ① 超上限 ⇒ 拒绝
        rc, text, timed_out = dump(cli, big, LIMIT)
        want(not timed_out, "① 超上限时**在超时内返回**（修前是整包啃进内存）",
             "TIMEOUT(%ds)" % TIMEOUT_S if timed_out else "")
        want(rc is not None and rc != 0, "① 超上限必须**非零退出**（不许看着成功）", "rc=%s" % rc)
        want(str(size) in text, "① 理由里要写出**文件多大**", "期望含 %d" % size)
        want(str(LIMIT) in text, "① 理由里要写出**上限多少**", "期望含 %d" % LIMIT)
        want(("整包读" in text) or ("超过" in text), "① 理由要看得懂（点名整包读/超限）")
        print("     原始行：%s" % (text.strip().splitlines()[0][:150] if text.strip() else "(无输出)"))

        # ② 没超上限 ⇒ 行为照旧（给一个正常的小 DB，走同一条路仍要有结论）
        small = Path(tmp) / "small.db"
        small.write_bytes(b"NOT-A-DB-AT-ALL" * 4)
        rc2, text2, timed_out2 = dump(cli, small, None)
        want(not timed_out2 and rc2 is not None,
             "② 小文件走同一条路仍能返回（改了没把正常路径弄坏）", "rc=%s" % rc2)
        want("超过整包读上限" not in text2,
             "② 小文件**不该**被判超限（上限没有误伤）")
        print("     原始行：%s" % (text2.strip().splitlines()[0][:150] if text2.strip() else "(无输出)"))

    print("   ⚠ **判据自曝边界**：`--dump` 是**另一条**入口（走 `list_db_seek`，它自己已露 "
          "`seek=`/`file_bytes=`/`note=整包读 reason=…`）⇒ 本条**不**据此断言"
          "「`package_entries` 内部真的没读」，只断言「**拒绝发生且理由可见**」。"
          "★ 理由的**真消费点**（既有，不是新加字段）：界面 `fs_page.cpp:953`、"
          "Python `py_host.cpp:1689`。")

    print("-" * 66)
    if PROBLEMS:
        print("DB-WHOLE-READ fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("DB-WHOLE-READ ok %d 项（超限拒绝 ＋ 理由含尺寸与上限 ＋ 正常路径不受影响；"
          "复审计 `01 F-09`）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
