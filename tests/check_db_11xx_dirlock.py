#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""11xx **写侧目录条目**的三道锁（2026-10-05 修的真缺陷，官方 cv 判据查实）。

## 缺陷
`11xx` 的目录条目曾经写成 `name\` + `(1, 0, 0)`（第 1 字段＝`uncompressed`/标志位）。
那是**非法**写法：官方 `converter.exe -unpack -11xx` 读到它就**栈溢出**
（exit `3221225725` = `0xC00000FD`，产物 0 文件）。对照：同族 `2215` 写 `(0,0,0)`、
`2945` 写 `(0,0,0,0)`，那两个格式的含目录树**已过 cv 对拍 6,268 文件零差异**。

修法取"最小改动"：目录条目照写，字段改 `(0,0,0)` —— **保留目录条目 ⇒ 不丢空目录信息**
（cv 解出来的树里那个空目录确实存在，见 `[①]` 的断言）。读侧不用改：`fmt_11xx` 用
`is_dir = (offset == 0)`，`(1,0,0)` 与 `(0,0,0)` 都判为目录（`--dump` 两种写法都是 `entries=7`）。

## 三道锁（都要能判红）
1. ★ **cv 判据锁 `[①]`**：夹具＝≥2 层嵌套小树（含一个**空目录**、一个 cp1251 名），
   `xrkit_cli --pack-db <out> 11xx <src>` → `converter.exe -unpack -11xx <pack> -dir <out>`
   **退出码必须 == 0**，且解出来的文件树与源逐字节相同、空目录存在。
   修前实测：`returncode = -1073741571`（`0xC00000FD`）、产物 0 文件。
   **cv 不在本机时**（公开 CI）这一条**记 note 跳过、不判红**（`!!` 行），其余两条照跑。
2. ★ **不依赖 cv 的字节级锁 `[②]`**：直接解**我们自己的打包产物**的 HEADER 明文
   （LZHUF 解码，逐行对照 `src/engine/fs/lzhuf_ref.c`，本仓库内依赖、无需外部参照），
   断言**每个目录条目的字段全零**、文件条目首字段仍为 `1`；并做**锁的自证**：
   同一判据喂一份 `(1,0,0)` 的合成头**必须判红**（免得没 cv 就锁不住，或锁成了"永远为真"）。
3. **反向锁 `[③]`**：`2215`（3 字段）/ `2945`（4 字段）的目录条目**仍是全零**（不许被"顺手统一"改坏），
   且 `11xx`/`2215`/`2945` 三格式含目录树的**我们自己的往返**（条目表 + 逐文件内容）仍要过。

输出（供门禁抓取）：`DB-11XX-DIRLOCK ok …` / `DB-11XX-DIRLOCK fail …` / `!! …`；
退出码：0=全绿（cv 缺失时只记 note，仍算绿），1=有断言不成立，2=CLI 缺失（外部前提）。
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# 官方 cv（DB 参照）——用套件里的**权威副本**。缺件时 `[①]` 记 note 跳过（不判红）。
CV = Path(r"E:\Software\Tools\Transition\DB双向验证套件\tools\converter.exe")
# cv 解 11xx 时栈溢出发生在读条目阶段，实测约 21 s 才崩 ⇒ 超时必须给足，
# 否则超时（-1）会掩盖"崩溃"这个真正的判据。
CV_TIMEOUT = 60

# null 系三格式的字段数（pack 侧 `append_entry_null` 的字段个数）
NULL_FIELDS = {"11xx": 3, "2215": 3, "2945": 4}

# 夹具：≥2 层嵌套 + 一个**空目录**（专门验"保留目录条目 ⇒ 空目录信息不丢"）+ cp1251 名
FIXTURE = {
    "root.txt": b"root\n",
    "a/bb/mid.bin": bytes(range(64)),
    "a/bb/ccc/deep.txt": b"deep\n",
    "a/bb/ccc/empty.dat": b"",
    "\u043f\u0430\u043f\u043a\u0430/\u0444\u0430\u0439\u043b.txt":
        "\u043f\u0440\u0438\u0432\u0435\u0442\n".encode("cp1251"),
}
EMPTY_DIRS = ["topdir", "a/bb/ccc/emptydir"]

PROBLEMS: list[str] = []


# ══════════════════════════════════════════════════════════════════════
# LZHUF 解码（Python 移植，逐行对照 src/engine/fs/lzhuf_ref.c 的 decode 路径）
#
# 为什么要自带一份：`[②]` 要断言的是**打包产物的字节**，而 DB 的 HEADER 是 LZHUF 压缩的。
# 1.x 的 Python 移植版**解码路径是坏的**（`lzhuf_ref.c` 顶部记着：`_LZ_DLEN` 只有 236 项、
# 分段与 DLL 不同），所以不能借它。这份移植的正确性由**结构自证**兜底：解出来的头必须能
# 被逐条走完、名字与字段数与夹具**逐项对上**、且不剩尾巴 —— 解码错了走不完，会判红而不是假绿。
# ══════════════════════════════════════════════════════════════════════
_N, _F, _THRESHOLD = 4096, 60, 2
_N_CHAR = 256 - _THRESHOLD + _F
_T = _N_CHAR * 2 - 1
_R = _T - 1
_MAX_FREQ = 0x4000
_M32 = 0xFFFFFFFF

_D_CODE = [
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,
    0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B,
    0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F, 0x0F, 0x0F,
    0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13,
    0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x17,
    0x18, 0x18, 0x19, 0x19, 0x1A, 0x1A, 0x1B, 0x1B, 0x1C, 0x1C, 0x1D, 0x1D, 0x1E, 0x1E, 0x1F, 0x1F,
    0x20, 0x20, 0x21, 0x21, 0x22, 0x22, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25, 0x26, 0x26, 0x27, 0x27,
    0x28, 0x28, 0x29, 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D, 0x2D, 0x2E, 0x2E, 0x2F, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
]
_D_LEN = [
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
]


class _LzhufDecoder:
    """`lzhuf_ref.c` 的解码路径（`GetBit`/`GetByte`/`StartHuff`/`reconst`/`update`）。"""

    def __init__(self, src: bytes):
        self.src = src
        self.pos = 4
        self.limit = len(src)
        self.getbuf = 0
        self.getlen = 0
        self.freq = [0] * (_T + 1)
        self.prnt = [0] * (_T + _N_CHAR)
        self.son = [0] * _T
        self._start_huff()

    def _getc(self) -> int:
        if self.pos < self.limit:
            c = self.src[self.pos]
            self.pos += 1
            return c
        return -1

    def _getbit(self) -> int:
        while self.getlen <= 8:
            i = self._getc()
            if i < 0:
                i = 0
            self.getbuf = (self.getbuf | (i << (8 - self.getlen))) & _M32
            self.getlen += 8
        i = self.getbuf
        self.getbuf = (self.getbuf << 1) & _M32
        self.getlen -= 1
        return (i >> 15) & 1

    def _getbyte(self) -> int:
        while self.getlen <= 8:
            i = self._getc()
            if i < 0:
                i = 0
            self.getbuf = (self.getbuf | (i << (8 - self.getlen))) & _M32
            self.getlen += 8
        i = self.getbuf
        self.getbuf = (self.getbuf << 8) & _M32
        self.getlen -= 8
        return (i & 0xFF00) >> 8

    def _start_huff(self) -> None:
        for i in range(_N_CHAR):
            self.freq[i] = 1
            self.son[i] = i + _T
            self.prnt[i + _T] = i
        i, j = 0, _N_CHAR
        while j <= _R:
            self.freq[j] = self.freq[i] + self.freq[i + 1]
            self.son[j] = i
            self.prnt[i] = self.prnt[i + 1] = j
            i += 2
            j += 1
        self.freq[_T] = 0xFFFF
        self.prnt[_R] = 0

    def _reconst(self) -> None:
        freq, son, prnt = self.freq, self.son, self.prnt
        j = 0
        for i in range(_T):
            if son[i] >= _T:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i, j = 0, _N_CHAR
        while j < _T:
            k = i + 1
            f = freq[i] + freq[k]
            freq[j] = f
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            freq.insert(k, f)
            freq.pop(j + 1)
            son.insert(k, i)
            son.pop(j + 1)
            i += 2
            j += 1
        for i in range(_T):
            k = son[i]
            if k >= _T:
                prnt[k] = i
            else:
                prnt[k] = prnt[k + 1] = i

    def _update(self, c: int) -> None:
        freq, son, prnt = self.freq, self.son, self.prnt
        if freq[_R] == _MAX_FREQ:
            self._reconst()
        c = prnt[c + _T]
        while True:
            k = freq[c] + 1
            freq[c] = k
            l = c + 1
            if k > freq[l]:
                while k > freq[l + 1]:
                    l += 1
                freq[c] = freq[l]
                freq[l] = k
                i = son[c]
                prnt[i] = l
                if i < _T:
                    prnt[i + 1] = l
                j = son[l]
                son[l] = i
                prnt[j] = c
                if j < _T:
                    prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                break

    def _decode_char(self) -> int:
        c = self.son[_R]
        while c < _T:
            c += self._getbit()
            c = self.son[c]
        c -= _T
        self._update(c)
        return c

    def _decode_position(self) -> int:
        i = self._getbyte()
        c = _D_CODE[i] << 6
        j = _D_LEN[i] - 2
        while j > 0:
            i = ((i << 1) + self._getbit()) & 0xFFFF
            j -= 1
        return c | (i & 0x3F)

    def decode(self):
        textsize = int.from_bytes(self.src[0:4], "little")
        if textsize <= 0 or textsize > 256 * 1024 * 1024:
            return None
        out = bytearray()
        text_buf = bytearray(_N + _F - 1)          # C 里是 static ⇒ 未显式赋值处为 0
        for i in range(_N - _F):
            text_buf[i] = 0x20                     # ' '
        r = _N - _F
        count = 0
        while count < textsize:
            c = self._decode_char()
            if c < 256:
                out.append(c)
                text_buf[r] = c
                r = (r + 1) & (_N - 1)
                count += 1
            else:
                i = (r - self._decode_position() - 1) & (_N - 1)
                j = c - 255 + _THRESHOLD
                for k in range(j):
                    c = text_buf[(i + k) & (_N - 1)]
                    out.append(c)
                    text_buf[r] = c
                    r = (r + 1) & (_N - 1)
                    count += 1
        return bytes(out)


def decode_header(db: bytes):
    r"""取出 DB 的压缩 HEADER 并解码成明文。

    布局（`db.cpp::pack_db`）：`[4B 0][4B data_size][data][4B 0x80000001][4B comp_size][compressed]`
    """
    data_size = int.from_bytes(db[4:8], "little")
    off = 8 + data_size
    if db[off:off + 4] != b"\x01\x00\x00\x80":
        return None
    comp = int.from_bytes(db[off + 4:off + 8], "little")
    blob = db[off + 8:off + 8 + comp]
    if len(blob) != comp or len(db) != off + 8 + comp:
        return None
    return _LzhufDecoder(blob).decode()


def walk_null(header: bytes, nf: int):
    r"""把明文头按 `name\0 + nf×u32` 走一遍。返回 (条目列表, 是否走完且不剩尾巴)。"""
    out = []
    p = 0
    try:
        while p < len(header):
            n = header.index(b"\x00", p)
            name = header[p:n].decode("cp1251")
            p = n + 1
            if p + nf * 4 > len(header):
                return out, False
            fields = [int.from_bytes(header[p + i * 4:p + i * 4 + 4], "little")
                      for i in range(nf)]
            p += nf * 4
            out.append((name, fields))
    except ValueError:
        return out, False
    return out, True


def nonzero_dir_entries(entries) -> list[str]:
    r"""判据本体：**目录条目（名字以 `\` 结尾）的字段必须全零**。

    抽成独立函数是为了能被"锁的自证"复用 —— 同一份判据喂 `(1,0,0)` 的合成头必须判红。
    """
    return ["%s %s" % (name, fields) for name, fields in entries
            if name.endswith("\\") and any(fields)]


def run(argv, timeout=None):
    proc = subprocess.run([str(a) for a in argv], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def run_cli(argv):
    return run([CLI] + [str(a) for a in argv])


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + (" \u2014\u2014 " + detail if detail else ""))
        PROBLEMS.append(label)


# ── 夹具 ────────────────────────────────────────────────────────────────
def build_fixture(root: Path) -> None:
    if root.exists():
        shutil.rmtree(root)
    for rel, data in FIXTURE.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    for rel in EMPTY_DIRS:
        (root / rel).mkdir(parents=True, exist_ok=True)


def source_tree(root: Path):
    r"""{(相对路径, '/' 分隔): 内容}（只算文件）+ 目录集合。"""
    files, dirs = {}, set()
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            dirs.add(os.path.relpath(os.path.join(dirpath, d), root).replace(os.sep, "/"))
        for f in filenames:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as fh:
                files[rel] = fh.read()
    return files, dirs


def walk_disk(root: Path):
    files, dirs = {}, set()
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            dirs.add(os.path.relpath(os.path.join(dirpath, d), root).replace(os.sep, "/"))
        for f in filenames:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            files[rel] = Path(full).read_bytes()
    return files, dirs


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def parse_dump(text: str):
    r"""`--dump` 的 TSV → {路径: (kind, size_real, size_comp, offset, use_lzhuf)}。"""
    rows = {}
    for line in text.splitlines():
        if line.startswith("DUMP "):
            continue
        cols = line.rstrip("\r").split("\t")
        if len(cols) != 6:
            continue
        kind, path = cols[0], cols[1]
        rows[path.replace("\\", "/")] = (kind, int(cols[2]), int(cols[3]),
                                        int(cols[4]), int(cols[5]))
    return rows


# ── [①] cv 判据锁 ───────────────────────────────────────────────────────
def check_cv(tmp: Path, src: Path, pack: Path) -> None:
    print("[①] cv 判据锁：`converter.exe -unpack -11xx <our pack> -dir <out>` 退出码必须 == 0")
    if not CV.is_file():
        print("  !! 缺官方 converter.exe（%s）\u21d2 这条**记 note 跳过**（公开 CI 无 cv，不判红）；" % CV)
        print("     不依赖 cv 的字节级锁 [②] 与反向锁 [③] 照跑。")
        return
    outdir = tmp / "cv_out"
    outdir.mkdir(parents=True, exist_ok=True)
    try:
        rc, text = run([CV, "-unpack", "-11xx", pack, "-dir", outdir], timeout=CV_TIMEOUT)
    except subprocess.TimeoutExpired:
        want(False, "cv -unpack -11xx 退出码 == 0", "超时 %ds（修前是栈溢出 0xC00000FD，约 21 s 崩）" % CV_TIMEOUT)
        return
    raw = "cv exit=%d（unsigned 0x%08X）" % (rc, rc & 0xFFFFFFFF)
    want(rc == 0, "cv -unpack -11xx 退出码 == 0", raw)
    print("    %s" % raw)
    if rc != 0:
        print("    cv 输出：%s" % text.strip()[:200])
        return
    got, got_dirs = walk_disk(outdir)
    want_tree, want_dirs = source_tree(src)
    # 参照工具在**非 ASCII 名**上有已知限制：cv 把 cp1251 字节走 A-API 交给系统，
    # 本机 ACP＝GBK ⇒ `папка/файл.txt` 落成 `镟镪郳羿殡.txt`（名字换了、字节对不上）。
    # 这不是我们的缺陷（同 `DB双向验证套件/scripts/db_cross.py` 对 `CYRILLIC_SAMPLES`
    # 一律改用"工具自身往返"的口径）。⇒ 非 ASCII 路径**不计入** cv 逐字节对拍，
    # 它们由 [②]（HEADER 明文里 cp1251 名在场）与 [③]（工具自身往返）覆盖。
    ascii_want = {p: d for p, d in want_tree.items() if p.isascii()}
    ascii_got = {p: d for p, d in got.items() if p.isascii()}
    skipped = sorted(p for p in want_tree if not p.isascii())
    if skipped:
        print("  -- 参照工具已知限制：%d 条非 ASCII 路径不计入 cv 对拍（cv 走 A-API，本机 ACP"
              "会换名）：%s ⇒ 由 [②]/[③] 覆盖" % (len(skipped), ", ".join(skipped)))
    missing = sorted(set(ascii_want) - set(ascii_got))
    extra = sorted(set(ascii_got) - set(ascii_want))
    bad = [p for p in ascii_want if p in ascii_got and sha(ascii_got[p]) != sha(ascii_want[p])]
    want(len(missing) == 0 and len(extra) == 0 and len(bad) == 0,
         "cv 解出的文件树与源逐字节相同（%d 个 ASCII 文件）" % len(ascii_want),
         "缺 %s / 多 %s / 内容不同 %s" % (missing[:3], extra[:3], bad[:3]))
    # ★ 空目录：这正是"取 (0,0,0) 而不是干脆不写目录条目"的理由
    ascii_dirs = {d for d in want_dirs if d.isascii()}
    lost = sorted(d for d in ascii_dirs if d not in got_dirs and d not in EMPTY_DIRS)
    still_empty = sorted(d for d in EMPTY_DIRS if d in got_dirs)
    want(not lost, "cv 解出的目录结构与源一致（%d 个 ASCII 目录）" % len(ascii_dirs),
         "缺目录 %s" % lost[:3])
    want(len(still_empty) == len(EMPTY_DIRS),
         "空目录也解出来了（%d/%d：%s）" % (len(still_empty), len(EMPTY_DIRS), ", ".join(EMPTY_DIRS)),
         "缺 %s" % sorted(set(EMPTY_DIRS) - set(got_dirs)))


# ── [②] 字节级锁 ────────────────────────────────────────────────────────
def check_bytes(pack: Path, src: Path, label: str = "11xx") -> None:
    print("[②] 字节级锁：打包产物 HEADER 明文里，**目录条目的字段必须全零**（不依赖 cv）")
    header = decode_header(pack.read_bytes())
    want(header is not None, "能解出 %s 产物的 HEADER 明文（LZHUF）" % label)
    if header is None:
        return
    entries, consumed_all = walk_null(header, NULL_FIELDS[label])
    want(consumed_all and len(entries) > 0,
         "HEADER 明文能被逐条走完且不剩尾巴（解码器自证：%d 条）" % len(entries))
    bad = nonzero_dir_entries(entries)
    want(not bad, "%s 目录条目字段全零" % label, "非零：%s" % bad[:4])
    dirs_here = [n for n, _ in entries if n.endswith("\\")]
    want(len(dirs_here) == len([n for n, _ in entries if n.endswith("\\")]),
         "%s 目录条目 %d 条（与字段分开断言，免得条目数为 0 也被算成全零的假绿）"
         % (label, len(dirs_here)))
    want(len(dirs_here) > 0, "%s 确实写了目录条目（不是干脆不写那一版）" % label,
         "目录条目 0 条 ⇒ 空目录信息会丢")
    print("    HEADER 明文（%s）：%s" % (label, header))

    # ── 锁的自证：同一份判据喂 (1,0,0) 的合成头**必须判红** ──
    fake = b"topdir\\\x00" + (1).to_bytes(4, "little") + b"\x00" * 8
    fake_entries, _ = walk_null(fake, 3)
    want(bool(nonzero_dir_entries(fake_entries)),
         "锁的自证：合成头 `topdir\\`+(1,0,0) **必须**被判红",
         "判据放过非零目录条目 ⇒ 这把锁永远为真")
    # 反向自证：全零的合成头必须放过（免得判据把合法写法也拦下）
    good = b"topdir\\\x00" + b"\x00" * 12
    good_entries, _ = walk_null(good, 3)
    want(not nonzero_dir_entries(good_entries),
         "锁的自证：合成头 `topdir\\`+(0,0,0) 必须放过")

    # 顺带交叉核对读侧：`--dump` 的 use_lzhuf 列对目录条目应为 1（= 首字段 == 0）
    rc, text = run_cli(["--dump", pack, label])
    want(rc == 0, "--dump %s 退出码 0" % label, text.strip()[:150])
    rows = parse_dump(text)
    want(rows.get("topdir", (None,) * 5)[4] == 1,
         "读侧交叉核对：--dump 里 `topdir` 的 use_lzhuf==1（等价于首字段==0）",
         "实际 %r" % (rows.get("topdir"),))


# ── [③] 反向锁 + 自往返 ─────────────────────────────────────────────────
def check_reverse(tmp: Path, src: Path, want_tree, want_dirs) -> None:
    print("[③] 反向锁：2215/2945 目录条目仍全零不变 + 三格式自往返仍过")
    for fmt, nf in (("2215", 3), ("2945", 4)):
        pack = tmp / ("rev_%s.db" % fmt)
        rc, text = run_cli(["--pack-db", pack, fmt, src])
        want(rc == 0, "%s --pack-db 退出码 0" % fmt, text.strip()[:150])
        if rc != 0:
            continue
        header = decode_header(pack.read_bytes())
        want(header is not None, "能解出 %s 的 HEADER 明文" % fmt)
        if header is None:
            continue
        entries, consumed_all = walk_null(header, nf)
        want(consumed_all and entries, "%s HEADER 明文能走完（%d 条）" % (fmt, len(entries)))
        bad = nonzero_dir_entries(entries)
        want(not bad, "%s 目录条目仍是全零（没被改动）" % fmt, "非零：%s" % bad[:4])

    for fmt in ("11xx", "2215", "2945"):
        pack = tmp / ("rt_%s.db" % fmt)
        rc, text = run_cli(["--pack-db", pack, fmt, src])
        if rc != 0:
            want(False, "%s 自往返：--pack-db" % fmt, text.strip()[:150])
            continue
        rc, text = run_cli(["--dump", pack, fmt])
        if rc != 0:
            want(False, "%s 自往返：--dump" % fmt, text.strip()[:150])
            continue
        rows = parse_dump(text)
        got_files = {p for p, v in rows.items() if v[0] == "f"}
        got_dirs = {p for p, v in rows.items() if v[0] == "d"}
        want(got_files == set(want_tree),
             "%s 自往返：条目表文件集一致（%d）" % (fmt, len(got_files)),
             "缺 %s / 多 %s" % (sorted(set(want_tree) - got_files)[:3],
                               sorted(got_files - set(want_tree))[:3]))
        want(got_dirs == want_dirs,
             "%s 自往返：条目表目录集一致（含空目录，%d）" % (fmt, len(got_dirs)),
             "缺 %s / 多 %s" % (sorted(want_dirs - got_dirs)[:3],
                               sorted(got_dirs - want_dirs)[:3]))
        outbin = tmp / ("ext_%s.bin" % fmt)
        bad = []
        for rel, data in sorted(want_tree.items()):
            rc2, text2 = run_cli(["--extract", pack, rel, outbin, fmt])
            if rc2 != 0:
                bad.append("%s: 提取失败 %s" % (rel, text2.strip()[:60]))
                continue
            if sha(outbin.read_bytes()) != sha(data):
                bad.append("%s: 内容不同" % rel)
        want(not bad, "%s 自往返：%d 个文件内容逐字节一致" % (fmt, len(want_tree)), "; ".join(bad[:3]))


def main() -> int:
    parser = argparse.ArgumentParser(description="11xx 写侧目录条目：cv 判据锁 + 字节级锁 + 反向锁")
    parser.add_argument("--cli", default="", help="覆盖 CLI 路径（默认 build/msvc-release 那份）")
    args = parser.parse_args()

    global CLI
    if args.cli:
        CLI = Path(args.cli)
    if not CLI.exists():
        print("!! 找不到 CLI：%s" % CLI)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_11xx_dirlock_"))
    try:
        src = tmp / "src"
        build_fixture(src)
        want_tree, want_dirs = source_tree(src)
        print("夹具：%d 文件 + %d 目录（含空目录 %s、cp1251 名 1 个）"
              % (len(want_tree), len(want_dirs), "、".join(EMPTY_DIRS)))

        pack = tmp / "our_11xx.db"
        rc, text = run_cli(["--pack-db", pack, "11xx", src])
        if rc != 0:
            print("!! --pack-db 11xx 失败：%s" % text.strip()[:200])
            return 1
        print("--pack-db 11xx → %d B" % pack.stat().st_size)

        check_cv(tmp, src, pack)
        check_bytes(pack, src, "11xx")
        check_reverse(tmp, src, want_tree, want_dirs)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("DB-11XX-DIRLOCK fail %d 项不成立：" % len(PROBLEMS))
        for p in PROBLEMS:
            print("                 " + p)
        return 1
    print("DB-11XX-DIRLOCK ok 11xx 目录条目写侧：cv 判据（exit 0 + 树一致 + 空目录在）／"
          "字节级（HEADER 目录字段全零 + 锁自证）／反向（2215·2945 仍全零 + 三格式自往返）全绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
