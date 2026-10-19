# -*- coding: utf-8 -*-
r"""**块级字节探针**（T-07 写侧，只比字节、不碰产品代码）：gzip 到底能不能逐字节对齐？

背景（用户 2026-10-19 的分析）
-----------------------------
参照实现 squashfs-tools-ng 1.3.2 的 gzip 参数已钉死：
`deflateInit2(level=9, Z_DEFLATED, windowBits=15, memLevel=8, Z_DEFAULT_STRATEGY)` 然后 `deflate(Z_FINISH)`。
而 zlib 版本两边不同：**参照 `zlib1.dll` = 1.2.12**，**我们的 zlib = 1.3.1**。
⇒ "算法没变"不能推定"逐字节相同"，必须用字节回答。

本探针做的最小实验（**不调用产品代码、不写仓库**）：
  1. 取三类固定块：8 KiB 元数据样块、128 KiB 数据样块、不可压缩块（随机字节）；
  2. 用 `ctypes` 调**参照 zlib1.dll** 的 `compress2(dest,&len,src,len,level=9)`；
     （`compress2` 在 zlib 里就是 `deflateInit2(level, Z_DEFLATED, MAX_WBITS=15,
       DEF_MEM_LEVEL=8, Z_DEFAULT_STRATEGY)` 的等价封装 —— 与 squashfs 的 gzip 参数一致）
  3. 用**我们的 zlib**（同为 `compress2`，参数相同）压同一批块；
  4. **逐字节比**，打印首个差异偏移。

⚠ 我们的 zlib 是**静态**链进引擎的（没有 DLL），所以本探针用 `--ours-dll` 显式指定一个
  与产品同版本的 `zlib1.dll`（例如从我们自己的构建产物/第三方预编译件里取）。
  没有可用 DLL 时本探针**如实 SKIP**，不假装有结论。

用法
----
    python tests/probe_zlib_deflate_bytes.py \
        --ref-dll "E:\...\squashfs-tools-ng-1.3.2-mingw64\bin\zlib1.dll" \
        --ours-dll "C:\path\to\zlib1-1.3.1.dll"

退出码：0=**逐字节一致**（⇒ 选项 1 可行）；1=有差异（⇒ 要么 vendor 1.2.12，要么改验收口径）；
        2=缺 DLL（外部前提，未结论）。
"""

from __future__ import annotations

import argparse
import ctypes
import os
import random
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

DEFAULT_REF = (r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit\deps"
               r"\squashfs-tools-ng-1.3.2-mingw64\bin\zlib1.dll")
U_BOUND = ctypes.c_ulong


def load(path: str):
    lib = ctypes.CDLL(path)
    lib.zlibVersion.restype = ctypes.c_char_p
    lib.compressBound.restype = U_BOUND
    lib.compressBound.argtypes = [U_BOUND]
    lib.compress2.restype = ctypes.c_int
    lib.compress2.argtypes = [ctypes.c_char_p, ctypes.POINTER(U_BOUND),
                              ctypes.c_char_p, U_BOUND, ctypes.c_int]
    return lib


def deflate(lib, data: bytes, level: int = 9) -> bytes:
    """与 squashfs gzip 参数等价的 compress2(level=9)（deflateInit2 的 15/8/默认策略）。"""
    bound = int(lib.compressBound(len(data)))
    buf = ctypes.create_string_buffer(bound)
    n = U_BOUND(bound)
    rc = lib.compress2(buf, ctypes.byref(n), data, len(data), level)
    if rc != 0:
        raise RuntimeError("compress2 rc=%d" % rc)
    return buf.raw[:n.value]


def fixtures() -> dict:
    rnd = random.Random(20261019)
    # ① 8 KiB「元数据」样：像 inode/目录表那样的高重复小结构
    meta = bytearray()
    while len(meta) < 8192:
        meta += b"\x03\x00\x00\x00inode" + bytes(range(16)) + b"\x01\x02\x00\x00"
    meta = bytes(meta[:8192])
    # ② 128 KiB「数据」样：混合文本/二进制，靠重复度中等
    data = bytearray()
    while len(data) < 128 * 1024:
        data += (b"the quick brown fox jumps over the lazy dog\n" * 3 + bytes(range(64)))
    data = bytes(data[:128 * 1024])
    # ③ 不可压缩：随机字节（squashfs 会原样存）
    incompressible = bytes(rnd.getrandbits(8) for _ in range(64 * 1024))
    return {"meta_8k": meta, "data_128k": data, "incompressible_64k": incompressible}


def first_diff(a: bytes, b: bytes) -> str:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return "首个差异 @%d：ref=0x%02x ours=0x%02x" % (i, a[i], b[i])
    if len(a) != len(b):
        return "长度不同：ref=%d ours=%d（前缀相同）" % (len(a), len(b))
    return "完全相同"


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--ref-dll", default=os.environ.get("XRKIT_ZLIB_REF", DEFAULT_REF))
    ap.add_argument("--ours-dll", default=os.environ.get("XRKIT_ZLIB_OURS", ""))
    args = ap.parse_args()

    print("== T-07 块级探针：gzip（zlib compress2 level=9）逐字节比 ==")
    if not os.path.isfile(args.ref_dll):
        print("!! 找不到参照 zlib：%s" % args.ref_dll)
        print("PROBE-ZLIB-BYTES: SKIP (ref-missing)")
        return 2
    if not args.ours_dll or not os.path.isfile(args.ours_dll):
        print("!! 没给 `--ours-dll`（或文件不存在）：我们的 zlib 是**静态**链进引擎的，"
              "没有 DLL 就没法在这里做字节对比。")
        print("   取法其一：① 从我们自己的构建产物里找 `zlib1.dll`；"
              "② 用 `third_party/zlib` 编一份 DLL；③ 指一个与产品同版本的 `zlib1.dll`。")
        print("PROBE-ZLIB-BYTES: SKIP (ours-missing)")
        return 2

    ref = load(args.ref_dll)
    ours = load(args.ours_dll)
    print("  参照 zlib = %s（%s）" % (ref.zlibVersion().decode(), os.path.basename(args.ref_dll)))
    print("  我方 zlib = %s（%s）" % (ours.zlibVersion().decode(), os.path.basename(args.ours_dll)))

    bad = 0
    for name, raw in fixtures().items():
        a = deflate(ref, raw)
        b = deflate(ours, raw)
        if a == b:
            print("  [%-16s] ✓ 逐字节一致（原 %d B → 压 %d B）" % (name, len(raw), len(a)))
        else:
            bad += 1
            print("  [%-16s] ✗ 不一致（ref %d B / ours %d B）：%s"
                  % (name, len(a), len(b), first_diff(a, b)))

    print("-" * 66)
    if bad == 0:
        print("PROBE-ZLIB-BYTES: 一致（%d/%d）⇒ gzip 写侧可与参照逐字节对齐，"
              "走「黄金集逐字节 ＋ 往返」验收。" % (len(fixtures()) - bad, len(fixtures())))
        return 0
    print("PROBE-ZLIB-BYTES: 有差异（%d/%d 不一致）⇒ 按用户口径二选一："
          "① 为写侧 vendor zlib 1.2.12；② 把新压缩器的验收改成「可读 ＋ 往返 ＋ 参数有记录」。"
          % (bad, len(fixtures())))
    return 1


if __name__ == "__main__":
    sys.exit(main())
