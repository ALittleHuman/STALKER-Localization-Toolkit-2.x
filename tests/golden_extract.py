#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提取路径的黄金语料差分：2.x（C++ 进程内解压） vs 1.x（走 rdsquashfs --cat）。

基线由 **1.x 的 sqfs_extract** 生成（它由 Python 把 --cat 重定向到文件，
不经 PowerShell 的文本重编码；直接用 `rdsquashfs --cat > file` 会把二进制搞坏）。

    python tests/golden_extract.py --write             # 生成基线（用 1.x）
    python tests/golden_extract.py                     # 对拍（跑 2.x CLI）
    python tests/golden_extract.py --id nlc-sqz-wasteland --per-image 8
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from collect_golden import CORPUS  # noqa: E402
from golden_sqfs import SQFS_IDS  # noqa: E402

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
BASELINE_DIR = HERE / "golden" / "extract"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"


def corpus_path(sample_id: str) -> Path:
    for item in CORPUS:
        if item["id"] == sample_id:
            return Path(item["path"])
    raise SystemExit("未知语料 id：%s" % sample_id)


def file_entries(sample_id: str) -> list[tuple[str, int]]:
    """从 golden_sqfs 的 TSV 里取非目录条目（path, size），按 path 排序。"""
    tsv = HERE / "golden" / "sqfs" / ("%s.tsv" % sample_id)
    if not tsv.exists():
        raise SystemExit("缺 %s（先跑 python tests/golden_sqfs.py --write）" % tsv)
    rows = []
    for line in tsv.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "f":
            rows.append((parts[1], int(parts[2])))
    return rows


def pick_samples(entries: list[tuple[str, int]], per_image: int) -> list[tuple[str, int]]:
    """确定性抽样：头部若干个（多为 fragment 小文件）+ 均匀分布 + 最大的几个。

    覆盖三类路径：① fragment 尾部（小文件）② 多数据块（大文件）③ 中间尺寸。
    """
    if not entries:
        return []
    picked: list[tuple[str, int]] = []
    seen: set[str] = set()

    def take(item: tuple[str, int]) -> None:
        if item[0] not in seen and len(picked) < per_image:
            seen.add(item[0])
            picked.append(item)

    for item in entries[:4]:
        take(item)
    step = max(1, len(entries) // 8)
    for i in range(0, len(entries), step):
        take(entries[i])
    for item in sorted(entries, key=lambda x: -x[1])[:4]:
        take(item)
    return picked


def baseline_1x(image: Path, rel: str, workdir: Path) -> tuple[str, int]:
    """用 1.x 的 sqfs_extract 提取 → (sha256, size)。"""
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from file_system import stalker_fs  # noqa: PLC0415

    out_dir = workdir / "ref"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    got = stalker_fs.sqfs_extract(str(image), str(out_dir),
                                  [{"path": rel, "is_dir": False}])
    target = out_dir / rel
    if got in (0, None) or not target.exists():
        raise RuntimeError("1.x 未能提取 %s（返回 %r，%s）"
                           % (rel, got, stalker_fs.sqfs_extract_last_error))
    data = target.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def candidate_2x_extract(image: Path, rel: str, out_file: Path) -> tuple[str, int]:
    proc = subprocess.run([str(CLI), "--extract", str(image), rel, str(out_file)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    if proc.returncode != 0:
        raise RuntimeError(text[:200])
    data = out_file.read_bytes()
    return hashlib.sha256(data).hexdigest(), len(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="提取路径黄金语料差分")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    parser.add_argument("--id", default="", help="只处理 id 含该子串的样本")
    parser.add_argument("--per-image", type=int, default=12, help="每个镜像抽几个文件")
    args = parser.parse_args()

    ids = [i for i in SQFS_IDS if not args.id or args.id in i]
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    failures = 0
    compared = 0
    skipped = 0

    with tempfile.TemporaryDirectory(prefix="xrkit_golden_") as tmp:
        workdir = Path(tmp)
        for sample_id in ids:
            image = corpus_path(sample_id)
            manifest_path = BASELINE_DIR / ("%s.json" % sample_id)
            print("[%s]" % sample_id)

            if not image.exists():
                print("  -- 样本缺失，跳过")
                skipped += 1
                continue

            entries = file_entries(sample_id)
            picked = pick_samples(entries, args.per_image)

            if args.write:
                records = []
                for rel, size in picked:
                    try:
                        digest, got = baseline_1x(image, rel, workdir)
                    except Exception as exc:  # noqa: BLE001
                        print("  !! %s：%s" % (rel, exc))
                        failures += 1
                        continue
                    if got != size:
                        print("  !! %s：1.x 提取 %d 字节，列表写的是 %d" % (rel, got, size))
                        failures += 1
                        continue
                    records.append({"path": rel, "size": got, "sha256": digest})
                    print("  ok %-52s %9d B" % (rel[:52], got))
                if not records:
                    print("  !! 一个都没成，不写空基线")
                    failures += 1
                    continue
                manifest_path.write_text(
                    json.dumps({"id": sample_id, "image": str(image), "files": records},
                               indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
                print("  已写基线 %d 条 → %s" % (len(records), manifest_path.relative_to(ROOT)))
                compared += 1
                continue

            if not manifest_path.exists():
                print("  !! 缺基线文件（先跑 --write）")
                failures += 1
                continue
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            bad = 0
            for record in manifest["files"]:
                out_file = workdir / "ours.bin"
                try:
                    digest, size = candidate_2x_extract(image, record["path"], out_file)
                except Exception as exc:  # noqa: BLE001
                    print("  ✗ %s：提取失败 %s" % (record["path"], exc))
                    bad += 1
                    continue
                if size != record["size"] or digest != record["sha256"]:
                    print("  ✗ %s：大小 %d→%d 或 sha256 不符" % (record["path"], record["size"], size))
                    bad += 1
            if bad:
                failures += 1
            else:
                print("  ✓ %d 个文件逐字节一致" % len(manifest["files"]))
            compared += 1

    print("")
    verdict = "FAIL" if failures else ("PARTIAL" if skipped else "PASS")
    print("GOLDEN-EXTRACT: %s (compared=%d skipped=%d)" % (verdict, compared, skipped))
    if failures:
        return 1
    return 2 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
