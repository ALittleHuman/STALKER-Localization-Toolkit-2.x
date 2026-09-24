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


# ── 整树导出（对照表 ⑥6.7~6.10/6.14）──────────────────────────────────
#
# 逐文件 `--extract` 只证明"解压对了"；**导出到磁盘**那一条路还有三件事要证：
#   · 6.7 路径越界（真实镜像里没有 `..` 条目，所以合成夹具那一节单独造）
#   · 6.8 空文件写出 0 字节（1.x 的旧代码把空文件当"解不出来"）
#   · 6.9 原子写：不留 `.part`、不写坏既有文件
# 判据与逐文件那条一样：**整棵树按路径 + 大小 + sha256 逐项对拍**。

def tree_baseline_1x(image: Path, rels: list[str], workdir: Path) -> list[dict]:
    """用 1.x 的 sqfs_extract **一次**导出多个条目 → 树的基线记录。"""
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from file_system import stalker_fs  # noqa: PLC0415

    out_dir = workdir / "tree_ref"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    got = stalker_fs.sqfs_extract(str(image), str(out_dir),
                                  [{"path": r, "is_dir": False} for r in rels])
    if got in (0, None):
        raise RuntimeError("1.x 整树导出失败（返回 %r，%s）"
                           % (got, stalker_fs.sqfs_extract_last_error))
    records = []
    for rel in rels:
        target = out_dir / rel.replace("\\", "/")
        if not target.exists():
            raise RuntimeError("1.x 没写出 %s（%s）" % (rel, stalker_fs.sqfs_extract_last_error))
        data = target.read_bytes()
        records.append({"path": rel, "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    return records


def tree_pick(entries: list[tuple[str, int]], picked: list[tuple[str, int]], limit: int = 6) -> list[str]:
    """整树对拍的样本：抽样结果里取前 limit 个，并**优先塞一个 0 字节文件**
    （6.8 的判据在真实镜像上也要走到，不能只靠合成夹具）。"""
    chosen: list[str] = []
    zero = [p for p, size in entries if size == 0]
    if zero:
        chosen.append(zero[0])
    for rel, _size in picked:
        if len(chosen) >= limit:
            break
        if rel not in chosen:
            chosen.append(rel)
    return chosen


def candidate_2x_tree(image: Path, out_dir: Path, rels: list[str]) -> str:
    """跑 2.x 的 `--unpack-tree --only`。返回 CLI 输出（失败时由调用方报出）。"""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    list_file = out_dir.parent / "only.txt"
    list_file.write_text("\n".join(rels) + "\n", encoding="utf-8")
    proc = subprocess.run([str(CLI), "--unpack-tree", str(image), str(out_dir),
                           "--only", str(list_file)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace").strip()


def compare_tree(out_dir: Path, records: list[dict]) -> list[str]:
    """逐项对拍：路径集合必须**恰好**一致，且每项大小与 sha256 相同。"""
    problems = []
    want = {r["path"]: r for r in records}
    got = {p.relative_to(out_dir).as_posix(): p
           for p in out_dir.rglob("*") if p.is_file()}
    for rel in sorted(set(want) - set(got)):
        problems.append("少了 %s" % rel)
    for rel in sorted(set(got) - set(want)):
        problems.append("多了 %s" % rel)
    for rel in sorted(set(want) & set(got)):
        data = got[rel].read_bytes()
        if len(data) != want[rel]["size"]:
            problems.append("%s：大小 %d→%d" % (rel, want[rel]["size"], len(data)))
        elif hashlib.sha256(data).hexdigest() != want[rel]["sha256"]:
            problems.append("%s：sha256 不符" % rel)
    stray = list(out_dir.rglob("*.part"))
    if stray:
        problems.append("留下临时文件：%s" % ", ".join(p.name for p in stray))
    return problems


def candidate_2x_tree_all(cli: Path, image: Path, out_dir: Path) -> tuple[int, str]:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    proc = subprocess.run([str(cli), "--unpack-tree", str(image), str(out_dir), "--all"],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace").strip()


def synthetic_tree_checks(cli: Path, workdir: Path) -> list[str]:
    """合成夹具：空文件 + 嵌套目录 → 封包 → 整树导出 → 逐字节回比。

    真实镜像里没有 `..` 条目、"选中集为空"也不方便造，所以这几条判据在合成夹具上做：
      · 6.8 0 字节文件必须写出 0 字节文件
      · 6.10 **空清单 ≠ 全部**：1.x 的 `if not files:` 会退化成整镜像导出，2.0 必须拒绝
      · 6.9 不留 `.part`、不写坏既有文件
    """
    problems: list[str] = []
    src = workdir / "syn_src"
    if src.exists():
        shutil.rmtree(src)
    (src / "sub" / "deep").mkdir(parents=True)
    (src / "empty.bin").write_bytes(b"")                             # 6.8
    (src / "small.bin").write_bytes(b"hello stalker\n")
    (src / "sub" / "nested.bin").write_bytes(bytes(range(256)) * 3)
    (src / "sub" / "deep" / "x.bin").write_bytes(b"\x00" * 4096)

    image = workdir / "syn.sqfs"
    proc = subprocess.run([str(cli), "--pack-sqfs", str(image), str(src)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if proc.returncode != 0:
        return ["合成封包失败：%s" % proc.stdout.decode("utf-8", errors="replace")[:200]]

    # ① 整镜像导出（All）：文件必须逐个字节相同（目录由文件父目录顺带建出）
    out_all = workdir / "syn_all"
    rc, out = candidate_2x_tree_all(cli, image, out_all)
    if rc != 0:
        problems.append("整镜像导出失败：%s" % out[:200])
    else:
        want = {p.relative_to(src).as_posix(): p.read_bytes()
                for p in src.rglob("*") if p.is_file()}
        got = {p.relative_to(out_all).as_posix(): p.read_bytes()
               for p in out_all.rglob("*") if p.is_file()}
        if set(want) != set(got):
            problems.append("整镜像导出路径集不符：少 %s，多 %s"
                            % (sorted(set(want) - set(got)), sorted(set(got) - set(want))))
        for rel in sorted(set(want) & set(got)):
            if want[rel] != got[rel]:
                problems.append("%s：内容不同（%d vs %d 字节）"
                                % (rel, len(want[rel]), len(got[rel])))
        empty = out_all / "empty.bin"
        if not empty.exists() or empty.stat().st_size != 0:
            problems.append("6.8 空文件没写成 0 字节文件")

    # ② 空清单必须被拒（6.10）
    out_none = workdir / "syn_none"
    empty_list = workdir / "empty_list.txt"
    empty_list.write_text("", encoding="utf-8")
    if out_none.exists():
        shutil.rmtree(out_none)
    proc = subprocess.run([str(cli), "--unpack-tree", str(image), str(out_none),
                           "--only", str(empty_list)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode == 0:
        problems.append("6.10 空清单居然成功了（1.x 会退化成整镜像导出）")
    elif "拒绝" not in text:
        problems.append("6.10 空清单被拒但没说清理由：%s" % text.strip()[:200])
    if out_none.exists() and any(out_none.rglob("*")):
        problems.append("6.10 空清单被拒却还是写了东西")

    # ③ 选中集只含目录 ⇒ 一个文件都不该写出（1.x 在这里静默返回 0）
    out_dirs = workdir / "syn_dirs"
    dir_list = workdir / "dir_list.txt"
    dir_list.write_text("sub\n", encoding="utf-8")
    if out_dirs.exists():
        shutil.rmtree(out_dirs)
    proc = subprocess.run([str(cli), "--unpack-tree", str(image), str(out_dirs),
                           "--only", str(dir_list)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    files = [p for p in out_dirs.rglob("*") if p.is_file()] if out_dirs.exists() else []
    if files:
        problems.append("选中集只有目录却写出了文件：%s" % [p.name for p in files])
    if proc.returncode == 0 and "files=0" not in text:
        problems.append("选中集只有目录的收尾行不实：%s" % text.strip()[:200])

    # ④ 选了镜像里没有的条目 ⇒ 单项失败、退出码非 0、理由可见
    out_ghost = workdir / "syn_ghost"
    ghost_list = workdir / "ghost_list.txt"
    ghost_list.write_text("no/such/file.bin\n", encoding="utf-8")
    if out_ghost.exists():
        shutil.rmtree(out_ghost)
    proc = subprocess.run([str(cli), "--unpack-tree", str(image), str(out_ghost),
                           "--only", str(ghost_list)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode == 0:
        problems.append("选了不存在的条目却报成功")
    elif "fail" not in text:
        problems.append("单项失败没打印 fail 行：%s" % text.strip()[:200])

    return problems


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
                # 整树导出的基线：1.x **一次**导出多个条目（含一个 0 字节文件），
                # 记下整棵树的路径/大小/sha256。
                tree_records: list[dict] = []
                tree_rels = tree_pick(entries, picked)
                try:
                    tree_records = tree_baseline_1x(image, tree_rels, workdir)
                    print("  ok 整树 %d 项（1.x 一次导出）" % len(tree_records))
                except Exception as exc:  # noqa: BLE001
                    print("  !! 整树基线失败：%s" % exc)
                    failures += 1
                    continue
                manifest_path.write_text(
                    json.dumps({"id": sample_id, "image": str(image), "files": records,
                                "tree": tree_records},
                               indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
                print("  已写基线 %d 条 + 整树 %d 项 → %s"
                      % (len(records), len(tree_records), manifest_path.relative_to(ROOT)))
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

            # 整树导出：同样的条目，但走"选中集一次导出 + 落盘"这条路
            tree_records = manifest.get("tree") or []
            if not tree_records:
                print("  !! 基线里没有整树记录（用 --write 重新生成）")
                failures += 1
                continue
            out_dir = workdir / "tree_ours"
            rc, out = candidate_2x_tree(image, out_dir, [r["path"] for r in tree_records])
            tree_bad = compare_tree(out_dir, tree_records)
            if rc != 0:
                tree_bad.append("CLI 退出码 %d：%s" % (rc, out[:200]))
            for problem in tree_bad:
                print("  ✗ 整树 %s" % problem)
            if tree_bad:
                failures += 1
            else:
                empty_count = sum(1 for r in tree_records if r["size"] == 0)
                print("  ✓ 整树 %d 项逐字节一致（其中 0 字节条目 %d 个；无 `.part` 残留）"
                      % (len(tree_records), empty_count))

            compared += 1

        # 合成夹具：真实镜像造不出的那几条判据（6.8/6.10/单项失败）
        syn_problems = synthetic_tree_checks(CLI, workdir)
        for problem in syn_problems:
            print("  ✗ 合成夹具 %s" % problem)
        if syn_problems:
            failures += 1
        else:
            print("  ✓ 合成夹具：空文件 0 字节、空清单被拒、单项失败可见")

    print("")
    verdict = "FAIL" if failures else ("PARTIAL" if skipped else "PASS")
    print("GOLDEN-EXTRACT: %s (compared=%d skipped=%d)" % (verdict, compared, skipped))
    if failures:
        return 1
    return 2 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
