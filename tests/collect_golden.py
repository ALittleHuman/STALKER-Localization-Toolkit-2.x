#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""黄金语料（golden corpus）清单：收集与校验。

为什么要它：2.0 的引擎判据是**与 1.x 逐字节对齐**，前提是"我们到底拿哪几个字节
当基准"必须可复现。所以把真机样本的大小 + sha256 + 用途钉进 manifest.json，
之后任何人跑门禁都能验证"基准还是那一份"。

用法：
    python tests/collect_golden.py            # 校验（默认）
    python tests/collect_golden.py --write    # 首次收集 / 有意更新基准
    python tests/collect_golden.py --list     # 只列清单不校验
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests" / "golden" / "manifest.json"

# 真机样本根（2026-09-24 实测：已从 E:\Human 迁到 Games\ 下）
NLC = Path(r"E:\Software\Games\STALKER\Games\NLC Improved\NLC Improved [OBT]")
GS = Path(r"E:\Software\Games\STALKER\Games\Golden Sphere")
SOC = Path(r"E:\Software\Games\STALKER\SoC_Source\S.T.A.L.K.E.R. Shadow of Chernobyl_RUS")

# 语料选择原则：覆盖每一类**解析路径**，而不是把硬盘上的包全抄一遍。
CORPUS = [
    {"id": "nlc-xrengine", "path": NLC / "bin_x64" / "xrEngine.exe",
     "why": "引擎补丁目标（原版）；与 xrEngine_patched.exe 逐字节对拍是最硬判据"},
    {"id": "nlc-xrengine-patched",
     "path": Path(r"E:\Software\Tools\Ghidra\Projects\NLC Improved\Exported\xrEngine_patched.exe"),
     "why": "已确认正确的补丁结果：与它逐字节对拍是最硬判据"},
    {"id": "nlc-sq-base", "path": NLC / "gamedata.sq_base",
     "why": "NLC 加密包（ZZZZ 头 + XOR 流密码）：通用解密挂钩 + 解密后按 hsqs 处理"},
    {"id": "nlc-sqzy-patch", "path": NLC / "gamedata.sqzy_patch",
     "why": "明文 SquashFS 大包（1065 项那一族）；同名文件合并语义的现场来源"},
    {"id": "nlc-sq-meshes", "path": NLC / "gamedata.sq_meshes",
     "why": "明文 SquashFS（4104 项）；列路径 0.07s 那族"},
    {"id": "nlc-sq-sounds", "path": NLC / "gamedata.sq_sounds",
     "why": "明文 SquashFS（较小）"},
    {"id": "nlc-sq-levels", "path": NLC / "gamedata.sq_levels",
     "why": "明文 SquashFS（3209 项）"},
    {"id": "nlc-sq-textures", "path": NLC / "gamedata.sq_textures",
     "why": "明文 SquashFS 最大包（19641 项）：虚拟化与吞吐压测基准"},
    {"id": "nlc-sqz-autumn", "path": NLC / "Текстурные дополнения" / "gamedata.sqz_autumn",
     "why": "带西里尔目录名的样本（路径/编码口径）"},
    {"id": "nlc-sqz-wasteland", "path": NLC / "Текстурные дополнения" / "gamedata.sqz_wasteland",
     "why": "同上，第二个样本"},
    {"id": "gs-db-base", "path": GS / "gamedata.db_base",
     "why": "DB 六格式（Golden Sphere 家族）：xdb/2947/2945/2215/11xx 的现场来源"},
    {"id": "soc-db0", "path": SOC / "gamedata.db0",
     "why": "SoC 原版 DB 家族（.db0 这种后缀正是 1.x '只判前缀' 口径的来源）"},
]


def sha256_of(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    size = 0
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(8 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def load_manifest() -> dict:
    if MANIFEST.exists():
        return json.loads(MANIFEST.read_text(encoding="utf-8"))
    return {"entries": []}


def main() -> int:
    parser = argparse.ArgumentParser(description="黄金语料收集/校验")
    parser.add_argument("--write", action="store_true", help="收集并写 manifest")
    parser.add_argument("--list", action="store_true", help="只列清单")
    parser.add_argument("--only", default="", help="只处理 id 含该子串的条目（写模式下只更新这几条）")
    args = parser.parse_args()

    selected = [it for it in CORPUS if not args.only or args.only in it["id"]]
    old = {e["id"]: e for e in load_manifest().get("entries", [])}

    if args.list:
        for item in selected:
            mark = "存在" if item["path"].exists() else "缺失"
            print("%-24s %-4s %s" % (item["id"], mark, item["path"]))
        return 0

    if args.write:
        # 只更新选中的条目，其余原样保留 —— 否则补一条就要把十几 GB 重算一遍
        entries = [] if args.only == "" and not old else [e for e in old.values()
                                                          if e["id"] not in {i["id"] for i in selected}]
        for item in selected:
            path = item["path"]
            if not path.exists():
                print("  --  %-22s 缺失，跳过写入" % item["id"])
                continue
            t0 = time.time()
            digest, size = sha256_of(path)
            entries.append({
                "id": item["id"],
                "path": str(path),
                "why": item["why"],
                "size_bytes": size,
                "sha256": digest,
                "mtime": int(path.stat().st_mtime),
            })
            print("  ok  %-22s %12d B  %s  (%.1fs)" % (item["id"], size, digest[:16], time.time() - t0))
        entries.sort(key=lambda e: e["id"])
        MANIFEST.parent.mkdir(parents=True, exist_ok=True)
        MANIFEST.write_text(
            json.dumps({"note": "黄金语料基准；由 tests/collect_golden.py --write 生成",
                        "entries": entries}, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")
        print("已写 %s（%d 条）" % (MANIFEST.relative_to(ROOT), len(entries)))
        return 0

    # 校验模式
    if not MANIFEST.exists():
        print("CORPUS: FAIL 缺少清单 %s（先跑 --write）" % MANIFEST.relative_to(ROOT))
        return 1

    missing = changed = ok = 0
    for entry in load_manifest()["entries"]:
        path = Path(entry["path"])
        if not path.exists():
            print("  --  %-22s 样本缺失：%s" % (entry["id"], path))
            missing += 1
            continue
        digest, size = sha256_of(path)
        if size != entry["size_bytes"] or digest != entry["sha256"]:
            print("  !!  %-22s 与基准不符（大小 %d→%d，sha %s→%s）"
                  % (entry["id"], entry["size_bytes"], size,
                     entry["sha256"][:12], digest[:12]))
            changed += 1
            continue
        print("  ok  %-22s %s" % (entry["id"], digest[:16]))
        ok += 1

    print("CORPUS: ok=%d missing=%d changed=%d" % (ok, missing, changed))
    if changed:
        print("CORPUS: FAIL（基准变了 —— 要么恢复样本，要么确认后 --write 重记）")
        return 1
    if missing:
        print("CORPUS: PARTIAL（样本缺失按外部前提处理，由门禁按档位判定）")
        return 2
    print("CORPUS: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
