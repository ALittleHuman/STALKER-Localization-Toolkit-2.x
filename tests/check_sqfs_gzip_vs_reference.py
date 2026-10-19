# -*- coding: utf-8 -*-
"""判据 T-07（收口条件①）：**我们的 gzip 产物与参照 `tar2sqfs -c gzip` 逐字节一致**。

背景（用户 2026-10-19 裁决）
---------------------------
「只有往返一致不够，往返只能证明能读回，证明不了产物与**参照同族**」。
⇒ 本判据做的正是"参照工具造镜像 vs 我们的 writer，逐字节比"。

★ 关键细节（第一版踩过）：参照 tar **必须按 1.x `stalker_fs.sqfs_pack` 同款构造** ——
  `TarInfo` ＋ **mtime=0** ＋ 目录 `0755` / 文件 `0644`。用 Python 的 `tar.add()`（会把磁盘上的
  真实 mtime/uid 带进去）会造出**不同的元数据**，然后在超块 `@40 bytes_used` 处假红 ——
  我第一次就是这么误判"和参照对不上"的。

本判据
------
A `tar2sqfs -c gzip -q` 参照镜像 vs 我们 `--pack-sqfs --compressor gzip`：**逐字节一致**（含元数据）；
B 对照：`-c xz` 参照 vs 我们默认（或 `--compressor xz`）：也**逐字节一致**
  —— 证明"默认路径没被 gzip 改动碰到"，也是本判据自身的**正向对照**；
C 两次的 `compression` 编号分别是 1 / 4。

前提与 SKIP 口径（用户要求：改动区的专项判据**不能静默 SKIP**）
------------------------------------------------------------
缺 `tar2sqfs` 或 CLI 时返回 **2**，并**明确打印缺哪一件** —— 门禁侧按"未覆盖"记账，
不接受"悄悄跳过"。

退出码：0=绿 / 1=红 / 2=缺前提。
`--ours-compressor` 只为取证（把它指成 `xz` ⇒ A 必须红，用来证判据有牙）。
"""

from __future__ import annotations

import argparse
import io
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
REF_BIN = Path(__import__("os").environ.get(
    "XRKIT_SQFS_BIN",
    r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\deps"
    r"\squashfs-tools-ng-1.3.2-mingw64\bin"))

# 夹具：与 golden 系列同风格（文本 ＋ 子目录；小到能一眼看完，又不至于全是单块）
FILES = {
    "a.txt": b"hello squashfs\n",
    "sub/b.txt": b"beta\n",
    "z.dat": bytes(range(64)) * 8,
}

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


def build_tree(root: Path) -> None:
    for rel, data in FILES.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def build_tar(root: Path, tar_path: Path) -> None:
    """**1.x `sqfs_pack` 同款**：TarInfo ＋ mtime=0（默认）＋ 目录 0755 / 文件 0644。"""
    with tarfile.open(tar_path, "w") as tar:
        for rel in ["."] + sorted(FILES):
            if rel == ".":
                for d in sorted({str(Path(r).parent).replace("\\", "/") for r in FILES}):
                    if d in (".", ""):
                        continue
                    ti = tarfile.TarInfo(d + "/")
                    ti.type = tarfile.DIRTYPE
                    ti.mode = 0o755
                    ti.size = 0
                    tar.addfile(ti)
                continue
            data = (root / rel).read_bytes()
            ti = tarfile.TarInfo(rel)
            ti.size = len(data)
            ti.mode = 0o644
            tar.addfile(ti, io.BytesIO(data))


def ref_image(tar_path: Path, comp: str, out: Path) -> str:
    tar2sqfs = REF_BIN / "tar2sqfs.exe"
    with open(tar_path, "rb") as fh:
        r = subprocess.run([str(tar2sqfs), "-c", comp, "-q", str(out)],
                           stdin=fh, capture_output=True)
    if r.returncode != 0 or not out.is_file():
        return "tar2sqfs -c %s 失败（rc=%d）：%s" % (
            comp, r.returncode, r.stderr.decode("utf-8", "replace")[:160])
    return ""


def ours_image(root: Path, comp: str, out: Path) -> str:
    r = subprocess.run([str(CLI), "--pack-sqfs", str(out), str(root), "--compressor", comp],
                       capture_output=True)
    if r.returncode != 0 or not out.is_file():
        return "我们的 writer 失败（rc=%d）：%s" % (
            r.returncode, ((r.stdout or b"") + (r.stderr or b"")).decode("utf-8", "replace")[:160])
    return ""


def first_diff(a: bytes, b: bytes) -> str:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return "@%d ref=%s ours=%s" % (i, a[i:i + 6].hex(), b[i:i + 6].hex())
    return "长度不同 ref=%d ours=%d" % (len(a), len(b))


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--ours-compressor", default="gzip",
                    help="取证用：把「我方」换成别的压缩器（例如 xz）⇒ A 必须红")
    args = ap.parse_args()

    print("== T-07 收口条件①：gzip 产物 vs 参照 tar2sqfs -c gzip 逐字节 ==")
    if not CLI.is_file():
        print("!! 缺 CLI 构建产物：%s" % CLI)
        print("SQFS-GZIP-REF: SKIP (cli-missing)")
        return 2
    if not (REF_BIN / "tar2sqfs.exe").is_file():
        print("!! 缺参照工具：%s（可用 XRKIT_SQFS_BIN 覆盖）" % (REF_BIN / "tar2sqfs.exe"))
        print("SQFS-GZIP-REF: SKIP (tar2sqfs-missing)")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="t07ref_"))
    try:
        tree = tmp / "in"
        tree.mkdir()
        build_tree(tree)
        tar_path = tmp / "onepointx.tar"
        build_tar(tree, tar_path)

        for comp, ours_comp, label, want_id in (
                ("gzip", args.ours_compressor, "A gzip 参照 vs 我方", 1),
                ("xz", "xz", "B 对照 xz 参照 vs 我方", 4)):
            ref = tmp / ("ref_%s.sqfs" % comp)
            err = ref_image(tar_path, comp, ref)
            if err:
                want(False, label + "（参照侧跑不起来）", err)
                continue
            ours = tmp / ("ours_%s.sqfs" % comp)
            err = ours_image(tree, ours_comp, ours)
            if err:
                want(False, label + "（我方跑不起来）", err)
                continue
            a, b = ref.read_bytes(), ours.read_bytes()
            same = a == b
            want(same, label + "：**逐字节一致**",
                 "ref=%d ours=%d id=%d/%d %s" % (len(a), len(b), a[20], b[20],
                                                 "" if same else first_diff(a, b)))
            want(a[20] == want_id and b[20] == want_id,
                 label + "：`compression` 编号都是 %d" % want_id,
                 "%d/%d" % (a[20], b[20]))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("SQFS-GZIP-REF fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SQFS-GZIP-REF ok %d 项（gzip 与参照**同族且逐字节一致**；xz 对照也一致；"
          "编号 1/4 正确）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
