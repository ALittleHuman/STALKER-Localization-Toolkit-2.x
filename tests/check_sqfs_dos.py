#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：SquashFS **读侧**三条「打开畸形/恶意包即挂」的 DoS（审计 01 F-01 / F-02 / F-03，均 High）。

为什么要有这个脚本 —— 三条都不是"功能没写"，而是"**闸确实在别处**、这三条边界上不成立"：
只有特定镜像头/特定目录项才能触发，真机语料一个样本都没有，所以只能现造最小夹具，
而且必须**红/绿两向可判**（修复前红、修复后绿），否则就是自我安慰。

① **F-01 目录项成环**（`src/engine/fs/squashfs.cpp` 的 `Image::list()`）：
   修前用"栈 + 前缀"展开目录树，**无条件压栈**、没有 visited 集 / 深度上限 / 条目上限 ⇒
   目录项指回自己（或祖先）就无限展开 `a/a/a/…`：路径串总量按层数平方涨（实测 6 s 就吃到
   1.2 GiB、23 s 到 2 GiB 硬闸），既卡死又烧内存。修后＝"目录项成环/回指"明确报错。
   夹具 A＝自造镜像，根目录里放一条 `("a", 0, K_TYPE_DIR)` —— `child_ref=(sblk<<16)|e_off=0`
   正好指回根 inode 自己；另放一条正常文件条目做对照。

② **F-02 xz 字典炸弹**（`src/engine/fs/decompress.cpp` 的 xz 分支）：
   修前把 `memlimit` 置成 `UINT64_MAX` ⇒ 解码器自身那道闸被关掉，`.xz` 块头里声明的
   `dict_size` 最大可到约 1.5 GiB 且 liblzma 会**真去分配**它；输出缓冲由 `out_capacity`
   约束，可**管不住解码器内存**。夹具 C＝把元数据块的 xz 流里 LZMA2 的 dict_size 从 8 MiB
   改到 1.5 GiB（只动这一个字节 + 重算块头 CRC32，数据仍用小字典压的 ⇒ 修前**解压成功**）。
   修后＝"xz 解码内存超过上限"明确报错。

③ **F-03 超块 block_size 未校验**（`parse_superblock` + fragment 解压那条路）：
   修前不校验超块 `block_size`，而 fragment 解压把超块里的 `bs` 直接当**输出容量**传给
   `decompress_block`，后者第一句是 `out.assign(out_capacity, 0)` ⇒ `block_size=0xFFFFFFFF`
   时一次性 `assign` 约 4 GiB（实测峰值 4105 MiB、耗时 0.99 s，且**抽出来的文件是"成功"的**）。
   夹具 B＝超块 `block_size=0xFFFFFFFF` + 一个**带 fragment 的 REG inode**（fragment=0、
   block_offset=0、size=16）+ 一张 8 字节 fragment 查找表 + 一个装着 16 字节 fragment 条目的
   元数据块（**压缩态**且长度 ≥4 字节，否则 `src_len==0` 会在巨量分配之前提前返回）。
   修后＝"超块 block_size 非法…上限="明确报错。

判据（每条都断言"判定 + 理由"，不许靠"反正读不出来"蒙过）：
  A：`--dump` 必须 ≤20 s 返回、rc≠0、理由含`上限`且属"成环/回指"那一族，且**零产物**；
     修复前＝子进程不返回（超时/被内存安全线强杀 ⇒ 判红，detail 里写已耗时与峰值）。
  B：`--extract`（**选它的原因见 `check_big_block` 注释**：只有它走 `read_file_data` 的
     fragment 那条路）必须 rc≠0、理由含`上限`、峰值 ≪ 1 GiB（量不到就退化成 elapsed 上界）、
     且**不留半成品输出文件**。修复前＝rc=0「EXTRACT ok」+ 4 GiB 峰值（判红）。
  C：`--dump` 同样 rc≠0、理由含`上限`且属"字典/内存/xz"那一族；修复前＝rc=0「DUMP ok」
     + 1.5 GiB 峰值（判红）。另加"**正常镜像不受影响**"：现打一枚正常镜像（writer 用 xz 块）
     → 抽回来与源文件**逐字节相等**。
  反向锁：三份夹具都**不是**"随便什么坏文件都会红"——4 字节垃圾文件跑同一命令必须是
     "明确拒因（rc≠0 且理由非空）"且**不命中本判据的关键词**；另加一条"只有 hsqs magic 的
     4 字节残缺文件"证明 SquashFS 分支本身也会明确拒它。

自我证明：`selfcheck()` 给上面**每条断言**各喂一个反例（判据谓词与正式判据共用同一份代码），
  逐条打印；反例不通过 ⇒ 本脚本自己判 fail。其中 xz 那条的反例是**真做出来的**：
  把篡改后重算好的块头 CRC32 再改坏 ⇒ 独立进程里 liblzma 必须解不出来（证明 CRC 重算是承重的）。

机器安全（判红必须能安全地红）：三条夹具在修前分别会卡死 / 吃 4 GiB / 吃 1.5 GiB，所以每个
  子进程都套在 Windows **Job Object 的进程内存硬闸**（默认 2 GiB）里跑，并带墙钟超时；
  这道闸只保证"卡死子进程不会拖垮整机"，**不参与判定**。修前要**完整复现**那 4 GiB 分配
  （峰值 4105 MiB、rc=0 且抽出 16 字节）就 `--mem-cap-mb 6144`（或 `0` = 不设闸）；
  实测口径：默认 2 GiB 闸下修前的 F-03 是"撞闸 bad_alloc 崩"（rc=0xC0000409，零理由），
  一样判红 —— 红的是"没有明确拒因"这条，不是崩法。

输出：`✓` / `✗` 行 + 收尾行 `SQFS-DOS ok …` / `SQFS-DOS fail N 项`；
退出码 0=全绿、1=有断言不成立、2=外部前提缺失（构建产物／同目录判据原语缺件）。
"""
import argparse
import copy
import ctypes
import hashlib
import lzma
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

# ★ 照 run_ci.py 的同一条口径（它自己踩过这个坑）：脚本要**自己**保证可跑，不许依赖外部环境
#   变量。stdout 是管道/控制台而本地编码不是 UTF-8 时，打印 `✓/✗` 会直接 UnicodeEncodeError
#   ⇒ 退 1 且看不出是哪条断言（本地实测：cp936 控制台下 `print("\u2713")` 就炸）。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:      # 老 Python / 已重定向的自定义流：退回原行为，不影响判定
    pass

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# ── 复用同目录的"自造最小 SquashFS"原语（**直接 import，不抄**）──────────────────
#    夹具原语只有一份实现：check_fs_hardening.py 里的 `_dir_inode/_file_inode/_dir_group/
#    _meta_block/_superblock/_assemble` 与常量 `MAGIC/K_TYPE_*/K_UNCOMPRESSED_BIT`。
#    脚本按路径被运行（run_ci 就是 `sys.executable tests/check_*.py`），同目录 import 可行。
sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import check_fs_hardening as cfh
    CFH_ERR = ""
except Exception as _exc:      # 判据原语缺件 ⇒ 前提缺失（退出码 2），不是本判据 fail
    cfh = None
    CFH_ERR = repr(_exc)

PROBLEMS = []
CLI = DEFAULT_CLI

# ── 判据口径 ─────────────────────────────────────────────────────────────────
KEY = "上限"                     # 三条共用的必需关键词（"明确报错"的证据）
CYCLE_MARKS = ("成环", "回指", "环")        # F-01 那一族的理由用词
XZ_MARKS = ("字典", "内存", "xz")           # F-02 那一族的理由用词
BUDGET_S = 20.0                  # A/C 的墙钟预算：超时 ⇒ 判红（修前就是"不返回"）
GB_BUDGET_S = 5.0                # B 的"没有 GB 级分配"弱代理（修前 4 GiB 只花 0.99 s，故只是代理）
GB_PEAK = 1024 * 1024 * 1024     # 直接量：子进程峰值内存必须 ≪ 1 GiB
DEFAULT_MEM_CAP_MB = 2048        # 子进程内存安全线（Job Object 硬闸，只保机器、不参与判定）
K_XZ = 4                         # include/xrkit/engine/fs/decompress.h: Compression::Xz = 4
INVALID64 = 0xFFFFFFFFFFFFFFFF


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


# ── 一次 CLI 子进程的观测结果 + 判据谓词 ────────────────────────────────────────
#    谓词与正式判据**共用同一份代码**：selfcheck 拿合成/突变过的观测值喂同一个谓词，
#    翻不过来（该绿不绿 / 该红不红）就是谓词写坏了 —— 这就是"红/绿两向可判"的自证。
class Probe(object):
    __slots__ = ("name", "rc", "text", "reason", "elapsed", "killed", "peak", "artifacts")

    def __init__(self, name="", rc=0, text="", reason=None, elapsed=0.0, killed=None,
                 peak=None, artifacts=()):
        self.name = name
        self.rc = rc
        self.text = text
        self.reason = reason          # `DUMP|EXTRACT error reason=<这里>`；取不到 ⇒ None
        self.elapsed = elapsed        # 墙钟秒
        self.killed = killed          # None / "timeout" / "mem-cap"
        self.peak = peak              # 子进程峰值内存（字节；量不到 ⇒ None）
        self.artifacts = list(artifacts)

    def dup(self, **kw):
        p = copy.copy(self)
        for k, v in kw.items():
            setattr(p, k, v)
        return p

    def brief(self):
        return "rc=%s elapsed=%.2fs killed=%s peak=%s reason=%r" % (
            self.rc, self.elapsed, self.killed,
            ("%.1f MiB" % (self.peak / 1024.0 ** 2)) if self.peak is not None else "n/a",
            self.reason)


def pred_rejected(p):
    """明确拒因：rc≠0 + 理由非空 + 理由里带『上限』（零静默，不许只给 bad_alloc/崩溃）。"""
    return p.rc != 0 and bool(p.reason) and KEY in p.reason


def pred_within_budget(p, budget=None):
    """在预算内**正常返回**（不是被超时/内存闸强杀）。

    `budget=None` ⇒ 现取全局 `BUDGET_S`（`--budget` 会改它）：默认参数是**定义时**求值的，
    写成 `budget=BUDGET_S` 会让 `--budget` 只影响强杀时刻、不影响判定阈值（口径分家）。
    """
    return p.killed is None and p.elapsed <= (BUDGET_S if budget is None else budget)


def pred_no_artifacts(p):
    """零产物：失败时不许留输出目录/半成品文件。"""
    return not p.artifacts


def pred_no_gb_alloc(p):
    """没有发生 GB 级分配：峰值内存量得到就直判（<1 GiB），量不到就只认 elapsed 上界。"""
    if p.peak is not None and p.peak >= GB_PEAK:
        return False
    return p.elapsed <= GB_BUDGET_S


def pred_cycle_reason(p):
    """理由属"成环/回指"那一族（把 F-01 的失败钉在环检测上，而不是别的"上限"错误）。"""
    return bool(p.reason) and any(m in p.reason for m in CYCLE_MARKS)


def pred_xz_reason(p):
    """理由属"字典/内存/xz"那一族（把 F-02 的失败钉在解码器内存闸上）。"""
    return bool(p.reason) and any(m in p.reason for m in XZ_MARKS)


def pred_reverse_lock(p):
    """反向锁：是**明确拒因**（rc≠0 + 理由非空），但**不许**命中本判据的关键词。"""
    if p.rc == 0 or not p.reason or KEY in p.reason:
        return False
    return not any(m in p.reason for m in CYCLE_MARKS + XZ_MARKS)


def pred_sane_block_size(bs):
    """A 夹具自证的一半：block_size 合法 ⇒ 失败**只能**来自环检测，不可能是 F-03 那道闸。"""
    return bs == 131072


def pred_a_fixture(block_size, root_ref, sblk, first_entry):
    """A 夹具自证：根 inode 在 0、组 sblk=0、第一条目录项 `a` 的偏移 0 且类型=DIR ⇒ ref=0。"""
    return (block_size == 131072 and root_ref == 0 and sblk == 0
            and first_entry == (0, 1, "a"))


def pred_b_fixture(block_size, frag, block_offset, size, fragments, frag_table_start,
                   frag_stored, frag_uncompressed):
    """B 夹具自证：超块 block_size 超大 + REG 真的指向一条**压缩态且长度≥4** 的 fragment。"""
    return (block_size == 0xFFFFFFFF and frag == 0 and block_offset == 0 and size == 16
            and fragments >= 1 and frag_table_start != INVALID64
            and frag_stored >= 4 and not frag_uncompressed)


def pred_tampered_xz_ok(rc, got_digest, want_digest, got_len, want_len):
    """C 夹具自证：改过字典声明的 xz 流**仍能解出原文**（改的是声明，不是把流改坏）。"""
    return rc == 0 and got_digest == want_digest and got_len == want_len


def pred_xz_dict_inflated(new_dict, old_dict):
    """C 夹具自证：篡改后的字典声明确实被放大到 ≥1 GiB（远远超过修后的上限）。"""
    return new_dict >= 1024 ** 3 and new_dict > old_dict


def pred_normal_unchanged(rc_pack, rc_get, byte_equal):
    """闸只拦超限，不动正常路：正常镜像必须打得出来、抽得回来且逐字节相等。"""
    return rc_pack == 0 and rc_get == 0 and byte_equal


# ── 跑 CLI：墙钟超时 + （Windows）Job Object 进程内存硬闸 ────────────────────────
JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
_JobObjectExtendedLimitInformation = 9


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [("ReadOperationCount", ctypes.c_ulonglong),
                ("WriteOperationCount", ctypes.c_ulonglong),
                ("OtherOperationCount", ctypes.c_ulonglong),
                ("ReadTransferCount", ctypes.c_ulonglong),
                ("WriteTransferCount", ctypes.c_ulonglong),
                ("OtherTransferCount", ctypes.c_ulonglong)]


class _BASIC_LIMIT(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", ctypes.c_uint32),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", ctypes.c_uint32),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", ctypes.c_uint32),
                ("SchedulingClass", ctypes.c_uint32)]


class _EXT_LIMIT(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", _BASIC_LIMIT),
                ("IoInfo", _IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


def _job_create(cap_bytes):
    if os.name != "nt" or not cap_bytes:
        return None, None
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        job = k32.CreateJobObjectW(None, None)
        if not job:
            return None, None
        info = _EXT_LIMIT()
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY
        info.ProcessMemoryLimit = ctypes.c_size_t(cap_bytes)
        if not k32.SetInformationJobObject(ctypes.c_void_p(job),
                                           _JobObjectExtendedLimitInformation,
                                           ctypes.byref(info), ctypes.sizeof(info)):
            k32.CloseHandle(ctypes.c_void_p(job))
            return None, None
        return k32, job
    except Exception:
        return None, None


def _job_peak(k32, job):
    if not job:
        return None
    try:
        info = _EXT_LIMIT()
        if k32.QueryInformationJobObject(ctypes.c_void_p(job),
                                         _JobObjectExtendedLimitInformation,
                                         ctypes.byref(info), ctypes.sizeof(info), None):
            return int(info.PeakProcessMemoryUsed)
    except Exception:
        pass
    return None


def reason_of(text, tool):
    """从 `DUMP|EXTRACT error reason=<r>` 行里取理由（取不到 ⇒ None：崩死/强杀就是"零理由"）。"""
    prefix = tool + " error reason="
    for line in text.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):]
    return None


def run_cli(tool, argv, timeout, mem_cap_mb=DEFAULT_MEM_CAP_MB, cwd=None, watch=()):
    """跑一次 CLI。返回 Probe（含 rc/输出/理由/耗时/是否被强杀/峰值内存/产物）。"""
    cmd = [str(CLI)] + [str(a) for a in argv]
    k32, job = _job_create(int(mem_cap_mb) * 1024 * 1024 if mem_cap_mb else 0)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd)
    if job:
        # 赋值失败（进程已退出 / 已在别的作业里）⇒ 这一趟**量不到**峰值，如实记成 None，
        # 别拿"没量到 = 0"冒充满足断言（否则"没有 GB 级分配"就成了假绿）。
        if not k32.AssignProcessToJobObject(ctypes.c_void_p(job),
                                            ctypes.c_void_p(int(proc._handle))):
            k32.CloseHandle(ctypes.c_void_p(job))
            job = None
    t0 = time.monotonic()
    killed = None
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        killed = "timeout"                  # 卡死 ⇒ 强杀（修前的 F-01 就是这一支）
        proc.kill()
        out, _ = proc.communicate()
    elapsed = time.monotonic() - t0
    peak = _job_peak(k32, job)
    if job:
        k32.CloseHandle(ctypes.c_void_p(job))
    if killed is None and peak is not None and mem_cap_mb and peak >= int(mem_cap_mb) * 1024 * 1024:
        killed = "mem-cap"                  # 撞到内存安全线（作业对象让分配失败/进程退出）
    text = out.decode("utf-8", errors="replace")
    artifacts = [str(p) for p in watch if Path(p).exists()]
    if cwd is not None:
        artifacts += [str(p) for p in sorted(Path(cwd).iterdir())]
    return Probe(name=" ".join(str(a) for a in argv), rc=proc.returncode, text=text,
                 reason=reason_of(text, tool), elapsed=elapsed, killed=killed,
                 peak=peak, artifacts=artifacts)


# ── 夹具 A：目录项成环（F-01）─────────────────────────────────────────────────
def build_dir_loop(path):
    """根目录 inode 在 offset 0，目录表里一条 `("a", 0, K_TYPE_DIR)` **指回根自己**。

    `_dir_group` 里 `sblk` 恒 0 且 `inode_off` 是 16 位 ⇒ `child_ref=(0<<16)|0=0`＝根 inode。
    再放一条正常文件条目（uncompressed 数据块）做对照，让镜像本身"看起来"是合法包。
    """
    off_root, off_ok = 0, 32
    payload = bytearray(b"\x00" * 96)
    payload[off_root:off_root + 32] = cfh._dir_inode(0, 0)          # size 稍后回填
    payload[off_ok:off_ok + 32] = cfh._file_inode(0, 8)
    struct.pack_into("<I", payload, off_ok + 32, cfh.K_UNCOMPRESSED_BIT | 8)
    g_root = cfh._dir_group([("a", off_root, cfh.K_TYPE_DIR),
                             ("ok.txt", off_ok, cfh.K_TYPE_FILE)])
    payload[off_root:off_root + 32] = cfh._dir_inode(0, len(g_root))
    image, starts, _ = cfh._assemble(bytes(payload), g_root, [(off_ok, b"LOOP-OK\n")],
                                     131072, 3)
    buf = bytearray(image)
    # 数据块落点拼完才知道 ⇒ 回填 REG 的 start_block（inode 表第 0 块，块头 2 字节）
    struct.pack_into("<I", buf, 96 + 2 + off_ok + 16, starts[off_ok])
    path.write_bytes(bytes(buf))
    return bytes(buf)


def witness_dir_loop(blob):
    """夹具自证：把"根 inode 在 0 + 第一条目录项指回根"从 bytes 里读回来。"""
    block_size = struct.unpack_from("<I", blob, 12)[0]
    root_ref = struct.unpack_from("<Q", blob, 32)[0]
    inode_table = struct.unpack_from("<Q", blob, 64)[0]
    dir_table = struct.unpack_from("<Q", blob, 72)[0]
    body = inode_table + 2                     # 跳过 inode 元数据块头
    dir_block = struct.unpack_from("<I", blob, body + 16)[0]
    dir_off = struct.unpack_from("<H", blob, body + 26)[0]
    p = dir_table + 2 + dir_block + dir_off
    sblk = struct.unpack_from("<I", blob, p + 4)[0]           # 组头第 2 个 u32 = sblk
    e_off, _ino, e_type = struct.unpack_from("<HHH", blob, p + 12)
    nsz = struct.unpack_from("<H", blob, p + 18)[0] + 1
    name = bytes(blob[p + 20:p + 20 + nsz]).decode("utf-8")
    return dict(block_size=block_size, root_ref=root_ref, sblk=sblk,
                first_entry=(e_off, e_type, name))


# ── 夹具 B：超块 block_size 未校验（F-03）─────────────────────────────────────
def build_big_block(path):
    """`block_size=0xFFFFFFFF` + 一个带 fragment 的 REG inode（fragment=0、offset=0、size=16）。

    `cfh._superblock` 把 fragments 写死成 0、fragment_table_start 写死成 -1，所以这里**在返回
    的 bytes 上回填**：fragments@16（u32）、fragment_table_start@80（u64），并手工铺上
      · fragment 数据块（**压缩态**：`compression=1`(gzip) ⇒ 用 `zlib.compress`，别置 bit24）；
      · 一个未压缩元数据块（`cfh._meta_block`）装 16 字节 fragment 条目：
        u64 start_block + u32 size（低 24 位＝存储长度、bit24＝"未压缩"标志）；
      · 一张 u64 绝对偏移数组（查找表），指向上面的元数据块。
    长度必须 ≥4 字节：`decompress_block` 在 `src_len==0` 时会提前返回，那就不走巨量分配了。
    """
    off_root, off_f = 0, 32
    payload = bytearray(b"\x00" * 8192)        # 元数据体给足，省得跨块逻辑干扰
    payload[off_root:off_root + 32] = cfh._dir_inode(0, 0)
    rec = bytearray(cfh._file_inode(0, 16))
    struct.pack_into("<I", rec, 20, 0)         # fragment = 0（**不是** K_INVALID_FRAG）
    struct.pack_into("<I", rec, 24, 0)         # block_offset = 0
    struct.pack_into("<I", rec, 28, 16)        # file_size = 16
    payload[off_f:off_f + 32] = bytes(rec)
    g_root = cfh._dir_group([("f.bin", off_f, cfh.K_TYPE_FILE)])
    payload[off_root:off_root + 32] = cfh._dir_inode(0, len(g_root))
    image, _starts, _ = cfh._assemble(bytes(payload), g_root, [], 0xFFFFFFFF, 2)

    buf = bytearray(image)
    frag_payload = zlib.compress(b"F" * 64)    # 解出 64 字节 ≥ frag_size(=16)
    frag_off = len(buf)
    buf += frag_payload
    entry = struct.pack("<Q", frag_off) + struct.pack("<I", len(frag_payload)) + struct.pack("<I", 0)
    meta_off = len(buf)
    buf += cfh._meta_block(entry)
    lookup_off = len(buf)
    buf += struct.pack("<Q", meta_off)
    struct.pack_into("<I", buf, 16, 1)                 # fragments = 1
    struct.pack_into("<Q", buf, 80, lookup_off)        # fragment_table_start
    struct.pack_into("<Q", buf, 40, len(buf))          # bytes_used
    path.write_bytes(bytes(buf))
    return bytes(buf)


def witness_big_block(blob):
    """夹具自证：把"这条路真的会走到 fragment 解压"从 bytes 里读回来。"""
    block_size = struct.unpack_from("<I", blob, 12)[0]
    fragments = struct.unpack_from("<I", blob, 16)[0]
    frag_table = struct.unpack_from("<Q", blob, 80)[0]
    inode_table = struct.unpack_from("<Q", blob, 64)[0]
    body = inode_table + 2 + 32                        # 第 2 条 inode（REG @32）
    frag = struct.unpack_from("<I", blob, body + 20)[0]
    block_offset = struct.unpack_from("<I", blob, body + 24)[0]
    size = struct.unpack_from("<I", blob, body + 28)[0]
    lookup = struct.unpack_from("<Q", blob, frag_table)[0]
    ent = struct.unpack_from("<QI", blob, lookup + 2 + 0)   # 元数据块头 2 字节 + 条目
    frag_stored = ent[1] & 0xFFFFFF
    frag_uncompressed = bool(ent[1] & cfh.K_UNCOMPRESSED_BIT)
    return (block_size, frag, block_offset, size, fragments, frag_table,
            frag_stored, frag_uncompressed)


# ── 夹具 C：xz 字典炸弹（F-02）───────────────────────────────────────────────
def xz_dict_from_props(x):
    """LZMA2 滤镜 props 那**一个字节**编码的字典大小（.xz 规范：40 是 0xFFFFFFFF 特例）。"""
    if x == 40:
        return 0xFFFFFFFF
    return (2 | (x & 1)) << (x // 2 + 11)


def xz_props_for_at_least(wanted):
    """找一个"声明字典 ≥ wanted"的 props 值（从小往大，最小改动）。"""
    for x in range(0, 41):
        if xz_dict_from_props(x) >= wanted:
            return x
    return 40


def xz_tamper_dict(raw, new_props):
    """把 xz 流里 LZMA2 的 dict_size 改成 new_props，并**重算 block header 的 CRC32**。

    为什么要重算：块头尾部 4 字节是覆盖"块头（含滤镜 props）"的 CRC32，liblzma 先验它；
    不重算就会被当成"数据损坏"拒掉 —— 那判据测到的就不是"内存闸"那一条了。
    块头布局（.xz 规范）：[size 字节][flags][可选 compressed size varint][可选 uncompressed
    size varint][逐滤镜：id varint / props 长度 varint / props…][padding 到 4 的倍数][CRC32]，
    其中 size 字节 n ⇒ 块头总长 (n+1)*4（**含** CRC32）。
    """
    b = bytearray(raw)
    assert bytes(b[0:6]) == b"\xfd7zXZ\x00", bytes(b[0:6])
    start = 12                                  # 12 字节流头（magic+flags+CRC32）之后
    end = start + (b[start] + 1) * 4
    flags = b[start + 1]
    p = start + 2

    def varint(p):
        v = sh = 0
        while True:
            x = b[p]
            p += 1
            v |= (x & 0x7F) << sh
            if not (x & 0x80):
                return v, p
            sh += 7

    if flags & 0x40:
        _, p = varint(p)                        # 可选 compressed size
    if flags & 0x80:
        _, p = varint(p)                        # 可选 uncompressed size
    fid, p = varint(p)                          # 滤镜 id（LZMA2 = 0x21）
    psize, p = varint(p)                        # props 长度（LZMA2 恒 1）
    assert fid == 0x21 and psize == 1, (fid, psize)
    old_props = b[p]
    b[p] = new_props
    crc = zlib.crc32(bytes(b[start:end - 4])) & 0xFFFFFFFF
    b[end - 4:end] = struct.pack("<I", crc)
    return bytes(b), dict(props_off=p - start, old_props=old_props, new_props=new_props,
                          hdr_start=start, hdr_end=end, crc=crc)


def xz_break_crc(tampered):
    """反例用：把重算好的块头 CRC32 再改坏一位（独立进程里必须解不出来）。"""
    b = bytearray(tampered)
    end = 12 + (b[12] + 1) * 4
    crc = struct.unpack_from("<I", b, end - 4)[0] ^ 0x00000001
    struct.pack_into("<I", b, end - 4, crc)
    return bytes(b)


_XZ_VERIFY_CODE = ("import lzma,sys,hashlib;"
                   "d=lzma.decompress(open(sys.argv[1],'rb').read());"
                   "sys.stdout.write(hashlib.sha256(d).hexdigest()+' '+str(len(d)))")


def xz_decode_in_subprocess(path):
    """**独立进程**里解压（就是任务要求的那条验证：`python -c "import lzma; …"`）。

    返回 (rc, sha256, 长度)；解不出来时 sha256/长度为 None。用独立进程而不是 in-process：
    证明的是"另一个干净的 liblzma 也认这段流"，而不是"我这个进程恰好还留着状态"。
    """
    exe = sys.executable
    if not exe:
        try:
            data = lzma.decompress(Path(path).read_bytes())
            return 0, hashlib.sha256(data).hexdigest(), len(data)
        except Exception:
            return 1, None, None
    proc = subprocess.run([exe, "-c", _XZ_VERIFY_CODE, str(path)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    parts = proc.stdout.decode("utf-8", errors="replace").strip().split()
    if proc.returncode != 0 or len(parts) != 2:
        return proc.returncode or 1, None, None
    return 0, parts[0], int(parts[1])


def build_xz_bomb(path, tampered_out):
    """超块 compression=xz + **xz 压缩的 inode 元数据块**，其 dict_size 被改到 1.5 GiB。

    元数据块头仍是 cfh 的格式（u16：bit15=1 ⇒ "未压缩"；这里要 **0** ⇒ 压缩态，
    低 15 位＝压缩后长度），所以不能直接用 `cfh._meta_block`（它写死未压缩）。
    `_assemble` 也把元数据块写死成未压缩 8192 ⇒ 这里用**同一套原语**手工摆块，顺序与
    `_assemble` 一致：超块 → inode 元数据块 → 目录元数据块（无数据块）。
    """
    off_root, off_f = 0, 32
    payload = bytearray(b"\x00" * 128)
    payload[off_root:off_root + 32] = cfh._dir_inode(0, 0)
    payload[off_f:off_f + 32] = cfh._file_inode(0, 8)
    struct.pack_into("<I", payload, off_f + 32, cfh.K_UNCOMPRESSED_BIT | 8)
    g_root = cfh._dir_group([("c.bin", off_f, cfh.K_TYPE_FILE)])
    payload[off_root:off_root + 32] = cfh._dir_inode(0, len(g_root))
    inode_plain = bytes(payload)

    props = xz_props_for_at_least(1536 * 1024 * 1024)      # 1.5 GiB
    legal = lzma.compress(inode_plain, format=lzma.FORMAT_XZ, preset=6)
    tampered, info = xz_tamper_dict(legal, props)
    Path(tampered_out).write_bytes(tampered)
    broken = xz_break_crc(tampered)
    broken_path = Path(str(tampered_out) + ".badcrc")
    broken_path.write_bytes(broken)

    inode_block = struct.pack("<H", len(tampered)) + tampered      # bit15=0 ⇒ 压缩态
    dir_block = cfh._meta_block(g_root)
    inode_table_start = 96
    directory_table_start = inode_table_start + len(inode_block)
    total = directory_table_start + len(dir_block)
    buf = bytearray(cfh._superblock(total, inode_table_start, directory_table_start, 0,
                                    131072, 2))
    struct.pack_into("<H", buf, 20, K_XZ)                  # compression = xz
    struct.pack_into("<Q", buf, 40, total)                 # bytes_used
    buf += inode_block
    buf += dir_block
    Path(path).write_bytes(bytes(buf))

    verify = xz_decode_in_subprocess(tampered_out)
    broken_verify = xz_decode_in_subprocess(broken_path)
    info.update(dict(legal=legal, tampered=tampered, broken_path=str(broken_path),
                     inode_plain=inode_plain, blob=bytes(buf),
                     dict_old=xz_dict_from_props(info["old_props"]),
                     dict_new=xz_dict_from_props(props),
                     meta_header=struct.unpack_from("<H", buf, inode_table_start)[0],
                     compression=struct.unpack_from("<H", buf, 20)[0],
                     verify=verify, broken_verify=broken_verify,
                     expect_digest=hashlib.sha256(inode_plain).hexdigest(),
                     expect_len=len(inode_plain)))
    return info


# ── 判据 ─────────────────────────────────────────────────────────────────────
def check_dir_loop(tmp, work):
    """F-01：`--dump` 遇到"指回自己"的目录项必须**明确报错**，不许无限展开。"""
    print("[F-01] 目录项成环：根目录里一条指回根 inode 自己的目录项（自造最小 SquashFS）")
    img = tmp / "dir_loop.sqfs"
    blob = build_dir_loop(img)
    w = witness_dir_loop(blob)
    want(pred_a_fixture(**w) and pred_sane_block_size(w["block_size"]),
         "夹具自证：block_size=131072（合法）、根 inode 在 0、第一条目录项 `a` 偏移 0 且类型 DIR "
         "⇒ child_ref=(0<<16)|0=0 指回根",
         "实测 %r" % (w,))

    # 选 `--dump`：它走 `list_entries` → `Image::list`，成环就在那里展开。
    # cwd 指到一个空沙箱：这样"零产物"这条断言查的是**本次运行真的没写字**（含隐式落点）。
    sandbox = work / "f01_cwd"
    sandbox.mkdir(parents=True, exist_ok=True)
    p = run_cli("DUMP", ["--dump", str(img)], timeout=BUDGET_S, cwd=sandbox,
                watch=[tmp / "dir_loop.sqfs.out", tmp / "a", tmp / "ok.txt"])
    want(pred_within_budget(p),
         "F-01 `--dump` 必须在 %.0f s 内返回（修前＝无限展开 ⇒ 超时/被内存安全线强杀）" % BUDGET_S,
         p.brief())
    want(pred_rejected(p), "F-01 必须明确拒因：rc≠0 且理由含『上限』",
         p.brief() + "\n" + p.text.strip()[:400])
    want(pred_cycle_reason(p), "F-01 理由必须属『成环/回指』那一族（不许拿别的『上限』错误顶替）",
         p.brief())
    want(pred_no_artifacts(p), "F-01 失败时不许留下任何输出目录/文件",
         "产物：%s\n%s" % (p.artifacts, p.text.strip()[:300]))
    return p


def check_big_block(tmp, work):
    """F-03：`block_size=0xFFFFFFFF` + fragment 那条路 ⇒ 修前一次 `assign` 约 4 GiB。

    ★ 为什么选 `--extract`（而不是 `--dump`）：巨量分配发生在 `decompress_block` 的第一句
      `out.assign(out_capacity, 0)`，而它只在**解压**时被调用。`--dump` 只走 `list()`（读 inode
      头 + 目录表，全是元数据读），**永远不会**碰 fragment 解压；`--extract` 走
      `read_entry_file` → `read_file_data`，REG 的 `fragment != K_INVALID_FRAG` 时会调
      `fragment_entry()` 取条目、再调 `decompress_block(…, static_cast<size_t>(bs), …)`，
      容量那个参数就是超块里的 `block_size` ⇒ 正好命中。
    """
    print("[F-03] 超块 block_size 未校验：block_size=0xFFFFFFFF + 带 fragment 的 REG inode")
    img = tmp / "big_block.sqfs"
    blob = build_big_block(img)
    w = witness_big_block(blob)
    want(pred_b_fixture(*w),
         "夹具自证：block_size=0xFFFFFFFF、REG 的 fragment=0/offset=0/size=16、"
         "fragments≥1 且那条 fragment 是压缩态且长度≥4",
         "实测 (block_size, frag, off, size, fragments, table, stored, uncompressed)=%r" % (w,))

    sandbox = work / "f03_cwd"
    sandbox.mkdir(parents=True, exist_ok=True)
    out_file = work / "f03_out.bin"
    p = run_cli("EXTRACT", ["--extract", str(img), "f.bin", str(out_file)],
                timeout=BUDGET_S, cwd=sandbox, watch=[out_file])
    want(pred_rejected(p), "F-03 必须明确拒因：rc≠0 且理由含『上限』",
         p.brief() + "\n" + p.text.strip()[:400])
    want(pred_within_budget(p), "F-03 必须在 %.0f s 内返回（不许卡在 GB 级分配的 memset 上）" % BUDGET_S,
         p.brief())
    want(pred_no_gb_alloc(p),
         "F-03 不许发生 GB 级分配（峰值 <1 GiB；量不到峰值就只认 elapsed ≤%.0f s）" % GB_BUDGET_S,
         p.brief())
    want(pred_no_artifacts(p), "F-03 失败时不许留下半成品输出文件",
         "产物：%s\n%s" % (p.artifacts, p.text.strip()[:300]))
    return p


def check_xz_bomb(tmp, work):
    """F-02：元数据块的 xz 流声明 1.5 GiB 字典 ⇒ 必须被"解码器内存上限"明确拒掉。"""
    print("[F-02] xz 字典炸弹：元数据块的 xz 块头声明 1.5 GiB 字典（只改 props 一个字节 + 重算 CRC32）")
    img = tmp / "xz_bomb.sqfs"
    info = build_xz_bomb(img, tmp / "xz_tampered.xz")

    print("[F-02] 篡改明细：LZMA2 props 字节 %d（%d 字节字典）→ %d（%d 字节字典），"
          "块头 CRC32 重算为 0x%08X；坏了 CRC 的反例流留在 %s"
          % (info["old_props"], info["dict_old"], info["new_props"], info["dict_new"],
             info["crc"], info["broken_path"]))
    want(pred_xz_dict_inflated(info["dict_new"], info["dict_old"]),
         "夹具自证：篡改后的字典声明 ≥1 GiB 且确实比原来的大",
         "old=%d new=%d" % (info["dict_old"], info["dict_new"]))
    want(info["compression"] == K_XZ and (info["meta_header"] & 0x8000) == 0
         and (info["meta_header"] & 0x7FFF) == len(info["tampered"]),
         "夹具自证：超块 compression=xz(4) 且元数据块头 bit15=0（压缩态）+ 长度==xz 流长",
         "compression=%d header=0x%04X xz_len=%d" % (info["compression"], info["meta_header"],
                                                     len(info["tampered"])))
    rc, digest, length = info["verify"]
    want(pred_tampered_xz_ok(rc, digest, info["expect_digest"], length, info["expect_len"]),
         "夹具自证：改完字典声明后**独立进程**里仍能解出原文（改的是声明，不是把流改坏）",
         "verify rc=%s got=%s/%s want=%s/%s" % (rc, digest, length, info["expect_digest"],
                                                info["expect_len"]))

    sandbox = work / "f02_cwd"
    sandbox.mkdir(parents=True, exist_ok=True)
    # 选 `--dump`：第一条要读的就是 inode 元数据块（xz 那条）⇒ `read_inode` → `meta_block`
    # → `decompress_block(Compression::Xz, …, kMetaBlockSize, …)`，正好命中内存闸。
    p = run_cli("DUMP", ["--dump", str(img)], timeout=BUDGET_S, cwd=sandbox,
                watch=[tmp / "xz_bomb.sqfs.out"])
    want(pred_rejected(p), "F-02 必须明确拒因：rc≠0 且理由含『上限』",
         p.brief() + "\n" + p.text.strip()[:400])
    want(pred_xz_reason(p), "F-02 理由必须属『字典/内存/xz』那一族（把失败钉在解码器内存闸上）",
         p.brief())
    want(pred_within_budget(p), "F-02 必须在 %.0f s 内返回（修前会真分配 1.5 GiB 字典）" % BUDGET_S,
         p.brief())
    want(pred_no_gb_alloc(p),
         "F-02 不许发生 GB 级分配（峰值 <1 GiB；量不到峰值就只认 elapsed ≤%.0f s）" % GB_BUDGET_S,
         p.brief())
    return p, info


def check_normal_unchanged(tmp):
    """正常镜像不受影响：现打一枚（writer 的块就是 xz 压的）→ 抽回来逐字节相等。

    为什么必须带上这条：F-02 的修法是给 xz 解码器**设上限**，上限给小了会把合法块也拒掉
    （元数据块 8 KiB / 数据块 ≤1 MiB），所以"闸只拦超限、正常路不动"必须一起断言。
    """
    print("[F-02 附带] 正常镜像不受影响：封包（xz 块）→ 抽取，逐字节相等")
    src = tmp / "normal_src"
    src.mkdir(parents=True, exist_ok=True)
    compressible = bytes(range(256)) * 1200          # 300 KiB：跨多个数据块
    incompressible = os.urandom(40960)               # 40 KiB 随机：逼出"存不压缩"那一支
    (src / "a.bin").write_bytes(compressible)
    (src / "b.bin").write_bytes(incompressible)
    img = tmp / "normal.sqfs"
    rc_pack, out_pack = _plain(["--pack-sqfs", str(img), str(src)])
    rcs = [rc_pack]
    outs = [out_pack]
    byte_equal = True
    for name, expect in (("a.bin", compressible), ("b.bin", incompressible)):
        dst = tmp / ("normal_out_" + name)
        rc_i, out_i = _plain(["--extract", str(img), name, str(dst)])
        got = dst.read_bytes() if dst.is_file() else b""
        byte_equal = byte_equal and rc_i == 0 and got == expect
        rcs.append(rc_i)
        outs.append(out_i)
    rc_get = max(rcs[1:])
    want(pred_normal_unchanged(rc_pack, rc_get, byte_equal),
         "正常镜像照打照抽且逐字节相等（压缩块 + 不压缩块两条路）",
         "rc_pack=%d rc_get=%d byte_equal=%s\n%s\n%s"
         % (rc_pack, rc_get, byte_equal, outs[0].strip()[:200],
            "\n".join(o.strip()[:200] for o in outs[1:])))


def _plain(argv):
    """不带内存闸的普通调用（正常镜像那条路要跑真正的压缩/解压，别被安全线干扰）。"""
    proc = subprocess.run([str(CLI)] + list(argv), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def check_reverse_lock(tmp, work):
    """反向锁：本判据不是"随便什么坏文件都会红"。

    4 字节垃圾文件（`JUNK`）跑同一条命令，必须给出**明确拒因**（rc≠0 + 理由非空），
    而理由里**不许**出现本判据的关键词（`上限` / 成环族 / 字典族）—— 否则说明判据
    命中的是"任何坏文件都有的通用错误"，而不是这三条 DoS。
    再加一条"只有 hsqs magic 的 4 字节残缺文件"：它进的是 **SquashFS 分支**，
    必须同样明确拒它（证明拒绝发生在解析入口，而不是靠 DB 分支兜底）。
    """
    print("[反向锁] 4 字节垃圾 / 只有 magic 的残缺文件：必须是明确拒因，且不命中判据关键词")
    junk = tmp / "junk.bin"
    junk.write_bytes(b"JUNK")
    magic_only = tmp / "magic_only.sqfs"
    magic_only.write_bytes(b"hsqs")
    cases = [("垃圾文件 --dump", "DUMP", ["--dump", str(junk)]),
             ("垃圾文件 --extract", "EXTRACT",
              ["--extract", str(junk), "f.bin", str(work / "j.bin")]),
             ("只有 magic 的残缺包 --dump", "DUMP", ["--dump", str(magic_only)])]
    for label, tool, argv in cases:
        p = run_cli(tool, argv, timeout=BUDGET_S, watch=[work / "j.bin"])
        print("      %s → rc=%d reason=%r" % (label, p.rc, p.reason))
        want(pred_reverse_lock(p), "反向锁：%s 必须明确拒因且不命中判据关键词" % label,
             p.brief() + "\n" + p.text.strip()[:300])


# ── 自我证明 ─────────────────────────────────────────────────────────────────
def selfcheck(c_info):
    """每条断言各喂一个反例：谓词翻不过来 ⇒ 谓词写坏了 ⇒ 本脚本判 fail。

    关键口径：**正例一律用合成的"修复后该长什么样"的观测值**，不拿真实运行结果当正例 ——
    否则"修前红"的时候连自我证明都会跟着红，判据就不再是"红/绿两向可判"了。
    反例分两类：① 突变关键字段（`Probe.dup(...)`）；② **真做出来的**：把 C 夹具那块重算好的
    块头 CRC32 再改坏，独立进程里必须解不出来（证明 CRC 重算是承重的，不是装饰）。
    """
    print("[selfcheck] 每条断言各喂一个反例（谓词与正式判据共用同一份代码）")
    a = Probe(name="synthetic-F-01", rc=1, elapsed=0.01, peak=2 * 1024 * 1024,
              reason="目录项成环/回指（同一个目录 inode 被重复展开）：条目「a」指向 ref=0，"
                     "已拒绝继续遍历（上限=4194304 条目 / 256 层）")
    b = Probe(name="synthetic-F-03", rc=1, elapsed=0.01, peak=2 * 1024 * 1024,
              reason="超块 block_size 非法（必须是 4096..1 MiB 之间的 2 的幂）："
                     "block_size=4294967295 上限=1048576")
    c = Probe(name="synthetic-F-02", rc=1, elapsed=0.01, peak=2 * 1024 * 1024,
              reason="元数据块解压失败 @96：xz 解码内存超过上限（块头声明的字典过大）："
                     "块声明的字典需要 1610678328 字节以上，上限=8388608 字节")
    junk = Probe(name="synthetic-junk", rc=1, elapsed=0.01,
                 reason="认不出这个包的格式（DB 自洽判定有歧义；或它就是加密包/需要解密插件）")
    normal = Probe(name="synthetic-normal", rc=0, elapsed=0.01, reason=None)
    # 修前实测值（留着当反例的真值）：F-03 峰值 4105 MiB / F-02 峰值 1540 MiB、都"成功"、零理由
    b_pre = Probe(name="pre-fix-F-03", rc=0, elapsed=0.99, peak=4105 * 1024 ** 2, reason=None)
    c_pre = Probe(name="pre-fix-F-02", rc=0, elapsed=0.01, peak=1540 * 1024 ** 2, reason=None)

    # ① 「明确拒因」：关键词换成不可能出现的串 / rc=0 且零理由 ⇒ 必须变红
    want(pred_rejected(a) and not pred_rejected(a.dup(reason="Zzz-永不出现的串"))
         and not pred_rejected(c_pre),
         "反例自证①：`上限` 换成不可能出现的串 / rc=0 且零理由（修前原样）⇒ 必须判红",
         "pos=%s neg_key=%s neg_rc0=%s"
         % (pred_rejected(a), pred_rejected(a.dup(reason="Zzz")), pred_rejected(c_pre)))
    # ② 预算：被超时强杀 / elapsed 超预算 ⇒ 必须变红
    want(pred_within_budget(a) and not pred_within_budget(a.dup(killed="timeout", elapsed=20.0))
         and not pred_within_budget(a.dup(elapsed=BUDGET_S + 5)),
         "反例自证②：被超时强杀 / elapsed 超预算 ⇒ 必须判红",
         "pos=%s neg_killed=%s neg_slow=%s"
         % (pred_within_budget(a), pred_within_budget(a.dup(killed="timeout", elapsed=20.0)),
            pred_within_budget(a.dup(elapsed=BUDGET_S + 5))))
    # ③ GB 级分配：真值取自修前实测（F-03 峰值 4105 MiB / F-02 峰值 1540 MiB）⇒ 必须判红
    want(pred_no_gb_alloc(b) and not pred_no_gb_alloc(b_pre)
         and not pred_no_gb_alloc(c_pre)
         and not pred_no_gb_alloc(b.dup(peak=8 * 1024 ** 2, elapsed=GB_BUDGET_S + 1)),
         "反例自证③：峰值 4105 MiB / 1540 MiB（修前实测）或 elapsed 超上界 ⇒ 必须判红",
         "pos=%s neg_4105=%s neg_1540=%s neg_slow=%s"
         % (pred_no_gb_alloc(b), pred_no_gb_alloc(b_pre), pred_no_gb_alloc(c_pre),
            pred_no_gb_alloc(b.dup(peak=8 * 1024 ** 2, elapsed=GB_BUDGET_S + 1))))
    # ④ 零产物：留了半成品必须判红（修前 F-03 就是"成功"写出 16 字节）
    want(pred_no_artifacts(a)
         and not pred_no_artifacts(b_pre.dup(artifacts=["f03_out.bin"])),
         "反例自证④：失败/成功后留了输出文件（修前 F-03 写出 16 字节）⇒ 必须判红",
         "pos=%s neg=%s" % (pred_no_artifacts(a),
                            pred_no_artifacts(b_pre.dup(artifacts=["f03_out.bin"]))))
    # ⑤ 族别关键词：拿 F-03 的理由去喂 F-01/F-02 的族别谓词 ⇒ 必须变红（两条判据分得开）
    want(pred_cycle_reason(a) and not pred_cycle_reason(b)
         and pred_xz_reason(c) and not pred_xz_reason(b),
         "反例自证⑤：把 F-03 的理由（超块 block_size）喂给 F-01/F-02 的族别谓词 ⇒ 必须判红",
         "cycle(a)=%s cycle(b)=%s xz(c)=%s xz(b)=%s"
         % (pred_cycle_reason(a), pred_cycle_reason(b), pred_xz_reason(c), pred_xz_reason(b)))
    # ⑥ 反向锁：正常镜像（rc=0）/ 命中关键词的 A 夹具 ⇒ 必须变红
    want(pred_reverse_lock(junk) and not pred_reverse_lock(normal) and not pred_reverse_lock(a),
         "反例自证⑥：正常镜像（rc=0，无拒因）与 A 夹具（理由含『上限』）喂给反向锁 ⇒ 必须判红",
         "junk=%s normal=%s cycle=%s"
         % (pred_reverse_lock(junk), pred_reverse_lock(normal), pred_reverse_lock(a)))
    # ⑦ A 夹具自证：根 inode 不在 0 / 项指的不是 0 / block_size 换成 B 的超大值 ⇒ 必须变红
    good_a = dict(block_size=131072, root_ref=0, sblk=0, first_entry=(0, 1, "a"))
    want(pred_a_fixture(**good_a)
         and not pred_a_fixture(0xFFFFFFFF, 0, 0, (0, 1, "a"))
         and not pred_a_fixture(131072, 32, 0, (0, 1, "a"))
         and not pred_a_fixture(131072, 0, 1, (0, 1, "a"))
         and not pred_a_fixture(131072, 0, 0, (32, 1, "a")),
         "反例自证⑦：block_size 换成 0xFFFFFFFF / 根 inode 不在 0 / sblk≠0 / 项指向 32 ⇒ 必须判红",
         "pos=%s bs=%s root=%s sblk=%s off=%s"
         % (pred_a_fixture(**good_a), pred_a_fixture(0xFFFFFFFF, 0, 0, (0, 1, "a")),
            pred_a_fixture(131072, 32, 0, (0, 1, "a")), pred_a_fixture(131072, 0, 1, (0, 1, "a")),
            pred_a_fixture(131072, 0, 0, (32, 1, "a"))))
    # ⑧ B 夹具自证：fragment 没了 / 长度<4 / 未压缩标志 / block_size 合法 ⇒ 必须变红
    good_b = (0xFFFFFFFF, 0, 0, 16, 1, 100, 24, False)
    want(pred_b_fixture(*good_b)
         and not pred_b_fixture(0xFFFFFFFF, 0xFFFFFFFF, 0, 16, 1, 100, 24, False)
         and not pred_b_fixture(0xFFFFFFFF, 0, 0, 16, 1, 100, 3, False)
         and not pred_b_fixture(0xFFFFFFFF, 0, 0, 16, 1, 100, 24, True)
         and not pred_b_fixture(131072, 0, 0, 16, 1, 100, 24, False),
         "反例自证⑧：fragment 没了 / 长度<4 / 未压缩标志 / block_size 合法 ⇒ B 夹具自证必须判红",
         "pos=%s nofrag=%s tiny=%s uncomp=%s smallbs=%s"
         % (pred_b_fixture(*good_b),
            pred_b_fixture(0xFFFFFFFF, 0xFFFFFFFF, 0, 16, 1, 100, 24, False),
            pred_b_fixture(0xFFFFFFFF, 0, 0, 16, 1, 100, 3, False),
            pred_b_fixture(0xFFFFFFFF, 0, 0, 16, 1, 100, 24, True),
            pred_b_fixture(131072, 0, 0, 16, 1, 100, 24, False)))
    # ⑨ C 夹具自证：字典没被放大 / 变小 ⇒ 必须变红
    want(pred_xz_dict_inflated(c_info["dict_new"], c_info["dict_old"])
         and not pred_xz_dict_inflated(8388608, 8388608)
         and not pred_xz_dict_inflated(1024, 8388608),
         "反例自证⑨：字典声明没被放大（8 MiB→8 MiB）/ 反而变小 ⇒ C 夹具自证必须判红",
         "pos=%s same=%s tiny=%s"
         % (pred_xz_dict_inflated(c_info["dict_new"], c_info["dict_old"]),
            pred_xz_dict_inflated(8388608, 8388608), pred_xz_dict_inflated(1024, 8388608)))
    # ⑨′ **真做的反例**：把重算好的块头 CRC32 改坏 ⇒ 独立进程必须解不出来
    rc_ok, dig_ok, len_ok = c_info["verify"]
    rc_bad, dig_bad, len_bad = c_info["broken_verify"]
    want(pred_tampered_xz_ok(rc_ok, dig_ok, c_info["expect_digest"], len_ok, c_info["expect_len"])
         and not pred_tampered_xz_ok(rc_bad, dig_bad, c_info["expect_digest"], len_bad,
                                     c_info["expect_len"]),
         "反例自证⑨′（真做的）：块头 CRC32 改坏 ⇒ 独立进程必须解不出来（证明 CRC 重算承重）",
         "好流 rc=%s sha=%s len=%s / 坏流 rc=%s sha=%s len=%s（坏流留在 %s）"
         % (rc_ok, dig_ok, len_ok, rc_bad, dig_bad, len_bad, c_info["broken_path"]))
    # ⑩ 正常镜像谓词：封包失败 / 抽取失败 / 字节不等 ⇒ 必须变红
    want(pred_normal_unchanged(0, 0, True)
         and not pred_normal_unchanged(1, 0, True)
         and not pred_normal_unchanged(0, 1, True)
         and not pred_normal_unchanged(0, 0, False),
         "反例自证⑩：封包失败 / 抽取失败 / 字节不等 ⇒ 『正常镜像不变』必须判红",
         "pos=%s pack=%s get=%s bytes=%s"
         % (pred_normal_unchanged(0, 0, True), pred_normal_unchanged(1, 0, True),
            pred_normal_unchanged(0, 1, True), pred_normal_unchanged(0, 0, False)))


def main():
    global CLI, BUDGET_S
    parser = argparse.ArgumentParser(description="SquashFS 读侧三条 DoS（01 F-01/F-02/F-03）的判红锁")
    parser.add_argument("--keep", action="store_true", help="保留临时目录（排查用）")
    parser.add_argument("--cli", default="", help="覆盖 CLI 路径（默认 build/msvc-release 那份）")
    parser.add_argument("--budget", type=float, default=BUDGET_S,
                        help="A/C 的墙钟预算秒数（默认 20；只有『先证红』时才需要调小）")
    parser.add_argument("--mem-cap-mb", type=int, default=DEFAULT_MEM_CAP_MB,
                        help="子进程内存安全线 MiB（默认 2048，只保机器不参与判定；0=不设闸）")
    args = parser.parse_args()
    BUDGET_S = args.budget

    if cfh is None:
        print("!! 缺同目录夹具原语 tests/check_fs_hardening.py：%s" % CFH_ERR)
        print("SQFS-DOS skip 前提缺失（夹具原语）")
        return 2

    CLI = Path(args.cli) if args.cli else DEFAULT_CLI
    if not CLI.exists():
        print("!! 缺构建产物 xrkit_cli.exe：%s（外部前提）" % CLI)
        print("SQFS-DOS skip 缺构建产物")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_sqfs_dos_"))
    work = tmp / "work"
    work.mkdir(parents=True, exist_ok=True)
    c_info = None
    try:
        print("CLI = %s（预算 %.0f s / 子进程内存安全线 %s MiB）"
              % (CLI, BUDGET_S, args.mem_cap_mb))
        check_dir_loop(tmp, work)
        check_big_block(tmp, work)
        _p, c_info = check_xz_bomb(tmp, work)
        check_normal_unchanged(tmp)
        check_reverse_lock(tmp, work)
        selfcheck(c_info)
    except Exception as exc:      # 脚本自身/环境异常也必须"可见地红"，不许只丢个 traceback
        import traceback
        traceback.print_exc()
        PROBLEMS.append("脚本异常：%r" % (exc,))
    finally:
        if args.keep:
            print("（--keep：临时目录保留在 %s）" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("SQFS-DOS fail %d 项：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("SQFS-DOS ok F-01（成环明确报错+零产物）/ F-02（字典炸弹被内存闸拒+正常镜像不变）"
          "/ F-03（block_size 非法明确报错+不 GB 分配）三组判据 + 反向锁 + 自我证明全绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
