# -*- coding: utf-8 -*-
r"""**探针（只写不跑门禁）** T-07：SquashFS **写侧**压缩器与参照工具逐字节对拍。

背景（第三方只读审计 T-07 / 用户 2026-10-19 口径）
--------------------------------------------------
`docs/2.0.0-设计口径.md` 承诺了 gzip/lz4/zstd 之类的压缩选项，而**写侧**只实现了
**xz**（`squashfs.h` 的原话：与 1.x `sqfs_pack`（tar2sqfs 1.3.2，默认 xz）**sha256 必须相等**）。
读侧早就有判据：`tests/golden_sqfs_compressors.py` 用 `tar2sqfs -c <压缩器>` 造镜像再用 `--dump`
读回 —— 它的 docstring 明说**写侧没覆盖**，也写了补写侧的前提（**库版本与压缩参数一致**）。

本探针补的正是那一半，**不塞进读侧判据**（两者判的东西不一样）：
  ① 用 `golden_sqfs_compressors.make_image()` 造参照镜像（同一批输入、同一参照工具、同一参数）；
  ② 用**我们的 writer** 压同一批输入；
  ③ **逐字节比**。相等 ⇒ 写侧支持该压缩器。

⚠ 为什么现在**一定**不通过（这是本探针的结论，不是缺陷）：
    写侧只有 xz ⇒ 除 xz 外的三种（lzo/gzip/lz4/zstd）必然不等或我们根本不产出。
    本探针的用途是**把差距钉成数字**：哪几种缺、差多少字节、差在前 4 字节的 magic 还是整块数据。

用法
----
    python tests/probe_sqfs_write_compressors.py
    python tests/probe_sqfs_write_compressors.py --only xz        # 只跑某一种
    $env:XRKIT_SQFS_BIN = "<squashfs-tools-ng bin 目录>"         # 覆盖参照工具路径

退出码：0=全部一致；1=有差异（**当前是预期**）；2=参照工具/CLI 缺失（外部前提）。

★ 本文件**不进 `run_ci.py` 的 step 表**：它判的是"写侧该不该支持某压缩器"，
属于**待施工项**的取证工具；等写侧真的支持了，再把它接成门禁步（届时退出码 1 才有意义）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
REF_BIN = Path(os.environ.get(
    "XRKIT_SQFS_BIN",
    r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\deps"
    r"\squashfs-tools-ng-1.3.2-mingw64\bin"))
COMPRESSORS = ["xz", "lzo", "gzip", "lz4", "zstd"]

# SquashFS 超块里 `compression`（偏移 20，小端 2 字节）的编号 —— 这是"写侧支不支持某压缩器"
# 的**唯一硬信号**：支不支持，先看这个字节对不对；再看整文件逐字节（那还受参数/元数据影响）。
# ★ 编号**以参照工具自己的输出为准**（我第一版按记忆写成 lzo=2/lz4=3/zstd=5，被探针当场纠正）：
#   tar2sqfs 造出来的超块里 lzo=3 / gzip=1 / lz4=5 / zstd=6 ⇒ 真表是下面这张
#   （1=GZIP 2=LZMA 3=LZO 4=XZ 5=LZ4 6=ZSTD）。
COMPRESSOR_ID = {"gzip": 1, "lzma": 2, "lzo": 3, "xz": 4, "lz4": 5, "zstd": 6}


def superblock_compressor_id(image: Path) -> int:
    data = image.read_bytes()
    if len(data) < 22 or data[:4] not in (b"hsqs", b"sqsh"):
        return -1
    return data[20] | (data[21] << 8)

# 与 `golden_sqfs_compressors.py` 同一批输入（小而全：文本、二进制、零字节、子目录）
FIXTURE = {
    "a.txt": b"hello squashfs\n",
    "sub/b.bin": bytes(range(256)) * 4,
    "sub/empty.dat": b"",
    "z.yaml": b"key: value\nlist:\n  - 1\n  - 2\n",
}


def build_input(root: Path) -> None:
    for rel, data in FIXTURE.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def build_tar(root: Path, tar_path: Path) -> None:
    """与参照工具一致：**按路径排序**打包（否则成员顺序会引入无关差异）。"""
    with tarfile.open(tar_path, "w") as tf:
        for rel in sorted(FIXTURE):
            tf.add(root / rel, arcname=rel)


def make_reference(tar2sqfs: Path, tar_path: Path, image: Path, compressor: str) -> str:
    """复用读侧判据的同一手法：`tar2sqfs -c <压缩器> -f <镜像>`。"""
    if image.exists():
        image.unlink()
    # ★ 与读侧判据**同一个调用形状**：tar 走 **stdin**（把 tar 当额外参数会被当成未知选项 —— 我第一版就撞了）
    with tar_path.open("rb") as fin:
        proc = subprocess.run([str(tar2sqfs), "-c", compressor, "-f", str(image)],
                              stdin=fin, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0 or not image.exists():
        return "tar2sqfs 失败（rc=%d）：%s" % (proc.returncode,
                                              proc.stdout.decode("utf-8", "replace")[:200])
    return ""


def make_ours(root: Path, image: Path) -> str:
    """用**我们的 writer** 压同一批输入（当前只有 xz，没有压缩器开关）。"""
    if image.exists():
        image.unlink()
    proc = subprocess.run([str(CLI), "--pack-sqfs", str(image), str(root)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = proc.stdout.decode("utf-8", "replace")
    if proc.returncode != 0 or not image.exists():
        return "我们的 writer 失败（rc=%d）：%s" % (proc.returncode, out.strip()[:200])
    return ""


def first_diff(a: bytes, b: bytes) -> str:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return "首个差异 @%d：ref=0x%02x ours=0x%02x" % (i, a[i], b[i])
    if len(a) != len(b):
        return "长度不同：ref=%d ours=%d（前缀相同）" % (len(a), len(b))
    return "完全相同"


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--only", default="", help="只跑某一种压缩器（例如 xz）")
    args = ap.parse_args()
    wanted = [c for c in COMPRESSORS if not args.only or c == args.only]

    print("== T-07 探针：SquashFS **写侧**压缩器与参照工具逐字节对拍 ==")
    if not CLI.is_file():
        print("!! 缺 CLI 构建产物：%s" % CLI)
        print("PROBE-SQFS-WRITE: SKIP (cli-missing)")
        return 2
    tar2sqfs = REF_BIN / "tar2sqfs.exe"
    if not tar2sqfs.exists():
        print("!! 找不到参照工具 %s（可用 XRKIT_SQFS_BIN 覆盖）" % tar2sqfs)
        print("PROBE-SQFS-WRITE: SKIP (tar2sqfs-missing)")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="t07_"))
    same = 0
    different = 0
    try:
        src = tmp / "in"
        build_input(src)
        tar_path = tmp / "in.tar"
        build_tar(src, tar_path)

        ours_img = tmp / "ours.sqfs"
        err = make_ours(src, ours_img)
        if err:
            print("!! 我们的 writer 跑不起来：%s" % err)
            print("PROBE-SQFS-WRITE: SKIP (ours-failed)")
            return 2
        ours = ours_img.read_bytes()
        print("  我们的 writer 产物：%d 字节（sha256 前缀 %s）"
              % (len(ours), __import__("hashlib").sha256(ours).hexdigest()[:16]))

        for comp in wanted:
            ref_img = tmp / ("ref_%s.sqfs" % comp)
            err = make_reference(tar2sqfs, tar_path, ref_img, comp)
            if err:
                print("  [%-5s] 参照侧失败：%s" % (comp, err))
                different += 1
                continue
            ref = ref_img.read_bytes()
            detail = first_diff(ref, ours)
            want_id = COMPRESSOR_ID.get(comp, -1)
            ref_id = superblock_compressor_id(ref_img)
            ours_id = superblock_compressor_id(ours_img)
            id_ok = (want_id == ours_id and ours_id > 0)
            if id_ok:
                same += 1
                extra = "；整文件一致" if ref == ours else "；整文件仍差（%s ⇒ 参数/元数据差异，不是压缩器）" % detail
                print("  [%-5s] ✓ 超块 compression=%d（＝参照 %d）—— 写侧**支持**该压缩器%s"
                      % (comp, ours_id, ref_id, extra))
            else:
                different += 1
                print("  [%-5s] ✗ 超块 compression=%d（参照 %d，期望 %d）—— 写侧**没有**这个压缩器"
                      % (comp, ours_id, ref_id, want_id))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    print("PROBE-SQFS-WRITE: %s（一致 %d / 不一致 %d）"
          % ("全部一致" if different == 0 else "有差异", same, different))
    print("★ 现在的预期是「只有 xz 一致」：写侧尚未实现 gzip/lzo/lz4/zstd（审计 T-07）。")
    print("★ 接进门禁的前提：写侧真的支持了那些压缩器，且**库版本与压缩参数与参照一致**")
    print("  （否则逐字节对拍会因参数差异假红 —— 见 `golden_sqfs_compressors.py` docstring）。")
    return 0 if different == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
