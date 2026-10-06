#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：文件系统层的两条**不可信输入**边界（本批 F-1 / H-7）。

为什么要有这个脚本 —— 两条都属于"校验确实在、但边界条件下不成立"那一族
（`..` 是精确字符串匹配 / 声明尺寸没上限），只在特定条目名与特定镜像头下悄悄错：

① F-1：`safe_out_path` 的 `..` 拦截曾是**精确匹配** ⇒ `".. "`（两点+尾随空格）、
   `".. ."` 这类"Windows 落盘时会剥掉尾随空格/点"的段能**通过判定**。
   夹具＝**自造的最小 SquashFS**（不依赖任何真机语料、不碰游戏目录），条目里同时放：
     · `.. `   + `.. /x`    ← 尾随空格绕过形态
     · `.. .`  + `.. ./y`   ← 尾随空格+点绕过形态
     · `a. `   + `a. /z.txt`← **不是** `..`，但落点必须按 Windows 的有效名归一（`a/z.txt`）
     · `ok`    + `ok/good.txt` ← 哨兵：正常条目必须照写
   断言（四条，缺一条都不算过）：
     · 那 4 条 `..`/`.. .` 形态**一律拒**，且理由是 `分段含 ..`（判定与理由一起断言；
       靠"反正写不出去"蒙过不算 —— 判定错就是错）；
     · 输出目录**之外一个文件都不许有**（域外零文件）；
     · 正常条目照写且逐字节相等（`ok/good.txt`）；
     · 尾随点目录里的文件按**有效名**落盘（`a. /z.txt` → `a/z.txt`，逐字节相等）。
   ★ 实测口径（与报告里【没做到/风险】一节逐字对应）：**"真的写到输出目录之外"这一步在本机
     复现不出来** —— Win32 的尾随空格/点剥离**只作用于最后一个组件**，中间段 `".. "` 会被
     原样当目录名 ⇒ `out/.. /x` 在 `CreateFileW` 层报"找不到路径"（实测：cmd / .NET /
     PowerShell 三条路一致，`mkdir` 中间段也一样）。所以本锁钉的是**判定与理由**（外加域外零
     文件这条安全网），而不是"已经写出去了"。修前这两条判定是**假绿**，见同目录的判红记录。

② H-7：SquashFS 读取器缺"解压炸弹闸"（`read_file_data` 的 `out.reserve(inode.size)`、
   `meta_read` 的 `out.reserve(len)`），而 `db.cpp` 有 `kMaxDecompressBytes = 512MB`。
   夹具＝自造镜像，一个 LREG inode 声明 `file_size = 1 TiB`（`block_size` 调到 1 GiB，
   让 `block_sizes` 那一步先过关 ⇒ **正好命中 `reserve`**；修前是 `bad_alloc` 冒到顶/进程非正常
   退出，修后是"明确报错不分配"）。
   断言：**报错不 OOM**（退出码非 0 + 理由里带"上限"），且**正常镜像逐字节不变**（现打包一枚
   正常镜像 → 抽出来的字节与源文件相等）。

输出：`✓` / `✗` 行 + 收尾行 `FS-HARDEN ok …` / `FS-HARDEN fail …`；
退出码 0=全绿、1=有断言不成立、2=构建产物缺失（外部前提，按档位判定）。
"""
import argparse
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


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


# ── 自造最小 SquashFS（小端 v4；**只为判红锁造夹具**，不追求格式完整性）──────────
#
# 字段偏移/类型全部照本工程 reader（`src/engine/fs/squashfs.cpp`）：超块 96 字节、
# 元数据块 = u16 头（bit15 = 未压缩 | 低 15 位 = 长度）+ 数据、DIR inode 取 type@0 /
# dir_block@16 / size@24(u16) / dir_offset@26(u16)、REG inode 取 type@0 / start_block@16 /
# fragment@20 / block_offset@24 / file_size@28(u32)（block_sizes 紧跟在 32 字节记录之后）、
# LREG（FILE_X）file_size 是 @24 的 u64（block_sizes 从 56 起）、目录项 =
# (offset u16, inode u16, type u16, name_size-1 u16, name…) + 哨兵（type=0）。

MAGIC = 0x73717368  # 'hsqs'：与 reader 的 kMagicLE 同值（按小端解析）
K_TYPE_DIR = 1
K_TYPE_FILE = 2
K_TYPE_FILE_X = 9
K_UNCOMPRESSED_BIT = 1 << 24
K_INVALID_FRAG = 0xFFFFFFFF


def _dir_inode(dir_offset, size):
    b = bytearray(32)
    struct.pack_into("<H", b, 0, K_TYPE_DIR)
    struct.pack_into("<I", b, 16, 0)          # dir_block：目录表内第 0 个元数据块
    struct.pack_into("<H", b, 24, size)
    struct.pack_into("<H", b, 26, dir_offset)
    return bytes(b)


def _file_inode(start_block, size):
    b = bytearray(32)
    struct.pack_into("<H", b, 0, K_TYPE_FILE)
    struct.pack_into("<I", b, 16, start_block)
    struct.pack_into("<I", b, 20, K_INVALID_FRAG)
    struct.pack_into("<I", b, 24, 0)
    struct.pack_into("<I", b, 28, size)
    return bytes(b)


def _file_x_inode(start_block, size):
    b = bytearray(56)
    struct.pack_into("<H", b, 0, K_TYPE_FILE_X)
    struct.pack_into("<Q", b, 16, start_block)
    struct.pack_into("<Q", b, 24, size)
    struct.pack_into("<I", b, 44, K_INVALID_FRAG)
    struct.pack_into("<I", b, 48, 0)
    return bytes(b)


def _dir_group(entries):
    """目录表里的一组：12 字节组头（count / start / inode_number）+ 条目 + 哨兵。

    reader 的循环是 `k <= count` ⇒ 组里**实际写 count+1 条**（含哨兵）。
    """
    b = struct.pack("<III", len(entries), 0, 1)
    for name, inode_off, inode_type in entries:
        raw = name.encode("utf-8")
        b += struct.pack("<HHHH", inode_off, 0, inode_type, len(raw) - 1) + raw
    b += struct.pack("<HHHH", 0, 0, 0, 0) + b"\x00"  # 哨兵（type=0）
    return b


def _meta_block(payload, plain_size=8192):
    """元数据块：头 = 0x8000(未压缩) | 长度；不足补零到 plain_size（reader 按头里的长度取数据）。"""
    assert len(payload) <= plain_size
    body = payload + b"\x00" * (plain_size - len(payload))
    return struct.pack("<H", 0x8000 | len(body)) + body


def _superblock(total, inode_table_start, directory_table_start, root_inode, block_size, inodes):
    b = bytearray()
    b += struct.pack("<I", MAGIC)
    b += struct.pack("<I", inodes)
    b += struct.pack("<I", 0)                 # mkfs_time
    b += struct.pack("<I", block_size)
    b += struct.pack("<I", 0)                 # fragments
    b += struct.pack("<H", 1)                 # compression（元数据块全是"未压缩"，用不到）
    b += struct.pack("<H", 17)                # block_log
    b += struct.pack("<H", 0)                 # flags
    b += struct.pack("<H", 0)                 # no_ids
    b += struct.pack("<H", 4)                 # s_major
    b += struct.pack("<H", 0)                 # s_minor
    b += struct.pack("<Q", root_inode)
    b += struct.pack("<Q", total)             # bytes_used
    b += struct.pack("<Q", 0)                     # id_table_start
    b += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)    # xattr_id_table_start
    b += struct.pack("<Q", inode_table_start)
    b += struct.pack("<Q", directory_table_start)
    b += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)    # fragment_table_start
    b += struct.pack("<Q", 0xFFFFFFFFFFFFFFFF)    # export_table_start
    assert len(b) == 96, len(b)
    return bytes(b)


def _assemble(inode_payload, dir_payload, data_blocks, block_size, inodes):
    """按"超块 → inode 元数据块 → 目录元数据块 → 数据块（按 data_blocks 顺序）"摆放。"""
    inode_block = _meta_block(inode_payload)
    dir_block = _meta_block(dir_payload)
    inode_table_start = 96
    directory_table_start = inode_table_start + len(inode_block)
    data_start = directory_table_start + len(dir_block)
    out = bytearray()
    out += _superblock(0, inode_table_start, directory_table_start, 0, block_size, inodes)
    out += inode_block
    out += dir_block
    starts = {}
    for key, payload in data_blocks:
        starts[key] = len(out)
        out += payload
    out[40:48] = struct.pack("<Q", len(out))  # bytes_used（补零之后的真实长度）
    return bytes(out), starts, data_start


def build_dotdot_image(path: Path) -> None:
    """F-1 夹具：`.. `/`.. .` 绕过形态 + 一条尾随点的正常名 + 一条正常哨兵。"""
    # inode 记录（32 字节一段；REG 的 block_sizes 紧跟在其后 4 字节）
    off_root, off_dot1, off_dot2, off_dotname, off_ok = 0, 32, 64, 96, 128
    off_x, off_y, off_z, off_good = 160, 196, 232, 268
    payload = bytearray(b"\x00" * 304)
    payload[off_root:off_root + 32] = _dir_inode(0, 0)     # 大小稍后回填
    payload[off_dot1:off_dot1 + 32] = _dir_inode(0, 0)
    payload[off_dot2:off_dot2 + 32] = _dir_inode(0, 0)
    payload[off_dotname:off_dotname + 32] = _dir_inode(0, 0)
    payload[off_ok:off_ok + 32] = _dir_inode(0, 0)
    for off, size in ((off_x, 10), (off_y, 10), (off_z, 5), (off_good, 8)):
        payload[off:off + 32] = _file_inode(0, size)  # start_block 稍后回填
        struct.pack_into("<I", payload, off + 32, K_UNCOMPRESSED_BIT | size)

    # 目录表：5 组（根 + 4 个子目录各一组）
    g_ok = _dir_group([("good.txt", off_good, K_TYPE_FILE)])
    g_z = _dir_group([("z.txt", off_z, K_TYPE_FILE)])
    g_y = _dir_group([("y", off_y, K_TYPE_FILE)])
    g_x = _dir_group([("x", off_x, K_TYPE_FILE)])
    g_root = _dir_group([(".. ", off_dot1, K_TYPE_DIR),
                         (".. .", off_dot2, K_TYPE_DIR),
                         ("a. ", off_dotname, K_TYPE_DIR),
                         ("ok", off_ok, K_TYPE_DIR)])
    # 摆放顺序与上面的大小回填一一对应（组头 12 字节 + 条目）
    d_root = 0
    d_dot1 = d_root + len(g_root)
    d_dot2 = d_dot1 + len(g_x)
    d_dotname = d_dot2 + len(g_y)
    d_ok = d_dotname + len(g_z)
    dir_payload = g_root + g_x + g_y + g_z + g_ok
    for off, size, doff in ((off_root, len(g_root), d_root),
                            (off_dot1, len(g_x), d_dot1),
                            (off_dot2, len(g_y), d_dot2),
                            (off_dotname, len(g_z), d_dotname),
                            (off_ok, len(g_ok), d_ok)):
        payload[off:off + 32] = _dir_inode(doff, size)

    data = [(off_x, b"ESCAPED-X\n"), (off_y, b"ESCAPED-Y\n"),
            (off_z, b"Z-OK\n"), (off_good, b"GOOD-OK\n")]
    image, starts, _ = _assemble(bytes(payload), dir_payload, data, 131072, 9)
    # 数据块落点只有拼完才知道 ⇒ 回填 start_block
    buf = bytearray(image)
    inode_table_start = 96
    body = 2  # 元数据块头
    for off, payload_bytes in data:
        struct.pack_into("<I", buf, inode_table_start + body + off + 16, starts[off])
    path.write_bytes(bytes(buf))


def build_bomb_image(path: Path) -> None:
    """H-7 夹具：一个 LREG inode 声明 1 TiB（block_size = 1 GiB ⇒ block_sizes 那步先过关）。"""
    off_root, off_bomb = 0, 32
    payload = bytearray(b"\x00" * 8192)   # 8192 字节元数据体：好让 4096 字节 block_sizes 读得到
    payload[off_root:off_root + 32] = _dir_inode(0, 0)
    payload[off_bomb:off_bomb + 56] = _file_x_inode(0, 1 << 40)   # 1 TiB
    struct.pack_into("<I", payload, off_bomb + 56, K_UNCOMPRESSED_BIT | 8)
    g_root = _dir_group([("bomb.bin", off_bomb, K_TYPE_FILE_X)])
    payload[off_root:off_root + 32] = _dir_inode(0, len(g_root))
    image, _starts, _ = _assemble(bytes(payload), g_root, [], 1 << 30, 2)
    path.write_bytes(image)


# ── 判据 ─────────────────────────────────────────────────────────────────────

def run_cli(argv):
    proc = subprocess.run([str(CLI)] + [str(a) for a in argv],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def fail_reason(text, entry_path):
    """从 `UNPACK fail path=<p> reason=<r>` 行里取理由（取不到 ⇒ None）。"""
    prefix = "UNPACK fail path=" + entry_path + " reason="
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):]
    return None


def check_dotdot(tmp: Path) -> None:
    img = tmp / "dotdot.sqfs"
    build_dotdot_image(img)

    # 先只列条目：夹具本身必须能被 reader 读出来（不然下面的断言全是假的）
    rc_list, out_list = run_cli(["--dump", str(img)])
    # TSV：<d|f> TAB <路径> TAB <大小>
    entries = []
    for line in out_list.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3:
            entries.append(parts[1])
    entries = sorted(entries)
    want(rc_list == 0 and any(e.endswith(".. /x") for e in entries),
         "夹具可读：自造镜像能列出 `.. /x` 这类条目",
         "rc=%d\n%s" % (rc_list, out_list.strip()[:400]))

    out = tmp / "out_f1"
    rc, text = run_cli(["--unpack-tree", str(img), str(out)])
    outside = [tmp / n for n in ("x", "y")]
    escaped = [p for p in outside if p.exists()]

    # ① 4 条 `..`/`.. .` 形态：拒 + 理由是 `分段含 ..`
    for rel in (".. ", ".. /x", ".. .", ".. ./y"):
        reason = fail_reason(text, rel)
        want(reason is not None and "分段含 .." in reason,
             "F-1 必须拒且写理由是『分段含 ..』：%r" % rel,
             "实得 reason=%r\n%s" % (reason, text.strip()[:600]))
    # ② 域外零文件（安全网：修前也不许有 —— 判红靠上面那条判据，不靠这条）
    want(not escaped, "F-1 输出目录之外一个文件都不许有（%s）" % ", ".join(p.name for p in outside),
         "域外出现了：%s" % ", ".join(str(p) for p in escaped))
    # ③ 正常条目照写（逐字节）
    good = out / "ok" / "good.txt"
    want(good.is_file() and good.read_bytes() == b"GOOD-OK\n",
         "F-1 正常条目照写且逐字节相等（ok/good.txt）",
         "存在=%s 内容=%r" % (good.is_file(), good.read_bytes() if good.is_file() else b""))
    # ④ 尾随点的**正常**目录名按 Windows 有效名落盘（`a. ` → `a`）
    z = out / "a" / "z.txt"
    want(z.is_file() and z.read_bytes() == b"Z-OK\n",
         "F-1 尾随点目录名按有效名归一落盘（a. /z.txt → a/z.txt，逐字节相等）",
         "存在=%s 内容=%r\n%s" % (z.is_file(), z.read_bytes() if z.is_file() else b"",
                                 text.strip()[:600]))
    # 计数：只有 4 条被拒，2 文件 2 目录
    okline = ""
    for line in text.splitlines():
        if line.startswith("UNPACK ok "):
            okline = line
    stats = dict()
    for part in okline[len("UNPACK ok "):].split():
        k, _, v = part.partition("=")
        if v.isdigit():
            stats[k] = int(v)
    want(stats.get("failed") == 4 and rc != 0,
         "F-1 失败台账恰好 4 条 `..` 形态且退出码非 0",
         "rc=%d %s\n%s" % (rc, okline, text.strip()[:600]))
    want(stats.get("files") == 2 and stats.get("dirs") == 2,
         "F-1 产物数=2 文件 / 2 目录（`a. ` 与 `ok`，不许把被拒的算成建了目录）",
         "rc=%d %s" % (rc, okline))


def check_bomb(tmp: Path) -> None:
    img = tmp / "bomb.sqfs"
    build_bomb_image(img)
    out_file = tmp / "bomb_out.bin"
    rc, text = run_cli(["--extract", str(img), "bomb.bin", str(out_file)])
    want(rc != 0, "H-7 超限声明必须失败（退出码非 0）",
         "rc=%d\n%s" % (rc, text.strip()[:400]))
    want("上限" in text and "reason=" in text,
         "H-7 失败理由必须明确写出『上限』（零静默，不许只给 bad_alloc/崩溃）",
         "rc=%d\n%s" % (rc, text.strip()[:400]))
    want(not out_file.exists(), "H-7 失败时不许留下输出文件",
         "落盘了：%s" % out_file)


def check_normal_unchanged(tmp: Path) -> None:
    """正常镜像逐字节不变：现打一枚 → 抽回来与源文件相等（闸只拦超限，不动正常路）。"""
    src_dir = tmp / "pack_src"
    src_dir.mkdir(parents=True, exist_ok=True)
    payload = bytes(range(256)) * 40  # 10240 B
    (src_dir / "a.bin").write_bytes(payload)
    img = tmp / "normal.sqfs"
    rc_pack, out_pack = run_cli(["--pack-sqfs", str(img), str(src_dir)])
    rc_get, out_get = run_cli(["--extract", str(img), "a.bin", str(tmp / "normal_out.bin")])
    got = tmp / "normal_out.bin"
    want(rc_pack == 0 and rc_get == 0 and got.is_file() and got.read_bytes() == payload,
         "H-7 正常镜像不受影响：封包 → 抽取，字节与源文件逐字节相等",
         "rc_pack=%d rc_get=%d\n%s\n%s" % (rc_pack, rc_get, out_pack.strip()[:200],
                                           out_get.strip()[:200]))


def main() -> int:
    parser = argparse.ArgumentParser(description="F-1 路径逃逸 / H-7 解压炸弹闸的判红锁")
    parser.add_argument("--keep", action="store_true", help="保留临时目录（排查用）")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 缺构建产物 xrkit_cli.exe：%s（外部前提）" % CLI)
        print("FS-HARDEN skip 缺构建产物")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_fs_harden_"))
    try:
        print("[F-1] 路径逃逸：`.. ` / `.. .` 绕过形态（自造最小 SquashFS 夹具）")
        check_dotdot(tmp)
        print("[H-7] SquashFS 解压炸弹闸：1 TiB 声明 + 正常镜像不变")
        check_bomb(tmp)
        check_normal_unchanged(tmp)
    finally:
        if args.keep:
            print("（--keep：临时目录保留在 %s）" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("FS-HARDEN fail %d 条断言不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("FS-HARDEN ok F-1（判定+理由+域外零文件+正常条目）与 H-7（超限报错+正常不变）全绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
