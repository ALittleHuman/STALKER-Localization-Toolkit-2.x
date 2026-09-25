"""探针：Python 的 utf-16/utf-32 家族在 errors="replace" 下的确切行为（2.x 有损编解码待补缺口）。

用法：python probe_utf_lossy.py
输出：解码（原始字节 → 码位序列）与编码（文本 → 字节）两张真值表，供 C++ 侧逐字节复刻。
"""
import sys

CODECS = ["utf-16", "utf-16-le", "utf-16-be", "utf-32", "utf-32-le", "utf-32-be"]

DECODE_CASES = {
    "empty": b"",
    "ascii_le": b"A\x00B\x00",
    "bom_le": b"\xff\xfeA\x00",
    "bom_be": b"\xfe\xff\x00A",
    "bom_only": b"\xff\xfe",
    "bom_only_be": b"\xfe\xff",
    "rev_bom": b"\xfe\xffA\x00",          # 反向 BOM + 小端数据（'utf-16' 会翻序）
    "odd_tail_1": b"A\x00B",              # 尾部 1 字节
    "odd_tail_3": b"A\x00B\x00C\x00\x00",  # 尾部 3 字节（utf-32 里是完整的）
    "lone_high": b"\x00\xd8",             # 单独高位代理 D800
    "lone_low": b"\x00\xdc",              # 单独低位代理 DC00
    "high_then_ascii": b"\x00\xd8A\x00",
    "pair": b"\x3d\xd8\x00\xde",          # D83D DE00 -> U+1F600
    "low_then_high": b"\x00\xdc\x00\xd8",
    "fffe": b"\xff\xfe\xff\xfe",          # BOM 之后的 U+FFFE
    "nul": b"\x00\x00A\x00",
    "utf32_ascii_le": b"A\x00\x00\x00",
    "utf32_bom_le": b"\xff\xfe\x00\x00A\x00\x00\x00",
    "utf32_bom_be": b"\x00\x00\xfe\xff\x00\x00\x00A",
    "utf32_odd_1": b"A\x00\x00",
    "utf32_odd_2": b"A\x00",
    "utf32_too_big": b"\x00\x00\x11\x00",  # U+110000 越界
    "utf32_surrogate": b"\x00\xd8\x00\x00",  # U+D800 单代理
}

ENCODE_CASES = [
    "A",
    "\u4e2d",
    "\U0001f600",
    "\ufeff",
    "\ud800",          # 单代理（Python 里能构造出来）
    "A\ud800B",
]


def show_seq(s: str) -> str:
    return " ".join("%04X" % ord(ch) for ch in s) if s else "<empty>"


def decode_table() -> None:
    print("== decode (errors=replace) ==")
    for name, raw in DECODE_CASES.items():
        row = [name, raw.hex()]
        for codec in CODECS:
            try:
                text = raw.decode(codec, errors="replace")
            except Exception as exc:  # noqa: BLE001
                row.append("%s:EXC(%s)" % (codec, type(exc).__name__))
                continue
            row.append("%s:%s" % (codec, show_seq(text)))
        print(" | ".join(row))


def encode_table() -> None:
    print("== encode (errors=replace) ==")
    for text in ENCODE_CASES:
        row = ["U+%s" % show_seq(text)]
        for codec in CODECS:
            try:
                out = text.encode(codec, errors="replace")
            except Exception as exc:  # noqa: BLE001
                row.append("%s:EXC(%s)" % (codec, type(exc).__name__))
                continue
            row.append("%s:%s" % (codec, out.hex()))
        print(" | ".join(row))


def roundtrip_notes() -> None:
    print("== notes ==")
    print("native byte order (no BOM) for 'utf-16':", b"A\x00".decode("utf-16"))
    print("sys.byteorder:", sys.byteorder)


if __name__ == "__main__":
    decode_table()
    encode_table()
    roundtrip_notes()
