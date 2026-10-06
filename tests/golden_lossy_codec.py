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
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
# 默认用 `msvc-release`（门禁 [12] 就是它）。`XRKIT_CLI` 是"先证红"时的逃生舱：
# 同一个源码用 `msvc-shell` 的 CLI 也能跑（两个 preset 的 CLI 是同一份源码、
# 同一套引擎）。**只**用于红/绿对照，不改默认行为。
CLI = Path(os.environ.get("XRKIT_CLI", str(ROOT / "build" / "msvc-release" / "tools" /
                                     "cli" / "xrkit_cli.exe")))

# 与引擎里实现对拍过的编码（门禁 [12]）。utf-16/utf-32 家族**故意不在列**：
# `decode_lossy` 对它们如实报错（不做没对拍过的语义）。
#
# ★ 2026-10-06（D2 批）：把 **ascii** 加进两列。为什么必须在这里加：编码转换页的
#   目标编码下拉有一条硬纪律「**只列我们对拍过的编码**」—— 1.x 的目标下拉里有 ascii，
#   引擎（`etxt::kAscii`）也早就实现了，但它一直不在下拉里，原因正是"没对拍过"。
#   补上对拍（Python 的 `ascii` codec 同样走 `errors="replace"`：非 ASCII 编码成 `?`、
#   非法字节解成 U+FFFD），它才有资格出现在下拉里。
#
# ★ 2026-10-05（D3 批）：把 **big5 / shift_jis / euc-kr** 加进两列。1.x 的
#   `ENCODING_CHOICES` 源侧一直有这三个名字，2.x 一直没实现（下拉里也没列，理由同样是
#   "没对拍过"）。★ 这批的教训来自 ALPHA.44 的 `ascii`：**只有解码侧**的编码会让
#   `encode_strict` 报"未知编码"，`encode_lossy` 于是走"逐码位、表示不了就写 `?`"的
#   兜底 ⇒ **连纯 ASCII 正文都被整篇写成 `?`**。所以三个新编码**两侧都要**，而且
#   下面 `REVERSE_LOCK` 专门盯"编码侧分支还在不在"。
DECODE_CODECS = ["windows-1251", "windows-1252", "latin-1", "utf-8", "gb18030", "gbk", "gb2312",
                 "ascii", "big5", "shift_jis", "euc-kr"]
ENCODE_CODECS = ["windows-1251", "windows-1252", "latin-1", "utf-8", "gb18030", "ascii",
                 "big5", "shift_jis", "euc-kr"]


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


# ── D3 批：big5 / shift_jis / euc-kr ─────────────────────────────────────
#
# 用例形状照生成器 `tools/gen_encoding_tables.py` 里 `MW_SPEC` 的三张表：
#   big5      lead 0xA1..0xF9（0xC8 整列空洞）、trail ∈ [0x40..0x7E] ∪ [0xA1..0xFE]
#   shift_jis lead ∈ [0x81..0x9F] ∪ [0xE0..0xFC]、trail ∈ [0x40..0x7E] ∪ [0x80..0xFC]
#             （另有单字节半角片假名 0xA1..0xDF）
#   euc-kr    lead 0xA1..0xFD、trail 0xA1..0xFE
#
# 每个 lead 都配上"最小/最大 trail、4 个非法 trail、截断、合法对 + ASCII"这几刀，
# 因为**这些正是三条实测出来的语义的分界**（Python 同名 codec 实测）：
#   ① trail 是**离散集合**：big5 `A1 7F` → 替换 + 0x7F（0x7F 不是 trail，只吃 lead），
#      而 `A1 80` → **两个**替换（0x80 也不是 trail，但它自身要被单独处理一次）；
#   ② shift_jis 的单字节回退：`A1 A1` → `'｡｡'`（表里有 `A1A1`）、
#      `A1 7F` → `'｡\x7f'`（表里没有但有单字节形态）、`81 20` → `'� '`（无单字节形态）；
#   ③ euc-kr `A4 D4` → 恰好一个 U+FFFD 且**丢弃整个剩余缓冲**（Python 的
#      KS X 1001 字母拼合序列分支，见 `encoding.cpp` 的 `mw_decode_lossy` 注释）。
MW_LEADS = {
    "big5": list(range(0xA1, 0xFA)),
    "shift_jis": list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)),
    "euc-kr": list(range(0xA1, 0xFE)),
}
MW_TRAILS = {
    "big5": list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)),
    "shift_jis": list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)),
    "euc-kr": list(range(0xA1, 0xFF)),
}


def mw_cases(codec: str) -> list[bytes]:
    leads = MW_LEADS[codec]
    trails = MW_TRAILS[codec]
    cases = [
        b"", b"ascii only",
        bytes(range(0x80, 0x100)),          # 高位全排：非法首字节逐个过一遍
        bytes(range(0xA1, 0xE0)),           # 半角片假名区间（shift_jis 单字节回退）
        bytes([0xA4, 0xD4]),                # euc-kr 的字母拼合序列（其余编码当普通字节）
        bytes([0xA4, 0xD4, 0x41, 0x41]),
        bytes([0xA4, 0xD4, 0xCA, 0xF7, 0xD6]),
    ]
    for b1 in leads:
        cases.append(bytes([b1]))                       # 截断
        cases.append(bytes([b1, trails[0]]))
        cases.append(bytes([b1, trails[-1]]))
        cases.append(bytes([b1, 0x00]))
        cases.append(bytes([b1, 0x20]))
        cases.append(bytes([b1, 0x7F]))
        cases.append(bytes([b1, 0x80]))
        cases.append(bytes([b1, 0xFF]))
        cases.append(bytes([b1, trails[0], 0x41]))      # 合法对 + ASCII
    return cases


# 代表码位（从生成器同源的枚举里挑的**真实可编码位**，每个编码都用得上）
MW_ENCODE_CASES = {
    "big5": ["中文", "潛行者", "漢字", "a中b", "﹛", "十", "丅", "Ｃ", "￥", "ascii only"],
    "shift_jis": ["日本語", "こんにちは", "ハン漢字", "｡ﾟｱ", "‐、。", "中文", "aあb",
                  "｡ﾟｱｰｲ", "ascii only"],
    "euc-kr": ["한국어", "안녕하세요", "中文", "a한b", "히힝", "ascii only"],
}

# ── ★ 编码侧反向锁 ───────────────────────────────────────────────────────
# 判据是"**把编码分支删掉就立刻变红**"，所以期望值写成**字面量**（不是现算自 Python）：
# 现算的话，引擎没有编码侧时 `encode_lossy` 会走"逐码位写 `?`"的兜底，而 Python 的
# `errors="replace"` 也会对不可表示码位写 `?` —— 两边**恰好一样**，锁就成了摆设。
# 这正是 ALPHA.44 那次 `ascii` 缺陷的形状：`期望 6173636969206f6e6c79，得到 3f3f…`。
#
# 第一项 `"ascii only"` 是**纯 ASCII 正文**：只要编码侧分支在，它就是直通；
# 一旦分支被删，整串会变成 `3f3f3f3f…`，一个字都不留 —— 与 ascii 那次的报错同形。
REVERSE_LOCK = [
    # (编码, 正文, 期望字节)
    ("big5", "ascii only", b"ascii only"),
    ("shift_jis", "ascii only", b"ascii only"),
    ("euc-kr", "ascii only", b"ascii only"),
    ("big5", "中文", b"\xa4\xa4\xa4\xe5"),
    ("shift_jis", "こんにちは", b"\x82\xb1\x82\xf1\x82\xc9\x82\xbf\x82\xcd"),
    ("euc-kr", "안녕하세요", b"\xbe\xc8\xb3\xe7\xc7\xcf\xbc\xbc\xbf\xe4"),
    # shift_jis 的单字节半角片假名（走"单字节槽位"，与双字节槽位不同一条路）
    ("shift_jis", "｡", b"\xa1"),
    # big5 的 4 处同义重复：CPython 的编码器选的是顺序**较大**的那一格（不是最小序）
    ("big5", "／", b"\xa2\x41"),
    ("big5", "＼", b"\xa2\x42"),
    ("big5", "十", b"\xa4\x51"),
    ("big5", "卅", b"\xa4\xca"),
    # 不可表示 ⇒ 恰好一个 `?`（零静默：不许静默丢弃、不许截断）
    ("big5", "😀", b"?"),
    ("shift_jis", "😀", b"?"),
    ("euc-kr", "😀", b"?"),
    # ★ euc-kr 的 `A4 D4` 是**解码侧**的字母拼合序列；**编码侧不为它产字**
    #   （CPython 的 `'€'.encode('euc-kr')` 是 `A2 E6`）⇒ 反过来编码"一个字符"这件事
    #   必须仍按表来。这条同时钉住"别把 A4D4 当成可编码序列写出去"。
    ("euc-kr", "€", b"\xa2\xe6"),
]


def run_reverse_lock(work: Path, failures: list[str]) -> int:
    """编码侧反向锁：逐条按**字面量**核对（删分支即红）。"""
    compared = 0
    for codec, text, want in REVERSE_LOCK:
        compared += 1
        rc, got, out = run_probe("encode", codec, text.encode("utf-8"), work)
        if rc != 0:
            failures.append("反向锁 encode %s %r：CLI 退出码 %d（%s）"
                            % (codec, text[:12], rc, out[:120]))
        elif got != want:
            failures.append("反向锁 encode %s %r：期望 %s，得到 %s"
                            % (codec, text[:12], want[:32].hex(), got[:32].hex()))
    return compared


# ── `--full`：反向表**逐项**核对（覆盖三张表的全部可编码码位）──────────────
#
# 默认不跑。为什么值得单开一档：`REVERSE_LOCK` 只钉住了 16 条**手工挑的**期望值，
# 而反向表是"扫表 + 优先级规则"造出来的 —— 行数写错、优先级写反、覆盖项漏写这几种
# 错都可能在**没被挑到**的那些码位上。这里换成"整表导出 + Python 侧枚举同名 codec
# 能编出的**全部**码位逐条比"：一次调用覆盖 big5 13706 / shift_jis 6942 / euc-kr 8225。
#
# ★ 实测抓到的两个错（都是这一档才能发现的）：① shift_jis 建反向表时行数写成了
#   `188`（表真实只有 124 行）⇒ 循环读出表尾之外的内存，凭空造出 `FD44 → U+FF61`
#   这种假映射，`'｡'.encode` 于是变成 `fd44` 而不是 `a1`；② shift_jis 的单字节半角
#   片假名与双字节格子**优先级写反**（CPython 显式路由到单字节）。
MW_LEAD_INFO = {
    # 编码: (lead_first, cols, trail_first, rows)
    "big5": (0xA1, 190, 0x40, 89),
    "shift_jis": (0x81, 189, 0x40, 124),
    "euc-kr": (0xA1, 94, 0xA1, 93),
}


def dump_encode_table() -> dict[str, dict[int, str]]:
    proc = subprocess.run([str(CLI), "--codec-encode-table"], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace")
    if proc.returncode != 0:
        raise RuntimeError("--codec-encode-table 退出码 %d：%s" % (proc.returncode, text[:200]))
    out: dict[str, dict[int, str]] = {}
    ended = False
    for line in text.splitlines():
        s = line.strip()
        if s == "T END":
            ended = True
            continue
        if not s.startswith("T "):
            raise RuntimeError("--codec-encode-table 输出不可解析：%r" % line[:120])
        _, codec, cp, hexbytes = s.split()
        out.setdefault(codec, {})[int(cp)] = hexbytes
    if not ended:
        raise RuntimeError("--codec-encode-table 没打收尾行 `T END`（可能中断）")
    return out


def run_full_reverse_table(failures: list[str]) -> int:
    try:
        dumped = dump_encode_table()
    except Exception as exc:  # noqa: BLE001
        failures.append("整表导出失败：%s" % exc)
        return 0
    compared = 0
    for codec, table in dumped.items():
        # Python 侧枚举：这张表**能编出的全部码位**（与生成器同源）
        leads = MW_LEADS.get(codec)
        trails = MW_TRAILS.get(codec)
        if leads is None:
            failures.append("整表导出里出现了未知编码：%s" % codec)
            continue
        want_cps = set()
        for b1 in leads:
            for b2 in trails:
                try:
                    s = bytes([b1, b2]).decode(codec)
                except UnicodeDecodeError:
                    continue
                if len(s) == 1:
                    want_cps.add(ord(s))
        if codec == "shift_jis":
            for b in range(0xA1, 0xE0):
                want_cps.add(ord(bytes([b]).decode("shift_jis")))
        got_cps = set(table)
        missing = sorted(want_cps - got_cps)
        extra = sorted(got_cps - want_cps)
        if missing:
            failures.append("%s 反向表缺 %d 个码位（前几个：%s）"
                            % (codec, len(missing),
                               ", ".join("U+%04X" % c for c in missing[:6])))
        if extra:
            failures.append("%s 反向表多出 %d 个码位（表尾越界？前几个：%s）"
                            % (codec, len(extra), ", ".join("U+%04X" % c for c in extra[:6])))
        # 逐条比"编出来的字节"
        for cp, got in sorted(table.items()):
            compared += 1
            want = chr(cp).encode(codec)
            if bytes.fromhex(got) != want:
                if len(failures) < 40:
                    failures.append("%s U+%04X 反向表给 %s，Python 给 %s"
                                    % (codec, cp, got, want.hex()))
        print("  %-10s 逐项核对 %d 个码位（缺 %d / 多 %d）"
              % (codec, len(table), len(missing), len(extra)))
    return compared


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
    parser.add_argument("--full", action="store_true",
                        help="额外做三张新表反向编码的**逐项**核对（覆盖全部可编码码位）")
    args = parser.parse_args()

    if args.list:
        print("utf-8 %d 例 / 单字节 %d 例 / gb %d 例 / 编码方向 %d 例 / 反向锁 %d 例"
              % (len(utf8_cases()), len(single_byte_cases()), len(gb_cases()),
                 len(encode_cases()), len(REVERSE_LOCK)))
        for codec in ("big5", "shift_jis", "euc-kr"):
            print("  %-10s 解码 %d 例 / 编码 %d 例"
                  % (codec, len(mw_cases(codec)), len(MW_ENCODE_CASES[codec])))
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
        # ★ D2 批：ascii 的"有损"语义 = 非 ASCII 一律 `?` / 非法字节一律 U+FFFD。
        #   用例组与单字节家族同一套（含 `bytes(range(256))` 全排、纯 ascii、空、`\xff`*4）。
        ("ascii", single_byte_cases()),
        # ★ D3 批：三张东亚表（每个 lead 都覆盖到，见 mw_cases 的说明）
        ("big5", mw_cases("big5")),
        ("shift_jis", mw_cases("shift_jis")),
        ("euc-kr", mw_cases("euc-kr")),
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
        # ★ D3 批：三张新表的"真实可编码位"挑字（ASCII 正文 + 表里查得到的码位），
        #   走现算的 Python 参照 —— 它盯的是**表里那批码位**编出来对不对。
        for codec in ("big5", "shift_jis", "euc-kr"):
            for text in MW_ENCODE_CASES[codec]:
                want = encode_reference(codec, text)
                rc, got, out = run_probe("encode", codec, text.encode("utf-8"), work)
                compared += 1
                if rc != 0:
                    failures.append("encode %s %r：CLI 退出码 %d（%s）"
                                    % (codec, text[:16], rc, out[:120]))
                elif got != want:
                    failures.append("encode %s %r：期望 %s，得到 %s"
                                    % (codec, text[:16], want[:32].hex(), got[:32].hex()))
        # ★ 编码侧反向锁：字面量期望值 ⇒ **删掉编码分支立刻红**（见 REVERSE_LOCK 注释）
        compared += run_reverse_lock(work, failures)
        # ★ `--full`：反向表逐项核对（覆盖三张表的全部可编码码位；默认不跑）
        if args.full:
            compared += run_full_reverse_table(failures)
        else:
            print("  （反向表逐项核对未跑：加 `--full`。句柄锁已由 REVERSE_LOCK 负责）")

    for item in failures[:12]:
        print("  ✗ %s" % item)
    if len(failures) > 12:
        print("  …（还有 %d 条）" % (len(failures) - 12))
    verdict = "FAIL" if failures else "PASS"
    print("GOLDEN-LOSSY-CODEC: %s (compared=%d failed=%d)" % (verdict, compared, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    # ★★ 2026-10-06（审计处理批·追加一 / 三·补3）：**任何失败路径都必须先打自己的收尾行**。
    #   病根（S1 批首跑 `--release` 撞到的偶发红）：这一步失败时**连 `GOLDEN-LOSSY-CODEC:` 行
    #   都没有** —— 别人无从判断它跑到哪、无法与旧日志比对（当初那趟总时长只有 8.6 min，
    #   远小于本步 600 s 超时 ⇒ 不是超时，就是"异常穿透、什么都没打"）。
    #   现在：未捕获异常/非零退出都在这里兜住，先打带 reason 的收尾行再退非零。
    #   顺手加**开始/结束时间戳与耗时**（下一个偶发自然留证；平时也不碍事）。
    import time as _time
    _t0 = _time.time()
    print("GOLDEN-LOSSY-CODEC: START %s"
          % _time.strftime("%Y-%m-%d %H:%M:%S", _time.localtime(_t0)), flush=True)
    try:
        _rc = main()
    except BaseException as _exc:   # noqa: BLE001 —— 连 KeyboardInterrupt/SystemExit 都要留证
        _dt = _time.time() - _t0
        print("GOLDEN-LOSSY-CODEC: FAIL (reason=%s: %s; elapsed=%.1fs)"
              % (type(_exc).__name__, _exc, _dt), flush=True)
        raise SystemExit(1)
    print("GOLDEN-LOSSY-CODEC: END rc=%d elapsed=%.1fs" % (_rc, _time.time() - _t0), flush=True)
    sys.exit(_rc)
