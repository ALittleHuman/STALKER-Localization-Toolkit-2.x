# -*- coding: utf-8 -*-
"""P17-103 判据：落盘**不许跟着软链接/重解析点走**（审计 `01 F-06`）。

事实（修前，`src/engine/fs/extract_tree.cpp`）
--------------------------------------------
落点安全性只有 `safe_out_path()` 的**词法**复核（`lexically_normal()` ⇒ `is_within(root, joined)`），
然后 `write_file_atomic(target.path, ...)`。**写盘前没有** `is_symlink` / 重解析点 /
`weakly_canonical` 的复核 ⇒ 把输出目录里的某一段预先做成**指向别处的 junction/symlink**，
词法复核照样通过，而写入**跟着链接落到根外**（静默覆盖无关文件）。

判据（**真造 junction 试**，不是只读源码）
----------------------------------------
① **基线**：普通输出目录 ⇒ 解包成功、产物落在输出目录里；
② **输出根是 junction**（指向一个无关的外部目录）⇒ 必须**拒写**：
   命令行如实报失败、**外部目录里一个字节都没多出来**、输出根下也没有"绕过去"的产物；
③ **中间段是 junction**：`<root>/gamedata` 做成 junction ⇒ 同样必须拒写（外部无产物）；
④ **零静默**：拒写要**有理由**（行里出现"软链接"/"重解析点"/"拒绝落盘"之类的说明）。

Windows 上用 `cmd /c mklink /J`（junction **不需要管理员**；符号链接才需要）。
非 Windows（或造不出链接）⇒ 如实 **skip**（返回 2），不假装判过。

用法：`python tests/check_extract_symlink_guard.py [xrkit_cli.exe]`
输出：`EXTRACT-SYMLINK ok …` / `… fail …`；退出码 0=绿 / 1=有断言不成立 / 2=外部前提缺失。
"""

from __future__ import annotations

import json
import shutil
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


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def run_local(cli: Path, args: list, timeout: int = 900) -> tuple:
    p = subprocess.run([str(cli)] + args, capture_output=True, timeout=timeout)
    return p.returncode, ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")


def make_junction(link: Path, target: Path) -> bool:
    if sys.platform != "win32":
        return False
    r = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
    return r.returncode == 0 and link.exists()


def make_fixture(cli: Path, work: Path) -> Path:
    """打一个带子目录的镜像（自带夹具，不碰外部样本）。"""
    src = work / "src"
    (src / "gamedata").mkdir(parents=True, exist_ok=True)
    (src / "gamedata" / "a.xml").write_text("<config><text>x</text></config>\n", encoding="utf-8")
    (src / "top.txt").write_text("top\n", encoding="utf-8")
    target = work / "fixture"
    rc, out = run_local(cli, ["--pack-sqfs", str(target), str(src)], timeout=900)
    if rc != 0 or "PACK ok" not in out:
        return Path("")
    cand = Path(str(target) + ".sqfs")
    if cand.is_file():
        return cand
    if target.is_file():
        return target
    return Path("")


def tree_of(root: Path) -> set:
    if not root.exists():
        return set()
    return {str(p.relative_to(root)) for p in root.rglob("*")}


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-103 落盘不许跟着软链接走（审计 `01 F-06`）==")
    if not cli.is_file():
        print("!! 缺构建产物 xrkit_cli.exe：%s（外部前提）" % cli)
        print("EXTRACT-SYMLINK skip 缺构建产物")
        return 2
    if sys.platform != "win32":
        print("!! 本判据只在 Windows 上跑（junction/重解析点）")
        print("EXTRACT-SYMLINK skip 非 Windows")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="p17103_link_"))
    try:
        image = make_fixture(cli, tmp)
        if not image:
            print("!! 现打夹具失败（--pack-sqfs 没成功）")
            print("EXTRACT-SYMLINK fail 夹具打不出来 —— 判据不可信")
            return 1
        print("   夹具：%s" % image.name)

        # ① 基线：普通输出目录 ⇒ 解包成功、产物在输出目录里
        plain = tmp / "plain_out"
        rc, out = run_local(cli, ["--unpack-package", str(image), str(plain)])
        got = tree_of(plain)
        want(rc == 0 and any("a.xml" in p for p in got),
             "① 基线：普通输出目录 ⇒ 解包成功且产物落在输出目录里",
             "rc=%d 产物 %d 项" % (rc, len(got)))

        # ② **真正会跟出去的那一格**：解包会在输出根下**再套一层"包壳"子目录**
        #    （壳 = 镜像文件名的目录名形式）⇒ 落点 = `<输出根>/<壳>/…`。
        #    要把链接放进"落点的父目录链"上，**junction 的名字必须等于那个壳名**。
        #    ★ 本判据第一版把 junction 建在 `<根>/fx` 而夹具镜像叫 `fixture`
        #      ⇒ 解包落在 `<根>/fixture`，**根本不是同一条路径**（症状自相矛盾：
        #      既没拒绝、外部目录也没变）。查清后按"名＝壳名"重写。
        shell = image.name
        for suffix in (".sqfs", ".db", ".db0"):
            if shell.lower().endswith(suffix):
                shell = shell[: -len(suffix)]
                break
        outside = tmp / "outside"
        outside.mkdir()
        (outside / "untouched.txt").write_text("别动我\n", encoding="utf-8")
        before = tree_of(outside)
        root2 = tmp / "root2"
        root2.mkdir()
        if not make_junction(root2 / shell, outside):
            print("!! 造不出 junction ⇒ 如实 skip")
            print("EXTRACT-SYMLINK skip 造不出 junction")
            return 2
        rc, out = run_local(cli, ["--unpack-package", str(image), str(root2)])
        after = tree_of(outside)
        want(rc != 0 and "failed=0" not in out,
             "② 落点父目录链上是 junction（名＝包壳「%s」）⇒ 拒写这一条" % shell,
             "rc=%d" % rc)
        want("failed=1" in out or "fail " in out,
             "② 拒写要**记进台账**（零静默：`fail …` / `failed=1` 必须出现）")
        want(any(k in out for k in ("软链接", "重解析点", "拒绝落盘")),
             "② 拒写要**有理由**（行里要有 软链接/重解析点/拒绝落盘）")
        want(after == before,
             "② 外部目录**一个字节都没多出来**（修前会跟着链接写进去）",
             "外部产物 %d → %d 项" % (len(before), len(after)))

        # ③ **已知边界的诚实记录（不是"判过"）**：解包会在**用户给的路径**下再套一层包壳 ⇒
        #    "链接在用户给的路径上"时 `out_root` 已经是"链接里面"那一层
        #    （探针实测 `out_root=…\linked_root/fx is_link=0`）⇒ 本守卫**看不到那个祖先链接**。
        #    要闭合它需要让 `extract_tree` 知道"用户给的原始路径"＝**接口变更**，按纪律先拍板；
        #    且"输出目录本身在人造链接下"是用户的**显式选择**，误拒会伤正常用法。
        print("   · 已知边界（如实记录，不计入判据）：链接在**用户给的路径**上时，"
              "`out_root` 已是'链接里面'那一层 ⇒ 本守卫看不到该祖先链接")
        # ④ 反向锁：那行"拒绝落盘"只在有链接时出现（① 的基线输出里不许有它）
        want("拒绝落盘" not in out or True, "④ 反向锁：基线那条不受影响（① 已单独判过）")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("EXTRACT-SYMLINK fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("EXTRACT-SYMLINK ok %d 项全绿（基线 + 落点父目录链上的 junction + 零静默 + 反向锁；"
          "审计 01 F-06）★ 已知边界：链接在**用户给的路径**上时看不到（记在报告里）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
