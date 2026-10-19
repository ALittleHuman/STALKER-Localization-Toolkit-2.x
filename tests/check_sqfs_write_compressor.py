# -*- coding: utf-8 -*-
"""判据 T-07：SquashFS **写侧压缩器可选**（第三只读审计 T-07；用户 2026-10-19 选项 1）。

背景
----
参照实现 squashfs-tools-ng 1.3.2 的 gzip 参数（用户钉死）：
`deflateInit2(level=9, Z_DEFLATED, window=15, memLevel=8, Z_DEFAULT_STRATEGY)` ＋ `deflate(Z_FINISH)`，
默认参数**不写** options 块。块级字节探针（`tests/probe_zlib_deflate_bytes.py`）实测
**zlib 1.2.12 与 1.3.2 输出逐字节一致（3/3）** ⇒ gzip 这一支可以按"逐字节"验收。

本判据（黑盒，跑真 CLI）
------------------------
A `--compressor xz` 与**不带 `--compressor`** 产出的镜像**逐字节相同**，且超块 `compression==4`
  —— **默认路径一个字节都不许变**（`golden_sqfs_pack.py` 那条黄金判据依赖它）；
B `--compressor gzip` ⇒ 超块 `compression==1`；`--dump` 能读；**解包往返逐字节一致**；
C 不认识的压缩器名 ⇒ 非零退出 ＋ 有明确理由，且**不产出文件**（不许静默回落成 xz）；
D 静态锁：`squashfs_write.cpp` 里**没有**残留的 `Compression::Xz`（8 处都必须跟着 `opt_.compressor`），
  且 `opt_.compressor` 至少出现 8 次 —— 特别是 `load_frag_block` 的**解压**必须与写同一个压缩器
  （漏了它，去重会读到错数据：产物看起来能生成、内容是坏的）。

退出码：0=绿 / 1=红 / 2=缺输入（CLI 产物或源码）。
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WRITE_CPP = ROOT / "src" / "engine" / "fs" / "squashfs_write.cpp"

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


def run(args: list, timeout: int = 600) -> tuple:
    try:
        p = subprocess.run([str(CLI)] + args, capture_output=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as exc:
        return None, "RUN-ERROR: %s" % exc
    return p.returncode, ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")


def superblock_id(image: Path) -> int:
    data = image.read_bytes()
    if len(data) < 22 or data[:4] not in (b"hsqs", b"sqsh"):
        return -1
    return data[20] | (data[21] << 8)


def main() -> int:
    print("== T-07：SquashFS 写侧压缩器可选（xz 默认不变 ＋ gzip 可用）==")
    if not CLI.is_file():
        print("SQFS-COMPRESSOR skip 缺 CLI 构建产物")
        return 2
    if not WRITE_CPP.is_file():
        print("SQFS-COMPRESSOR skip 缺 squashfs_write.cpp")
        return 2

    src = WRITE_CPP.read_text(encoding="utf-8", errors="replace")
    want("Compression::Xz" not in src,
         "D1 `squashfs_write.cpp` 里不再有硬编码 `Compression::Xz`")
    want(src.count("opt_.compressor") >= 8,
         "D2 `opt_.compressor` 至少 8 处（超块 id ＋ 数据块 ＋ 元数据块 ＋ 两张表 ＋ fragment ＋ 解压回读）",
         "实测 %d" % src.count("opt_.compressor"))

    tmp = Path(tempfile.mkdtemp(prefix="t07c_"))
    try:
        tree = tmp / "in"
        (tree / "sub").mkdir(parents=True)
        (tree / "a.txt").write_bytes(b"hello squashfs\n")
        (tree / "sub" / "b.txt").write_bytes(b"beta\n")

        img_default = tmp / "default.sqfs"
        rc, out = run(["--pack-sqfs", str(img_default), str(tree)])
        want(rc == 0 and img_default.is_file(), "A0 默认（不带 --compressor）能打包", "rc=%s" % rc)
        img_xz = tmp / "xz.sqfs"
        rc, out = run(["--pack-sqfs", str(img_xz), str(tree), "--compressor", "xz"])
        want(rc == 0 and img_xz.is_file(), "A1 `--compressor xz` 能打包", "rc=%s" % rc)
        if img_default.is_file() and img_xz.is_file():
            same = img_default.read_bytes() == img_xz.read_bytes()
            want(same, "A2 显式 `xz` 与**默认**产出逐字节相同（默认路径一个字节都不许变）")
            want(superblock_id(img_xz) == 4, "A3 xz 的超块 `compression==4`",
                 "实测 %d" % superblock_id(img_xz))

        img_gz = tmp / "gzip.sqfs"
        rc, out = run(["--pack-sqfs", str(img_gz), str(tree), "--compressor", "gzip"])
        want(rc == 0 and img_gz.is_file(), "B0 `--compressor gzip` 能打包", "rc=%s" % rc)
        if img_gz.is_file():
            want(superblock_id(img_gz) == 1, "B1 gzip 的超块 `compression==1`",
                 "实测 %d" % superblock_id(img_gz))
            rc, out = run(["--dump", str(img_gz)])
            want(rc == 0 and "DUMP ok" in out, "B2 gzip 镜像 `--dump` 能读", "rc=%s" % rc)
            outdir = tmp / "extracted"
            rc, out = run(["--unpack-package", str(img_gz), str(outdir)])
            # `--unpack-package` 会按包名建一层目录 ⇒ **递归找**，别写死层级
            found = sorted(outdir.rglob("a.txt")) if outdir.is_dir() else []
            ok_rt = rc == 0 and bool(found) and found[0].read_bytes() == (tree / "a.txt").read_bytes()
            want(ok_rt, "B3 gzip 镜像**解包往返逐字节一致**",
                 "rc=%s 找到=%s" % (rc, [str(f.relative_to(outdir)) for f in found][:2]))

        img_bad = tmp / "bogus.sqfs"
        rc, out = run(["--pack-sqfs", str(img_bad), str(tree), "--compressor", "bogus"])
        want(rc is not None and rc != 0, "C1 不认识的压缩器 ⇒ 非零退出", "rc=%s" % rc)
        want("不支持" in out or "可选" in out, "C2 理由要点明「不支持/可选」（零静默）",
             [l.strip() for l in out.splitlines() if "不支持" in l][:1])
        want(not img_bad.is_file(), "C3 拒绝时**不产出**文件（不静默回落成 xz）")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("SQFS-COMPRESSOR fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SQFS-COMPRESSOR ok %d 项（默认 xz 逐字节不变；gzip 可打包/可读/往返一致；"
          "未知压缩器明确拒绝且不产出）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
