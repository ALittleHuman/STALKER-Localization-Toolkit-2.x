#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""生成 xrkit::engine::font 的三张数据表（.inc）—— 与编码表同一姿势：生成器 + `--check` 锁。

为什么用生成器而不是手抄：这三张表都是**成百上千项**的标定数据（推进宽 1250 项、
简→繁 2640 项、东亚宽区间 122 段），手抄必错；而且必须能把"表是从哪份数据来的"钉死。

数据来源与判据（判据层级：原版 > 1.x）：
  1. `adv_tables.json`（1.x `font_pack/`）—— 10 个字号 × 125 个码位的**推进宽**。
     ★ 交叉核对**原版**成品 `汉化包生成工具2024\gamedata\textures\ui\ui_font_NN_chs.ini`：
       重叠码位 102/102 逐条相等（实测，见 --verify-reference）。判据是"与 1.x 逐字节一致"，
       所以表**取 1.x 的 JSON**；原版成品只用来证明这份 JSON 有原版背书，不是拍脑袋。
  2. `st_fallback.json`（1.x `font_pack/`）—— OpenCC STCharacters 的派生简→繁单字表（2640 条）。
  3. EAW∈{W,F} 的码位区间 + 1.x 的 `FORCED_FULLWIDTH` —— 1.x 用 Python `unicodedata`
     判"方块字符"，所以这里也由同一个 `unicodedata` 导出（版本记在文件头里；Python 升级
     导致 Unicode 版本变化时 `--check` 会红，且原因一眼可见）。

生成物：
  src/engine/font/adv_tables.inc    kFontSlotSizes / kFontAdvCodepoints / kFontAdvWidths
  src/engine/font/st_fallback.inc   kStFallback（升序 from→to）
  src/engine/font/eaw_tables.inc    kEawWideRanges / kForcedFullwidth

用法：
    python tools/gen_font_tables.py             # 生成/刷新
    python tools/gen_font_tables.py --check      # 只校验磁盘上的表与数据源一致（门禁用）
"""

from __future__ import annotations

import os
import argparse
import hashlib
import json
import re
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "engine" / "font"

# 1.x 参照实现的仓库根（本工程的门禁脚本也用同一路径口径）
# ★★ 2026-10-10（P17-23，审计 04 F-08）：**不要写死本机的 1.x 路径** ——
#   优先读环境变量 `XRKIT_TOOLKIT_1X`（换机器/换检出位置不用改源码），没有才回落到本机既有位置。
TOOLKIT_1X = Path(os.environ.get("XRKIT_TOOLKIT_1X")
                  or r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
# 原版工具"汉化包生成工具2024"的成品（第三方交叉核对；缺了就报 SKIP，不算失败）
REFERENCE_UI = Path(r"E:\Software\Games\STALKER\Localization\Tools\Tool\汉化包生成工具2024"
                    r"\gamedata\textures\ui")

# 1.x `font_pack.SIZES` 的 10 个档位（= 槽位名）。**顺序即 .inc 里的行序**。
SLOT_SIZES = [11, 13, 15, 16, 17, 18, 20, 21, 23, 25]

# 1.x `font_pack.FORCED_FULLWIDTH`：EAW 为 A 但中文语境全角，原版按字号+1
# ★ 导出时**按码位升序**（C++ 侧二分查找）：集合语义与顺序无关，升序只是查表需要。
FORCED_FULLWIDTH = sorted([0x201C, 0x201D, 0x2018, 0x2019, 0x2014, 0x2016])

# 原版成品 ini 的一行：`00253= x1, y1, x2, y2`
REF_LINE = re.compile(r"^(\d{5})= (\d+), (\d+), (\d+), (\d+)\s*$")


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ── 1. 推进宽表 ───────────────────────────────────────────────────────
def load_adv(toolkit: Path):
    path = toolkit / "font_pack" / "adv_tables.json"
    raw = json.loads(path.read_text(encoding="utf-8"))
    tables = {int(k): {int(cp): int(w) for cp, w in v.items()} for k, v in raw.items()}
    if sorted(tables) != sorted(SLOT_SIZES):
        raise SystemExit("!! adv_tables.json 的字号集合与 1.x SIZES 不符：%s" % sorted(tables))
    cps = sorted(tables[SLOT_SIZES[0]])
    for size in SLOT_SIZES:
        if sorted(tables[size]) != cps:
            raise SystemExit("!! 字号 %d 的码位集合与其他档位不同" % size)
    for size, table in tables.items():
        for cp, w in table.items():
            if not (0 <= w <= 255):
                raise SystemExit("!! 字号 %d 的 U+%04X 宽度 %d 超出 uint8" % (size, cp, w))
            if not (0 <= cp <= 0xFFFF):
                raise SystemExit("!! U+%04X 超出 BMP（表按 uint16 存）" % cp)
    return path, tables, cps


def verify_reference(tables, cps) -> str:
    """拿原版成品 ini 交叉核对推进宽表。返回一行结论（供打印与写进文件头）。"""
    if not REFERENCE_UI.is_dir():
        return "原版交叉核对：SKIP（未找到 %s）" % REFERENCE_UI
    overlap = 0
    bad = []
    for ini in sorted(REFERENCE_UI.glob("ui_font_*_chs.ini")):
        m = re.match(r"^ui_font_(\d+)_chs\.ini$", ini.name)
        if not m:
            continue
        size = int(m.group(1))
        if size not in tables:
            continue
        for ln in ini.read_text(encoding="utf-8").splitlines():
            mm = REF_LINE.match(ln)
            if not mm:
                continue
            cp = int(mm.group(1))
            if cp not in tables[size]:
                continue
            w = int(mm.group(4)) - int(mm.group(2))
            overlap += 1
            if w != tables[size][cp]:
                bad.append("U+%04X@%d 原版 %d vs 1.x %d" % (cp, size, w, tables[size][cp]))
    if bad:
        raise SystemExit("!! 原版成品与 1.x 推进宽表在 %d 处不一致（判据层级：原版 > 1.x，"
                         "需人工裁决后重建表）：\n   %s" % (len(bad), "\n   ".join(bad[:20])))
    return ("原版交叉核对：与 汉化包生成工具2024 的 ui_font_NN_chs.ini 重叠 %d 条，"
            "不一致 0 条" % overlap)


def emit_adv(source: Path, source_sha: str, cross: str, tables, cps) -> str:
    lines = []
    lines.append("// 本文件由 tools/gen_font_tables.py 生成，请勿手改。\n")
    lines.append("// 数据源：1.x font_pack/adv_tables.json（sha256 %s）\n" % source_sha)
    lines.append("// %s\n" % cross)
    lines.append("//\n")
    lines.append("// 语义：`rect_w_for` 的第一步 —— 表里有就用表里的值。表外的字符再看是不是\n")
    lines.append("// 方块字符（eaw_tables.inc），还不是才按字体 advance 现算。\n\n")
    lines.append("// 槽位名（= 1.x SIZES 的 10 个档位；行序即此序）\n")
    lines.append("static const int kFontSlotSizes[%d] = {%s};\n\n"
                 % (len(SLOT_SIZES), ", ".join(str(s) for s in SLOT_SIZES)))
    lines.append("// 有标定宽度的码位（升序；10 个档位共用同一批）\n")
    lines.append("static const uint16_t kFontAdvCodepoints[%d] = {\n" % len(cps))
    for i in range(0, len(cps), 10):
        lines.append("    " + ", ".join("0x%04X" % cp for cp in cps[i:i + 10]) + ",\n")
    lines.append("};\n\n")
    lines.append("// 推进宽：行 = kFontSlotSizes 下标，列 = kFontAdvCodepoints 下标\n")
    lines.append("static const uint8_t kFontAdvWidths[%d][%d] = {\n" % (len(SLOT_SIZES), len(cps)))
    for size in SLOT_SIZES:
        row = [tables[size][cp] for cp in cps]
        lines.append("    {")
        for i in range(0, len(row), 20):
            sep = "\n     " if i else ""
            lines.append("%s%s" % (sep, " ".join("%2d," % w for w in row[i:i + 20])))
        lines.append("\n    },\n")
    lines.append("};\n")
    return "".join(lines)


# ── 2. 简→繁单字表 ───────────────────────────────────────────────────
def load_st(toolkit: Path):
    path = toolkit / "font_pack" / "st_fallback.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    table = data.get("map")
    if not isinstance(table, dict):
        raise SystemExit("!! st_fallback.json 里没有 `map` 对象")
    pairs = []
    for k, v in table.items():
        if len(k) != 1 or len(v) != 1:
            raise SystemExit("!! 非单字条目：%r → %r" % (k, v))
        if ord(k) > 0xFFFF or ord(v) > 0xFFFF:
            raise SystemExit("!! 非 BMP 条目：%r → %r（表按 uint16 存）" % (k, v))
        pairs.append((ord(k), ord(v)))
    pairs.sort()
    if len({a for a, _ in pairs}) != len(pairs):
        raise SystemExit("!! 简化字有重复键")
    meta = {k: data[k] for k in ("_license", "_source", "_transform")
            if k in data and isinstance(data[k], str)}
    return path, pairs, meta


def emit_st(source: Path, source_sha: str, pairs, meta) -> str:
    lines = []
    lines.append("// 本文件由 tools/gen_font_tables.py 生成，请勿手改。\n")
    lines.append("// 数据源：1.x font_pack/st_fallback.json（sha256 %s）\n" % source_sha)
    for key in ("_source", "_license", "_transform"):
        if key in meta:
            lines.append("//   %s: %s\n" % (key, meta[key]))
    lines.append("//\n")
    lines.append("// 用法（1.x render_font）：某字在该字体里**没有字形**（`mask.bbox() is None`）时，\n")
    lines.append("// 用它的繁体字形补上，**码位不变** —— 引擎仍按原码位索引槽位。\n")
    lines.append("// 表里已排除「繁体里照用原字」的条目（干/后/里/台/表/丑/只…），否则替换必错。\n")
    lines.append("// 升序，按 `from` 二分。\n\n")
    lines.append("struct StFallbackPair { uint16_t from; uint16_t to; };\n")
    lines.append("static const StFallbackPair kStFallback[%d] = {\n" % len(pairs))
    for i in range(0, len(pairs), 8):
        lines.append("    " + " ".join("{0x%04X,0x%04X}," % p for p in pairs[i:i + 8]) + "\n")
    lines.append("};\n")
    return "".join(lines)


# ── 3. 东亚宽 + 强制全角 ─────────────────────────────────────────────
def eaw_ranges():
    cps = []
    for cp in range(0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        if unicodedata.east_asian_width(chr(cp)) in ("W", "F"):
            cps.append(cp)
    ranges = []
    start = prev = None
    for cp in cps:
        if start is None:
            start = prev = cp
        elif cp == prev + 1:
            prev = cp
        else:
            ranges.append((start, prev))
            start = prev = cp
    if start is not None:
        ranges.append((start, prev))
    return ranges, len(cps)


def emit_eaw(ranges, total) -> str:
    lines = []
    lines.append("// 本文件由 tools/gen_font_tables.py 生成，请勿手改。\n")
    lines.append("// 数据源：本机 Python 的 unicodedata（Unicode 版本 %s）"
                 % unicodedata.unidata_version)
    lines.append(" —— 1.x 判「方块字符」用的正是它，\n")
    lines.append("// 所以这里也由同一个源导出。Python 换版本会改 Unicode 版本 ⇒ --check 会红，"
                 "那是要人看一眼的。\n")
    lines.append("// EAW∈{W,F} 共 %d 个码位，压成 %d 段闭区间（升序、互不相邻）。\n\n"
                 % (total, len(ranges)))
    lines.append("struct EawWideRange { uint32_t from; uint32_t to; };\n")
    lines.append("static const EawWideRange kEawWideRanges[%d] = {\n" % len(ranges))
    for i in range(0, len(ranges), 4):
        lines.append("    " + " ".join("{0x%04X,0x%04X}," % r for r in ranges[i:i + 4]) + "\n")
    lines.append("};\n\n")
    lines.append("// 1.x `FORCED_FULLWIDTH`：EAW 为 A 但中文语境全角，原版按字号+1（升序）。\n")
    lines.append("static const uint32_t kForcedFullwidth[%d] = {%s};\n"
                 % (len(FORCED_FULLWIDTH), ", ".join("0x%04X" % c for c in FORCED_FULLWIDTH)))
    return "".join(lines)


def build(toolkit: Path):
    files: dict[Path, str] = {}

    adv_path, tables, cps = load_adv(toolkit)
    cross = verify_reference(tables, cps)
    files[OUT_DIR / "adv_tables.inc"] = emit_adv(
        adv_path, sha256_of(adv_path), cross, tables, cps)

    st_path, pairs, meta = load_st(toolkit)
    files[OUT_DIR / "st_fallback.inc"] = emit_st(st_path, sha256_of(st_path), pairs, meta)

    ranges, total = eaw_ranges()
    files[OUT_DIR / "eaw_tables.inc"] = emit_eaw(ranges, total)

    print("    推进宽：%d 档 × %d 码位（%d 项）" % (len(SLOT_SIZES), len(cps),
                                                len(SLOT_SIZES) * len(cps)))
    print("    %s" % cross)
    print("    简→繁：%d 条" % len(pairs))
    print("    EAW∈{W,F}：%d 码位 → %d 段（Unicode %s）"
          % (total, len(ranges), unicodedata.unidata_version))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="生成/校验 xrkit::engine::font 的数据表")
    parser.add_argument("--check", action="store_true", help="只校验磁盘上的表是否与数据源一致")
    parser.add_argument("--toolkit-1x", default=str(TOOLKIT_1X), help="1.x 参照仓库根目录")
    args = parser.parse_args()

    toolkit = Path(args.toolkit_1x)
    if not (toolkit / "font_pack" / "adv_tables.json").is_file():
        print("!! 找不到 1.x 的 font_pack/adv_tables.json：%s" % toolkit)
        print("GEN-FONT-TABLES: FAIL")
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    files = build(toolkit)

    if args.check:
        bad = 0
        for path, content in files.items():
            if not path.exists():
                print("  !! 缺文件：%s" % path.relative_to(ROOT))
                bad += 1
            elif path.read_bytes() != content.encode("utf-8"):
                print("  !! 与数据源不一致（改过表、换了 1.x 数据、或换了 Python）：%s"
                      % path.relative_to(ROOT))
                bad += 1
            else:
                print("  ok %s（%d 字节）" % (path.relative_to(ROOT), len(content.encode("utf-8"))))
        print("GEN-FONT-TABLES: %s" % ("FAIL" if bad else "PASS"))
        return 1 if bad else 0

    for path, content in files.items():
        # ★ bytes 写：Windows 文本模式会把 \n 翻成 \r\n，而 .gitattributes 规定源码一律 LF。
        path.write_bytes(content.encode("utf-8"))
        print("  已写 %s（%d 字节）" % (path.relative_to(ROOT), len(content)))
    print("GEN-FONT-TABLES: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
