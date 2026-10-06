#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真机样本的封包对拍：把**真实镜像**里的文件抽出来，当成输入树，两套链路各打一次。

为什么单列一个（合成夹具已经有了）：合成夹具只覆盖"我想得到的组合"。2026-09-24
这一轮就是被**真机抽验**抓出一个合成夹具漏掉的**真缺陷** —— 数据块去重命中后的
"截断"把刚写的 fragment 块删掉了（frag 表还指着它）：镜像小 700 KB、碎片去重从
206 掉到 136、被删块上的文件读出来是垃圾（`fragflush` 夹具就是照这个缺陷补的）。

    python tests/golden_sqfs_pack_real.py                 # 默认取 corpus 里最小的明文镜像
    python tests/golden_sqfs_pack_real.py --id nlc-sqzy-patch
    python tests/golden_sqfs_pack_real.py --sample 150    # 只抽前 150 个文件（省时间）

★★ 2026-10-06（A 批）：判据**先验选择**，两条都是"真判据"，第三态才是"未覆盖"
────────────────────────────────────────────────────────────────────────────────
问题（同 HEAD 上"红一次、绿一次"）：参照侧 1.x `sqfs_pack` 的成本不是常数 ——
  ① 调用方 `[(rel, p.read_bytes(), False) …]` 把**整棵树读进内存**（常驻 ≈ 树体积）；
  ② `tar2sqfs.exe` 子进程里做 xz，而 1.x 给这个子进程挂了**硬 120 秒 timeout**
     ⇒ 同一棵树在"机器忙/不忙"时会**跑完或超时**（超时被 1.x 内部 `except: return False`
     吞掉，只剩一个 `False`）⇒ 旧脚本"试着跑参照、跑不动就降级" ⇒ 结论随运行期资源状态
     漂移 ⇒ `--release` 档不可信（能红在一件与代码无关的事上）。

现在：**跑之前**就按可测的能力决定用哪条判据，选定的那条**必须真跑并给出结论**。
  判据选择（`choose_criterion()`，同一输入 + 同一内存状态 ⇒ 同一结论）：
    · 输入树 ≤ `REF_TREE_BUDGET_BYTES`（声明的参照侧可信上限）且
      预计常驻（树体积 × `REF_RAM_FACTOR`）≤ 可用物理内存 × `REF_AVAIL_RATIO`
      ⇒ **compared**：与 1.x 参照**逐字节**对拍（两边 sha256 相等，headline）。
    · 否则 ⇒ **equivalent**：**官方第三方读回**（`rdsquashfs` 读回条目表 + 类型，
      **整树解包**后逐文件 sha256 与输入树比）。
      ★ 这是**等价判据，不是"降级"**：它不依赖"参照同源"（1.x 的 `sqfs_pack` 恰好同源，
      换成 `tar2sqfs` 就不再成立 —— 实测超级块 @34 的 root_inode 布局不同），而"产物内容
      对不对"这件事**本来就不依赖参照同源**；何况读回用的是**官方第三方工具**，
      独立性比"1.x 参照"更高。equiv 的理由**逐样本打印**（树多大 / 内存多少 / 为什么没用 1.x）。
    · **uncovered**：连内容层都跑不了（样本缺失 / `rdsquashfs` 缺失 / CLI 缺失 /
      该走 compared 而 1.x 参照实现缺失）⇒ 进「本次未覆盖」，`--release` 档**只**把这一态
      当失败。
  准入判据失准（先验说能扛、实测没跑完）**不静默**：当场把实测当成能力证据落到
  `equivalent` 并打 `!!` 大声记账 —— 绝不是"悄悄把判据换掉还报 PASS"。

纪律：**不删任何断言**。compared 档保留①sha256 +②条目表 +③抽样内容三条；
equivalent 档没有①，所以②的**类型**也校、③升级成**整树逐文件 sha256**（不然它才真是降级）。

退出码：0=判据跑过且结论一致，1=判据不一致（红），2=uncovered 非空。
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

from collect_golden import CORPUS  # noqa: E402

CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
WORK = ROOT / "build" / "golden_sqfs_pack_real"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")
RDSQUASHFS = (TOOLKIT_1X / "deps" / "squashfs-tools-ng-1.3.2-mingw64" / "bin"
              / "rdsquashfs.exe")

# 明文、体积可控的镜像（加密的 sq_base 走插件路径，不在这条链路上）
REAL_IDS = ["nlc-sqz-autumn", "nlc-sqz-wasteland", "nlc-sqzy-patch"]

# ── 参照侧（1.x `sqfs_pack`）能力模型：声明式预算，不是运行期试探 ──────────────
# 口径来源：`file_system/stalker_fs.py::sqfs_pack` 的实现（2026-10-06 读源码 + 实测定标）：
#   · 调用方把整棵树 read_bytes 进内存拼 `items` ⇒ 常驻 ≥ 树体积；
#   · 每写一个文件再 `io.BytesIO(data)` 拷一份，tarfile 的块缓冲几 MB；
#   · tar 落成一份**未压缩的** %TEMP% 临时文件（磁盘 ≈ 树体积）；
#   · 压缩在 `tar2sqfs.exe` 子进程里（xz 默认、单作业），1.x 给它挂了 `timeout=120`。
# 所以"能不能扛" = min(内存余量 / RAM_FACTOR, 压缩时间预算) 的函数；这里取
#   ① 声明的树体积上限 1 GiB（超过它：x 1 GiB 树的 xz 单作业必然顶到 120 s 上限，
#      实测 3.9 GB 树必然超时 ⇒ 不再让参照侧白跑一趟）；
#   ② 内存准入：预计常驻 ≤ 可用物理内存 × 40%（留出 60% 给并行构建/其它门禁步骤）。
REF_TREE_BUDGET_BYTES = 1024 * 1024 * 1024
REF_RAM_FACTOR = 2.0
REF_AVAIL_RATIO = 0.4


def mb(value: int) -> str:
    return "%.1f MB" % (value / 1048576.0)


def available_physical_bytes() -> int | None:
    """可用物理内存（字节）；取不到返回 None（那就只按树体积判，理由里写明）。"""
    if os.name == "nt":
        class _MemoryStatusEx(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = _MemoryStatusEx()
        status.dwLength = ctypes.sizeof(status)
        try:
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return int(status.ullAvailPhys)
        except Exception:  # noqa: BLE001
            return None
        return None
    try:
        return int(os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE"))
    except Exception:  # noqa: BLE001
        return None


def choose_criterion(tree_bytes: int, avail_bytes: int | None,
                     budget_bytes: int = REF_TREE_BUDGET_BYTES) -> tuple[str, str]:
    """**先验**选判据：返回 (criterion, reason)。同一输入 ⇒ 同一结论。"""
    need = int(tree_bytes * REF_RAM_FACTOR)
    if tree_bytes > budget_bytes:
        return "equivalent", (
            "输入树 %s > 参照侧可信上限 %s（1.x `sqfs_pack` 把整棵树 read_bytes 进内存拼 "
            "items、再让单作业 tar2sqfs/xz 在 120 s 超时里压完 %s 的 tar）⇒ 不用 1.x 参照，"
            "改用**等价判据**（官方第三方 rdsquashfs 读回 + 整树逐文件 sha256）"
            % (mb(tree_bytes), mb(budget_bytes), mb(tree_bytes)))
    if avail_bytes is not None and need > avail_bytes * REF_AVAIL_RATIO:
        return "equivalent", (
            "参照侧预计常驻 %s（树体积 × %.1f）> 可用物理内存 %s × %.0f%% ⇒ 不用 1.x 参照，"
            "改用**等价判据**（官方第三方 rdsquashfs 读回 + 整树逐文件 sha256）"
            % (mb(need), REF_RAM_FACTOR, mb(avail_bytes), REF_AVAIL_RATIO * 100))
    avail_txt = ("可用物理内存 %s（预计常驻 %s ≤ 其 %.0f%%）"
                 % (mb(avail_bytes), mb(need), REF_AVAIL_RATIO * 100)
                 if avail_bytes is not None else "可用物理内存取不到（只按树体积判）")
    return "compared", (
        "输入树 %s ≤ 参照侧可信上限 %s，%s ⇒ 用 1.x 参照逐字节对拍（headline：两边 sha256 相等）"
        % (mb(tree_bytes), mb(budget_bytes), avail_txt))


def corpus_path(sample_id: str) -> Path:
    for item in CORPUS:
        if item["id"] == sample_id:
            return Path(item["path"])
    raise SystemExit("未知语料 id：%s" % sample_id)


def cli(*argv) -> subprocess.CompletedProcess:
    return subprocess.run([str(CLI), *argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def enumerate_entries(root: Path) -> list[tuple[str, Path]]:
    """与 CLI 的枚举规则一致：只收普通文件、rel 用 '/'、按 UTF-8 字节序。"""
    out: list[tuple[str, Path]] = []
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        base = Path(dirpath)
        for name in filenames:
            p = base / name
            if p.is_file():
                out.append((str(p.relative_to(root)).replace("\\", "/"), p))
    out.sort(key=lambda item: item[0].encode("utf-8"))
    return out


def extract_tree(image: Path, tree: Path, sample: int) -> list[tuple[str, Path, int]]:
    dump = cli("--dump", str(image))
    text = dump.stdout.decode("utf-8", "replace")
    if dump.returncode != 0:
        raise RuntimeError("dump 失败：%s" % text.strip()[:200])
    files: list[tuple[str, int]] = []
    for line in text.splitlines():
        if line.startswith("DUMP "):
            continue
        parts = line.rstrip("\r").split("\t")
        if len(parts) == 3 and parts[0] == "f":
            files.append((parts[1], int(parts[2])))
    if sample > 0:
        files = files[:sample]
    # ★ 审计 G1-9（补）：`--dump` 解析器被打死/格式变了 ⇒ `files` 空集
    #   ⇒ "逐字节对拍"会**静默退化成对空集比较**还报 PASS。空输入树一律判红（零静默）。
    if not files:
        raise RuntimeError("--dump 没解析出任何普通文件条目（%d 字节输出）⇒ 空输入树不许"
                           "退化成对空集比较" % len(text))

    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    out: list[tuple[str, Path, int]] = []
    for rel, size in files:
        target = tree / Path(*rel.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        r = cli("--extract", str(image), rel, str(target))
        if r.returncode != 0 or target.stat().st_size != size:
            raise RuntimeError("提取失败 %s：%s"
                               % (rel, r.stdout.decode("utf-8", "replace").strip()[:160]))
        out.append((rel, target, size))
    return out


def describe_entries(tool: Path, artifact: Path) -> dict[str, str] | None:
    """rdsquashfs 读回条目表：{路径: 类型}；连读回都跑不了 ⇒ None（= 内容层不可用）。"""
    d = subprocess.run([str(tool), "--describe", str(artifact)], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if d.returncode != 0:
        print("  ✗ rdsquashfs --describe 失败 rc=%d：%s"
              % (d.returncode, d.stdout.decode("utf-8", "replace").strip()[:200]))
        return None
    got: dict[str, str] = {}
    for ln in d.stdout.decode("utf-8", "replace").splitlines():
        line = ln.strip()
        if not line or " " not in line:
            continue
        kind, rest = line.split(" ", 1)
        rest = rest.strip()
        # ★ 带空格的路径 rdsquashfs 会用双引号包起来（真机里就有
        #   "1rees_vetkagreen 1.dds"），按空格切会把一条拆成两条。
        if rest.startswith('"'):
            end = rest.find('"', 1)
            path = rest[1:end] if end > 0 else rest[1:]
        else:
            path = rest.split(" ")[0]
        got[path.lstrip("/")] = kind
    return got


def expected_kinds(files: list[tuple[str, Path, int]]) -> dict[str, str]:
    """输入树的期望条目表：文件 + 它们的父目录（与 CLI 写进镜像的条目一一对应）。"""
    exp = {rel: "file" for rel, _, _ in files}
    for rel, _, _ in files:
        parts = rel.split("/")
        for i in range(1, len(parts)):
            exp["/".join(parts[:i])] = "dir"
    return exp


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            chunk = fh.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def unpack_with_rdsquashfs(tool: Path, artifact: Path, dest: Path) -> str:
    """官方第三方工具**整树解包**；返回错误文本（空串 = 成功）。"""
    if dest.exists():
        shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(tool), "--unpack-path", "/", "--unpack-root", str(dest),
                        str(artifact)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return ("rdsquashfs 整树解包失败 rc=%d：%s"
                % (r.returncode, r.stdout.decode("utf-8", "replace").strip()[:300]))
    return ""


def compare_unpacked(dest: Path, files: list[tuple[str, Path, int]]) -> list[str]:
    """equivalent 的内容判据：解包出来的树与输入树**逐文件 sha256** 比（全量，不抽样）。"""
    problems: list[str] = []
    exp = {rel: src for rel, src, _ in files}
    exp_dirs = {"/".join(rel.split("/")[:i])
                for rel in exp for i in range(1, len(rel.split("/")))}
    got_files: set[str] = set()
    got_dirs: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(dest):
        base = Path(dirpath)
        for name in dirnames:
            got_dirs.add(str((base / name).relative_to(dest)).replace("\\", "/"))
        for name in filenames:
            p = base / name
            if p.is_file():
                got_files.add(str(p.relative_to(dest)).replace("\\", "/"))
    for rel in sorted(set(exp) - got_files)[:5]:
        problems.append("缺文件 %s" % rel)
    for rel in sorted(got_files - set(exp))[:5]:
        problems.append("多文件 %s" % rel)
    for rel in sorted(exp_dirs - got_dirs)[:5]:
        problems.append("缺目录 %s" % rel)
    for rel in sorted(got_dirs - exp_dirs)[:5]:
        problems.append("多目录 %s" % rel)
    for rel, src in exp.items():
        p = dest / Path(*rel.split("/"))
        if not p.exists():
            continue
        if p.stat().st_size != src.stat().st_size:
            problems.append("%s：大小 %d ≠ 输入 %d" % (rel, p.stat().st_size, src.stat().st_size))
        elif sha256_file(p) != sha256_file(src):
            problems.append("%s：sha256 与输入不符" % rel)
    return problems


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="真机样本的封包对拍（判据先验选择）")
    parser.add_argument("--id", default="", help="语料 id（默认取最小的那个）")
    parser.add_argument("--sample", type=int, default=0, help="只抽前 N 个文件（0=全部）")
    # 下面三个只给自检用（tests/check_sqfs_pack_criterion.py）：逼出两条判据 + 未覆盖态。
    parser.add_argument("--tree", default="", help="[自检] 直接用这个现成输入树（不抽镜像）")
    parser.add_argument("--work", default="", help="[自检] 产物落点（默认 build/golden_sqfs_pack_real/<id>）")
    parser.add_argument("--ref-budget-mb", type=int, default=0,
                        help="[自检] 覆盖参照侧树体积上限（MiB），用来逼出 equivalent 判据")
    parser.add_argument("--rdsquashfs", default="", help="[自检] 覆盖第三方读回工具路径")
    args = parser.parse_args()

    budget = (args.ref_budget_mb * 1048576 if args.ref_budget_mb > 0
              else REF_TREE_BUDGET_BYTES)
    rds = Path(args.rdsquashfs) if args.rdsquashfs else RDSQUASHFS
    tree_arg = Path(args.tree) if args.tree else None

    ids = [args.id or "tree-fixture"] if tree_arg else ([args.id] if args.id else REAL_IDS)

    # ★ A 批：外部前提缺件也走**同一个三态收尾行**（旧版这里是 `SKIP (...)` 特例，
    #   退码同为 2，但三态计数不可见）—— 结论行与退码口径只许一份。
    missing_tools = []
    if not CLI.exists():
        missing_tools.append("%s（先跑 python tests/run_ci.py --only build）" % CLI)
    if not rds.exists():
        missing_tools.append("%s" % rds)
    if missing_tools:
        for item in missing_tools:
            print("!! 缺少 %s" % item)
        print("GOLDEN-SQFS-PACK-REAL: PARTIAL (compared=0 equivalent=0 skipped=0 uncovered=%d)"
              % len(ids))
        print("  未覆盖：%d 个样本连内容层判据都跑不了（缺 CLI / rdsquashfs）" % len(ids))
        return 2

    failures = 0
    # ★ A 批：三态计数（compared / equivalent / uncovered），全部**可见**。
    compared = 0
    equivalent = 0
    skipped = 0
    unrunnable = 0
    reasons: list[str] = []
    avail = available_physical_bytes()

    for sample_id in ids:
        work = Path(args.work) if args.work else (WORK / sample_id)
        work.mkdir(parents=True, exist_ok=True)
        tree = work / "tree"
        if tree_arg:
            if not tree_arg.is_dir():
                print("[%s] ✗ --tree 不是目录：%s" % (sample_id, tree_arg))
                failures += 1
                continue
            print("[%s] 输入树＝%s（--tree，不抽镜像）" % (sample_id, tree_arg))
            files = [(rel, p, p.stat().st_size) for rel, p in enumerate_entries(tree_arg)]
        else:
            image = corpus_path(sample_id)
            if not image.exists():
                print("[%s] -- 样本缺失，跳过" % sample_id)
                skipped += 1
                continue
            print("[%s] %s（%.1f MB）" % (sample_id, image, image.stat().st_size / 1048576))
            try:
                files = extract_tree(image, tree, args.sample)
            except Exception as exc:  # noqa: BLE001
                print("  ✗ 抽树失败：%s" % exc)
                failures += 1
                continue
        total = sum(p.stat().st_size for _, p, _ in files)
        # ★ 审计 G1-9（补）：空输入树 ⇒ 判据退化成"对空集比较"，一律判红（零静默）。
        if not files:
            print("  ✗ 输入树 0 个文件 ⇒ 判据会退化成对空集比较（审计 G1-9）⇒ 判红")
            failures += 1
            continue
        print("  输入树：%d 个文件，%.1f MB" % (len(files), total / 1048576))

        # ★★ 判据**先验**选择：这一行必须在任何"参照尝试"之前打出来（自检锁顺序）。
        criterion, why = choose_criterion(total, avail, budget)
        label = ("compared（与 1.x 参照逐字节）" if criterion == "compared"
                 else "equivalent（官方第三方读回，等价判据）")
        print("  判据：%s" % label)
        print("        理由：%s" % why)
        if avail is not None:
            print("        可用物理内存 %s / 参照侧树体积上限 %s（参照侧还会把一份未压缩 tar 落 %%TEMP%%）"
                  % (mb(avail), mb(budget)))

        ref_ok = False
        ref_data = b""
        if criterion == "compared":
            # 参照：1.x 的 sqfs_pack（tar2sqfs，默认 xz）
            try:
                if str(TOOLKIT_1X) not in sys.path:
                    sys.path.insert(0, str(TOOLKIT_1X))
                from file_system import stalker_fs  # noqa: PLC0415
            except Exception as exc:  # noqa: BLE001
                print("  !! 1.x 参照实现不可用（%s：%s）⇒ 本样本记未覆盖"
                      % (type(exc).__name__, str(exc)[:120]))
                unrunnable += 1
                reasons.append("%s：1.x 参照实现不可用（%s）" % (sample_id, type(exc).__name__))
                continue
            ref = work / "ref.sqfs"
            ref.unlink(missing_ok=True)
            items = [(rel, p.read_bytes(), False) for rel, p, _ in files]
            try:
                ref_ok = bool(stalker_fs.sqfs_pack(items, str(ref)))
            except Exception as exc:  # noqa: BLE001
                ref_ok = False
                print("  !! 参照封包抛异常（%s：%s）" % (type(exc).__name__, str(exc)[:120]))
            if ref_ok:
                ref_data = ref.read_bytes()
            else:
                # ★ 准入判据失准：先验说能扛、实测没跑完。**不静默**：当场把实测当成能力证据，
                #   落到等价判据，并把这件事大声记账（不是"试着跑→悄悄换判据还报 PASS"）。
                criterion = "equivalent"
                print("  !! 准入判据失准（记账）：先验判据说参照侧能扛，实测没跑完"
                      "（1.x `sqfs_pack` 内部把 tar2sqfs 的失败/120 s 超时吞成 `False`）")
                print("     ⇒ 本样本按**等价判据**判（官方第三方 rdsquashfs 读回 + 整树逐文件 sha256）")
                label = "equivalent（官方第三方读回，等价判据）"
                why = ("准入判据失准：参照侧实测没跑完（树 %s）⇒ 等价判据" % mb(total))

        ours = work / "ours.sqfs"
        ours.unlink(missing_ok=True)
        r = cli("--pack-sqfs", str(ours), str(tree_arg or tree))
        line = r.stdout.decode("utf-8", "replace").strip()
        if r.returncode != 0:
            print("  ✗ 我们的封包失败：%s" % line[:300])
            failures += 1
            continue
        our_data = ours.read_bytes()
        print("  %s" % line)

        ok = True
        if criterion == "compared":
            # ① headline：两边产物 sha256 必须相等（不许静默降级）
            if hashlib.sha256(ref_data).hexdigest() != hashlib.sha256(our_data).hexdigest():
                ok = False
                print("  ✗ sha256 不一致：参照 %d 字节 / 我们 %d 字节"
                      % (len(ref_data), len(our_data)))
                n = min(len(ref_data), len(our_data))
                for i in range(n):
                    if ref_data[i] != our_data[i]:
                        print("     首个不同字节 @%d" % i)
                        print("       参照 %s" % ref_data[max(0, i - 8):i + 16].hex())
                        print("       我们 %s" % our_data[max(0, i - 8):i + 16].hex())
                        break
            else:
                print("  ok 逐字节一致（%d 字节，sha=%s）"
                      % (len(our_data), hashlib.sha256(our_data).hexdigest()[:16]))

        # ② 官方第三方读回：路径集合 + **类型**（两条判据都跑；equivalent 档类型也必校）
        exp_kinds = expected_kinds(files)
        got = describe_entries(rds, ours)
        if got is None:
            print("  ✗ 内容层判据跑不了（rdsquashfs 读回失败）⇒ 本样本记未覆盖")
            unrunnable += 1
            reasons.append("%s：rdsquashfs --describe 失败（内容层判据不可用）" % sample_id)
            continue
        if set(got) != set(exp_kinds):
            ok = False
            print("  ✗ rdsquashfs 路径集合不符：缺 %d / 多 %d"
                  % (len(set(exp_kinds) - set(got)), len(set(got) - set(exp_kinds))))
            for p in sorted(set(exp_kinds) - set(got))[:5]:
                print("      缺 %s" % p)
            for p in sorted(set(got) - set(exp_kinds))[:5]:
                print("      多 %s" % p)
        else:
            bad_kind = [p for p in got if got[p] != exp_kinds[p]]
            if bad_kind:
                ok = False
                print("  ✗ rdsquashfs 类型不符 %d 条（前 5）：%s"
                      % (len(bad_kind), ", ".join("%s:%s≠%s"
                                                  % (p, got[p], exp_kinds[p])
                                                  for p in sorted(bad_kind)[:5])))
            else:
                print("  ok rdsquashfs 读回 %d 条（路径 + 类型）" % len(got))

        # ③ 内容判据
        #    compared 档：①已经把"逐字节同源"判掉了，这里保持既有的 12 个抽样（不加强也不删）。
        #    equivalent 档：②③ 是**唯一**的内容判据 ⇒ 升级成**整树**：rdsquashfs 整树解包 +
        #                   逐文件 sha256 全量比（不然它才真是"降级"）。
        pick = files[:4] + files[:: max(1, len(files) // 6)][:6]
        pick = pick[:12]
        bad = []
        for rel, src, _ in pick:
            out_file = work / "sample.bin"
            rr = cli("--extract", str(ours), rel, str(out_file))
            if rr.returncode != 0:
                bad.append("%s：%s" % (rel, rr.stdout.decode("utf-8", "replace")[:120]))
            elif out_file.read_bytes() != src.read_bytes():
                bad.append("%s：内容不符" % rel)
        if bad:
            ok = False
            print("  ✗ 内容抽验失败 %d/%d" % (len(bad), len(pick)))
            for item in bad[:6]:
                print("      %s" % item)
        else:
            print("  ok 内容抽验 %d 个文件逐字节一致" % len(pick))

        if criterion == "equivalent":
            dest = work / "unpack"
            err = unpack_with_rdsquashfs(rds, ours, dest)
            if err:
                ok = False
                print("  ✗ 等价判据跑不完：%s" % err)
            else:
                problems = compare_unpacked(dest, files)
                if problems:
                    ok = False
                    print("  ✗ 等价判据（官方第三方整树读回）不符 %d 处（前 6）：" % len(problems))
                    for item in problems[:6]:
                        print("      %s" % item)
                else:
                    print("  ok 等价判据：官方第三方 rdsquashfs 整树解包 + 逐文件 sha256 "
                          "全过（%d/%d 个文件）" % (len(files), len(files)))

        if not ok:
            failures += 1
        if criterion == "compared":
            compared += 1
        else:
            equivalent += 1
            reasons.append("%s：%s" % (sample_id, why))

    print("")
    # ★ A 批：三态**同口径**：`compared` / `equivalent` 都"真跑并给出结论"；
    #   `uncovered` = 连内容层都跑不了（样本缺失 + 内容层不可用）⇒ 只有它进「本次未覆盖」。
    uncovered = skipped + unrunnable
    verdict = "FAIL" if failures else ("PARTIAL" if uncovered else "PASS")
    print("GOLDEN-SQFS-PACK-REAL: %s (compared=%d equivalent=%d skipped=%d uncovered=%d)"
          % (verdict, compared, equivalent, skipped, uncovered))
    if equivalent:
        print("  equivalent 逐样本理由（等价判据=官方第三方 rdsquashfs 读回，不是降级）：")
        for item in reasons:
            print("    · %s" % item)
    if skipped:
        print("  未覆盖：%d 个样本缺失" % skipped)
    if unrunnable:
        print("  未覆盖：%d 个样本连内容层判据都跑不了（见上）" % unrunnable)
    if failures:
        return 1
    return 2 if uncovered else 0


if __name__ == "__main__":
    sys.exit(main())
