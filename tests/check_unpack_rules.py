#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""真机判红锁：解包**落点**（包名壳判定）与**报数口径**（最终产物 vs 包内记录数）。

本批（2026-10-04）修的两条**已查实**缺陷，证据在：
  · 壳规则审计报告（原版反编译行号 + 258 包形态全量表）
  · IWP 逐包对比报告（`dup_evidence.txt` / `independent_lzhuf_verify.tsv`）

① 落点：`paths_carry_shell` 的量词曾是"**全部**条目都带壳" ⇒ 9 个 VESSEL 真机包每个**恰含一条**
   根级 `readme.md`(8148 B)、其余条目全在 `壳/…` 下 ⇒ 被判"不带壳" ⇒ 解出 `ai/ai/alife/…`
   （违背用户口径「这个案例，就解出来一个 configs。」）。现在量词是"**过半**带壳"。
   反向锁（防改过头）：条目顶段与包名壳**无关**的包必须仍然叠壳 ——
   `Golden Sphere\gamedata.db_levels`（壳 `gamedata`、条目全 `levels/…`）与
   `IWP\database\maps\jupiter.db`（壳 `jupiter`、条目全 `levels/…`）。

② 报数：32 个 `IWP\database\maps\*.db` 的**包内条目表把除根记录外的每条记录登记了两遍**
   （同路径/同 offset ⇒ 同一份数据，独立解码已证实重复在包内，不是 dump 的 bug）⇒ 报数曾是
   盘上真相的 2 倍 −1。现在 `--unpack-package` 的 `entries=/files=/dirs=/bytes=` 走**最终产物**
   口径；**`--dump` 保持"忠实报包内记录数"**（协议层，另有用途）——两者分工见 CLI 源码注释。

"只看计划、不落盘"那两条（反向锁）怎么做到：`--only <清单>` 给一个**包内不存在**的条目 ⇒ 每个
  包都被跳过（`done=0`），而 `plan … target=` 行在跳过**之前**就打印了 ⇒ 拿它断言落点，不必去
  解开 1.9 GB 的 `gamedata.db_levels`。

输出（供门禁抓取）：`UNPACK-LOCK ok …` / `UNPACK-LOCK fail …` / `!! 缺真机样本`；
退出码：0=全绿，1=有断言不成立，2=真机样本缺失（外部前提，交门禁按档位判定）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# 真机样本（与 tests/collect_golden.py 同一口径：写死绝对路径，缺件按外部前提处理）
# ★★ 2026-10-24：样本已搬家（`Transition\DB双向验证套件\samples\原件\`）⇒ 统一走
#   `tests/samples.py` 的多根解析；脚本只写相对片段。
sys.path.insert(0, str(Path(__file__).resolve().parent))
import samples  # noqa: E402

GAMES = samples.ROOTS[0]
VESSEL_AI = samples.find("VESSEL", "S.T.A.L.K.E.R. VESSEL", "database", "configs", "ai.db")
IWP_MAPS = samples.find_any(("IWP", "database", "maps"), ("IWP_database", "maps"))
IWP_FAKE_START = IWP_MAPS / "fake_start.db"
IWP_JUPITER = IWP_MAPS / "jupiter.db"
GS_LEVELS = samples.find("Golden Sphere", "gamedata.db_levels")

# 真机上**查实过**的硬数字（改回旧口径立刻对不上）
VESSEL_README_BYTES = 8148
VESSEL_FILES = 25
VESSEL_BYTES = 22308
FAKE_START_ENTRIES = 15       # 产物口径（去重后）
FAKE_START_RECORDS = 29       # 包内记录数（与 --dump 同口径）
FAKE_START_FILES = 13
FAKE_START_DIRS = 2
FAKE_START_BYTES = 2225844
FAKE_START_DUPS = 13


def run_cli(argv):
    proc = subprocess.run([str(CLI)] + [str(a) for a in argv],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def plan_target(text):
    """从 `plan … target=<T> entries=<n> records=<m>` 行里取 (target, entries, records)。"""
    for line in text.splitlines():
        if not line.startswith("UNPACK-PKG plan "):
            continue
        body = line[len("UNPACK-PKG plan "):]
        m = re.match(r"image=(?P<img>.*) target=(?P<target>.*) entries=(?P<e>\d+) records=(?P<r>\d+)$",
                     body)
        if m:
            return m.group("target"), int(m.group("e")), int(m.group("r"))
    return None, None, None


def summary_ok(text):
    """从 `UNPACK-PKG ok …` 行里取 key=value。"""
    for line in text.splitlines():
        if line.startswith("UNPACK-PKG ok "):
            out = {}
            for part in line[len("UNPACK-PKG ok "):].split():
                k, _, v = part.partition("=")
                if k and v.isdigit():
                    out[k] = int(v)
            return out
    return None


def norm_path(p):
    return os.path.normcase(os.path.normpath(str(p).replace("/", os.sep)))


def disk_stats(root):
    files = 0
    nbytes = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            files += 1
            nbytes += os.path.getsize(os.path.join(dirpath, name))
    return files, nbytes


PROBLEMS = []


def want(cond, label, detail=""):
    if cond:
        print("  ✓ " + label)
    else:
        print("  ✗ " + label + (" —— " + detail if detail else ""))
        PROBLEMS.append(label)


def check_vessel(tmp):
    """判红锁②（落点）：VESSEL `configs\\ai.db` ⇒ `<根>/ai/alife` 有、`<根>/ai/ai` **没有**。"""
    out = tmp / "vessel"
    rc, text = run_cli(["--unpack-package", VESSEL_AI, out])
    target, entries, records = plan_target(text)
    ok_line = summary_ok(text)
    want(rc == 0, "VESSEL ai.db 解包退出码 0", "exit=%d\n%s" % (rc, text.strip()[:300]))
    want(norm_path(target) == norm_path(out),
         "落点＝输出根**不叠第二层壳**（target=%s）" % target)
    want((out / "ai" / "alife").is_dir(), "产物有 <根>/ai/alife/")
    want(not (out / "ai" / "ai").exists(), "产物**没有** <根>/ai/ai/（改前的现场）")
    readme = out / "readme.md"
    want(readme.is_file() and readme.stat().st_size == VESSEL_README_BYTES,
         "根级 readme.md 落对了（%s B，期望 %d）" % (readme.stat().st_size if readme.is_file() else "-",
                                                 VESSEL_README_BYTES))
    disk_files, disk_bytes = disk_stats(out)
    want(ok_line is not None and ok_line.get("files") == disk_files == VESSEL_FILES,
         "报数 files==盘上真实（报 %s / 盘 %d / 期望 %d）"
         % ((ok_line or {}).get("files"), disk_files, VESSEL_FILES))
    want(ok_line is not None and ok_line.get("bytes") == disk_bytes == VESSEL_BYTES,
         "报数 bytes==盘上真实（报 %s / 盘 %d / 期望 %d）"
         % ((ok_line or {}).get("bytes"), disk_bytes, VESSEL_BYTES))
    print("    VESSEL ai.db：entries=%s records=%s files=%s bytes=%s（盘上 %d 文件 / %d B）"
          % (entries, records, (ok_line or {}).get("files"), (ok_line or {}).get("bytes"),
             disk_files, disk_bytes))


def check_fake_start(tmp):
    """判红锁③（报数）：`maps\\fake_start.db` 报数 == 盘上真实（13 个文件 / 2,225,844 B）。"""
    out = tmp / "fake_start"
    rc, text = run_cli(["--unpack-package", IWP_FAKE_START, out])
    target, entries, records = plan_target(text)
    ok_line = summary_ok(text)
    disk_files, disk_bytes = disk_stats(out)
    want(rc == 0, "IWP fake_start.db 解包退出码 0", "exit=%d\n%s" % (rc, text.strip()[:300]))
    want(entries == FAKE_START_ENTRIES and records == FAKE_START_RECORDS,
         "计划行两个口径分开报（entries=%s 期望 %d / records=%s 期望 %d）"
         % (entries, FAKE_START_ENTRIES, records, FAKE_START_RECORDS))
    want(norm_path(target) == norm_path(out / "fake_start"),
         "落点＝<根>/fake_start（条目顶段 `levels` 与壳无关 ⇒ 照常叠壳；target=%s）" % target)
    want(ok_line is not None and ok_line.get("files") == disk_files == FAKE_START_FILES,
         "报数 files==盘上真实文件数（报 %s / 盘 %d / 期望 %d）"
         % ((ok_line or {}).get("files"), disk_files, FAKE_START_FILES))
    want(ok_line is not None and ok_line.get("bytes") == disk_bytes == FAKE_START_BYTES,
         "报数 bytes==盘上真实字节（报 %s / 盘 %d / 期望 %d）"
         % ((ok_line or {}).get("bytes"), disk_bytes, FAKE_START_BYTES))
    want(ok_line is not None and ok_line.get("dirs") == FAKE_START_DIRS,
         "报数 dirs 也是产物口径（报 %s / 期望 %d）"
         % ((ok_line or {}).get("dirs"), FAKE_START_DIRS))
    want(ok_line is not None and ok_line.get("dups") == FAKE_START_DUPS,
         "重复记录如实报出（dups=%s / 期望 %d）"
         % ((ok_line or {}).get("dups"), FAKE_START_DUPS))
    # 协议层**不去重**：`--dump` 必须仍然报包内记录数（29），这是分工的另一半
    rc_dump, dump_text = run_cli(["--dump", IWP_FAKE_START])
    dump_line = [ln for ln in dump_text.splitlines() if ln.startswith("DUMP ok")]
    want(rc_dump == 0 and dump_line and " entries=%d" % FAKE_START_RECORDS in dump_line[0],
         "`--dump` 保持**忠实报包内记录数**（协议层不去重）", dump_line[0] if dump_line else "")
    print("    fake_start.db：entries=%s records=%s files=%s dirs=%s bytes=%s dups=%s"
          "（盘上 %d 文件 / %d B）"
          % (entries, records, (ok_line or {}).get("files"), (ok_line or {}).get("dirs"),
             (ok_line or {}).get("bytes"), (ok_line or {}).get("dups"), disk_files, disk_bytes))


def check_reverse(tmp):
    """判红锁①的反向锁：与包名壳无关的条目 ⇒ **必须仍然叠壳**（防"过半"改成"存在"就过头）。"""
    phantom = tmp / "phantom_only.txt"
    phantom.write_text("__no_such_entry__\n", encoding="utf-8")
    out = tmp / "reverse"
    for pkg, shell in ((GS_LEVELS, "gamedata"), (IWP_JUPITER, "jupiter")):
        rc, text = run_cli(["--unpack-package", pkg, out, "--only", phantom])
        target, entries, records = plan_target(text)
        ok_line = summary_ok(text)
        want(rc == 0 and norm_path(target) == norm_path(out / shell),
             "%s ⇒ 仍然叠壳 <根>/%s（target=%s）" % (pkg.name, shell, target))
        # 反向锁只看计划 ⇒ 一个文件都不许落盘（`--only` 只给了包内不存在的条目）
        want(ok_line is not None and ok_line.get("done") == 0 and ok_line.get("files") == 0,
             "%s ⇒ 只看计划、不落盘（done=%s files=%s）"
             % (pkg.name, (ok_line or {}).get("done"), (ok_line or {}).get("files")))
        print("    %s：entries=%s records=%s target=%s" % (pkg.name, entries, records, target))


def main():
    parser = argparse.ArgumentParser(description="真机解包落点/报数判红锁")
    parser.add_argument("--cli", default="", help="覆盖 CLI 路径（默认 build/msvc-release 那份）")
    args = parser.parse_args()

    global CLI
    if args.cli:
        CLI = Path(args.cli)
    if not CLI.exists():
        print("!! 找不到 CLI：%s" % CLI)
        return 2

    required = [VESSEL_AI, IWP_FAKE_START, IWP_JUPITER, GS_LEVELS]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        print("!! 缺真机样本（外部前提，按档位判定）：")
        for m in missing:
            print("         " + m)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_unpack_lock_"))
    try:
        print("[①落点] VESSEL `configs\\ai.db`（裸壳项 + 一条根级 readme.md + 一堆 ai/…）")
        check_vessel(tmp)
        print("[②报数] IWP `maps\\fake_start.db`（包内条目表每条登记两遍）")
        check_fake_start(tmp)
        print("[③反向] 条目顶段与包名壳无关 ⇒ 仍叠壳")
        check_reverse(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("UNPACK-LOCK fail %d 项不成立：" % len(PROBLEMS))
        for p in PROBLEMS:
            print("           " + p)
        return 1
    print("UNPACK-LOCK ok 落点（VESSEL 不叠第二层壳 + 反向两条仍叠壳）与报数"
          "（fake_start 报数==盘上真实、--dump 仍报记录数）全绿")
    return 0


if __name__ == "__main__":
    sys.exit(main())
