#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 xrkit::text 的编码表（.inc）—— 表从 **Python 的同名 codec** 导出，可复现。

为什么用生成器而不是手抄：2.0 的文本模块判据是"与 1.x 逐字节一致"，而 1.x 用的是
Python 的 codecs。手抄表一定会错（GB18030 双字节表约 2.4 万项），错了就是整篇乱码。

生成物：
  src/engine/text/cp1251_tables.inc   cp1251 0x80..0xFF → Unicode（0xFFFF = 未定义）
  src/engine/fs/cp1251_tables.inc     同上，但未定义映 **U+FFFD**（见下面 ★ H1-1）
  src/engine/text/cp1252_tables.inc   同上
  src/engine/text/gb18030_tables.inc  双字节表（126×191）+ 四字节线性段 + 单字节 0x80
  src/engine/text/gbk_tables.inc      同上双字节形状（gbk 只有两字节形态）
  src/engine/text/gb2312_tables.inc   同上
  src/engine/text/big5_tables.inc     big5 双字节表（lead 0xA1..0xF9 × trail 0x40..0xFE）
  src/engine/text/shift_jis_tables.inc  shift_jis 双字节表 + 单字节半角片假名表
  src/engine/text/euc_kr_tables.inc   euc-kr 双字节表（lead 0xA1..0xFD × trail 0xA1..0xFE）

★ `0xFFFF = 未定义` 是**严格性**的载体：cp1251 的 0x98、cp1252 的 0x81/0x8D/0x8F/0x90/0x9D
在 Python 里严格解码会抛 UnicodeDecodeError。若把它们当成 U+FFFD 放行，六编码链就会在
"windows-1251"这一步**错误地解成功**，落点从 latin-1 变成 cp1251 —— 解码结果直接变。

用法：
    python tools/gen_encoding_tables.py            # 生成/刷新
    python tools/gen_encoding_tables.py --check     # 只校验磁盘上的表与 codec 一致（门禁用）
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "engine" / "text"
# ★★ 2026-10-07（审计补充 H1-1）：`src/engine/fs/cp1251_tables.inc` 原来是一份**手写孤儿** ——
#   生成器只写 text/、门禁只查 text/，而 `src/engine/fs/db.cpp` 用的是**引号 include**
#   ⇒ 先命中同目录的 fs 版。把 fs 版删掉/改名，db.cpp 会**静默**改用 0xFFFF 那份且编译不报错
#   （0x98 的语义就此改变），两份同名副本内容还不一样。
#   两份的语义**确实不同**（fs = `decode(cp1251, replace)` 的 U+FFFD；text = 严格解码必须失败的
#   0xFFFF），所以不是"合并成一份"，而是：**两份都由本生成器产出**（同一份 codec 数据，只差
#   "未定义字节怎么映"），并由 `--check` 对**两份都逐字节**校验 ⇒ 谁被手改/删掉/改名立刻红。
FS_OUT_DIR = ROOT / "src" / "engine" / "fs"

HEADER = ("// 本文件由 tools/gen_encoding_tables.py 从 Python 的 {codec} codec 生成，请勿手改。\n"
          "// 0xFFFF = 该字节/该序列在本编码里未定义 —— **严格解码必须失败**，\n"
          "// 不能当 U+FFFD 放行（放行会让六编码链在这步错误地解成功）。\n\n")

# fs 版的头（**逐字节照抄历史文件**：这份表的用途是 DB 归档文件名，走 replace 语义）
FS_HEADER = ("// 本文件由 Python 的 cp1251 codec 生成，请勿手改。\n"
             "// 用途：DB 归档里的文件名字节是 cp1251，转成 UTF-8 必须逐码位准确。\n"
             "// 未定义字节（如 0x98）映到 U+FFFD —— 与 1.x 的 decode(cp1251, replace) 一致。\n\n")
FS_TABLE_COMMENT = "// 0x80..0xFF 对应的 Unicode 码点（共 128 项）\n"

# ── 单字节表（0x80..0xFF）─────────────────────────────────────────────
SINGLE_BYTE = [
    ("cp1251", "kCp1251ToUnicode"),
    ("cp1252", "kCp1252ToUnicode"),
]


def table_1byte(codec: str):
    out = []
    for b in range(0x80, 0x100):
        try:
            s = bytes([b]).decode(codec)
            out.append(ord(s) if len(s) == 1 else 0xFFFF)
        except UnicodeDecodeError:
            out.append(0xFFFF)
    return out


def emit_1byte(codec: str, name: str, values: list[int]) -> str:
    lines = [HEADER.format(codec=codec), "static const uint16_t %s[128] = {\n" % name]
    for i in range(0, len(values), 8):
        chunk = ", ".join("0x%04X" % v for v in values[i:i + 8])
        lines.append("    " + chunk + ",\n")
    lines.append("};\n")
    return "".join(lines)


def table_1byte_replace(codec: str) -> list[int]:
    """fs 版：未定义字节映 **U+FFFD**（1.x 的 `decode(codec, errors="replace")` 语义）。"""
    out = []
    for b in range(0x80, 0x100):
        try:
            s = bytes([b]).decode(codec)
            out.append(ord(s) if len(s) == 1 else 0xFFFD)
        except UnicodeDecodeError:
            out.append(0xFFFD)
    return out


def emit_1byte_replace(codec: str, name: str, values: list[int]) -> str:
    lines = [FS_HEADER, FS_TABLE_COMMENT, "static const uint16_t %s[128] = {\n" % name]
    for i in range(0, len(values), 8):
        lines.append("    " + ", ".join("0x%04X" % v for v in values[i:i + 8]) + ",\n")
    lines.append("};\n")
    return "".join(lines)


# ── GB18030 ──────────────────────────────────────────────────────────
GB_TWO_W = 126 * 191            # lead 0x81..0xFE × trail 0x40..0xFE
GB_FOUR_W = 126 * 10 * 126 * 10  # (b1 b2 b3 b4) 全空间


def gb_two_byte(codec: str = "gb18030") -> list[int]:
    out = [0xFFFF] * GB_TWO_W
    for b1 in range(0x81, 0xFF):
        for b2 in range(0x40, 0xFF):
            idx = (b1 - 0x81) * 191 + (b2 - 0x40)
            try:
                s = bytes([b1, b2]).decode(codec)
                out[idx] = ord(s) if len(s) == 1 else 0xFFFF
            except UnicodeDecodeError:
                out[idx] = 0xFFFF
    return out


def emit_two_byte(codec: str, name: str, values: list[int]) -> str:
    lines = [HEADER.format(codec=codec)]
    lines.append("// 下标 = (b1-0x81)*191 + (b2-0x40)，b1∈0x81..0xFE、b2∈0x40..0xFE。\n")
    lines.append("// 单字节只认 0x00..0x7F（0x80 在 %s 里是非法首字节）。\n\n" % codec)
    lines.append("static const uint16_t %s[%d] = {\n" % (name, GB_TWO_W))
    for i in range(0, len(values), 12):
        lines.append("    " + ", ".join("0x%04X" % v for v in values[i:i + 12]) + ",\n")
    lines.append("};\n")
    return "".join(lines)


# ── big5 / shift_jis / euc-kr（2026-10-05 D3 批）────────────────────────
#
# 三张表的**形状不一样**，所以不能照 gb 那套 (b1-0x81)*191+(b2-0x40) 硬套：
#   · big5      lead 0xA1..0xF9（0xC8 整列空洞）、trail ∈ [0x40..0x7E] ∪ [0xA1..0xFE]
#               —— 中间 [0x7F..0xA0] 与 0xFF **不是 trail**（实测 `A1 7F` → 替换 + 0x7F，
#               而 `A1 80` → 两个替换，差别就在这条）。
#   · shift_jis lead ∈ [0x81..0x9F] ∪ [0xE0..0xFC]、trail ∈ [0x40..0xFC] 去掉 0x7F；
#               另有**单字节半角片假名** 0xA1..0xDF → U+FF61..U+FF9F（要单独一张表）。
#   · euc-kr    lead 0xA1..0xFD（0xAD/0xAE/0xAF/0xC9 空洞）、trail 0xA1..0xFE。
# 下标统一记 (b - FIRST) * NTRAIL + (b2 - TRAIL_MIN)，空洞位写 0xFFFF。
MW_SPEC = [
    # codec, 表名, lead 区间, trail 区间
    ("big5", "kBig5TwoByte", [(0xA1, 0xF9)], [(0x40, 0x7E), (0xA1, 0xFE)]),
    ("shift_jis", "kShiftJisTwoByte", [(0x81, 0x9F), (0xE0, 0xFC)],
     [(0x40, 0x7E), (0x80, 0xFC)]),
    ("euc-kr", "kEucKrTwoByte", [(0xA1, 0xFD)], [(0xA1, 0xFE)]),
]


def _expand(ranges) -> list[int]:
    out = []
    for lo, hi in ranges:
        out.extend(range(lo, hi + 1))
    return out


def mw_table(codec: str, leads: list[int], trails: list[int]) -> list[int]:
    out = [0xFFFF] * ((leads[-1] + 1 - leads[0]) * (trails[-1] + 1 - trails[0]))
    for b1 in leads:
        for b2 in trails:
            try:
                s = bytes([b1, b2]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out[(b1 - leads[0]) * (trails[-1] + 1 - trails[0]) + (b2 - trails[0])] = ord(s)
    return out


def _range_text(ranges) -> str:
    return " ∪ ".join("[%02X..%02X]" % (lo, hi) for lo, hi in ranges)


def emit_mw(codec: str, name: str, leads: list[int], trails: list[int],
            values: list[int]) -> str:
    ntrail = trails[-1] + 1 - trails[0]
    lines = [HEADER.format(codec=codec)]
    lines.append("// lead ∈ %s，下标 = (b1-%02X)*%d + (b2-%02X)。\n"
                 % (_range_text(_lead_ranges_for(codec)), leads[0], ntrail, trails[0]))
    lines.append("// trail ∈ %s（**离散集合，中间的空档不是 trail**）。\n"
                 % _range_text(_trail_ranges_for(codec)))
    lines.append("// 单字节只认 0x00..0x7F%s。\n\n"
                 % ("与 0xA1..0xDF（半角片假名，见下表）" if codec == "shift_jis" else ""))
    lines.append("static const uint16_t %s[%d] = {\n" % (name, len(values)))
    for i in range(0, len(values), 12):
        lines.append("    " + ", ".join("0x%04X" % v for v in values[i:i + 12]) + ",\n")
    lines.append("};\n")
    return "".join(lines)


def _lead_ranges_for(codec: str):
    for c, _, leads, _ in MW_SPEC:
        if c == codec:
            return leads
    raise KeyError(codec)


def _trail_ranges_for(codec: str):
    for c, _, _, trails in MW_SPEC:
        if c == codec:
            return trails
    raise KeyError(codec)


def sjis_single_table() -> list[int]:
    """shift_jis 的单字节半角片假名：0xA1..0xDF → Unicode（0xFFFF = 未定义）。"""
    out = []
    for b in range(0xA1, 0xE0):
        try:
            s = bytes([b]).decode("shift_jis")
            out.append(ord(s) if len(s) == 1 else 0xFFFF)
        except UnicodeDecodeError:
            out.append(0xFFFF)
    return out


def emit_sjis_single(values: list[int]) -> str:
    lines = [HEADER.format(codec="shift_jis")]
    lines.append("// 单字节半角片假名 0xA1..0xDF：下标 = b - 0xA1（0xFFFF = 未定义）。\n")
    lines.append("// 双字节表查不到时先落到这里：**只有这里也有定义**才谈得上「单字节回退」；\n")
    lines.append("// 落不下来（例如 lead 0x81..0x9F/0xE0..0xFC 自己没有单字节形态）⇒ 报错/替换。\n\n")
    lines.append("static const uint16_t kShiftJisSingle[%d] = {\n" % len(values))
    for i in range(0, len(values), 8):
        lines.append("    " + ", ".join("0x%04X" % v for v in values[i:i + 8]) + ",\n")
    lines.append("};\n")
    return "".join(lines)


def gb_four_byte_ranges():
    """四字节空间 → Unicode 码点的**分段线性**表示（只覆盖合法序列）。

    做法：把 1 587 600 个四字节序列拼成一条大缓冲，按 (b1,b2) 分成 1260 组、每组 1260 条：
      · 整组能一次严格解码 → 全组合法，直接从文本里取映射；
      · 整组失败 → 组内逐条解（只有"非法空洞"那一片会走到，实测约 15 万条）。
    非法序列**不进段表** —— 它们必须严格失败（当初把它们当 U+FFFD 塞进段表，挤出了 18 MB）。
    """
    groups = []
    for b1 in range(0x81, 0xFF):
        for b2 in range(0x30, 0x3A):
            raw = bytearray()
            for b3 in range(0x81, 0xFF):
                for b4 in range(0x30, 0x3A):
                    raw += bytes((b1, b2, b3, b4))
            groups.append(bytes(raw))

    cps: list[int | None] = []
    for raw in groups:
        try:
            text = raw.decode("gb18030")
            cps.extend(ord(ch) for ch in text)
        except UnicodeDecodeError:
            for i in range(0, len(raw), 4):
                try:
                    cps.append(ord(raw[i:i + 4].decode("gb18030")))
                except UnicodeDecodeError:
                    cps.append(None)
    assert len(cps) == GB_FOUR_W, "四字节空间长度不符：%d" % len(cps)

    ranges = []
    start, delta = None, 0
    for i, cp in enumerate(cps):
        if cp is None:
            if start is not None:
                ranges.append((start, i - 1, delta))
                start = None
            continue
        if start is None:
            start, delta = i, cp - i
        elif cp - i != delta:
            ranges.append((start, i - 1, delta))
            start, delta = i, cp - i
    if start is not None:
        ranges.append((start, GB_FOUR_W - 1, delta))

    valid = sum(1 for cp in cps if cp is not None)
    print("    四字节：合法 %d / 非法 %d / 压成 %d 段"
          % (valid, GB_FOUR_W - valid, len(ranges)))
    return ranges


def emit_gb18030(two: list[int], four: list[tuple[int, int, int]]) -> str:
    lines = [HEADER.format(codec="gb18030")]
    lines.append("// ★ 单字节只认 0x00..0x7F（直通 ASCII）。**0x80 是非法** —— Python 的\n")
    lines.append("//   gb18030 codec 对 0x80 抛 'incomplete multibyte sequence'，不是欧元符号。\n\n")
    lines.append("// 双字节表：下标 = (b1-0x81)*191 + (b2-0x40)，b1∈0x81..0xFE、b2∈0x40..0xFE。\n")
    lines.append("static const uint16_t kGb18030TwoByte[%d] = {\n" % GB_TWO_W)
    for i in range(0, len(two), 12):
        lines.append("    " + ", ".join("0x%04X" % v for v in two[i:i + 12]) + ",\n")
    lines.append("};\n\n")
    lines.append("// 四字节表：下标 linear = ((b1-0x81)*10+(b2-0x30))*1260 + (b3-0x81)*10+(b4-0x30)，\n")
    lines.append("// 码点 = linear + delta（分段线性）。段**只覆盖合法序列**：查不到段的 linear\n")
    lines.append("// 必须严格失败（非法空洞约 50 万个）。\n")
    lines.append("struct Gb18030Range { uint32_t from; uint32_t to; int32_t delta; };\n")
    lines.append("static const Gb18030Range kGb18030FourByte[%d] = {\n" % len(four))
    for item in four:
        lines.append("    {0x%07X, 0x%07X, %d},\n" % item)
    lines.append("};\n")
    return "".join(lines)


def build() -> dict[Path, str]:
    files: dict[Path, str] = {}
    for codec, name in SINGLE_BYTE:
        files[OUT_DIR / ("%s_tables.inc" % codec)] = emit_1byte(codec, name, table_1byte(codec))
    # ★ H1-1：fs 版（DB 归档文件名，replace 语义）——**同一个生成器的产物**，不再手写孤儿。
    files[FS_OUT_DIR / "cp1251_tables.inc"] = emit_1byte_replace(
        "cp1251", "kCp1251ToUnicode", table_1byte_replace("cp1251"))
    print("    fs/cp1251_tables.inc：0x98 → 0xFFFD（replace 语义，DB 文件名专用）")
    two = gb_two_byte("gb18030")
    four = gb_four_byte_ranges()
    files[OUT_DIR / "gb18030_tables.inc"] = emit_gb18030(two, four)
    valid_two = sum(1 for v in two if v != 0xFFFF)
    print("    gb18030：双字节有效 %d/%d，四字节 %d 段（共 %d 个序列）"
          % (valid_two, GB_TWO_W, len(four), GB_FOUR_W))
    # gbk：decode_bytes 的回落候选里有它（`BYTE_DECODE_CANDIDATES`），只有双字节形态
    gbk = gb_two_byte("gbk")
    files[OUT_DIR / "gbk_tables.inc"] = emit_two_byte("gbk", "kGbkTwoByte", gbk)
    print("    gbk：双字节有效 %d/%d" % (sum(1 for v in gbk if v != 0xFFFF), GB_TWO_W))
    # gb2312：**只用于"XML 声明里写了 gb2312"这一条**（判定链第 4 步）。1.x 的
    # `bytes_fit_encoding` 直接 `raw.decode("gb2312")`，所以我们必须有同一张表，
    # 否则"声明 gb2312、字节真是 gb2312"的文件会被否掉、一路落到默认 windows-1251
    # （实测：字符集少 6 个汉字，产物就不一致了）。
    gb2312 = gb_two_byte("gb2312")
    files[OUT_DIR / "gb2312_tables.inc"] = emit_two_byte("gb2312", "kGb2312TwoByte", gb2312)
    print("    gb2312：双字节有效 %d/%d" % (sum(1 for v in gb2312 if v != 0xFFFF), GB_TWO_W))
    # ── D3 批：big5 / shift_jis / euc-kr（用户 2026-10-05：「顺手补上就是了」）──
    # 依旧是"从 Python 同名 codec 导出"同一姿势 ⇒ 生成物与 1.x 字节级同源。
    # shift_jis 还要多一张"单字节半角片假名"表（解码时的单字节回退就落在这张表上），
    # 所以它由两段拼成：先双字节表、再单字节表。
    for codec, name, lead_ranges, trail_ranges in MW_SPEC:
        leads = _expand(lead_ranges)
        trails = _expand(trail_ranges)
        vals = mw_table(codec, leads, trails)
        body = emit_mw(codec, name, leads, trails, vals)
        if codec == "shift_jis":
            single_sjis = sjis_single_table()
            body += "\n" + emit_sjis_single(single_sjis)
            print("    shift_jis：单字节半角片假名（0xA1..0xDF）有效 %d/%d"
                  % (sum(1 for v in single_sjis if v != 0xFFFF), len(single_sjis)))
        files[OUT_DIR / ("%s_tables.inc" % codec.replace("-", "_"))] = body
        print("    %s：lead %d 个 / trail %d 个，双字节有效 %d/%d"
              % (codec, len(leads), len(trails),
                 sum(1 for v in vals if v != 0xFFFF), len(vals)))
    for codec, _ in SINGLE_BYTE:
        vals = table_1byte(codec)
        print("    %s：未定义 %d 个字节" % (codec, sum(1 for v in vals if v == 0xFFFF)))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="生成/校验 xrkit::text 的编码表")
    parser.add_argument("--check", action="store_true", help="只校验磁盘上的表是否与 codec 一致")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    FS_OUT_DIR.mkdir(parents=True, exist_ok=True)
    files = build()

    if args.check:
        bad = 0
        for path, content in files.items():
            if not path.exists():
                print("  !! 缺文件：%s" % path.relative_to(ROOT))
                bad += 1
            elif path.read_bytes() != content.encode("utf-8"):
                print("  !! 与 codec 不一致（改过表、换了 Python 版本、或行尾被转换）：%s"
                      % path.relative_to(ROOT))
                bad += 1
            else:
                print("  ok %s（%d 字节）" % (path.relative_to(ROOT), len(content)))
        print("GEN-TABLES: %s" % ("FAIL" if bad else "PASS"))
        return 1 if bad else 0

    for path, content in files.items():
        # ★ 用 bytes 写：Windows 文本模式会把 \n 翻成 \r\n，而 .gitattributes 规定
        #   源码一律 LF —— 混着来会让 --check 在"刚生成"和"刚检出"两种状态下结论不同。
        path.write_bytes(content.encode("utf-8"))
        print("  已写 %s（%d 字节）" % (path.relative_to(ROOT), len(content)))
    print("GEN-TABLES: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
