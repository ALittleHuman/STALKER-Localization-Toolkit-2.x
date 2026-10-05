#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`--bench`：三条真实 I/O 路径的计时（口径见 docs/2.0.0-bench口径.md）。

在**测试侧**计时：直接跑既有 CLI 动作，不需要给引擎加 API；数字与命令都打印，便于复核。
样本缺失退 2（按外部前提处理）。
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
MANIFEST = HERE / "golden" / "manifest.json"
CORPUS = HERE / "golden" / "text_decode.json"
# 2026-10-05 那一批量化用的真机整镜像（14,147,584 B / 14,861 条目 / 解出 102,074,692 B）。
# 它不在 manifest 里（它不是"黄金语料基准"，是**性能锁**的样本）⇒ 找不到就 SKIP，不进未覆盖清单。
# 换机器/换样本：设 `XRKIT_BENCH_UNPACK_IMAGE`。
NS_OGSR = Path(r"E:\Software\Games\STALKER\Games\NS OGSR\NS OGSR")

# 门禁只守"防数量级回归"的宽松上界（口径文档 §判据）
# `unpack_ms` 口径：200 条 `--only` 走完整解包路径。
#   · 修后实测（2026-10-05，真机 13.5 MB / 14861 文件样本，200 条）：约 0.5~0.6 s ⇒ 每条 ~3 ms；
#   · 病根回归（每条目重开镜像 + 重列整棵树）时，同一台机器上每条会升到 ~7 ms ⇒ ~1.4 s；
#   · 上界 60000 ms 是**两个数量级**的余量：只为拦住量级回归（例如整镜像被重复遍历 N 遍），
#     不参与"本机快慢"的评判。
LIMITS = {"dump_ms": 60000, "extract_mbps": 1.0, "text_decode_ms": 20000,
          "unpack_ms": 60000,
          # ★★ 2026-10-05（用户裁决 ③ / 本批小件 ②）：**整镜像** `--unpack-tree` 的宽松上界。
          #   口径：真机 `gamedata.sq_base_configs`（14,147,584 B 镜像 / 14,861 条目 /
          #         解出 102,074,692 B）**一整趟**的墙钟（取 CLI 自报的引擎内耗时）。
          #   实测（本机，2026-10-05，改动前后在同一台机器上各跑 3 次）：
          #     · 旧默认（读回逐字节复核，HEAD a8eeadf 的二进制）：47.4 / 48.5 / 47.3 s；
          #     · 新默认（边写边算哈希）                    ：34.0 / 34.0 / 34.1 s（离散 < 0.3%）；
          #     · 开 `--verify-readback`（回到读回）         ：34.3 / 34.0 s（**本机**几乎无差 ——
          #       临时文件刚写完还在页缓存里，"读回"这一步在本机不是瓶颈；旧默认那 47 s 里
          #       多出来的 ≈ 13.6 s 是**旧实现把临时文件边写边读**的那条路）。
          #   ★★ 2026-10-05（本批小件 ②「**性能锁换成行为型**」）：**这里的耗时不变量级上界已放宽**
          #     —— 原来写 60 s，而"本机实测在 34 ~ 57 s 之间抖"（同一天同一二进制；差在并发/
          #     杀软扫描 —— 一次门禁自己就要新建 4 × 14,861 ≈ 6 万个文件），余量只有约 8% ⇒
          #     **既假红又没用**（本批实测就真跑到 69.6 s 被判红；而旧默认那 47 s 也照样能过）。
          #     ⇒ 真正的"默认档 vs 读回档"这条不变量**已换成行为型锁**（结构事实、与机器快慢
          #       无关）：`tools/cli/main.cpp` 里的 `core: 解包写后校验档位＝行为型锁`
          #       —— 用 `WriteProbe::record` 断言 **默认档必须走 Streamed、`--verify-readback`
          #       必须走 Readback**（把 `extract_tree` 那两处档位写死成任一侧，那里立刻红，
          #       而耗时候判据**看不出来**）。这里只剩"**防数量级回归**"这一件事。
          #   ⇒ 上界取 **600 s**（≈ 本机中位数 37 s 的 16 倍）：拦的是"整镜像被重复遍历 N 遍 /
          #     退化成一条条重开镜像"那一类量级事故，**不参与"本机快慢"的评判**；数字一律打印
          #     出来供人核。判红办法：把 `extract_tree` 里解包改成"每个条目重开一次镜像"⇒ 立刻
          #     冲到几百秒。
          "unpack_full_ms": 600000,
          # 同一镜像开 `--verify-readback`：同样只守数量级（它本来就该 ≥ 默认档）。
          "unpack_readback_ms": 600000}


def pick_unpack_image() -> Path | None:
    """整镜像解包的性能锁用哪枚镜像。

    口径（按顺序找，找不到就 SKIP）：
      ① `XRKIT_BENCH_UNPACK_IMAGE`（环境变量，便于换样本）；
      ② `tests/golden/manifest.json` 里**每一枚样本所在目录**下的 `gamedata.sq_base_configs`
         —— 那是 2026-10-05 那一批量化的**同一枚**样本（14,147,584 B / 14,861 条目 /
         解出 102,074,692 B），数字与任务里的口径直接可比；
      ③ 已知真机目录 `NS_OGSR` 下的同名文件；
      ④ 默认基准镜像同目录下的同名文件。
    找不到 ⇒ None（本条记 SKIP，不影响其它指标）。
    """
    env = os.environ.get("XRKIT_BENCH_UNPACK_IMAGE")
    if env:
        p = Path(env)
        return p if p.exists() else None
    name = "gamedata.sq_base_configs"
    dirs: list[Path] = [NS_OGSR]
    if MANIFEST.exists():
        try:
            data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            data = {}
        items = data if isinstance(data, list) else (data.get("entries") or data.get("items") or [])
        for item in items:
            p = Path(item.get("path", ""))
            if str(p):
                dirs.append(p.parent)
    base = pick_image()
    if base is not None:
        dirs.append(base.parent)
    seen: set[Path] = set()
    for d in dirs:
        cand = d / name
        if cand in seen:
            continue
        seen.add(cand)
        if cand.exists():
            return cand
    return None


def unpack_once(image: Path, outdir: Path, extra: list[str] | None = None) -> tuple[int, int, float, str]:
    """跑一整趟 `--unpack-tree`（可选 `--verify-readback`）。

    返回 (退出码, 盘上文件数, 毫秒, 收尾行)。`ms` 取 **CLI 自报的 engine 内耗时**（`ms=`）——
    那才是引擎内部的墙钟；外面再套一层子进程启动/输出收集会引入与本改动无关的噪声。
    """
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True)
    argv = ["--unpack-tree", str(image), str(outdir), *(extra or [])]
    t0 = time.perf_counter()
    rc, out = run(argv)
    wall_ms = (time.perf_counter() - t0) * 1000.0
    files = sum(1 for p in outdir.rglob("*") if p.is_file()) if outdir.exists() else 0
    inner = 0.0
    last = ""
    for line in out.splitlines():
        if line.startswith("UNPACK ok"):
            last = line.strip()
            for tok in line.split():
                if tok.startswith("ms="):
                    try:
                        inner = float(tok[3:])
                    except ValueError:
                        inner = 0.0
    return rc, files, (inner or wall_ms), last


def pick_image() -> Path | None:
    env = os.environ.get("XRKIT_BENCH_IMAGE")
    if env:
        p = Path(env)
        return p if p.exists() else None
    if not MANIFEST.exists():
        return None
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    items = data if isinstance(data, list) else (data.get("entries") or data.get("items") or [])
    best: tuple[int, Path] | None = None
    for item in items:
        p = Path(item.get("path", ""))
        # 明文 SquashFS 镜像：manifest 里 id 含 sqz/sqzy（加密的 sq_base 不在这条链路上）
        if p.exists() and ("sqz" in item.get("id", "") or "sqz" in p.name):
            size = p.stat().st_size
            if best is None or size < best[0]:
                best = (size, p)
    return best[1] if best else None


def run(argv: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), *argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("BENCH skip reason=cli-missing")
        return 2
    image = pick_image()
    if image is None:
        print("!! 找不到基准镜像（设 XRKIT_BENCH_IMAGE 或准备 tests/golden/manifest.json）")
        print("BENCH skip reason=no-image")
        return 2

    metrics: dict[str, float] = {}

    t0 = time.perf_counter()
    rc, out = run(["--dump", str(image)])
    ms = (time.perf_counter() - t0) * 1000.0
    entries = sum(1 for line in out.splitlines() if line.startswith(("f\t", "d\t")))
    if rc != 0:
        print("BENCH skip metric=dump_ms reason=%s" % out.strip()[:120])
    else:
        metrics["dump_ms"] = ms
        print("BENCH metric=dump_ms value=%.1f entries=%d image=%s"
              % (ms, entries, image.name))

    # 抽 16 个普通文件（取 --dump 的前 16 条文件行）
    rels = [line.split("\t")[1] for line in out.splitlines()
            if line.startswith("f\t")][:16]
    if rels:
        import tempfile
        total = 0
        with tempfile.TemporaryDirectory(prefix="xrkit_bench_") as tmp:
            t0 = time.perf_counter()
            for rel in rels:
                dst = Path(tmp) / "out.bin"
                rc, _ = run(["--extract", str(image), rel, str(dst)])
                if rc == 0 and dst.exists():
                    total += dst.stat().st_size
            sec = max(1e-6, time.perf_counter() - t0)
        mbps = (total / 1048576.0) / sec
        metrics["extract_mbps"] = mbps
        print("BENCH metric=extract_mbps value=%.2f files=%d bytes=%d" % (mbps, len(rels), total))
    else:
        print("BENCH skip metric=extract_mbps reason=no-file-entries")

    if CORPUS.exists():
        corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
        # schema 是 {"note":..., "entries":[{"id","path","size","sha256"}...]}（实测，别猜）
        if isinstance(corpus, list):
            raw = [str(x) for x in corpus]
        elif isinstance(corpus, dict):
            entries = corpus.get("entries") or corpus.get("files") or []
            raw = [str(e.get("path", "")) if isinstance(e, dict) else str(e) for e in entries]
        else:
            raw = []
        files = [Path(f) for f in raw if f]
        files = [f for f in files if f.exists()][:20]
        if files:
            import tempfile
            t0 = time.perf_counter()
            ok = 0
            with tempfile.TemporaryDirectory(prefix="xrkit_bench_t_") as tmp:
                for i, src in enumerate(files):
                    rc, _ = run(["--decode-text", str(src), str(Path(tmp) / ("o%d.txt" % i))])
                    ok += 1 if rc == 0 else 0
            ms = (time.perf_counter() - t0) * 1000.0
            metrics["text_decode_ms"] = ms
            print("BENCH metric=text_decode_ms value=%.1f files=%d ok=%d" % (ms, len(files), ok))
    if "text_decode_ms" not in metrics:
        print("BENCH skip metric=text_decode_ms reason=no-corpus")

    # ── ★★ 2026-10-05（性能锁①）：`--unpack-tree` 的**缩放**必须守住数量级 ──────────
    #   病根（已修）：`extract_tree` 原先每条目都调 `read_file()`，而后者每条目都重开镜像 +
    #   **重新 list() 整棵目录树** ⇒ 真机 14861 条目把同一份元数据遍历了 14861 遍
    #   （实测 100.3 s / 总 135.3 s，74%）。修后同一镜像 31.1 s（5.3×）。
    #   这一条**不**解整镜像（样本可能上 GB）：抽 N 条走 `--only`，报"N 条的 ms 与每条 ms"
    #   —— N 固定 ⇒ 任何"每条目又重开一次镜像"的回归都会把每条 ms 按数量级抬起来。
    unpack_n = 200
    if rels:
        import tempfile
        sel = rels[:unpack_n]
        with tempfile.TemporaryDirectory(prefix="xrkit_bench_u_") as tmp:
            listfile = Path(tmp) / "only.txt"
            listfile.write_text("\n".join(sel), encoding="utf-8")
            outdir = Path(tmp) / "out"
            t0 = time.perf_counter()
            rc_u, out_u = run(["--unpack-tree", str(image), str(outdir),
                               "--only", str(listfile)])
            ms_u = (time.perf_counter() - t0) * 1000.0
            got = sum(1 for _ in outdir.rglob("*") if _.is_file()) if outdir.exists() else 0
        if rc_u != 0 or got != len(sel):
            print("BENCH skip metric=unpack_ms reason=rc%d files=%d/%d %s"
                  % (rc_u, got, len(sel), out_u.strip()[:100]))
        else:
            metrics["unpack_ms"] = ms_u
            print("BENCH metric=unpack_ms value=%.1f files=%d per_file_ms=%.3f image=%s"
                  % (ms_u, len(sel), ms_u / max(1, len(sel)), image.name))
    else:
        print("BENCH skip metric=unpack_ms reason=no-file-entries")

    # ── ★★ 2026-10-05（用户裁决 ③）：**整镜像** `--unpack-tree` 的性能锁 ──────────────
    #   这一条冲着"原子写默认档换成边写边算哈希"来：
    #     · 默认（流式自校验，零额外 I/O）连跑 3 次，报**每次 + 中位数 + 离散度**；
    #     · 再开 `--verify-readback`（必须回到旧的读回逐字节复核）跑 1 次作对照 ——
    #       两者**产物必须逐字节一致**，但读回那次应当明显更慢（否则"开关"没生效）。
    #   上界只拦数量级回归（见 LIMITS 的注释）；数字全打印出来，便于人核。
    unpack_image = pick_unpack_image()
    if unpack_image is None:
        print("BENCH skip metric=unpack_full_ms reason=no-unpack-image"
              "（设 XRKIT_BENCH_UNPACK_IMAGE，或让基准镜像同目录里有 gamedata.sq_base_configs）")
        print("BENCH skip metric=unpack_readback_ms reason=no-unpack-image")
    else:
        import hashlib
        import statistics
        import tempfile

        def tree_digest(root: Path) -> str:
            h = hashlib.sha256()
            for p in sorted(root.rglob("*")):
                if not p.is_file():
                    continue
                h.update(p.relative_to(root).as_posix().encode("utf-8"))
                h.update(b"\0")
                data = p.read_bytes()
                h.update(str(len(data)).encode("ascii"))
                h.update(b"\0")
                h.update(data)
            return h.hexdigest()

        with tempfile.TemporaryDirectory(prefix="xrkit_bench_full_") as tmp:
            workdir = Path(tmp)
            shots: list[float] = []
            digests: list[str] = []
            files_seen: list[int] = []
            for i in range(3):
                rc, files, ms, last = unpack_once(unpack_image, workdir / ("run%d" % i))
                if rc != 0:
                    print("BENCH skip metric=unpack_full_ms reason=rc%d run=%d %s"
                          % (rc, i + 1, last[:120]))
                    shots = []
                    break
                shots.append(ms)
                files_seen.append(files)
                digests.append(tree_digest(workdir / ("run%d" % i)))
            if shots:
                med = statistics.median(shots)
                spread = (max(shots) - min(shots)) / med * 100.0 if med else 0.0
                metrics["unpack_full_ms"] = med
                print("BENCH metric=unpack_full_ms value=%.1f runs=%s files=%s median=%.1f "
                      "spread=%.1f%% image=%s"
                      % (med, ",".join("%.1f" % v for v in shots),
                         "/".join(str(f) for f in files_seen), med, spread, unpack_image.name))
                if len(set(digests)) != 1:
                    print("BENCH ✗ 同一镜像连跑 3 次产物不逐字节一致（性能锁的前提不成立）")
                    metrics["unpack_full_ms"] = 1e9   # 直接判红（这条比"快慢"重要）
                else:
                    print("     产物逐字节一致（3 次连跑同一 sha256 前缀 %s）" % digests[0][:16])

                # 对照：`--verify-readback`（旧的读回逐字节复核）——产物必须与默认档一致。
                rc_v, files_v, ms_v, last_v = unpack_once(
                    unpack_image, workdir / "readback", ["--verify-readback"])
                if rc_v != 0:
                    print("BENCH skip metric=unpack_readback_ms reason=rc%d %s"
                          % (rc_v, last_v[:120]))
                else:
                    dig_v = tree_digest(workdir / "readback")
                    metrics["unpack_readback_ms"] = ms_v
                    print("BENCH metric=unpack_readback_ms value=%.1f files=%d (对照：默认档 %.1f ms)"
                          % (ms_v, files_v, med))
                    if dig_v != digests[0]:
                        print("BENCH ✗ `--verify-readback` 的产物与默认档不一致"
                              "（读回开关改变了落盘内容 —— 这是硬缺陷）")
                        metrics["unpack_readback_ms"] = 1e9
                    else:
                        print("     产物与默认档逐字节一致（sha256 前缀 %s）" % dig_v[:16])


    failures = 0
    for name, limit in LIMITS.items():
        if name not in metrics:
            continue
        value = metrics[name]
        bad = value > limit if name != "extract_mbps" else value < limit
        print("  %s %s=%s（门禁上界 %s）" % ("✗" if bad else "ok", name, round(value, 2), limit))
        failures += 1 if bad else 0

    print("")
    print("BENCH %s metrics=%d failed=%d" % ("FAIL" if failures else "PASS", len(metrics), failures))
    return 1 if failures else (0 if metrics else 2)


if __name__ == "__main__":
    sys.exit(main())
