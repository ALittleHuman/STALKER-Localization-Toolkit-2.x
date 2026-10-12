# -*- coding: utf-8 -*-
"""P17-100 判据：SquashFS **超块版本/块对数**与**元数据块长度**必须有校验（审计 `01 F-17`）。

事实（修前，`src/engine/fs/squashfs.cpp`）
----------------------------------------
* `:220-221` 把 `sb.s_major = rd16(d + 28)` / `s_minor` **读出来就再没被比较过** ⇒
  一个 `s_major=99` 的坏镜像会一路被当成 SquashFS 4.x 解析，报出来的是**下游**的
  "某个表读不动"，而不是"这不是我认识的版本"（排障最贵的就是这种错位）；
* `block_log` 同样是**读了不校**：它和 `block_size` 是同一件事的两种写法
  （`block_size == 1 << block_log`），两者矛盾时下游所有"按 log 算"的路径都会算到别处；
* `:272-274`（未压缩元数据块那一支）**直接采信** `comp_size`（15 位掩码）——
  而未压缩块的**解压后**大小上限就是 `kMetaBlockSize`（8192）⇒ 声明得比它还大只能是坏镜像。

判据（**自带夹具**：用 `--pack-sqfs` 现打一个镜像，不依赖任何外部样本）
--------------------------------------------------------------------
① **基线**：现打的镜像必须照旧被接受（`--dump` rc=0 且列出条目）；
② `s_major` 改成 99 ⇒ 必须**拒绝**，理由里带「版本」；
③ `block_log` 改成与 `block_size` 矛盾 ⇒ 必须**拒绝**，理由里同时出现 `block_log` 与 `block_size`；
④ 把某个**未压缩**元数据块的 `comp_size` 改大 ⇒ 必须**拒绝**，理由里带「未压缩元数据块」。

输出：`SQFS-SB ok …` / `SQFS-SB fail …`；退出码 0=绿 / 1=有断言不成立（2=外部前提缺失）。

用法：`python tests/check_sqfs_superblock.py [xrkit_cli.exe]`
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS: list = []
CASES = 0

# 超块字段偏移（SquashFS 4.0，magic 之后的固定布局）
OFF_BLOCK_SIZE = 12
OFF_BLOCK_LOG = 22
OFF_S_MAJOR = 28
OFF_S_MINOR = 30
OFF_INODE_TABLE = 64
OFF_DIR_TABLE = 72
OFF_FRAG_TABLE = 80


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def run_dump(cli: Path, image: Path) -> tuple:
    p = subprocess.run([str(cli), "--dump", str(image)], capture_output=True, text=True, timeout=300)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def parse_sb(d: bytes) -> dict:
    be = d[:4] == b"sqsh"
    fmt16, fmt32, fmt64 = (">H", ">I", ">Q") if be else ("<H", "<I", "<Q")
    u16 = lambda o: struct.unpack_from(fmt16, d, o)[0]          # noqa: E731
    u32 = lambda o: struct.unpack_from(fmt32, d, o)[0]          # noqa: E731
    u64 = lambda o: struct.unpack_from(fmt64, d, o)[0]          # noqa: E731
    return {
        "be": be, "u16": u16, "u32": u32, "u64": u64,
        "block_size": u32(OFF_BLOCK_SIZE), "block_log": u16(OFF_BLOCK_LOG),
        "s_major": u16(OFF_S_MAJOR), "s_minor": u16(OFF_S_MINOR),
        "inode_table": u64(OFF_INODE_TABLE), "dir_table": u64(OFF_DIR_TABLE),
        "frag_table": u64(OFF_FRAG_TABLE),
    }


def patch_u16(d: bytearray, off: int, value: int, be: bool) -> None:
    struct.pack_into(">H" if be else "<H", d, off, value)


def make_fixture(cli: Path, work: Path) -> Path:
    """用 CLI 自己打一个最小镜像（自带夹具：不碰任何外部样本）。"""
    src = work / "src"
    (src / "a" / "b").mkdir(parents=True, exist_ok=True)
    (src / "hello.txt").write_text("P17-100 sqfs fixture", encoding="utf-8")
    (src / "a" / "b" / "deep.txt").write_text("deep", encoding="utf-8")
    target = work / "fixture"
    p = subprocess.run([str(cli), "--pack-sqfs", str(target), str(src)],
                       capture_output=True, text=True, timeout=600)
    out = (p.stdout or "") + (p.stderr or "")
    if "PACK ok" not in out:
        return Path("")
    cand = Path(str(target) + ".sqfs")
    return cand if cand.is_file() else Path("")


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-100 超块/元数据块校验（审计 `01 F-17`）==")
    if not cli.is_file():
        print("!! 缺构建产物 xrkit_cli.exe：%s（外部前提）" % cli)
        print("SQFS-SB skip 缺构建产物")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="p17100_sqfs_sb_"))
    try:
        base = make_fixture(cli, tmp)
        if not base:
            print("!! 现打夹具失败（--pack-sqfs 没成功）")
            print("SQFS-SB fail 夹具打不出来 —— 判据不可信")
            return 1
        original = base.read_bytes()
        sb = parse_sb(original)
        print("   夹具：%s（%d 字节；magic_be=%s block_size=%d block_log=%d s_major=%d.%d）"
              % (base.name, len(original), sb["be"], sb["block_size"], sb["block_log"],
                 sb["s_major"], sb["s_minor"]))

        # ① 基线：现打的镜像必须照旧被接受
        rc, text = run_dump(cli, base)
        want(rc == 0 and "hello.txt" in text, "① 基线：现打的镜像照旧被接受（rc=0 且列出条目）",
             "rc=%d" % rc)

        # ② s_major = 99 ⇒ 必须拒绝，理由里要有「版本」
        d = bytearray(original)
        patch_u16(d, OFF_S_MAJOR, 99, sb["be"])
        bad = tmp / "bad_major.sqfs"
        bad.write_bytes(d)
        rc, text = run_dump(cli, bad)
        want(rc != 0 and "版本" in text,
             "② `s_major=99` ⇒ 拒绝且理由点名「版本」（修前：读了不校 ⇒ 会被当 4.x 解析下去）",
             "rc=%d 理由=%s" % (rc, text.strip().splitlines()[-1][:90] if text.strip() else "（空）"))

        # ③ block_log 与 block_size 矛盾 ⇒ 必须拒绝，理由里两者都要出现
        d = bytearray(original)
        patch_u16(d, OFF_BLOCK_LOG, 0x1234, sb["be"])
        bad = tmp / "bad_log.sqfs"
        bad.write_bytes(d)
        rc, text = run_dump(cli, bad)
        want(rc != 0 and "block_log" in text and "block_size" in text,
             "③ `block_log` 与 `block_size` 矛盾 ⇒ 拒绝且理由同时点名两者",
             "rc=%d" % rc)

        # ④ 未压缩元数据块的 comp_size 改大 ⇒ 必须拒绝，理由里要有「未压缩元数据块」
        #    ★ 找一个**未压缩**（bit15==0）且**不是这块表最后一块**的元数据块：
        #      改它的 size 不能把"下一块"的起点算出界（否则会先撞上"读不到"那条错，判据就测偏了）。
        d = bytearray(original)
        u16 = sb["u16"]
        picked = -1
        for start, end in ((sb["inode_table"], sb["dir_table"]),
                           (sb["dir_table"], sb["frag_table"])):
            off = start
            while off + 2 < end:
                h = u16(off)
                size = h & 0x7FFF
                stored = (h >> 15) & 1
                nxt = off + 2 + size
                if stored == 1 and nxt < end and nxt + 2 <= len(original):
                    # 有下一块 ⇒ 改大 size 只会让"下一块"的起点前移/后移，仍在文件内
                    picked = off
                    break
                off = nxt
            if picked >= 0:
                break
        if picked < 0:
            # 夹具里没有"未压缩且非末块"的元数据块 ⇒ 自己造一个：把**压缩**的首块改成未压缩
            # 并把 size 写大（bit15 置位 + size=8193），起点仍在文件内。
            first = sb["dir_table"]
            h = u16(first)
            patch_u16(d, first, 0x8000 | 0x2001, sb["be"])
            picked = first
            print("   （夹具里没有「未压缩且非末块」的元数据块；把 %d 处改成未压缩 + size=8193）" % first)
        else:
            h = u16(picked)
            patch_u16(d, picked, 0x8000 | 0x2001, sb["be"])
            print("   （把 %d 处的元数据块头 0x%04X 改成未压缩 + size=8193）" % (picked, h))
        bad = tmp / "bad_meta.sqfs"
        bad.write_bytes(d)
        rc, text = run_dump(cli, bad)
        want(rc != 0 and "未压缩元数据块" in text,
             "④ 未压缩元数据块声明长度超上限 ⇒ 拒绝且理由点名「未压缩元数据块」",
             "rc=%d" % rc)

        # ⑤ 反向锁：坏镜像**不许**产出条目表（拒绝要干净）
        rc, text = run_dump(cli, bad)
        want(not text.count("\n") > 3 or "hello.txt" not in text,
             "⑤ 反向锁：坏镜像不许吐出正常条目表")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("SQFS-SB fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SQFS-SB ok %d 项全绿（基线 + 版本 + 块对数 + 元数据块长度 + 反向锁；审计 01 F-17）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
