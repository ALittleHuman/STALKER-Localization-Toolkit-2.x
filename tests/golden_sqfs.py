#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SquashFS 条目基线：用 **1.x 的实现**生成，再用 **2.x 的 CLI** 逐条对拍。

为什么基线必须由 1.x 生成：2.0 的目标是"与 1.x 行为对齐"，基线就是 1.x 的输出本身，
不是我以为应该长什么样。

    python tests/golden_sqfs.py --write                 # 生成/更新基线（用 1.x）
    python tests/golden_sqfs.py                         # 对拍（跑 2.x CLI）
    python tests/golden_sqfs.py --id nlc-sqz-wasteland # 只跑一个
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from collect_golden import CORPUS  # noqa: E402

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
BASELINE_DIR = HERE / "golden" / "sqfs"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# 这些是明文 SquashFS（加密的 sq_base 走插件解密路径，不在这一组）
SQFS_IDS = [
    "nlc-sqz-wasteland",
    "nlc-sqz-autumn",
    "nlc-sq-sounds",
    "nlc-sq-meshes",
    "nlc-sqzy-patch",
    "nlc-sq-levels",
    "nlc-sq-textures",
]


def corpus_path(sample_id: str) -> Path:
    for item in CORPUS:
        if item["id"] == sample_id:
            return Path(item["path"])
    raise SystemExit("未知语料 id：%s" % sample_id)


def baseline_1x(image: Path) -> tuple[list[str], str]:
    """用 1.x 的 stalker_fs 列出条目。返回 (行, 备注)。"""
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from file_system import stalker_fs  # noqa: PLC0415

    stalker_fs.sqfs_last_error = None
    entries = stalker_fs.sqfs_list(str(image))
    if entries is None:
        return [], "1.x sqfs_list 返回 None（%s）" % (stalker_fs.sqfs_last_error or "无原因")
    rows = []
    for e in sorted(entries, key=lambda x: x["path"]):
        rows.append("%s\t%s\t%d" % ("d" if e.get("is_dir") else "f", e["path"],
                                    int(e.get("size_real") or 0)))
    note = "1.x 备注：%s" % (stalker_fs.sqfs_last_error or "无")
    return rows, note


def candidate_2x(image: Path) -> list[str]:
    if not CLI.exists():
        raise SystemExit("找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
    proc = subprocess.run([str(CLI), "--dump", str(image)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    rows = []
    for line in text.splitlines():
        if line.startswith("DUMP ok") or line.startswith("DUMP error"):
            continue
        rows.append(line.rstrip("\r"))
    return rows


def parse_rows(rows: list[str]) -> dict[str, tuple[str, int]]:
    out: dict[str, tuple[str, int]] = {}
    for row in rows:
        parts = row.split("\t")
        if len(parts) != 3:
            continue
        kind, path, size = parts[0], parts[1], parts[2]
        try:
            out[path] = (kind, int(size))
        except ValueError:
            continue
    return out


def compare(base: dict, cand: dict) -> int:
    missing = [p for p in base if p not in cand]
    extra = [p for p in cand if p not in base]
    mismatched = [p for p in base if p in cand and base[p] != cand[p]]
    print("  基线 %d 条 / 候选 %d 条" % (len(base), len(cand)))
    if not (missing or extra or mismatched):
        print("  ✓ 逐条一致（路径、类型、大小）")
        return 0
    print("  ✗ 缺失 %d / 多余 %d / 不一致 %d" % (len(missing), len(extra), len(mismatched)))
    for label, items in (("缺失", missing), ("多余", extra), ("不一致", mismatched)):
        for p in items[:8]:
            if label == "不一致":
                print("      %s: 基线 %s  候选 %s" % (label, base[p], cand[p]))
            else:
                print("      %s: %s" % (label, p))
        if len(items) > 8:
            print("      … 其余 %d 条同类" % (len(items) - 8))
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="SquashFS 条目基线与对拍")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    parser.add_argument("--id", default="", help="只处理 id 含该子串的样本")
    args = parser.parse_args()

    ids = [i for i in SQFS_IDS if not args.id or args.id in i]
    BASELINE_DIR.mkdir(parents=True, exist_ok=True)
    failures = 0
    compared = 0
    skipped = 0

    for sample_id in ids:
        image = corpus_path(sample_id)
        target = BASELINE_DIR / ("%s.tsv" % sample_id)
        print("[%s] %s" % (sample_id, image))
        if not image.exists():
            print("  -- 样本缺失，跳过")
            skipped += 1
            continue

        if args.write:
            rows, note = baseline_1x(image)
            if not rows:
                print("  !! %s" % note)
                failures += 1
                continue
            target.write_text("\n".join(rows) + "\n", encoding="utf-8")
            print("  已写基线 %d 条 → %s" % (len(rows), target.relative_to(ROOT)))
            print("  %s" % note)
            compared += 1
            continue

        if not target.exists():
            print("  !! 缺基线文件（先跑 --write）")
            failures += 1
            continue
        base_rows = target.read_text(encoding="utf-8").splitlines()
        failures += compare(parse_rows(base_rows), parse_rows(candidate_2x(image)))
        compared += 1

    print("")
    # skipped 单独暴露：跳过不等于通过（门禁据此记未覆盖 / release 档 FAIL）
    verdict = "FAIL" if failures else ("PARTIAL" if skipped else "PASS")
    print("GOLDEN-SQFS: %s (compared=%d skipped=%d)" % (verdict, compared, skipped))
    if failures:
        return 1
    return 2 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
