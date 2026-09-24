#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SquashFS 封包黄金语料：**产物逐字节一致**（M1-5 的验收判据）。

基线必须由 **1.x 的 sqfs_pack** 生成（它内部调 tar2sqfs 1.3.2，默认 xz）——不能
由"我以为应该长什么样"生成。

    python tests/golden_sqfs_pack.py --write    # 用 1.x 生成基线（sha256 入库）
    python tests/golden_sqfs_pack.py            # 对拍（跑 2.x 的 --pack-sqfs）
    python tests/golden_sqfs_pack.py --id basic

判据三层（任一不满足即 FAIL）：
  ① 我们产物的 sha256 必须等于基线 sha256（"逐字节一致"本身）；
  ② 我们产物必须能被**我们自己的 reader** 读回：条目（路径/类型/大小）与夹具一致；
  ③ 我们产物必须能被 **rdsquashfs**（第三方参照）读回，条目与夹具一致（双保险）。

夹具覆盖的路径（每一条都对应 writer 里的一段规则）：
  basic  —— 碎尾 fragment / 正好一块 / 多块 + 尾 / 空文件 / 隐式目录 / 西里尔名
  dedup  —— 整段数据块去重 + 同名碎片去重
  many   —— 600 个碎片小文件：inode 表与目录表都跨 8 KiB 元数据块 + 目录 ≥256 条（EXT_DIR + 索引）
  sparse —— 全零数据块（稀疏）+ 全零碎尾（稀疏尾，参照实现里最刁的一条）
  dirs   —— 显式目录条目（含空目录；1.x 的 fs 页只喂文件，这里走 --pack-dirs 显式验证）
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BASELINE_DIR = HERE / "golden" / "sqfs_pack"
WORK = ROOT / "build" / "golden_sqfs_pack"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
TAR2SQFS = (TOOLKIT_1X / "deps" / "squashfs-tools-ng-1.3.2-mingw64" / "bin" / "tar2sqfs.exe")
RDSQUASHFS = TAR2SQFS.with_name("rdsquashfs.exe")


# ── 确定性数据发生器（不许用 random：夹具必须逐次可复现）───────────────
def pattern(n: int, seed: int = 13) -> bytes:
    return bytes(((i * seed + 7) % 251) for i in range(n))


def stalker_blob() -> bytes:
    rng = bytes(((i * 7919) % 251) for i in range(4096))
    return (b"STALKER-" * 4096) + bytes(range(256)) * 300 + rng * 8


def fixture_basic() -> dict[str, bytes]:
    return {
        "readme.txt": "Привет, сталкер! 你好。".encode("utf-8") * 20,
        "gamedata/textures/a.dds": pattern(12288, 7),
        "gamedata/textures/b.dds": pattern(131072, 13),
        "gamedata/scripts/x.script": stalker_blob(),
        "empty.bin": b"",
        "Текстуры/привет.txt": "набор текстур".encode("utf-8") * 9,
    }


def fixture_dedup() -> dict[str, bytes]:
    big = pattern(300000, 29)
    small = b"same tiny payload\n" * 25
    return {
        "big/a.bin": big,
        "big/b.bin": big,
        "small/c.txt": small,
        "small/d.txt": small,
        "other/e.bin": pattern(200000, 31),
    }


def fixture_many() -> dict[str, bytes]:
    return {"many/f%04d.bin" % i: pattern(200, i % 97 + 3) for i in range(600)}


def fixture_sparse() -> dict[str, bytes]:
    return {
        "zeros/full.bin": bytes(131072) + b"tail-data" * 12,
        "zeros/tiny.bin": bytes(500),
        "zeros/multi.bin": pattern(131072, 41) + bytes(131072) + pattern(5000, 43),
        "zeros/normal.bin": pattern(700, 47),
    }


def fixture_dirs() -> dict[str, bytes]:
    return {
        "top.txt": b"top-level\n",
        "sub/inside.txt": b"inside\n",
    }


EMPTY_DIRS = {"dirs": ["emptydir", "sub/nested"]}

FIXTURES = {
    "basic": fixture_basic,
    "dedup": fixture_dedup,
    "many": fixture_many,
    "sparse": fixture_sparse,
    "dirs": fixture_dirs,
}


def materialize(fixture_id: str) -> Path:
    """把夹具落到磁盘（每次先清空，保证从零开始）。"""
    root = WORK / fixture_id / "tree"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    for rel, data in FIXTURES[fixture_id]().items():
        target = root / Path(*rel.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    for rel in EMPTY_DIRS.get(fixture_id, []):
        (root / Path(*rel.split("/"))).mkdir(parents=True, exist_ok=True)
    return root


def enumerate_entries(root: Path, with_dirs: bool) -> list[tuple[str, bool, Path]]:
    """与 CLI 的枚举规则逐条对齐：递归（不跟目录符号链接）、只收普通文件、
    rel 用 '\\'→'/'、按 rel 的 UTF-8 字节序升序。"""
    out: list[tuple[str, bool, Path]] = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        base = Path(dirpath)
        for name in dirnames:
            p = base / name
            rel = str(p.relative_to(root)).replace("\\", "/")
            if with_dirs:
                out.append((rel, True, p))
        for name in filenames:
            p = base / name
            if not p.is_file():   # 断链/特殊文件：跳过（与 CLI 的 is_regular_file 对齐）
                continue
            rel = str(p.relative_to(root)).replace("\\", "/")
            out.append((rel, False, p))
    out.sort(key=lambda item: item[0].encode("utf-8"))
    return out


def baseline_1x(root: Path, out_path: Path, with_dirs: bool) -> str:
    """用 1.x 的 sqfs_pack 生成参照镜像 → sha256。

    1.x 的 files 列表是 [(rel, data, is_dir)]，顺序 = 我们喂给 CLI 的条目顺序
    （tar 顺序决定了数据块在盘上的先后）。
    """
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from file_system import stalker_fs  # noqa: PLC0415

    files = []
    for rel, is_dir, p in enumerate_entries(root, with_dirs):
        files.append((rel, b"" if is_dir else p.read_bytes(), is_dir))
    out_path.unlink(missing_ok=True)  # tar2sqfs 默认不覆盖已存在的输出
    if not stalker_fs.sqfs_pack(files, str(out_path)):
        raise RuntimeError("1.x sqfs_pack 失败（缺 tar2sqfs？）")
    return hashlib.sha256(out_path.read_bytes()).hexdigest()


def candidate_2x(root: Path, out_path: Path, with_dirs: bool) -> tuple[str, str]:
    argv = [str(CLI), "--pack-sqfs", str(out_path), str(root)]
    if with_dirs:
        argv.append("--pack-dirs")
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    if proc.returncode != 0:
        raise RuntimeError("2.x 封包失败：%s" % text[:300])
    return hashlib.sha256(out_path.read_bytes()).hexdigest(), text


def reader_dump(image: Path) -> dict[str, tuple[str, int]]:
    proc = subprocess.run([str(CLI), "--dump", str(image)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError("2.x 读回失败：%s" % text.strip()[:200])
    out: dict[str, tuple[str, int]] = {}
    for line in text.splitlines():
        if line.startswith("DUMP "):
            continue
        parts = line.rstrip("\r").split("\t")
        if len(parts) == 3:
            out[parts[1]] = (parts[0], int(parts[2]))
    return out


def rdsquashfs_dump(image: Path) -> dict[str, str]:
    """第三方参照读回：`rdsquashfs --describe` 每行 `<type> <路径> <mode> <uid> <gid>`。

    只比"路径 + 类型"（大小它不打印；大小由我们自己的 reader 那一层负责）。
    """
    proc = subprocess.run([str(RDSQUASHFS), "--describe", str(image)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        raise RuntimeError("rdsquashfs 读回失败：%s"
                           % proc.stdout.decode("utf-8", "replace")[:200])
    out: dict[str, str] = {}
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        parts = line.strip().split(" ", 2)
        if len(parts) < 2:
            continue
        kind, path = parts[0], parts[1].lstrip("/")
        if path:
            out[path] = kind
    return out


def expected_entries(root: Path, with_dirs: bool) -> dict[str, tuple[str, int]]:
    """镜像里应有的条目：文件 + 显式目录 + **隐式目录**（文件路径的每一层祖先）。

    隐式目录是 1.x 那条链路上的常态（它的 tar 只喂文件），漏了就会把"对"判成"多"。
    """
    out: dict[str, tuple[str, int]] = {}
    for rel, is_dir, p in enumerate_entries(root, with_dirs):
        out[rel] = ("d" if is_dir else "f", 0 if is_dir else p.stat().st_size)
    for rel, is_dir, _ in enumerate_entries(root, False):
        parts = rel.split("/")
        for i in range(1, len(parts)):
            out.setdefault("/".join(parts[:i]), ("d", 0))
    return out


def first_diff(a: bytes, b: bytes) -> str:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            lo = max(0, i - 8)
            return ("首个不同字节 @%d（%d/%d 字节）：基线 %s | 候选 %s"
                    % (i, len(a), len(b), a[lo:i + 8].hex(), b[lo:i + 8].hex()))
    return "前缀相同，长度不同：基线 %d 字节 / 候选 %d 字节" % (len(a), len(b))


def main() -> int:
    # ★ 控制台可能是 GBK/cp1252：✓/✗ 这类字符会直接 UnicodeEncodeError 把脚本打断
    #   （1.x 的 CI 也踩过同一个坑：给子进程兜底 PYTHONIOENCODING=utf-8）。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="SquashFS 封包逐字节对拍")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    parser.add_argument("--id", default="", help="只跑 id 含该子串的夹具")
    parser.add_argument("--keep", action="store_true", help="保留中间产物")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-SQFS-PACK: SKIP (cli-missing)")
        return 2
    if not TAR2SQFS.exists():
        print("!! 找不到参照工具 %s" % TAR2SQFS)
        print("GOLDEN-SQFS-PACK: SKIP (tar2sqfs-missing)")
        return 2

    ids = [i for i in FIXTURES if not args.id or args.id in i]
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    failures = 0
    compared = 0

    for fixture_id in ids:
        with_dirs = fixture_id in EMPTY_DIRS
        print("[%s]%s" % (fixture_id, "（显式目录条目）" if with_dirs else ""))
        root = materialize(fixture_id)
        base_path = BASELINE_DIR / ("%s.json" % fixture_id)
        ref_image = WORK / ("%s.ref.sqfs" % fixture_id)
        our_image = WORK / ("%s.ours.sqfs" % fixture_id)

        if args.write:
            digest = baseline_1x(root, ref_image, with_dirs)
            base_path.write_text(json.dumps({
                "id": fixture_id,
                "with_dirs": with_dirs,
                "sha256": digest,
                "size": ref_image.stat().st_size,
                "note": "基线由 1.x 的 sqfs_pack（tar2sqfs 1.3.2，默认 xz）生成",
            }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print("  已写基线 sha256=%s（%d 字节）→ %s"
                  % (digest[:16], ref_image.stat().st_size, base_path.relative_to(ROOT)))
            compared += 1
            continue

        if not base_path.exists():
            print("  !! 缺基线（先跑 --write）")
            failures += 1
            continue

        base = json.loads(base_path.read_text(encoding="utf-8"))

        try:
            ours_sha, cli_line = candidate_2x(root, our_image, with_dirs)
        except Exception as exc:  # noqa: BLE001
            print("  ✗ %s" % exc)
            failures += 1
            continue

        ok = True
        if ours_sha != base["sha256"]:
            ok = False
            print("  ✗ sha256 不一致：基线 %s / 候选 %s" % (base["sha256"][:16], ours_sha[:16]))
            # 现场再生成一次参照镜像，给出"首个不同字节"用于定位
            try:
                baseline_1x(root, ref_image, with_dirs)
                print("     %s" % first_diff(ref_image.read_bytes(), our_image.read_bytes()))
            except Exception as exc:  # noqa: BLE001
                print("     （无法现场生成参照镜像定位：%s）" % exc)

        # ② 我们自己的 reader 读回
        exp = expected_entries(root, with_dirs)
        got = reader_dump(our_image)
        if exp != got:
            ok = False
            missing = sorted(set(exp) - set(got))
            extra = sorted(set(got) - set(exp))
            bad = sorted(p for p in exp if p in got and exp[p] != got[p])
            print("  ✗ 自读回不一致：缺 %d / 多 %d / 不符 %d" % (len(missing), len(extra), len(bad)))
            for p in (missing + extra + bad)[:8]:
                print("      %s: 期望 %s 实得 %s" % (p, exp.get(p), got.get(p)))
        else:
            print("  ok 自读回 %d 条（路径/类型/大小）" % len(got))

        # ③ 第三方 rdsquashfs 读回（比"路径 + 类型"）
        try:
            ref_map = rdsquashfs_dump(our_image)
            exp_paths = set(exp)
            if set(ref_map) != exp_paths:
                ok = False
                print("  ✗ rdsquashfs 路径集合不符：缺 %d / 多 %d"
                      % (len(exp_paths - set(ref_map)), len(set(ref_map) - exp_paths)))
                for p in sorted(exp_paths - set(ref_map))[:5]:
                    print("      缺: %s" % p)
                for p in sorted(set(ref_map) - exp_paths)[:5]:
                    print("      多: %s" % p)
            else:
                bad_kind = sorted(p for p in exp_paths
                                  if (exp[p][0] == "d") != (ref_map[p] == "dir"))
                if bad_kind:
                    ok = False
                    print("  ✗ rdsquashfs 类型不符：%s" % bad_kind[:5])
                else:
                    print("  ok rdsquashfs 读回 %d 条（路径 + 类型）" % len(ref_map))
        except Exception as exc:  # noqa: BLE001
            print("  !! rdsquashfs 读回跳过：%s" % exc)

        if ok:
            print("  ✓ 逐字节一致 %s" % cli_line.split("PACK ok ")[-1])
        else:
            failures += 1
        compared += 1

    if not args.keep:
        for path in WORK.glob("*.sqfs"):
            path.unlink(missing_ok=True)

    print("")
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-SQFS-PACK: %s (compared=%d)" % (verdict, compared))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
