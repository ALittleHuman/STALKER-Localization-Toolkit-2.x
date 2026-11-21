# -*- coding: utf-8 -*-
"""判据：**单文件 + 原地 + 备份** 必须真的备份（2026-11-21 用户实测缺陷）。

修前事实：`--convert-batch <单文件> "" <src> <dst> --backup` 报 `backed_up=0 exit=1`，
备份目录一个都没建 —— 因为备份根取的是 `source`（此时是**文件路径**），
`relative(文件, 文件)` = "." ⇒ 备份路径变成 `<文件>/backup/./x.bak`，建不出来；
且 CLI 汇总只打 `backed_up`，失败计数没上屏（零静默）。

本判据走真实产品路径（CLI），钉四条：
  ① 单文件原地 + `--backup` ⇒ rc=0、`backed_up=1`、备份文件存在且**与原文逐字节相同**；
  ② 同一文件再跑一次 ⇒ `backup_existed=1`、已有备份**不被覆盖**；
  ③ 目录 + 原地 + `--backup` ⇒ 备份落在 `<源目录>/backup/<文件名>.bak`（1.x 口径）；
  ④ 汇总行必须带 `backup_existed=` / `backup_failed=`（零静默：失败要能上屏）。

用法：python tests/check_batch_backup.py    （0=全过 / 1=红 / 2=缺构建产物）
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = Path(os.environ.get("XRKIT_CLI", ROOT / "build" / "msvc-release" / "tools" / "cli" /
                          "xrkit_cli.exe"))

FAILURES: list[str] = []
BODY = ('<?xml version="1.0" encoding="utf-8"?>'
        '<string_table><string id="k"><text>Привет</text></string></string_table>')


def check(cond: bool, label: str, detail: str = "") -> None:
    if cond:
        print("  \u2713 " + label, flush=True)
    else:
        print("  \u2717 " + label + (("  —— " + detail) if detail else ""), flush=True)
        FAILURES.append(label)


def run(args: list[str], cwd: Path) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + args, cwd=str(cwd), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=120)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def field(out: str, name: str) -> str:
    for token in out.split():
        if token.startswith(name + "="):
            return token.split("=", 1)[1]
    return ""


def main() -> int:
    if not CLI.is_file():
        print("SKIP cli-missing: %s" % CLI)
        return 2
    work = Path(tempfile.mkdtemp(prefix="batch_backup_", dir=str(ROOT / "build")))
    try:
        src = work / "a.xml"
        src.write_bytes(BODY.encode("utf-8"))

        print("== ① 单文件 + 原地 + --backup ==")
        rc, out = run(["--convert-batch", "a.xml", "", "utf-8", "cp1251", "--backup"], work)
        bak = work / "backup" / "a.xml.bak"
        check(rc == 0, "① rc=0", "rc=%d out=%s" % (rc, out.strip()[:200]))
        check(field(out, "backed_up") == "1", "① backed_up=1", out.strip()[:200])
        check(bak.is_file(), "① 备份文件落在 <源目录>/backup/a.xml.bak", str(bak))
        if bak.is_file():
            check(bak.read_bytes() == BODY.encode("utf-8"),
                  "① 备份内容与原文逐字节相同")

        print("== ② 再跑一次：已有备份不被覆盖 ==")
        before = bak.read_bytes() if bak.is_file() else b""
        rc, out = run(["--convert-batch", "a.xml", "", "utf-8", "cp1251", "--backup"], work)
        check(rc == 0, "② rc=0", out.strip()[:200])
        check(field(out, "backup_existed") == "1", "② backup_existed=1", out.strip()[:200])
        if bak.is_file():
            check(bak.read_bytes() == before, "② 已有备份没被改写")

        print("== ③ 目录 + 原地 + --backup ==")
        sub = work / "sub"
        sub.mkdir()
        (sub / "b.xml").write_bytes(BODY.encode("utf-8"))
        rc, out = run(["--convert-batch", "sub", "", "utf-8", "cp1251", "--backup"], work)
        check(rc == 0 and field(out, "backed_up") == "1", "③ 目录内存量备份成功",
              out.strip()[:200])
        check((sub / "backup" / "b.xml.bak").is_file(), "③ 落在 <源目录>/backup/")

        print("== ④ 零静默：汇总行带两个计数字段 ==")
        check("backup_existed=" in out, "④ 汇总行有 backup_existed=")
        check("backup_failed=" in out, "④ 汇总行有 backup_failed=")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if FAILURES:
        print("BATCH-BACKUP: FAIL（%d 条）" % len(FAILURES), flush=True)
        return 1
    print("BATCH-BACKUP: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
