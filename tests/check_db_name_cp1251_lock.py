#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""DB 写侧 **cp1251 名编码**的锁（2026-10-06 用官方 cv 查实的真缺陷）。

## 缺陷
`db.cpp::utf8_to_cp1251` 对 **cp1251 不可表示**的码点 `return false`，而
`append_entry_null` / `append_entry_xdb` **忽略了返回值**、把"失败前已追加的部分"当名字写下去。
于是名字被**静默截断**，甚至截断成**空名**：

| 源路径（UTF-8） | 修前写进包里的名字 |
|---|---|
| `sounds/音效`（目录） | `sounds/`（=和已有的 `sounds` 目录**重名**） |
| `sounds/音效/虫鸣.wav` | `sounds/`（一个**空壳文件条目**，与目录撞名） |
| `中文/说明.txt` | ``（**空名**） |

实测后果（`converter.exe -unpack -2215`）：**栈溢出** `exit -1073741571` = `0xC00000FD`，
产物 0 文件。只含 ASCII 名 / 只含 cp1251 西里尔名的包**都不会**触发（前者逐字节一致；
后者 exit 0，只是 cv 自己按本机 ACP 换名 —— 那是 `DB双向验证套件` README §四.2 记的 cv 侧限制）。

## 修法
编码器改成**永不截断**：不可表示 ⇒ 逐码点写 `?`（Windows/XR 惯例），非 UTF-8 字节
也写 `?` 并前进 1 字节；返回值仍表示"是否有替换发生"。名字长度与源一一对应 ⇒ 不再有空名/撞名。

## 三道锁（都要能判红）
1. ★ **cv 判据锁 `[①]`**：夹具含 **CJK（cp1251 不可表示）名**，
   `--pack-db <out> 2215 <src>` → `converter.exe -unpack -2215 <pack> -dir <out>`
   **退出码必须 == 0**。修前实测 `-1073741571`（`0xC00000FD`）、产物 0 文件。
   cv 不在本机时这一条**记 note 跳过、不判红**（`!!` 行）。
2. ★ **不依赖 cv 的字节级锁 `[②]`**：自己解产物 HEADER 明文（LZHUF，复用
   `check_db_11xx_dirlock.py` 的解码器 —— 同一份已验证实现，不重复造），断言
   **没有空名条目**、**没有重名条目**、CJK 源文件 ⇒ 两个**互不相同且非空**的名字、
   cp1251 西里尔名**逐字节原样**；并做**锁的自证**：合成一份"空名 + 重名"的头必须判红。
3. **反向锁 `[③]`**：全 ASCII 名的树，包内名字必须与源路径**逐字节相同**（不许被新编码器
   动一下），且 cp1251 名在 HEADER 明文里是 cp1251 字节（不是 UTF-8）。

输出（供门禁抓取）：`DB-NAME1251-LOCK ok …` / `DB-NAME1251-LOCK fail …` / `!! …`；
退出码：0=全绿（cv 缺失时只记 note，仍算绿），1=有断言不成立，2=CLI 缺失（外部前提）。
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

CV = Path(r"E:\Software\Tools\Transition\DB双向验证套件\tools\converter.exe")
CV_TIMEOUT = 60

FMT = "2215"
NF = 3  # 2215 = null 系 3 字段

# 夹具：ASCII + cp1251 西里尔（可表示）+ **CJK（不可表示）** + 空目录 + 0 字节文件。
# 两个 CJK 文件放在**同一个 CJK 目录**下：修前它们都被截成空名 ⇒ 专门撞名。
CYR_DIR = "\u043f\u0430\u043f\u043a\u0430"                     # папка
CYR_FILE = "\u0444\u0430\u0439\u043b.txt"                      # файл.txt
CJK_DIR = "\u4e2d\u6587"                                        # 中文
CJK_A = "\u8bf4\u660e.txt"                                      # 说明.txt
CJK_B = "\u866b\u9e23.bin"                                      # 虫鸣.bin
FIXTURE = {
    "a/b/mid.bin": bytes(range(64)),
    "ascii_root.txt": b"root\n",
    "empty.dat": b"",
    CYR_DIR + "/" + CYR_FILE: "\u043f\u0440\u0438\u0432\u0435\u0442\n".encode("cp1251"),
    CJK_DIR + "/" + CJK_A: b"cjk-a\n",
    CJK_DIR + "/" + CJK_B: b"cjk-b\n",
}
EMPTY_DIRS = ["topdir", "a/b/emptydir"]

PROBLEMS: list[str] = []


def _load_sibling():
    r"""复用 `check_db_11xx_dirlock.py` 里那份**已验证**的 LZHUF 解码器 + 头部走路器。

    不复制那份 200 行表：两份实现会漂移，而"解码器错了 ⇒ 判红"的自证是靠它走完头部
    才算数的，必须是同一份。仓库内相对文件，缺了就是硬错。
    """
    path = ROOT / "tests" / "check_db_11xx_dirlock.py"
    if not path.is_file():
        raise SystemExit("!! 找不到 %s（本锁复用它已验证的 LZHUF 解码器）" % path)
    spec = importlib.util.spec_from_file_location("_lk11", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(argv, timeout=None):
    proc = subprocess.run([str(a) for a in argv], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def run_cli(argv):
    return run([CLI] + [str(a) for a in argv])


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + (" \u2014\u2014 " + detail if detail else ""))
        PROBLEMS.append(label)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def build_fixture(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    for rel, data in FIXTURE.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    for rel in EMPTY_DIRS:
        (root / rel).mkdir(parents=True, exist_ok=True)


def walk_disk(root: Path):
    files, dirs = {}, set()
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            dirs.add(os.path.relpath(os.path.join(dirpath, d), root).replace(os.sep, "/"))
        for f in filenames:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as fh:
                files[rel] = fh.read()
    return files, dirs


# ── [①] cv 判据锁 ───────────────────────────────────────────────────────
def check_cv(tmp: Path, src: Path, pack: Path) -> None:
    print("[①] cv 判据锁：`converter.exe -unpack -2215 <our pack> -dir <out>` 退出码必须 == 0")
    if not CV.is_file():
        print("  !! 缺官方 converter.exe（%s）\u21d2 这条**记 note 跳过**（公开 CI 无 cv，不判红）；" % CV)
        print("     不依赖 cv 的字节级锁 [②] 与反向锁 [③] 照跑。")
        return
    outdir = tmp / "cv_out"
    outdir.mkdir(parents=True, exist_ok=True)
    try:
        rc, text = run([CV, "-unpack", "-2215", pack, "-dir", outdir], timeout=CV_TIMEOUT)
    except subprocess.TimeoutExpired:
        want(False, "cv -unpack -2215 退出码 == 0", "超时 %ds" % CV_TIMEOUT)
        return
    raw = "cv exit=%d（unsigned 0x%08X）" % (rc, rc & 0xFFFFFFFF)
    want(rc == 0, "cv -unpack -2215 退出码 == 0", raw)
    print("    %s" % raw)
    if rc != 0:
        print("    cv 输出：%s" % text.strip()[:200])
        return
    got, got_dirs = walk_disk(outdir)
    # cv 对非 ASCII 名按本机 ACP 换名（套件 README §四.2）⇒ 只有 ASCII 路径计入逐字节对拍。
    ascii_want = {p: d for p, d in FIXTURE.items() if p.isascii()}
    ascii_got = {p: d for p, d in got.items() if p.isascii()}
    missing = sorted(set(ascii_want) - set(ascii_got))
    bad = [p for p in ascii_want if p in ascii_got and sha(ascii_got[p]) != sha(ascii_want[p])]
    want(not missing and not bad,
         "cv 解出的 ASCII 文件与源逐字节相同（%d 个；非 ASCII 名按 cv 已知限制不计）" % len(ascii_want),
         "缺 %s / 内容不同 %s" % (missing[:3], bad[:3]))
    want("topdir" in got_dirs, "空目录 topdir 也解出来了",
         "缺 topdir（有 %d 个目录）" % len(got_dirs))


# ── [②] 字节级锁（不依赖 cv）────────────────────────────────────────────
def names_of(header: bytes, lk):
    entries, consumed_all = lk.walk_null(header, NF)
    return entries, consumed_all


def judge_names(entries) -> list[str]:
    r"""判据本体：**不许有空名、不许有重名**。抽出来是为了能被"锁的自证"复用。"""
    bad = []
    seen = set()
    for name, _fields in entries:
        if name == "":
            bad.append("空名条目")
        if name in seen:
            bad.append("重名条目 %r" % name)
        seen.add(name)
    return bad


def check_header(pack: Path, lk) -> None:
    print("[②] 字节级锁：HEADER 明文里**不许有空名 / 重名条目**（不依赖 cv）")
    header = lk.decode_header(pack.read_bytes())
    want(header is not None, "能解出产物的 HEADER 明文（LZHUF）")
    if header is None:
        return
    entries, consumed_all = names_of(header, lk)
    want(consumed_all and len(entries) > 0,
         "HEADER 明文能被逐条走完且不剩尾巴（解码器自证：%d 条）" % len(entries))
    if not consumed_all or not entries:
        return
    print("    HEADER 明文条目：%s" % [n for n, _ in entries])
    bad = judge_names(entries)
    want(not bad, "没有空名/重名条目", "、".join(sorted(set(bad))[:4]))

    names = [n for n, _ in entries]
    # CJK 源 ⇒ 必须变成**两个不同且非空**的名字（修前双双截成空名 ⇒ 重名）
    cjk = [n for n in names if "?" in n]
    want(len(cjk) >= 3,
         "不可表示名按 `?` 顶替后**非空且在场**（CJK 2 文件 + 1 目录 ⇒ >=3 个含 `?` 的名字）",
         "含 `?` 的名字只有 %r" % (cjk[:4],))
    want(len(set(cjk)) == len(cjk), "含 `?` 的名字互不相同（逐码点顶替 ⇒ 一一对应）",
         "撞名：%r" % (sorted({n for n in cjk if cjk.count(n) > 1})[:4],))
    # cp1251 西里尔名必须**逐字节原样**（不是 UTF-8、不是 `?`）
    want(CYR_DIR + "\\" + CYR_FILE in names or (CYR_DIR + "/" + CYR_FILE) in names,
         "cp1251 可表示的西里尔名原样写出（%s）" % (CYR_DIR + "/" + CYR_FILE),
         "实际名字：%r" % (names[:6],))

    # ── 锁的自证：同一份判据喂"空名 + 重名"的合成头**必须判红** ──
    fake = b"\x00" + b"\x00" * 12 + b"dup\\\x00" + b"\x00" * 12 + b"dup\\\x00" + b"\x00" * 12
    fake_entries, _ = lk.walk_null(fake, NF)
    want(bool(judge_names(fake_entries)),
         "锁的自证：合成头（空名 + 重名）**必须**被判红",
         "判据放过空名/重名 ⇒ 这把锁永远为真")
    good = b"a\\b.txt\x00" + b"\x01\x00\x00\x00" + b"\x00" * 8 + b"c\\d.txt\x00" + b"\x01\x00\x00\x00" + b"\x00" * 8
    good_entries, _ = lk.walk_null(good, NF)
    want(not judge_names(good_entries), "锁的自证：合成头（正常名）必须放过")


# ── [③] 反向锁 ──────────────────────────────────────────────────────────
def check_reverse(tmp: Path, lk) -> None:
    print("[③] 反向锁：全 ASCII 名 ⇒ 包内名字与源路径逐字节相同（新编码器不许动 ASCII）")
    src = tmp / "ascii_src"
    if src.exists():
        shutil.rmtree(src)
    (src / "x" / "y").mkdir(parents=True, exist_ok=True)
    (src / "x" / "y" / "deep.bin").write_bytes(bytes(range(32)))
    (src / "plain.txt").write_bytes(b"plain\n")
    pack = tmp / "ascii_2215.db"
    rc, text = run_cli(["--pack-db", pack, FMT, src])
    want(rc == 0, "[③] --pack-db 全 ASCII 树退出码 0", text.strip()[:150])
    if rc != 0:
        return
    header = lk.decode_header(pack.read_bytes())
    want(header is not None, "[③] 能解出 HEADER 明文")
    if header is None:
        return
    entries, consumed_all = names_of(header, lk)
    want(consumed_all, "[③] HEADER 明文能走完（%d 条）" % len(entries))
    got_files = {n.replace("\\", "/") for n, _ in entries if not n.endswith("\\")}
    want(got_files == {"x/y/deep.bin", "plain.txt"},
         "[③] 全 ASCII 名逐字节原样（没有多出 `?`）", "实际：%r" % (sorted(got_files),))


def main() -> int:
    parser = argparse.ArgumentParser(description="DB 写侧 cp1251 名编码：cv 判据锁 + 字节级锁 + 反向锁")
    parser.add_argument("--cli", default="", help="覆盖 CLI 路径（默认 build/msvc-release 那份）")
    args = parser.parse_args()

    global CLI
    if args.cli:
        CLI = Path(args.cli)
    if not CLI.exists():
        print("!! 找不到 CLI：%s" % CLI)
        return 2

    lk = _load_sibling()
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_name1251_lock_"))
    try:
        src = tmp / "src"
        build_fixture(src)
        print("夹具：%d 文件（含 1 个 cp1251 西里尔名、2 个 CJK 名、1 个 0 字节）+ %d 空目录"
              % (len(FIXTURE), len(EMPTY_DIRS)))
        pack = tmp / "our_2215.db"
        rc, text = run_cli(["--pack-db", pack, FMT, src])
        if rc != 0:
            print("!! --pack-db %s 失败：%s" % (FMT, text.strip()[:200]))
            return 1
        print("--pack-db %s → %d B" % (FMT, pack.stat().st_size))

        check_cv(tmp, src, pack)
        check_header(pack, lk)
        check_reverse(tmp, lk)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("DB-NAME1251-LOCK fail %d 项不成立：" % len(PROBLEMS))
        for p in PROBLEMS:
            print("                  " + p)
        return 1
    print("DB-NAME1251-LOCK ok DB 写侧 cp1251 名编码：cv 判据（exit 0）／字节级"
          "（无空名无重名 + `?` 顶替一一对应 + 西里尔原样 + 锁自证）／反向（ASCII 名逐字节原样）全绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
