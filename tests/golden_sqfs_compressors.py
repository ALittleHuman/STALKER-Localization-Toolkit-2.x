#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""块压缩器**读侧**判据：lzo / gzip / lz4 / zstd（⑥6.5 一般化，2026-09-27）。

对每种压缩器：
 ① **正面**：用参照工具 `tar2sqfs -c <压缩器>` 造镜像 ⇒ 我们的 `--dump` 条目/大小与输入树一致；
 ② **反面**：截到连 96 字节超级块都不完整 ⇒ 必须**可见失败**（退出码非 0 且有理由），
    不许静默返回空表（"失败被吞掉"正是 ⑥6.5 要防的）。

★ 实测记录：`--dump` **只读元数据、不读数据块** ⇒ 把镜像截到 1/3 时它**照样能列出完整条目表**，
  那是正常的（比逐个抽文件快得多的原因）。所以反面的触发点是"超级块不完整"。

★ 写侧（我们产出这几种压缩器的镜像）**本脚本不覆盖** —— 现有 writer 只有 xz：
  `compress.cpp` 对其余压缩器显式报错"无参照基线"。要补写侧，先按本脚本的命令产出参照镜像，
  再确认 zlib/zstd/lz4 **库版本与压缩参数**一致（逐字节对拍的前提）。

用法：python tests/golden_sqfs_compressors.py
退出码：0=通过，1=不通过，2=参照工具/CLI 缺失（按外部前提处理）。
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
REF_BIN = Path(os.environ.get(
    "XRKIT_SQFS_BIN",
    r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\deps"
    r"\squashfs-tools-ng-1.3.2-mingw64\bin"))
COMPRESSORS = ["lzo", "gzip", "lz4", "zstd"]


def dump(image: Path) -> tuple[int, dict[str, int], str]:
    proc = subprocess.run([str(CLI), "--dump", str(image)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = proc.stdout.decode("utf-8", "replace")
    entries: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "f":
            entries[parts[1]] = int(parts[2])
    return proc.returncode, entries, out.strip()


def make_image(tar2sqfs: Path, tar_path: Path, image: Path, compressor: str) -> str:
    with tar_path.open("rb") as fin:
        rc = subprocess.run([str(tar2sqfs), "-c", compressor, "-f", str(image)],
                            stdin=fin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if rc.returncode != 0 or not image.exists():
        return rc.stderr.decode("utf-8", "replace")[:160]
    return ""


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-SQFS-COMPRESSORS: SKIP (cli-missing)")
        return 2
    tar2sqfs = REF_BIN / "tar2sqfs.exe"
    if not tar2sqfs.exists():
        print("!! 找不到参照工具 %s（可用 XRKIT_SQFS_BIN 覆盖）" % tar2sqfs)
        print("GOLDEN-SQFS-COMPRESSORS: SKIP (tar2sqfs-missing)")
        return 2

    failures = 0
    compared = 0
    with tempfile.TemporaryDirectory(prefix="xrkit_comp_") as tmp:
        work = Path(tmp)
        src = work / "src"
        src.mkdir()
        (src / "a.txt").write_bytes(b"hello-compressor")
        (src / "b.bin").write_bytes(b"x" * 40000)
        want = {"a.txt": 16, "b.bin": 40000}
        tar_path = work / "tree.tar"
        rc = subprocess.run(["tar", "-cf", str(tar_path), "-C", str(src), "."],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if rc.returncode != 0:
            print("GOLDEN-SQFS-COMPRESSORS: SKIP (tar-missing)")
            return 2

        for comp in COMPRESSORS:
            image = work / ("%s.sqfs" % comp)
            err = make_image(tar2sqfs, tar_path, image, comp)
            if err:
                print("✗ [%s] 参照工具产不出镜像：%s" % (comp, err))
                failures += 1
                continue
            compared += 1
            code, got, out = dump(image)
            if code != 0 or got != want:
                failures += 1
                print("✗ [%s] 读回不一致（退出码 %d）：期望 %s 实得 %s"
                      % (comp, code, want, got))
                continue
            print("ok [%s] 正面：读回 %d 个条目、大小全对" % (comp, len(got)))

            broken = work / ("%s_broken.sqfs" % comp)
            broken.write_bytes(image.read_bytes()[:64])
            code2, got2, out2 = dump(broken)
            if code2 == 0:
                failures += 1
                print("✗ [%s] 坏镜像被静默当成功（%s）—— ⑥6.5 要防的就是这个" % (comp, got2))
            else:
                print("ok [%s] 反面：坏镜像可见失败（%s）"
                      % (comp, out2.splitlines()[-1][:80] if out2 else "无输出"))

    print("")
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-SQFS-COMPRESSORS: %s (compared=%d failed=%d)"
          % (verdict, compared, failures))
    return 1 if failures else (0 if compared else 2)


if __name__ == "__main__":
    sys.exit(main())
