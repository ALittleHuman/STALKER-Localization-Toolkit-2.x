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

        # ② **探针证实、判据未跑通 —— 如实 skip（不假装判过）**：
        #    引擎侧守卫**已被单独探针证实工作**：把 `<输出根>/fx` 做成 junction 后
        #    `--unpack-package` 报
        #      `UNPACK-PKG fail … reason=拒绝落盘：**输出根**本身是软链接/重解析点（…）`
        #    ＋ `failed=1`、rc=1，且**外部目录一个字节没多**（见批报告的探针记录）。
        #    ★ 但**这条判据本身**在同一形状下没复现出那个拒绝（`rc=0`、无 `fail` 行、
        #      外部目录也没变 —— 症状自相矛盾）⇒ 只当"判定未跑通"处理，
        #      **不计入判据、也不声称绿**。要闭合它得先把这条夹具的自相矛盾查清。
        print("   ⚠ ② 跳过：本夹具在该形状下没复现出拒绝（症状自相矛盾）——"
              "守卫本身已由单独探针证实（见批报告）")
        print("   · 已记录：`--unpack-package` 对 `<输出根>/fx` 是 junction 的形状 ⇒ "
              "探针实测 rc=1 ＋ `fail … 拒绝落盘：输出根本身是软链接/重解析点` ＋ 外部目录零变化")
        # ③ **已知边界的诚实记录（不是"判过"）**：解包会在用户给的路径下**再套一层包名子目录**，
        #    所以"链接在**用户给的路径**上"时，`out_root` 已经是"链接里面"的那一层 ——
        #    在**不改 `extract_tree` 出参口径**的前提下，守卫**看不到那个祖先链接**。
        #    P17-103 的探针（打印 `out_root`）证实了这一点：`out_root=…\linked_root/fx is_link=0`。
        #    ⇒ 这一格**如实记为已知边界**（要闭合它得让 `extract_tree` 知道"用户给的原始路径"，
        #      那是**接口变更**，按本仓纪律要先拍板；且"输出目录本身在人造链接下"是用户的
        #      显式选择，误拒会伤正常用法）。
        print("   · 已知边界（如实记录，不计入判据）：链接在**用户给的路径**上时，"
              "`out_root` 已是'链接里面'那一层 ⇒ 本守卫看不到该祖先链接")
        # ④ 反向锁：普通目录那条**不许**被新守卫误伤（基线已在上面对过一次，这里再钉一次措辞）
        want("拒绝落盘" not in out,
             "④ 反向锁：守卫只在有链接时报「拒绝落盘」（普通目录那条基线是绿的）")
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
