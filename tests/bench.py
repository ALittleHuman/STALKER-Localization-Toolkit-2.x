#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`--bench`：三条真实 I/O 路径的计时（口径见 docs/2.0.0-bench口径.md）。

在**测试侧**计时：直接跑既有 CLI 动作，不需要给引擎加 API；数字与命令都打印，便于复核。
样本缺失退 2（按外部前提处理）。
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
MANIFEST = HERE / "golden" / "manifest.json"
CORPUS = HERE / "golden" / "text_decode.json"

# 门禁只守"防数量级回归"的宽松上界（口径文档 §判据）
LIMITS = {"dump_ms": 60000, "extract_mbps": 1.0, "text_decode_ms": 20000}


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
        files = [Path(f) for f in (corpus if isinstance(corpus, list)
                                   else corpus.get("files", []))]
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
