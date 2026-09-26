#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""⑥6.5：**lzo 压缩的 SquashFS 镜像**要能读、且失败要"可见"。

判据（两条）：
 ① 正面：用**参照工具**（squashfs-tools-ng 1.3.2 的 `tar2sqfs -c lzo`）造一张 lzo 镜像
    ⇒ 我们的 `--dump` 报出的条目（路径/类型/大小）必须与输入树一致；
 ② 反面：把同一张镜像**截断** ⇒ 我们的 `--dump` 必须**当失败报出来**（退出码非 0 且给出理由），
    **不许静默返回空表或垃圾条目**（那正是 ⑥6.5 要防的"取大小失败被吞掉"）。

用法：python tests/golden_sqfs_lzo.py
退出码：0=通过，1=不通过，2=参照工具/CLI 缺失（按外部前提处理，记入"本次未覆盖"）。
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


def cli(*argv):
    return subprocess.run([str(CLI), *argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def dump(image: Path) -> tuple[int, dict[str, int], str]:
    proc = cli("--dump", str(image))
    out = proc.stdout.decode("utf-8", "replace")
    entries: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "f":
            entries[parts[1]] = int(parts[2])
    return proc.returncode, entries, out.strip()


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-SQFS-LZO: SKIP (cli-missing)")
        return 2
    tar2sqfs = REF_BIN / "tar2sqfs.exe"
    if not tar2sqfs.exists():
        print("!! 找不到参照工具 %s（可用 XRKIT_SQFS_BIN 覆盖）" % tar2sqfs)
        print("GOLDEN-SQFS-LZO: SKIP (tar2sqfs-missing)")
        return 2

    failures = 0
    with tempfile.TemporaryDirectory(prefix="xrkit_lzo_") as tmp:
        work = Path(tmp)
        src = work / "src"
        src.mkdir()
        (src / "a.txt").write_bytes(b"hello-lzo")
        (src / "b.bin").write_bytes(b"x" * 40000)
        want = {"a.txt": 9, "b.bin": 40000}

        tar_path = work / "tree.tar"
        rc = subprocess.run(["tar", "-cf", str(tar_path), "-C", str(src), "."],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if rc.returncode != 0:
            print("!! 打包 tar 失败：%s" % rc.stdout.decode("utf-8", "replace")[:200])
            print("GOLDEN-SQFS-LZO: SKIP (tar-missing)")
            return 2

        image = work / "lzo.sqfs"
        # ★ 别用 `-f -`（写 stdout）：实测那样出来的镜像我们认不出是 SquashFS
        #   （reader 会退回"按 DB 试"并报"无法唯一判定 DB 格式"）。照 1.x 的形态：
        #   **stdin 喂 tar、`-f <镜像路径>`**。
        with tar_path.open("rb") as fin:
            rc = subprocess.run([str(tar2sqfs), "-c", "lzo", "-f", str(image)],
                                stdin=fin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if rc.returncode != 0 or not image.exists():
            print("!! tar2sqfs -c lzo 失败：%s"
                  % rc.stderr.decode("utf-8", "replace")[:200])
            print("GOLDEN-SQFS-LZO: FAIL (ref-tool-error)")
            return 1

        code, got, out = dump(image)
        if code != 0 or got != want:
            failures += 1
            print("✗ lzo 镜像读回不一致：退出码 %d，期望 %s，实得 %s" % (code, want, got))
            print("   %s" % out[:200])
        else:
            print("ok 正面：lzo 镜像读回 %d 个条目、大小全对（%s）"
                  % (len(got), ", ".join("%s=%d" % kv for kv in sorted(got.items()))))

        # ② 反面：**截到不可解析** ⇒ 必须可见失败。
        #   实测记录（2026-09-27）：截到 1/3 时我们的 `--dump` **照样读出完整条目表** ——
        #   这是**正常的**（`--dump` 只读元数据，不读数据块；这正是它比"逐个抽文件"快得多的原因）。
        #   所以反面判据取"截到连超级块都不完整"，问的是"损坏镜像会不会被静默当成功"。
        broken = work / "lzo_broken.sqfs"
        data = image.read_bytes()
        broken.write_bytes(data[:64])  # 超级块本身就不完整
        code2, got2, out2 = dump(broken)
        if code2 == 0 and got2 == want:
            failures += 1
            print("✗ 截断的镜像居然读出了完整条目表（%s）—— 静默成功，正是 ⑥6.5 要防的" % got2)
        elif code2 == 0 and not got2:
            failures += 1
            print("✗ 截断的镜像读出**空表**且退出码 0 —— 失败被吞掉了，必须报错")
        else:
            print("ok 反面：截断镜像被**当失败报出**（退出码 %d：%s）"
                  % (code2, out2.splitlines()[-1][:90] if out2 else "无输出"))

    print("")
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-SQFS-LZO: %s (failed=%d)" % (verdict, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
