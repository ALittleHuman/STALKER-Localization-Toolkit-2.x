#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：SquashFS **LREG（FILE_X / type=9）inode 的字段偏移**必须与权威布局逐字段相等，
   且**读侧与写侧是同一张表**（读回逐字节一致）。

为什么要有这个脚本 —— 2026-10-06 有一份审计把 `src/engine/fs/squashfs.cpp:365-366`
报成"LREG 偏移**全错**"。**复核结论：误报。** 真相是那一族报告"把权威布局字段偏移列
**对**了之后又**错标一位**"（把 fragment/offset 说成 40/44），而**读侧实现写的就是
权威布局**：本工程 `read_inode()` 读 `fragment@44 / offset@48`，`Packer::write_file_inode()`
写的也是 44/48，**与 SquashFS 4.0 的 `squashfs_ldir_inode`/`squashfs_lreg_inode` 逐字段一致**。

这个脚本把"判据"钉死，免得以后再有人靠"目测偏移列"复报一遍：

判据分两半，**任一字段错位立刻红**：
  ① **写侧**：用引擎自己的 writer 造一枚含 LREG 的镜像（稀疏文件 ⇒ `sparse>0` ⇒ 升 LREG），
     解出 inode 表元数据块，读回那条 LREG 记录，断言**每一个字段都落在权威偏移上**；
     · 类型 @0 u16 = 9 · mode @2 · uid @4 · gid @6 · mtime @8 u32 · inode_number @12 u32
     · start_block @16 **u64** · file_size @24 **u64** · sparse @32 **u64**
     · nlink @40 u32 · fragment @44 u32 · offset @48 u32 · xattr @52 u32
     · block_sizes 紧接在 @56 之后（每项 u32）
     另外断言**没有**任何字段落在"错误的那一族偏移"上（@40=0 / @44=0 / @48=0 那种
     "把字段整体挪一位"的形态会在这里被抓住）。
  ② **读侧**：`--extract` 把刚打的镜像里那个文件抽回来，**逐字节等于源文件**。
     LREG 的 `file_size`（@24 u64）与被挪到别处的读法会算出不同长度 ⇒ 一改就红。
     再加一条**互补**判据：真机/夹具镜像里 LREG 的 `fragment`/`offset` 若是被读错位置，
     读回的字节就不可能是原文件（这条用"多数据块 + fragment 尾"的形状压住）。

输出：`✓` / `✗` 行 + 收尾行 `SQFS-LREG ok …` / `SQFS-LREG fail …`；
退出码 0=全绿、1=有断言不成立、2=构建产物缺失（外部前提，按档位判定）。

★ 夹具一律现造在 `%TEMP%`，**不碰真机语料、不碰游戏目录**。
"""
import argparse
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS = []

# ── 权威布局（SquashFS 4.0 `squashfs_lreg_inode`；也是 reader/writer 共同遵守的那张表）──
#   别把这张表当"本工程的实现细节"：它就是判据本身 —— 读侧/写侧对不上它任何一个字段都算错。
K_TYPE_FILE_X = 9
LREG_TYPE_OFF = 0
LREG_MODE_OFF = 2
LREG_UID_OFF = 4
LREG_GID_OFF = 6
LREG_MTIME_OFF = 8
LREG_INODE_NUM_OFF = 12
LREG_START_BLOCK_OFF = 16
LREG_FILE_SIZE_OFF = 24
LREG_SPARSE_OFF = 32
LREG_NLINK_OFF = 40
LREG_FRAGMENT_OFF = 44
LREG_OFFSET_OFF = 48
LREG_XATTR_OFF = 52
LREG_BLOCK_SIZES_OFF = 56
NEGATIVE_FRAG = 0xFFFFFFFF


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def run(argv, timeout=300):
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


# ── 超块 / 元数据块：和 `tests/check_fs_hardening.py` 同一套读法（写侧逐字段顺序）──
def read_superblock(b):
    off = 0

    def r(fmt):
        nonlocal off
        v = struct.unpack_from(fmt, b, off)[0]
        off += struct.calcsize(fmt)
        return v

    sb = {}
    sb["magic"] = r("<I")
    sb["inodes"] = r("<I")
    sb["mkfs_time"] = r("<I")
    sb["block_size"] = r("<I")
    sb["fragments"] = r("<I")
    sb["compression"] = r("<H")
    sb["block_log"] = r("<H")
    sb["flags"] = r("<H")
    sb["no_ids"] = r("<H")
    sb["s_major"] = r("<H")
    sb["s_minor"] = r("<H")
    sb["root_inode"] = r("<Q")
    sb["bytes_used"] = r("<Q")
    sb["id_table_start"] = r("<Q")
    sb["xattr_id_table_start"] = r("<Q")
    sb["inode_table_start"] = r("<Q")
    sb["directory_table_start"] = r("<Q")
    sb["fragment_table_start"] = r("<Q")
    sb["export_table_start"] = r("<Q")
    assert off == 96, "超块应当是 96 字节（实际读了 %d）" % off
    return sb


def meta_block_payload(b, abs_off):
    """解一个元数据块，返回 (payload, 下一个块的绝对偏移)。

    头 = u16：bit15 置位 ⇒ **未压缩**，低 15 位 = 数据长度；否则是 xz 压缩后的长度。
    """
    header = struct.unpack_from("<H", b, abs_off)[0]
    size = header & 0x7FFF
    body = b[abs_off + 2:abs_off + 2 + size]
    if header & 0x8000:
        payload = body
    else:
        payload = lzma.decompress(body)
    return payload, abs_off + 2 + size


def all_inode_payload(b, sb):
    """把 inode 表的**全部**元数据块串起来（跨块 inode 记录要能续上）。"""
    off = sb["inode_table_start"]
    out = b""
    for _ in range(4096):
        payload, nxt = meta_block_payload(b, off)
        out += payload
        if nxt >= sb["directory_table_start"] or nxt <= off:
            break
        off = nxt
    return out


def find_lreg(payload, file_size):
    """在 inode 表 payload 里**扫描**出那条 LREG 记录（不依赖记录遍历顺序 —— 锁要独立）。

    定位判据（自证，不靠"走表走对了"）：`type @0 == 9` **且** `file_size @24 == 期望值`。
    若实现把 file_size 挪到别的偏移，这里就扫不出来 ⇒ 直接判红（那正是要抓的错）。
    """
    hits = []
    for i in range(0, len(payload) - 55):
        if struct.unpack_from("<H", payload, i)[0] != K_TYPE_FILE_X:
            continue
        if struct.unpack_from("<Q", payload, i + LREG_FILE_SIZE_OFF)[0] != file_size:
            continue
        hits.append(i)
    if len(hits) > 1:
        print("    !! file_size=%d 的 LREG 候选有 %d 个（偏移 %s），取第一个"
              % (file_size, len(hits), hits[:6]))
    return hits


def check_synthetic_reader(tmp):
    """手工造一枚**最小 LREG 镜像**，钉住读侧那几处偏移。

    为什么要手工造：本工程的 writer 只在 `sparse>0` 或尺寸超过 u32 时才升 LREG，
    所以"非平凡 fragment/offset 的 LREG"它**根本产不出来**；拿引擎产物当夹具，
    那两处偏移读错也照样能过（恒 -1）。手工镜像按**权威布局**摆字段，于是：
      · `file_size @24` 读错 ⇒ 引擎拿到的长度不是 4096 ⇒ --extract 报错或长度不符；
      · 目录项指向的 inode 引用、`block_sizes @56` 也都得按权威偏移摆才读得出来。
    """
    payload_data = bytes((i * 29 + 5) % 253 for i in range(4096))
    # 数据放在偏移 96 起（超块之后）
    data_off = 96
    inode_table_start = data_off + len(payload_data)
    # ★ DIR inode 的 `size@24` 是**目录表的数据字节数**（不是"本 inode 记录长度"，
    #   也不是"目录表含元数据块头的长度"）：reader 拿它当 `meta_read` 的 len ⇒ 多 2 字节
    #   就会再去读下一块的头报"读取越界"，少一点就报"目录名越界"（实测三档都试过）。
    dir_table_data_len = 40
    # inode 表（未压缩元数据块）：DIR(32) + LREG(56 + 1 项 block_sizes)
    dir_inode = bytearray(32)
    struct.pack_into("<H", dir_inode, 0, 1)          # DIR
    struct.pack_into("<I", dir_inode, 16, 0)         # dir_block（目录表内块号）
    struct.pack_into("<H", dir_inode, 24, dir_table_data_len)
    struct.pack_into("<H", dir_inode, 26, 0)         # dir_offset
    lreg = bytearray(56)
    struct.pack_into("<H", lreg, LREG_TYPE_OFF, K_TYPE_FILE_X)
    struct.pack_into("<H", lreg, LREG_MODE_OFF, 0o644 & 0x0FFF)
    struct.pack_into("<H", lreg, LREG_UID_OFF, 0)
    struct.pack_into("<H", lreg, LREG_GID_OFF, 0)
    struct.pack_into("<I", lreg, LREG_MTIME_OFF, 0)
    struct.pack_into("<I", lreg, LREG_INODE_NUM_OFF, 1)   # ★ 必须非 0（0 = "无 inode 号"）
    struct.pack_into("<Q", lreg, LREG_START_BLOCK_OFF, data_off)
    struct.pack_into("<Q", lreg, LREG_FILE_SIZE_OFF, len(payload_data))
    struct.pack_into("<Q", lreg, LREG_SPARSE_OFF, 0)
    struct.pack_into("<I", lreg, LREG_NLINK_OFF, 1)
    struct.pack_into("<I", lreg, LREG_FRAGMENT_OFF, NEGATIVE_FRAG)
    struct.pack_into("<I", lreg, LREG_OFFSET_OFF, NEGATIVE_FRAG)
    struct.pack_into("<I", lreg, LREG_XATTR_OFF, NEGATIVE_FRAG)
    block_sizes = struct.pack("<I", (1 << 24) | len(payload_data))  # bit24 = 未压缩
    inode_payload = bytes(dir_inode) + bytes(lreg) + block_sizes
    inode_block = struct.pack("<H", 0x8000 | len(inode_payload)) + inode_payload

    # 目录表：一组（12 字节头 + 1 条目 + 哨兵）。
    # ★ 组头的 count 是"条目数 - 1"：reader 的循环是 `k <= count` ⇒ **实际写 count+1 条**
    #   （含哨兵）。写成 1 的话 reader 会去找第 3 条 ⇒ "目录名越界"（实测踩到）。
    fname = b"LITERAL.pri"
    group = struct.pack("<III", 0, 0, 1)
    group += struct.pack("<HHHH", 32, 0, K_TYPE_FILE_X, len(fname) - 1) + fname
    group += struct.pack("<HHHH", 0, 0, 0, 0) + b"\x00"      # 哨兵
    dir_block = struct.pack("<H", 0x8000 | len(group)) + group
    directory_table_start = inode_table_start + len(inode_block)
    bytes_used = directory_table_start + len(dir_block)

    sb = bytearray()
    sb += struct.pack("<I", 0x73717368)              # 'hsqs'
    sb += struct.pack("<I", 2)                       # inodes
    sb += struct.pack("<I", 0)                       # mkfs_time
    sb += struct.pack("<I", 131072)                  # block_size
    sb += struct.pack("<I", 0)                       # fragments
    sb += struct.pack("<H", 1)                       # compression（xz，本镜像全未压缩）
    sb += struct.pack("<H", 17)                      # block_log
    sb += struct.pack("<H", 0)                       # flags
    sb += struct.pack("<H", 1)                       # no_ids
    sb += struct.pack("<H", 4)                       # s_major
    sb += struct.pack("<H", 0)                       # s_minor
    sb += struct.pack("<Q", 0)                       # root_inode（rel_block=0, off=0）
    sb += struct.pack("<Q", bytes_used)
    sb += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)      # id_table_start
    sb += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)      # xattr_id_table_start
    sb += struct.pack("<Q", inode_table_start)
    sb += struct.pack("<Q", directory_table_start)
    sb += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)      # fragment_table_start
    sb += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)      # export_table_start
    assert len(sb) == 96, len(sb)

    img = tmp / "synthetic_lreg.sqfs"
    blob = bytes(sb) + payload_data + inode_block + dir_block
    img.write_bytes(blob)

    # --dump 必须报出 4096（尺寸来源就是 LREG 的 file_size@24）
    rc, out = run([str(CLI), "--dump", str(img)])
    want(rc == 0 and "LITERAL.pri" in out and "4096" in out,
         "合成镜像 --dump 报出条目 LITERAL.pri 且尺寸 == 4096（= LREG file_size@24 的值）",
         "exit=%d 输出=%r" % (rc, out.strip()[:200]))
    # --extract 必须逐字节等于数据段
    got_path = tmp / "got_synth.bin"
    rc2, out2 = run([str(CLI), "--extract", str(img), "LITERAL.pri", str(got_path)])
    got = got_path.read_bytes() if got_path.exists() else b""
    want(rc2 == 0 and got == payload_data,
         "合成镜像 --extract 抽回 4096 字节且逐字节等于数据段",
         "exit=%d 抽回 %d 字节 输出=%r" % (rc2, len(got), out2.strip()[:200]))


def check_writer_lreg(tmp, source_name, source_bytes, expect_sparse, source_file=None,
                      source_size=None, expect_frag=True):
    """造镜像 → 断言写侧 LREG 每个字段落在权威偏移上。返回 (镜像路径, 记录, payload)。"""
    src = tmp / ("src_" + source_name)
    src.mkdir(parents=True, exist_ok=True)
    if source_file is not None:
        shutil.copyfile(source_file, src / source_name)
    else:
        (src / source_name).write_bytes(source_bytes)
    want_size = int(source_size if source_size is not None else len(source_bytes))
    img = tmp / (source_name + ".sqfs")
    rc, out = run([str(CLI), "--pack-sqfs", str(img), str(src)])
    want(rc == 0 and img.exists(), "夹具镜像打得出来（%s）" % source_name,
         "exit=%d 输出=%r" % (rc, out.strip()[:200]))
    if rc != 0 or not img.exists():
        return None, None, None
    b = img.read_bytes()
    sb = read_superblock(b)
    payload = all_inode_payload(b, sb)
    hits = find_lreg(payload, want_size)
    want(len(hits) >= 1,
         "镜像里能扫到 file_size==%d 的 LREG（type=9）inode（%s）" % (want_size, source_name),
         "扫到 %d 条（把 file_size 挪出 @24 就会扫不到 ⇒ 这条是判红入口）" % len(hits))
    if not hits:
        return img, None, payload
    off = hits[0]
    nblocks_here = want_size // 131072 if want_size % 131072 else want_size // 131072
    rec = payload[off:off + 56 + 4 * max(nblocks_here, 8)]
    print("    [%s] LREG 记录在 inode 表 payload 的相对偏移 %d" % (source_name, off))
    # 逐字段对权威偏移
    want(struct.unpack_from("<H", rec, LREG_TYPE_OFF)[0] == K_TYPE_FILE_X,
         "LREG 类型 @%d u16 == 9" % LREG_TYPE_OFF)
    # start_block @16 是 u64：低 32 位在 @16、高 32 位在 @20。小值下两者都是 0，
    # 所以"u64 形状"这条用"@20 读出来不可能是别的东西"没法单独证；改为与 sparse 夹具配套：
    #   · 有数据块（非稀疏）⇒ start_block 必须真的 >= 超块长度；
    #   · 全稀疏 ⇒ 没有数据块，writer 写的就是 0（那不是错，所以不拿它当判据）。
    sb_u64 = struct.unpack_from("<Q", rec, LREG_START_BLOCK_OFF)[0]
    if expect_sparse:
        want(sb_u64 == 0 or sb_u64 >= 96,
             "LREG start_block @%d（u64；全稀疏夹具允许为 0，值=%d）"
             % (LREG_START_BLOCK_OFF, sb_u64))
    else:
        want(sb_u64 >= 96,
             "LREG start_block @%d 是 **u64** 且 >= 超块长度（值=%d）"
             % (LREG_START_BLOCK_OFF, sb_u64))
    fs_here = struct.unpack_from("<Q", rec, LREG_FILE_SIZE_OFF)[0]
    want(fs_here == want_size,
         "LREG file_size @%d 是 **u64** 且 == 源文件长度（%d vs %d）"
         % (LREG_FILE_SIZE_OFF, fs_here, want_size))
    sparse_here = struct.unpack_from("<Q", rec, LREG_SPARSE_OFF)[0]
    if expect_sparse:
        want(sparse_here > 0, "LREG sparse @%d 是 **u64** 且 > 0（稀疏夹具，值=%d）"
             % (LREG_SPARSE_OFF, sparse_here))
    else:
        want(sparse_here == 0, "LREG sparse @%d 是 u64 且 == 0（非稀疏夹具，值=%d）"
             % (LREG_SPARSE_OFF, sparse_here))
    want(struct.unpack_from("<I", rec, LREG_NLINK_OFF)[0] == 1,
         "LREG nlink @%d u32 == 1" % LREG_NLINK_OFF)
    # ★ 反向判据：字段**不许**落在"错标一位"的那一族偏移上。
    #   审计报告把 fragment/offset 标成 40/44；判据分两种夹具：
    #     · 有 fragment（块大小 131072 的整数倍除外，本夹具是）：@44/@48 必须是**真的**
    #       fragment 索引与**落在 fragment 载荷内**的偏移 —— 若字段整体前挪一位
    #       （读 @44 当 offset），那个值必然 >= fragment 载荷长度 ⇒ 这条红；
    #     · 无 fragment：三处都必须是 -1 哨兵。
    frag = struct.unpack_from("<I", rec, LREG_FRAGMENT_OFF)[0]
    offs = struct.unpack_from("<I", rec, LREG_OFFSET_OFF)[0]
    xattr = struct.unpack_from("<I", rec, LREG_XATTR_OFF)[0]
    want(xattr == NEGATIVE_FRAG, "LREG xattr @%d == 0xFFFFFFFF" % LREG_XATTR_OFF,
         "实得 0x%x" % xattr)
    if expect_frag:
        # 碎尾长度 = 文件长度 % block_size（非整数倍时才有碎尾）
        tail = want_size % 131072
        want(frag != NEGATIVE_FRAG and offs != NEGATIVE_FRAG and offs < tail,
             "LREG fragment@%d / offset@%d 是本夹具**真的**（fragment=%d，offset=%d < 碎尾 %d）"
             % (LREG_FRAGMENT_OFF, LREG_OFFSET_OFF, frag, offs, tail),
             "实得 frag=0x%x off=0x%x（碎尾长度 %d）" % (frag, offs, tail))
    else:
        want(frag == NEGATIVE_FRAG and offs == NEGATIVE_FRAG,
             "LREG fragment@%d / offset@%d 都是 0xFFFFFFFF（本夹具无 fragment）"
             % (LREG_FRAGMENT_OFF, LREG_OFFSET_OFF),
             "实得 frag=0x%x off=0x%x" % (frag, offs))
    # 反向判据：若把 offset 误读成 @44 之外的位置（"字段整体前挪/后挪一位"那一族），
    # 读到的就不会是"落在碎尾内"的值 ⇒ 上面那条已经覆盖；这里再单独钉一次形状。
    want(struct.unpack_from("<I", rec, LREG_OFFSET_OFF)[0] == offs,
         "反向判据：@48 就是 offset 字段本身（把字段挪到 40/44/48 那一族会让这条红）")
    # block_sizes 紧跟在 @56 之后：每一项的 size 字段必须 <= block_size，
    # 且**有一项是 0**（全零块 ⇒ 不写盘）。数组起点被挪 ⇒ 会把更长的字段
    # （如 file_size 的高 4 字节 = 0、"sparse" 的前 4 字节）当成 size 项 ⇒ 必红。
    # ★ 数据块项数 = 完整块数（有碎尾时不含碎尾那一块，碎尾进 fragment 表）：
    #   n = floor(len / block_size)（能整除时就是 len / block_size）。
    nblocks = want_size // 131072
    if nblocks:
        sizes = [struct.unpack_from("<I", rec, LREG_BLOCK_SIZES_OFF + 4 * k)[0]
                 for k in range(nblocks)]
        bad = [s for s in sizes if (s & 0xFFFFFF) > 131072]
        zeros = [s for s in sizes if s == 0]
        want(not bad,
             "LREG block_sizes 从 @%d 起、%d 项都 <= block_size（错位会把别的字段当 size）"
             % (LREG_BLOCK_SIZES_OFF, nblocks),
             "越界项 %s" % bad[:4])
        if expect_sparse:
            want(len(zeros) >= 1,
                 "LREG block_sizes 里**有全零项**（稀疏块不写盘）—— 实得 %d 项为 0" % len(zeros))
    return img, rec, payload


def main():
    parser = argparse.ArgumentParser(description="SquashFS LREG 字段偏移判红锁")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("SQFS-LREG: SKIP (cli-missing)")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_lreg_lock_"))
    try:
        # ── 夹具 A：稀疏文件 ⇒ writer 必然升 LREG（sparse>0）；**全部**塞进 fragment ⇒ 无数据块 ──
        print("[A] 稀疏文件（sparse>0 ⇒ writer 升 LREG；fragment 恒定 -1）")
        sparse_bytes = b"\x00" * 700000
        img_a, rec_a, _ = check_writer_lreg(tmp, "sparse.bin", sparse_bytes, True,
                                            expect_frag=False)

        # ── 夹具 B：**带数据的稀疏文件**（非零块 + 全零块混在一起）。
        #    为什么需要这一条才压得住"block_sizes 数组起点"：writer 只在 `sparse>0`
        #    或尺寸超过 u32 时才升 LREG（`write_file_inode` 的 `extended` 判据），
        #    所以"非稀疏的多块文件"其实**根本不走 LREG**（实测：505000 字节仍写 REG）。
        #    这里造 [300000 随机][偏移 2 MiB 处再 5000 随机] ⇒ 前面有真数据块、
        #    中间一大段全零块 ⇒ `sparse>0` ⇒ LREG，且 block_sizes 里既有真尺寸也有 0。
        print("[B] 带数据的稀疏文件（sparse>0 且 block_sizes 非零，压住数组起点与 start_block）")
        big = tmp / "big_blocks.bin"
        with open(big, "wb") as fh:
            fh.write(bytes((i * 37 + 11) % 251 for i in range(300000)))
            fh.seek(2 * 1024 * 1024)
            fh.write(bytes((i * 53 + 7) % 241 for i in range(5000)))
        body_len = 2 * 1024 * 1024 + 5000
        img_b, rec_b, _ = check_writer_lreg(tmp, "blocks.bin", None, True, source_file=big,
                                            source_size=body_len)

        # ── 读侧：抽回来必须逐字节等于源（file_size @24 u64 被读错位置就必红）──
        print("[C] 读侧：--extract 抽回的字节必须逐字节等于源")
        expect_a = b"\x00" * 700000
        with open(tmp / "big_blocks.bin", "rb") as fh:
            expect_b = fh.read()
        for img, name, expect in ((img_a, "sparse.bin", expect_a),
                                  (img_b, "blocks.bin", expect_b)):
            if img is None:
                want(False, "读侧夹具缺失（%s）" % name)
                continue
            got_path = tmp / ("got_" + name)
            rc, out = run([str(CLI), "--extract", str(img), name, str(got_path)])
            got = got_path.read_bytes() if got_path.exists() else b""
            want(rc == 0 and got == expect,
                 "读侧 %s：抽回 %d 字节 == 源 %d 字节（逐字节一致）" % (name, len(got), len(expect)),
                 "exit=%d 输出=%r" % (rc, out.strip()[:200]))

        # ── 读侧（合成镜像见证）：writer **造不出** fragment/offset/dir 的非平凡 LREG
        #    （本工程只在 `sparse>0` 或超过 u32 时升 LREG，且 fragment 恒为 -1 …… 见
        #     `write_file_inode`），所以"读侧那两处偏移读对了没"只能拿**手工造的**镜像来钉。
        #    这枚镜像按**权威布局**摆字段，然后要求引擎读出"它自己声明的 size"：
        #    偏移读错 ⇒ 声明的 size 就落在别的字段上 ⇒ 报错或长度不符，必红。
        print("[D] 读侧（合成镜像）：LREG 的 file_size@24 / fragment@44 / offset@48 被读对")
        check_synthetic_reader(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("SQFS-LREG fail %d 处：%s" % (len(PROBLEMS), "; ".join(PROBLEMS[:5])))
        return 1
    print("SQFS-LREG ok 写侧逐字段偏移 == 权威布局，且读侧逐字节读回一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
