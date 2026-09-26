#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""有损编解码的黄金对拍：2.x（`errors="replace"` 的复刻） vs **Python 自己的 codec**。

为什么这条必须有：编码转换页的两个写出分支都用 `errors="replace"`，而 replace 的
输出**直接进产物** —— 它错了就是逐字节错，且错得"看着像对"（正文好好地在那，
只是替换符个数差一个）。所以判据不是"我们觉得应该这样"，而是**逐字节等于 Python**。

覆盖：
  · 单字节表：cp1251 / cp1252 的未定义字节、0x80..0xFF 全排；
  · UTF-8：坏前导、越界次字节、**输入末尾截断**（CPython 的"最大子部分"规则）；
  · GB 家族：两字节、四字节、截断、非法空洞、0x80/0xFF；
  · 编码方向：目标编码表示不了的码位 → `?`。

    python tests/golden_lossy_codec.py            # 对拍
    python tests/golden_lossy_codec.py --list     # 只打印用例
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# 与引擎里实现对拍过的编码（门禁 [12]）。utf-16/utf-32 家族**故意不在列**：
# `decode_lossy` 对它们如实报错（不做没对拍过的语义）。
DECODE_CODECS = ["windows-1251", "windows-1252", "latin-1", "utf-8", "gb18030", "gbk", "gb2312"]
ENCODE_CODECS = ["windows-1251", "windows-1252", "latin-1", "utf-8", "gb18030"]


def utf8_cases() -> list[bytes]:
    cases = [
        b"", b"a", b"abc",
        b"\xc2\xa9",                      # 合法两字节
        b"\xe4\xb8\xad",                  # 合法三字节
        b"\xf0\x9f\x98\x80",              # 合法四字节
        b"\xe4\xb8",                      # 末尾截断（吃 2 字节，1 个替换）
        b"\xe4\xb8\xe4",                  # 截断 + 坏前导
        b"\xe4A",                         # 越界次字节（只吃 e4）
        b"\xff\xfe",                      # 非法前导
        b"\xe0\x80\x80",                  # E0 的次字节必须 A0..BF
        b"\xed\xa0\x80",                  # 代理区
        b"\xf4\x90\x80\x80",              # F4 的次字节必须 80..8F
        b"\xc0\x80",                      # 过长编码
        b"\x80\x80",                      # 裸续字节
        b"ok\xe4\xb8\xadok\xff",          # 混合
        bytes(range(0x80, 0x100)),        # 高位全排
    ]
    return cases


def single_byte_cases() -> list[bytes]:
    return [bytes(range(256)), b"\x98\x91\x92", b"\x81\x8d\x8f\x90\x9d", b"ascii", b"", b"\xff" * 4]


def gb_cases() -> list[bytes]:
    return [
        b"", b"ascii", b"\xd6\xd0",              # 中文（两字节）
        b"\xd6", b"\xd6\xd0\xd6",                # 截断
        b"\x81\x30\x81\x30",                     # 四字节 → U+0080
        b"\x81\x30", b"\x81\x30\x81",            # 四字节截断
        b"\xff\xff", b"\x80\x80", b"\xa1\xa1",   # 非法
        b"\xc4\xe3\xba\xc3",                     # 你好
        bytes(range(0x80, 0x100)),
    ]


def encode_cases() -> list[str]:
    return [
        "ascii only",
        "中文",
        "中文 and ascii",
        "Привет",
        "emoji 😀 tail",
        "混合 Привет 中文 😀 ascii",
        "",
        "\u2018\u2019\u201c\u201d",   # cp1252 有、cp1251 没有的标点
    ]


def decode_reference(codec: str, raw: bytes) -> bytes:
    try:
        return raw.decode(codec, errors="replace").encode("utf-8")
    except LookupError:
        return b"<unknown-codec>"


def encode_reference(codec: str, text: str) -> bytes:
    try:
        return text.encode(codec, errors="replace")
    except LookupError:
        return b"<unknown-codec>"


def run_probe(mode: str, codec: str, data: bytes, work: Path) -> tuple[int, bytes, str]:
    src = work / "in.bin"
    dst = work / "out.bin"
    src.write_bytes(data)
    if dst.exists():
        dst.unlink()
    proc = subprocess.run([str(CLI), "--codec-lossy", mode, codec, str(src), str(dst)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    got = dst.read_bytes() if dst.exists() else b""
    return proc.returncode, got, text



def utf_family_cases() -> list[bytes]:
    """utf-16/32 家族的难样本（与 `tests/probe_utf_lossy.py` 的真值表同源）。

    BOM（LE/BE/反向/只有 BOM）、无 BOM 本机序、尾部奇数字节、单独高位/低位代理、代理对、
    低位在前、U+FFFE、utf-32 越界/代理区、utf-32 尾部 1~3 字节、空输入。
    """
    return [
        b"",
        b"A\x00B\x00",
        b"\xff\xfeA\x00",
        b"\xfe\xff\x00A",
        b"\xff\xfe",
        b"\xfe\xff",
        b"\xfe\xffA\x00",
        b"A\x00B",
        b"A\x00B\x00C\x00\x00",
        b"\x00\xd8",
        b"\x00\xdc",
        b"\x00\xd8A\x00",
        b"\x3d\xd8\x00\xde",
        b"\x00\xdc\x00\xd8",
        b"\xff\xfe\xff\xfe",
        b"A\x00\x00\x00",
        b"\xff\xfe\x00\x00A\x00\x00\x00",
        b"\x00\x00\xfe\xff\x00\x00\x00A",
        b"A\x00\x00",
        b"A\x00",
        b"\x00\x00\x11\x00",
        b"\x00\xd8\x00\x00",
    ]



# 2026-09-26：把 utf-16/32 六族并进"参与对拍"的集合（判据＝规格 §5.3 的九条）。
# ⚠ 教训：光往 cases_by_codec 加用例没用 —— main() 会用 DECODE_CODECS 过滤，
#   改完必须看 `compared=` 有没有涨。
DECODE_CODECS = set(DECODE_CODECS) | {"utf-16", "utf-16-le", "utf-16-be",
                                      "utf-32", "utf-32-le", "utf-32-be"}
ENCODE_CODECS = set(ENCODE_CODECS) | {"utf-16", "utf-16-le", "utf-16-be",
                                      "utf-32", "utf-32-le", "utf-32-be"}


def main() -> int:
    parser = argparse.ArgumentParser(description="有损编解码黄金对拍")
    parser.add_argument("--list", action="store_true", help="只打印用例")
    args = parser.parse_args()

    if args.list:
        print("utf-8 %d 例 / 单字节 %d 例 / gb %d 例 / 编码方向 %d 例"
              % (len(utf8_cases()), len(single_byte_cases()), len(gb_cases()),
                 len(encode_cases())))
        return 0

    if not CLI.exists():
        print("找不到 CLI：%s（先跑 python tests/run_ci.py --only build）" % CLI)
        return 2

    compared = 0
    failures: list[str] = []
    cases_by_codec: list[tuple[str, list[bytes]]] = [
        ("utf-8", utf8_cases()),
        ("windows-1251", single_byte_cases()),
        ("windows-1252", single_byte_cases()),
        ("latin-1", single_byte_cases()),
        ("gb18030", gb_cases()),
        ("gbk", gb_cases()),
        ("gb2312", gb_cases()),
    ("utf-16", utf_family_cases()),
    ("utf-16-le", utf_family_cases()),
    ("utf-16-be", utf_family_cases()),
    ("utf-32", utf_family_cases()),
    ("utf-32-le", utf_family_cases()),
    ("utf-32-be", utf_family_cases()),
    ]

    with tempfile.TemporaryDirectory(prefix="xrkit_lossy_") as tmp:
        work = Path(tmp)
        for codec, cases in cases_by_codec:
            if codec not in DECODE_CODECS:
                continue
            for raw in cases:
                want = decode_reference(codec, raw)
                rc, got, out = run_probe("decode", codec, raw, work)
                compared += 1
                if rc != 0:
                    failures.append("decode %s %s：CLI 退出码 %d（%s）"
                                    % (codec, raw[:16].hex(), rc, out[:120]))
                elif got != want:
                    failures.append("decode %s %s：期望 %s，得到 %s"
                                    % (codec, raw[:16].hex(), want[:32].hex(), got[:32].hex()))
        for codec in ENCODE_CODECS:
            for text in encode_cases():
                want = encode_reference(codec, text)
                rc, got, out = run_probe("encode", codec, text.encode("utf-8"), work)
                compared += 1
                if rc != 0:
                    failures.append("encode %s %r：CLI 退出码 %d（%s）"
                                    % (codec, text[:16], rc, out[:120]))
                elif got != want:
                    failures.append("encode %s %r：期望 %s，得到 %s"
                                    % (codec, text[:16], want[:32].hex(), got[:32].hex()))

    for item in failures[:12]:
        print("  ✗ %s" % item)
    if len(failures) > 12:
        print("  …（还有 %d 条）" % (len(failures) - 12))
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-LOSSY-CODEC: %s (compared=%d failed=%d)" % (verdict, compared, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
