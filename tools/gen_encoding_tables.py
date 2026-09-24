#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 xrkit::text 的编码表（.inc）—— 表从 **Python 的同名 codec** 导出，可复现。

为什么用生成器而不是手抄：2.0 的文本模块判据是"与 1.x 逐字节一致"，而 1.x 用的是
Python 的 codecs。手抄表一定会错（GB18030 双字节表约 2.4 万项），错了就是整篇乱码。

生成物：
  src/engine/text/cp1251_tables.inc   cp1251 0x80..0xFF → Unicode（0xFFFF = 未定义）
  src/engine/text/cp1252_tables.inc   同上
  src/engine/text/gb18030_tables.inc  双字节表（126×191）+ 四字节线性段 + 单字节 0x80

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

HEADER = ("// 本文件由 tools/gen_encoding_tables.py 从 Python 的 {codec} codec 生成，请勿手改。\n"
          "// 0xFFFF = 该字节/该序列在本编码里未定义 —— **严格解码必须失败**，\n"
          "// 不能当 U+FFFD 放行（放行会让六编码链在这步错误地解成功）。\n\n")

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
    for codec, _ in SINGLE_BYTE:
        vals = table_1byte(codec)
        print("    %s：未定义 %d 个字节" % (codec, sum(1 for v in vals if v == 0xFFFF)))
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description="生成/校验 xrkit::text 的编码表")
    parser.add_argument("--check", action="store_true", help="只校验磁盘上的表是否与 codec 一致")
    args = parser.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
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
