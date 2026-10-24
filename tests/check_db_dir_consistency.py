# -*- coding: utf-8 -*-
"""**DB 目录条目自洽**判据（batch-2 / D2+E3）。

口径（三种格式的"目录"定义不同，别用一把尺硬套）
-------------------------------------------------
| 格式 | 目录 ⇔ | 畸形（必须拒） |
|---|---|---|
| `xdb` / `2947ru` / `2947ww` | `offset==0 && size_real==0 && size_comp==0` | 文件条目 `offset==0` |
| `2945` / `2215`（null 系） | `fields[dir_i]==0`（就是 offset 字段）**且** size 全零 | 目录标志为 0 但 size≠0 |
| `11xx` | `offset==0` **且** `size==0` | `offset==0` 但 `size>0` |

修前事实（台账 02 的 D2/E3）：`entries_fit_size` 对目录条目**直接 `continue`**，
于是 `is_dir` 一旦判错，一个巨大的目录条目就能绕过全部算术检查；而 `unpack_db`
（"显式选定格式 + 整包读"那条路）**完全不做自洽判定** ⇒ 用户在下拉里选个格式就能绕过。

判据
----
A 正向（**必须能过**，防一刀切）：
  A1 `xdb`：目录 + 文件 ⇒ `--dump` 成功、条目数 2；
  A2 `11xx`：目录 + 文件 ⇒ 成功。
B 反向（**必须被拒**）：
  B1 `11xx`：目录条目 `offset==0` 但 `size>0` ⇒ 拒绝、理由点名"畸形"；
  B2 `2215`：目录标志 0 但 `size_comp>0` ⇒ 拒绝；
  B3 `xdb`：文件条目 `offset==0` ⇒ 拒绝。
C 自证：构造器本身可信 —— 正向镜像的字节里确实含有我们写进去的条目名。

夹具是**手工按格式逐字节拼的**（`--dump <文件> <格式>` 走的就是 parser + `unpack_db` 两条路），
不依赖任何外部样本。

退出码：0=绿 / 1=红 / 2=缺 CLI 产物。
"""
from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

problems: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            for line in detail.splitlines()[-6:]:
                print("      " + line)
        problems.append(label)


# ── 手工拼 DB 镜像（DATA 块 + 未压缩 HEADER 块）────────────────────────
def entry_xdb(name: str, size_real: int, size_comp: int, offset: int) -> bytes:
    nb = name.encode("cp1251")
    out = struct.pack("<H", len(nb) + 16)
    out += struct.pack("<III", size_real, size_comp, 0)
    out += nb
    out += struct.pack("<I", offset)
    return out


def entry_null(name: str, fields: list[int]) -> bytes:
    out = name.encode("cp1251") + b"\x00"
    for v in fields:
        out += struct.pack("<I", v)
    return out


def entry_11xx(name: str, uncomp: int, offset: int, size: int) -> bytes:
    return entry_null(name, [uncomp, offset, size])


def make_image(entries: bytes) -> bytes:
    data_payload = b"\x00" * 64
    img = struct.pack("<II", 0, len(data_payload)) + data_payload
    img += struct.pack("<II", 1, len(entries)) + entries
    return img


def dump(path: Path, fmt: str) -> tuple:
    proc = subprocess.run([str(CLI), "--dump", str(path), fmt], cwd=str(ROOT),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    print("== DB 目录条目自洽（D2+E3：三格式目录语义 + 畸形必须拒）==")
    if not CLI.is_file():
        print("DB-DIR skip 缺 %s" % CLI.relative_to(ROOT).as_posix())
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="dbdir_", dir=str(ROOT / "build")))
    try:
        # ── A1：xdb 正向 ────────────────────────────────────────────────
        good_xdb = entry_xdb("dir", 0, 0, 0) + entry_xdb("a.txt", 4, 4, 80)
        img = tmp / "ok_xdb.db"
        img.write_bytes(make_image(good_xdb))
        check(b"a.txt" in img.read_bytes(), "自证：正向镜像里确实含条目名 `a.txt`")
        rc, out = dump(img, "xdb")
        check(rc == 0 and "a.txt" in out, "A1 xdb：目录+文件能正常 dump（防一刀切）", out)

        # ── A2：11xx 正向 ──────────────────────────────────────────────
        good_11 = entry_11xx("dir", 0, 0, 0) + entry_11xx("a.txt", 1, 80, 4)
        img2 = tmp / "ok_11xx.db"
        img2.write_bytes(make_image(good_11))
        rc, out = dump(img2, "11xx")
        check(rc == 0 and "a.txt" in out, "A2 11xx：目录+文件能正常 dump", out)

        # ── B1：11xx 目录 offset==0 但 size>0 ⇒ 畸形 ───────────────────
        bad_11 = entry_11xx("dir", 1, 0, 999999) + entry_11xx("a.txt", 1, 80, 4)
        img3 = tmp / "bad_11xx.db"
        img3.write_bytes(make_image(bad_11))
        rc, out = dump(img3, "11xx")
        check(rc != 0 and ("畸形" in out or "自相矛盾" in out),
              "B1 11xx：目录带 size>0 被拒、理由点名「畸形/自相矛盾」", out)

        # ── B2：2215 目录（dir 标志=offset=0）但 size_comp>0 ⇒ 畸形 ────
        bad_2215 = entry_null("dir", [0, 0, 100]) + entry_null("a.txt", [80, 4, 4])
        img4 = tmp / "bad_2215.db"
        img4.write_bytes(make_image(bad_2215))
        rc, out = dump(img4, "2215")
        check(rc != 0 and ("畸形" in out or "自相矛盾" in out),
              "B2 2215：目录标志为 0 但 size≠0 被拒", out)

        # ── B3：xdb 文件条目 offset==0 ⇒ 畸形 ──────────────────────────
        bad_xdb = entry_xdb("dir", 0, 0, 0) + entry_xdb("a.txt", 4, 4, 0)
        img5 = tmp / "bad_xdb.db"
        img5.write_bytes(make_image(bad_xdb))
        rc, out = dump(img5, "xdb")
        check(rc != 0 and ("畸形" in out or "自相矛盾" in out or "offset" in out),
              "B3 xdb：文件条目 offset==0 被拒", out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("DB-DIR fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("DB-DIR ok 三格式目录语义一致：畸形目录/offset==0 文件条目都必须被拒，合法镜像不受影响")
    return 0


if __name__ == "__main__":
    sys.exit(main())
