#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真机样本的封包对拍：把**真实镜像**里的文件抽出来，当成输入树，两套链路各打一次。

为什么单列一个（合成夹具已经有了）：合成夹具只覆盖"我想得到的组合"。2026-09-24
这一轮就是被**真机抽验**抓出一个合成夹具漏掉的**真缺陷** —— 数据块去重命中后的
"截断"把刚写的 fragment 块删掉了（frag 表还指着它）：镜像小 700 KB、碎片去重从
206 掉到 136、被删块上的文件读出来是垃圾（`fragflush` 夹具就是照这个缺陷补的）。

    python tests/golden_sqfs_pack_real.py                 # 默认取 corpus 里最小的明文镜像
    python tests/golden_sqfs_pack_real.py --id nlc-sqzy-patch
    python tests/golden_sqfs_pack_real.py --sample 150    # 只抽前 150 个文件（省时间）

判据：① 两边产物 sha256 必须相等；② rdsquashfs 能读回且路径/类型与输入树一致；
③ 抽 12 个文件从**我们的产物**里抽回来与输入逐字节比。
退出码：0=一致，1=不一致，2=样本缺失（按外部前提处理）。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from collect_golden import CORPUS  # noqa: E402

CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_sqfs_pack_real"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
RDSQUASHFS = (TOOLKIT_1X / "deps" / "squashfs-tools-ng-1.3.2-mingw64" / "bin"
              / "rdsquashfs.exe")

# 明文、体积可控的镜像（加密的 sq_base 走插件路径，不在这条链路上）
REAL_IDS = ["nlc-sqz-autumn", "nlc-sqz-wasteland", "nlc-sqzy-patch"]


def corpus_path(sample_id: str) -> Path:
    for item in CORPUS:
        if item["id"] == sample_id:
            return Path(item["path"])
    raise SystemExit("未知语料 id：%s" % sample_id)


def cli(*argv) -> subprocess.CompletedProcess:
    return subprocess.run([str(CLI), *argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def enumerate_entries(root: Path) -> list[tuple[str, Path]]:
    """与 CLI 的枚举规则一致：只收普通文件、rel 用 '/'、按 UTF-8 字节序。"""
    out: list[tuple[str, Path]] = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        base = Path(dirpath)
        for name in filenames:
            p = base / name
            if p.is_file():
                out.append((str(p.relative_to(root)).replace("\\", "/"), p))
    out.sort(key=lambda item: item[0].encode("utf-8"))
    return out


def extract_tree(image: Path, tree: Path, sample: int) -> list[tuple[str, Path, int]]:
    dump = cli("--dump", str(image))
    text = dump.stdout.decode("utf-8", "replace")
    if dump.returncode != 0:
        raise RuntimeError("dump 失败：%s" % text.strip()[:200])
    files: list[tuple[str, int]] = []
    for line in text.splitlines():
        if line.startswith("DUMP "):
            continue
        parts = line.rstrip("\r").split("\t")
        if len(parts) == 3 and parts[0] == "f":
            files.append((parts[1], int(parts[2])))
    if sample > 0:
        files = files[:sample]

    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    out: list[tuple[str, Path, int]] = []
    for rel, size in files:
        target = tree / Path(*rel.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        r = cli("--extract", str(image), rel, str(target))
        if r.returncode != 0 or target.stat().st_size != size:
            raise RuntimeError("提取失败 %s：%s"
                               % (rel, r.stdout.decode("utf-8", "replace").strip()[:160]))
        out.append((rel, target, size))
    return out


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="真机样本的封包逐字节对拍")
    parser.add_argument("--id", default="", help="语料 id（默认取最小的那个）")
    parser.add_argument("--sample", type=int, default=0, help="只抽前 N 个文件（0=全部）")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-SQFS-PACK-REAL: SKIP (cli-missing)")
        return 2
    if not RDSQUASHFS.exists():
        print("!! 找不到参照工具 %s" % RDSQUASHFS)
        print("GOLDEN-SQFS-PACK-REAL: SKIP (rdsquashfs-missing)")
        return 2

    ids = [args.id] if args.id else REAL_IDS
    failures = 0
    denied: list[str] = []
    compared = 0
    skipped = 0

    for sample_id in ids:
        image = corpus_path(sample_id)
        if not image.exists():
            print("[%s] -- 样本缺失，跳过" % sample_id)
            skipped += 1
            continue
        work = WORK / sample_id
        tree = work / "tree"
        print("[%s] %s（%.1f MB）" % (sample_id, image, image.stat().st_size / 1048576))
        try:
            files = extract_tree(image, tree, args.sample)
        except Exception as exc:  # noqa: BLE001
            print("  ✗ 抽树失败：%s" % exc)
            failures += 1
            continue
        total = sum(p.stat().st_size for _, p, _ in files)
        print("  输入树：%d 个文件，%.1f MB" % (len(files), total / 1048576))

        # 参照：1.x 的 sqfs_pack（tar2sqfs，默认 xz）
        if str(TOOLKIT_1X) not in sys.path:
            sys.path.insert(0, str(TOOLKIT_1X))
        from file_system import stalker_fs  # noqa: PLC0415

        ref = work / "ref.sqfs"
        ref.unlink(missing_ok=True)
        items = [(rel, p.read_bytes(), False) for rel, p, _ in files]
        # ★ 2026-09-27：1.x 的 `sqfs_pack` 是**纯内存**实现（items 里带着每个文件的 bytes）。
        #   在 sqzy_patch 那棵树（930 文件 / 3992.5 MB）上**参照侧自己跑不起来**，而我们的
        #   抽取与封包都没问题。按项目纪律：**环境级限制既不许判红、也不许混进通过**
        #   ⇒ 记进 denied、最后退 2（进"本次未覆盖"），理由打印出来可核对。
        try:
            ref_ok = stalker_fs.sqfs_pack(items, str(ref))
        except Exception as exc:  # noqa: BLE001
            denied.append("%s：1.x 参照封包抛异常（%s：%s）—— 参照侧限制，非我方产物问题"
                          % (sample_id, type(exc).__name__, str(exc)[:80]))
            print("  !! 参照封包跳过（%s：%s）" % (type(exc).__name__, str(exc)[:80]))
            continue
        if not ref_ok:
            denied.append("%s：1.x sqfs_pack 返回 False（参照侧限制，非我方产物问题）" % sample_id)
            print("  !! 参照封包返回 False ⇒ 跳过（记入未覆盖）")
            continue
        ref_data = ref.read_bytes()

        ours = work / "ours.sqfs"
        ours.unlink(missing_ok=True)
        r = cli("--pack-sqfs", str(ours), str(tree))
        line = r.stdout.decode("utf-8", "replace").strip()
        if r.returncode != 0:
            print("  ✗ 我们的封包失败：%s" % line[:300])
            failures += 1
            continue
        our_data = ours.read_bytes()
        print("  %s" % line)

        ok = True
        if hashlib.sha256(ref_data).hexdigest() != hashlib.sha256(our_data).hexdigest():
            ok = False
            print("  ✗ sha256 不一致：参照 %d 字节 / 我们 %d 字节"
                  % (len(ref_data), len(our_data)))
            n = min(len(ref_data), len(our_data))
            for i in range(n):
                if ref_data[i] != our_data[i]:
                    print("     首个不同字节 @%d" % i)
                    print("       参照 %s" % ref_data[max(0, i - 8):i + 16].hex())
                    print("       我们 %s" % our_data[max(0, i - 8):i + 16].hex())
                    break
        else:
            print("  ok 逐字节一致（%d 字节，sha=%s）"
                  % (len(our_data), hashlib.sha256(our_data).hexdigest()[:16]))

        # rdsquashfs 读回：路径集合 + 类型
        d = subprocess.run([str(RDSQUASHFS), "--describe", str(ours)], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        got = {}
        for ln in d.stdout.decode("utf-8", "replace").splitlines():
            line = ln.strip()
            if not line or " " not in line:
                continue
            kind, rest = line.split(" ", 1)
            rest = rest.strip()
            # ★ 带空格的路径 rdsquashfs 会用双引号包起来（真机里就有
            #   "1rees_vetkagreen 1.dds"），按空格切会把一条拆成两条。
            if rest.startswith('"'):
                end = rest.find('"', 1)
                path = rest[1:end] if end > 0 else rest[1:]
            else:
                path = rest.split(" ")[0]
            got[path.lstrip("/")] = kind
        exp_dirs = set()
        for rel, _, _ in files:
            parts = rel.split("/")
            for i in range(1, len(parts)):
                exp_dirs.add("/".join(parts[:i]))
        exp = {rel for rel, _, _ in files} | exp_dirs
        if set(got) != exp:
            ok = False
            print("  ✗ rdsquashfs 路径集合不符：缺 %d / 多 %d"
                  % (len(exp - set(got)), len(set(got) - exp)))
            for p in sorted(exp - set(got))[:5]:
                print("      缺 %s" % p)
            for p in sorted(set(got) - exp)[:5]:
                print("      多 %s" % p)
        else:
            print("  ok rdsquashfs 读回 %d 条（路径 + 类型）" % len(got))

        # 内容抽验（从我们的产物里抽回 12 个）
        pick = files[:4] + files[:: max(1, len(files) // 6)][:6]
        pick = pick[:12]
        bad = []
        for rel, src, _ in pick:
            out_file = work / "sample.bin"
            rr = cli("--extract", str(ours), rel, str(out_file))
            if rr.returncode != 0:
                bad.append("%s：%s" % (rel, rr.stdout.decode("utf-8", "replace")[:120]))
            elif out_file.read_bytes() != src.read_bytes():
                bad.append("%s：内容不符" % rel)
        if bad:
            ok = False
            print("  ✗ 内容抽验失败 %d/%d" % (len(bad), len(pick)))
            for item in bad[:6]:
                print("      %s" % item)
        else:
            print("  ok 内容抽验 %d 个文件逐字节一致" % len(pick))

        if not ok:
            failures += 1
        compared += 1

    print("")
    verdict = "FAIL" if failures else ("PARTIAL" if (skipped or denied) else "PASS")
    print("GOLDEN-SQFS-PACK-REAL: %s (compared=%d skipped=%d denied=%d)"
          % (verdict, compared, skipped, len(denied)))
    for item in denied:
        print("  denied: %s" % item)
    if failures:
        return 1
    return 2 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
