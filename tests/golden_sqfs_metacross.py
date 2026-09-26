#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""**能红的锁**：SquashFS 元数据 **跨块逻辑偏移**（2026-09-27 真机整树跑抓到的缺陷）。

缺陷现场：`gamedata.sqzy_patch` 里的 `textures/verg/masks/hud_sci_1_5.dds` —— 它的 inode
起始偏移是 **8200 > 8 KiB**，参照 reader 会"减掉整块、顺 next 指针跳过去"，我们原先直接报
"元数据块越界（off=8200，块内只有 8192 字节）"。

判据（两条）：
 ① 我们 `--extract` 那个文件的 **sha256** 必须等于 **参照工具** `rdsquashfs --cat` 的输出；
 ② 大小也要等于参照报的 `File size`（这条顺带证明 block_sizes 全读对了，不只读了第一块）。
参照/镜像缺失时退码 2（按外部前提处理，记入"本次未覆盖"）。

用法：python tests/golden_sqfs_metacross.py
"""
from __future__ import annotations

import hashlib
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
IMAGE = Path(os.environ.get(
    "XRKIT_METACROSS_IMAGE",
    r"E:\Software\Games\STALKER\Games\NLC Improved\NLC Improved [OBT]\gamedata.sqzy_patch"))
MEMBER = "textures/verg/masks/hud_sci_1_5.dds"


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-SQFS-METACROSS: SKIP (cli-missing)")
        return 2
    rdsquashfs = REF_BIN / "rdsquashfs.exe"
    if not rdsquashfs.exists() or not IMAGE.exists():
        print("!! 缺参照工具或镜像（%s / %s）—— 可用 XRKIT_SQFS_BIN / XRKIT_METACROSS_IMAGE 覆盖"
              % (rdsquashfs, IMAGE))
        print("GOLDEN-SQFS-METACROSS: SKIP (sample-missing)")
        return 2

    with tempfile.TemporaryDirectory(prefix="xrkit_metacross_") as tmp:
        work = Path(tmp)
        # 参照：rdsquashfs --cat 把文件吐到 stdout（二进制），我们直接读它的字节
        ref = subprocess.run([str(rdsquashfs), "--cat", MEMBER, str(IMAGE)],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if ref.returncode != 0:
            print("!! 参照工具读不出该成员：%s" % ref.stderr.decode("utf-8", "replace")[:200])
            print("GOLDEN-SQFS-METACROSS: SKIP (ref-tool-error)")
            return 2
        ref_bytes = ref.stdout

        out = work / "member.bin"
        proc = subprocess.run([str(CLI), "--extract", str(IMAGE), MEMBER, str(out)],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        text = proc.stdout.decode("utf-8", "replace").strip()
        if proc.returncode != 0 or not out.exists():
            print("✗ 提取失败（跨块偏移没读对？）：%s" % text[:200])
            print("")
            print("GOLDEN-SQFS-METACROSS: FAIL (extract-error)")
            return 1
        ours = out.read_bytes()

        failures = 0
        if len(ours) != len(ref_bytes):
            failures += 1
            print("✗ 大小不符：参照 %d 字节 / 我们 %d 字节" % (len(ref_bytes), len(ours)))
        else:
            print("ok 大小一致：%d 字节（%s）" % (len(ours), text[:80]))
        if hashlib.sha256(ours).hexdigest() != hashlib.sha256(ref_bytes).hexdigest():
            failures += 1
            print("✗ 内容不符：参照 sha=%s / 我们 sha=%s"
                  % (hashlib.sha256(ref_bytes).hexdigest()[:16],
                     hashlib.sha256(ours).hexdigest()[:16]))
        else:
            print("ok 内容逐字节一致：sha=%s" % hashlib.sha256(ours).hexdigest()[:16])

    print("")
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-SQFS-METACROSS: %s (failed=%d)" % (verdict, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
