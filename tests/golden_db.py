#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DB 六格式的黄金语料差分：2.x（C++） vs 1.x（Python）。

基线由 **1.x 生成**，包含三件事：
  ① `pack_db` 的**产物字节**（连 LZHUF 编码 + scrambler 一起逐字节对拍）；
  ② `unpack_db` 的条目表（路径/offset/size_real/size_comp/is_dir/use_lzhuf）；
  ③ `extract_file` 的内容（sha256）。

    python tests/golden_db.py --write     # 生成基线（用 1.x）
    python tests/golden_db.py             # 对拍（跑 2.x CLI）
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from collect_golden import CORPUS  # noqa: E402

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
OUT = HERE / "golden" / "db"
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
FORMAT_KEYS = ["xdb", "2947ru", "2947ww", "2945", "2215", "11xx"]

# 合成语料：覆盖 空文件 / 纯 ASCII / 西里尔名（cp1251）/ 二进制 / 子目录
SYNTH_FILES = {
    "readme.txt": b"hello stalker\n",
    "data/nested.bin": bytes(range(256)) * 4,
    "data/empty.dat": b"",
    "bin/blob.dat": bytes([0, 1, 2, 255]) * 100,
    "папка/файл_тест.txt": "привет мир\n".encode("cp1251"),
}


def corpus_path(sample_id: str) -> Path:
    for item in CORPUS:
        if item["id"] == sample_id:
            return Path(item["path"])
    raise SystemExit("未知语料 id：%s" % sample_id)


def stalker_fs():
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from file_system import stalker_fs as sf  # noqa: PLC0415

    return sf


def build_synth_dir(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    for rel, data in SYNTH_FILES.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def walk_items(root: Path):
    """与 CLI 的 --pack-db 用同一套收集规则：相对路径（'/' 分隔）+ 目录项，按路径排序。"""
    items = []
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            rel = os.path.relpath(os.path.join(dirpath, d), root).replace(os.sep, "/")
            items.append((rel, b"", True))
        for f in filenames:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as fh:
                items.append((rel, fh.read(), False))
    items.sort(key=lambda t: t[0])
    return items


def entry_row(e: dict) -> str:
    return "%s\t%s\t%d\t%d\t%d\t%d" % (
        "d" if e.get("is_dir") else "f", e["path"], int(e.get("size_real") or 0),
        int(e.get("size_comp") or 0), int(e.get("offset") or 0),
        1 if e.get("use_lzhuf") else 0)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_cli(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def pick_for_extract(entries: list[dict], count: int = 6) -> list[dict]:
    files = [e for e in entries if not e.get("is_dir")]
    if not files:
        return []
    picked, seen = [], set()

    def take(e):
        if e["path"] not in seen and len(picked) < count:
            seen.add(e["path"])
            picked.append(e)

    for e in files[:3]:
        take(e)
    for e in sorted(files, key=lambda x: -(x.get("size_real") or 0))[:3]:
        take(e)
    return picked


# ── 基线 ────────────────────────────────────────────────────────────────
def write_baselines() -> int:
    sf = stalker_fs()
    OUT.mkdir(parents=True, exist_ok=True)
    denied = 0
    failures = 0

    # ① 合成六格式
    with tempfile.TemporaryDirectory(prefix="xrkit_db_") as td:
        root = Path(td) / "in"
        build_synth_dir(root)
        items = walk_items(root)
        print("[synth] %d 个条目（含目录）" % len(items))
        for key in FORMAT_KEYS:
            db = sf.pack_db([(p, c, d) for p, c, d in items], key)
            (OUT / ("synth_%s.db" % key)).write_bytes(db)
            entries = sf.unpack_db(db, key)
            rec = {
                "kind": "synth",
                "format": key,
                "db_sha256": sha256_bytes(db),
                "entries": [entry_row(e) for e in entries],
                "extract": [
                    {"path": e["path"], "sha256": sha256_bytes(sf.extract_file(db, e) or b""),
                     "size": len(sf.extract_file(db, e) or b"")}
                    for e in entries if not e.get("is_dir")
                ],
            }
            (OUT / ("synth_%s.json" % key)).write_text(
                json.dumps(rec, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print("  ok %-8s db=%d B 条目=%d" % (key, len(db), len(entries)))

    # ② 真机 DB：对每种格式解一遍，只记自洽的
    for rid in ("gs-db-base", "soc-db0"):
        path = corpus_path(rid)
        if not path.exists():
            # 实测 2026-09-27：`soc-db0` 的源树（SoC_Source\…）在磁盘上不见了 ⇒ 那是**环境缺失**，
            # 不是"DB 格式不一致" ⇒ 记 denied（可见、进"未覆盖"，退码 2），不判红。
            print("[%s] 样本缺失（源不可达，记未覆盖）" % rid)
            denied += 1
            continue
        raw = path.read_bytes()
        print("[%s] %d B" % (rid, len(raw)))
        for key in FORMAT_KEYS:
            entries = sf.unpack_db(raw, key)
            if not entries or not sf._entries_self_consistent(raw, entries):
                continue
            picks = pick_for_extract(entries)
            rec = {
                "kind": "real",
                "id": rid,
                "image": str(path),
                "format": key,
                "entries": [entry_row(e) for e in entries],
                "extract": [
                    {"path": e["path"], "sha256": sha256_bytes(sf.extract_file(raw, e) or b""),
                     "size": len(sf.extract_file(raw, e) or b"")}
                    for e in picks
                ],
            }
            (OUT / ("%s_%s.json" % (rid, key))).write_text(
                json.dumps(rec, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print("  ok %-8s 条目=%d（自洽）" % (key, len(entries)))

    print("已写基线到 %s" % OUT.relative_to(ROOT))
    print("GOLDEN-DB: %s (denied=%d)" % ("FAIL" if failures else ("PARTIAL" if denied else "PASS"), denied))
    if failures:
        return 1
    return 2 if denied else 0


# ── 对拍 ────────────────────────────────────────────────────────────────
def compare_entries(expect_rows: list[str], got_rows: list[str], label: str) -> int:
    exp = {r.split("\t")[1]: r for r in expect_rows}
    got = {r.split("\t")[1]: r for r in got_rows}
    missing = [p for p in exp if p not in got]
    extra = [p for p in got if p not in exp]
    diff = [p for p in exp if p in got and exp[p] != got[p]]
    if not (missing or extra or diff):
        print("  ✓ %s：%d 条条目逐字段一致" % (label, len(exp)))
        return 0
    print("  ✗ %s：缺失 %d / 多余 %d / 不一致 %d" % (label, len(missing), len(extra), len(diff)))
    for p in (missing + extra + diff)[:6]:
        print("      基线 %r" % exp.get(p))
        print("      候选 %r" % got.get(p))
    return 1


def check_baselines() -> int:
    failures = 0
    # 环境缺失（源树不在盘上）记这里：可见、进"未覆盖"，**不判红**（与 golden_text_decode 同一口径）
    denied = 0
    if not CLI.exists():
        raise SystemExit("找不到 %s" % CLI)

    # ① 合成：pack 字节 + 条目 + 提取
    with tempfile.TemporaryDirectory(prefix="xrkit_db_") as td:
        root = Path(td) / "in"
        outdb = Path(td) / "ours.db"
        outbin = Path(td) / "ours.bin"
        for key in FORMAT_KEYS:
            base_path = OUT / ("synth_%s.db" % key)
            rec_path = OUT / ("synth_%s.json" % key)
            if not base_path.exists() or not rec_path.exists():
                print("[synth/%s] !! 缺基线" % key)
                failures += 1
                continue
            rec = json.loads(rec_path.read_text(encoding="utf-8"))
            print("[synth/%s]" % key)
            case_bad = 0
            build_synth_dir(root)
            # ★★ 2026-10-10（P17-48，04 F-06 第二波）：本脚本在**同一个 ours.<key>.db** 上逐格式反复写
            #   （修前静默截断，现在是"存在即拒"）⇒ 必须显式声明"我知道要覆盖"。
            rc, out = run_cli(["--pack-db", str(outdb), key, str(root), "--force"])
            if rc != 0:
                print("  ✗ pack 失败：%s" % out.strip())
                failures += 1
                continue
            ours = outdb.read_bytes()
            base = base_path.read_bytes()
            if ours == base:
                print("  ✓ pack 产物逐字节一致（%d B）" % len(ours))
            else:
                print("  ✗ pack 产物的字节不同：基线 %d B / 候选 %d B" % (len(base), len(ours)))
                failures += 1

            rc, out = run_cli(["--dump", str(base_path), key])
            if rc != 0:
                print("  ✗ dump 失败：%s" % out.strip())
                failures += 1
                continue
            got_rows = [ln for ln in out.splitlines() if not ln.startswith("DUMP")]
            failures += compare_entries(rec["entries"], got_rows, "条目表")

            for item in rec["extract"]:
                rc, out = run_cli(["--extract", str(base_path), item["path"], str(outbin), key, "--force"])
                if rc != 0:
                    print("  ✗ 提取 %s 失败：%s" % (item["path"], out.strip()))
                    failures += 1
                    case_bad += 1
                    continue
                got = outbin.read_bytes()
                if sha256_bytes(got) != item["sha256"]:
                    print("  ✗ 提取 %s 内容不同（%d vs %d 字节）"
                          % (item["path"], item["size"], len(got)))
                    failures += 1
                    case_bad += 1
            if case_bad == 0:
                print("  ✓ 提取 %d 个文件逐字节一致" % len(rec["extract"]))

    # ② 真机 DB
    for rec_path in sorted(OUT.glob("*.json")):
        rec = json.loads(rec_path.read_text(encoding="utf-8"))
        if rec.get("kind") != "real":
            continue
        image = Path(rec["image"])
        key = rec["format"]
        print("[%s/%s]" % (rec["id"], key))
        if not image.exists():
            # 与上面那处同因（2026-09-27 实测：`soc-db0` 的源树在磁盘上不见了）：
            # **环境缺失**记 denied（可见、进"未覆盖"，退码 2），不判红。
            print("  -- 样本缺失（源不可达，记未覆盖）")
            denied += 1
            continue
        rc, out = run_cli(["--dump", str(image), key])
        if rc != 0:
            print("  ✗ dump 失败：%s" % out.strip())
            failures += 1
            continue
        got_rows = [ln for ln in out.splitlines() if not ln.startswith("DUMP")]
        failures += compare_entries(rec["entries"], got_rows, "条目表")

        with tempfile.TemporaryDirectory(prefix="xrkit_dbx_") as td:
            outbin = Path(td) / "ours.bin"
            ok = 0
            bad = 0
            for item in rec["extract"]:
                rc, out = run_cli(["--extract", str(image), item["path"], str(outbin), key, "--force"])
                if rc != 0:
                    print("  ✗ 提取 %s 失败：%s" % (item["path"], out.strip()))
                    failures += 1
                    bad += 1
                    continue
                if sha256_bytes(outbin.read_bytes()) != item["sha256"]:
                    print("  ✗ 提取 %s 内容不同" % item["path"])
                    failures += 1
                    bad += 1
                    continue
                ok += 1
            if bad == 0:
                print("  ✓ 提取 %d 个文件逐字节一致" % ok)

    print("")
    print("GOLDEN-DB: %s (denied=%d)"
          % ("FAIL" if failures else ("PARTIAL" if denied else "PASS"), denied))
    if failures:
        return 1
    return 2 if denied else 0


def main() -> int:
    parser = argparse.ArgumentParser(description="DB 六格式黄金语料差分")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    args = parser.parse_args()
    return write_baselines() if args.write else check_baselines()


if __name__ == "__main__":
    sys.exit(main())
