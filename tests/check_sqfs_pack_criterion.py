#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：真机封包判据的**先验选择**（A 批，2026-10-06）。

背景（为什么要有这条锁）：`tests/golden_sqfs_pack_real.py` 的旧口径是"**试着跑** 1.x 参照、
跑不动就降级成内容层判据"⇒ 结论随运行期资源状态漂移：
  · 1.x `sqfs_pack` 由调用方 `p.read_bytes()` 把**整棵树**读进内存拼 `items`（常驻 ≈ 树体积）；
  · 压缩在 `tar2sqfs.exe` 子进程里（xz 默认、单作业），而 1.x 给它挂了**硬 `timeout=120`**，
    超时被 `except Exception: return False` 吞成一个 `False`；
⇒ 同一棵树（真机 `nlc-sqz-autumn`，723 文件 / 760.8 MB）在机器忙时超时、闲时跑完 ——
`--release` 档能红在一件与代码无关的事上（"同一个 HEAD 红一次绿一次"）。
现在改成**跑之前**按可测能力选判据，两条都是真判据，只有"连内容层都跑不了"才叫未覆盖。

锁什么（每条都断言）：
  ① `choose_criterion()` 的先验结论：树 ≤ 预算且内存够 ⇒ `compared`；树 > 预算 ⇒ `equivalent`
     （理由必须带**体积数字**）；内存不够 ⇒ `equivalent`（理由必须带**内存数字**）；
     取不到内存 ⇒ 只按体积判（仍给 `compared`）。
  ② **端到端**（造一棵**超预算**的树，夹具在 `%TEMP%`）：脚本 rc=0、`equivalent=1 uncovered=0`，
     且**等价判据真跑到了** —— 「官方第三方 rdsquashfs 整树解包 + 逐文件 sha256 全过（N/N）」
     必须在输出里；理由逐样本可见。
  ③ **端到端**（小树）：`compared=1`、1.x 参照真跑且两边产物 sha256 相等。
  ④ **端到端**（`--rdsquashfs` 指向不存在的工具）：`uncovered=1` + rc=2
     ⇒ 第三态有**活的写入者**（不是永远的 0）。
  ⑤ **顺序锁**：`判据：` 行必须出现在**第一次封包输出（`PACK target=`）之前**
     ⇒ "先验选择"不是"跑完再补一句"。
  ⑥ 退码契约：三态与退码一一对应（equivalent ⇒ 0；uncovered ⇒ 2）。
  ⑦ **端到端**：**空输入树**（`--tree` 指向空目录）⇒ rc=1 —— 不许把"逐字节对拍"静默退化成
     "对空集比较"还报 PASS（审计 G1-9 补的那条零静默）。

夹具一律 `%TEMP%`；每次子进程带硬超时（挂死即判据本身）。
输出：`✓`/`✗` 行 + 收尾行 `SQFS-PACK-CRITERION ok/fail`；退出码 0/1/2（2=缺外部前提）。
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
SCRIPT = TESTS / "golden_sqfs_pack_real.py"
TOOLKIT_1X = pathlib.Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")

PROBLEMS = []
CHECKS = 0

sys.path.insert(0, str(TESTS))
import golden_sqfs_pack_real as gspr  # noqa: E402


def want(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def run_script(args, timeout):
    """返回 (rc, out)；超时给 rc=124（不挂死测试进程）。"""
    try:
        p = subprocess.run([sys.executable, str(SCRIPT)] + [str(a) for a in args],
                           cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or b"").decode("utf-8", errors="replace")
        return 124, partial + "\n[超时 %ss]" % timeout


def make_tree(root, sizes):
    """造输入树：稀疏风格（seek 到末尾再写 1 字节），体积可控、写盘快。"""
    root.mkdir(parents=True, exist_ok=True)
    for i, size in enumerate(sizes):
        with open(root / ("f%02d.bin" % i), "wb") as fh:
            fh.seek(size - 1)
            fh.write(b"\0")
    return root


def main() -> int:
    if not SCRIPT.exists():
        print("SQFS-PACK-CRITERION fail reason=判据脚本缺失：%s" % SCRIPT)
        return 2
    if not TOOLKIT_1X.is_dir():
        print("SQFS-PACK-CRITERION: SKIP (1x-ref-missing) —— 没有 1.x 参照就锁不住 compared 那一半：%s"
              % TOOLKIT_1X)
        return 2

    guild = 1024 ** 3
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="xrkit_sqfs_crit_"))
    print("  fixtures: %s" % tmp)
    print("[1] choose_criterion() 的先验结论（同一输入 ⇒ 同一结论）")
    crit, why = gspr.choose_criterion(8 * 1024 * 1024, 16 * guild)
    want(crit == "compared" and "1.x 参照逐字节对拍" in why,
         "小树 + 内存充足 ⇒ compared", "%s / %s" % (crit, why))
    crit, why = gspr.choose_criterion(gspr.REF_TREE_BUDGET_BYTES + 1, 16 * guild)
    want(crit == "equivalent" and "1024.0 MB" in why,
         "树超预算 ⇒ equivalent（理由带体积数字）", "%s / %s" % (crit, why))
    crit, why = gspr.choose_criterion(100 * 1024 * 1024, 64 * 1024 * 1024)
    want(crit == "equivalent" and "可用物理内存" in why,
         "树没过预算但内存不够 ⇒ equivalent（理由带内存数字）", "%s / %s" % (crit, why))
    crit, why = gspr.choose_criterion(100 * 1024 * 1024, None)
    want(crit == "compared" and "取不到" in why,
         "取不到可用内存 ⇒ 只按树体积判（仍 compared）", "%s / %s" % (crit, why))

    big = make_tree(tmp / "big_tree", [64 * 1024 * 1024] * 17)   # 1088.0 MiB > 1 GiB 预算
    small = make_tree(tmp / "small_tree", [1024 * 1024] * 3)

    print("[2] 端到端：超预算树 ⇒ equivalent（官方第三方整树读回）真跑")
    rc, out = run_script(["--tree", big, "--work", tmp / "work_big", "--id", "crit-big"],
                         timeout=1800)
    want(rc == 0 and "equivalent=1" in out and "uncovered=0" in out,
         "rc=0 + equivalent=1 + uncovered=0", "rc=%s\n%s" % (rc, out[-1500:]))
    want("官方第三方 rdsquashfs 整树解包 + 逐文件 sha256 全过（17/17 个文件）" in out,
         "等价判据真跑到（整树逐文件 sha256 全过 17/17）", out[-1500:])
    want("输入树 1088.0 MB > 参照侧可信上限 1024.0 MB" in out,
         "理由逐样本可见（带体积数字）", out[-1500:])
    want("equivalent 逐样本理由" in out and "等价判据" in out,
         "收尾把 equivalent 的逐样本理由摊开打印", out[-800:])
    ipos, ppos = out.find("判据："), out.find("PACK target=")
    want(ipos >= 0 and ppos >= 0 and ipos < ppos,
         "顺序锁：`判据：` 在任何封包输出之前（先验选择，不是跑完再补）",
         "判据@%s / PACK@%s" % (ipos, ppos))

    print("[3] 端到端：小树 ⇒ compared（1.x 参照真跑 + sha256 相等）")
    rc, out = run_script(["--tree", small, "--work", tmp / "work_small", "--id", "crit-small"],
                         timeout=1800)
    want(rc == 0 and "compared=1" in out and "equivalent=0" in out,
         "rc=0 + compared=1 + equivalent=0", "rc=%s\n%s" % (rc, out[-1500:]))
    want("ok 逐字节一致" in out, "headline 判据（两边 sha256 相等）真跑到", out[-1200:])
    want("ok rdsquashfs 读回" in out and "（路径 + 类型）" in out,
         "条目表连**类型**一起校（equivalent 档不许只比路径集）", out[-1200:])

    print("[4] 端到端：内容层工具缺失 ⇒ uncovered（三态第三态有活写入者）")
    rc, out = run_script(["--tree", small, "--work", tmp / "work_u", "--id", "crit-u",
                          "--rdsquashfs", tmp / "no_such_rdsquashfs.exe"], timeout=600)
    want(rc == 2 and "uncovered=1" in out,
         "rc=2 + uncovered=1（不是静默通过、也不是 0）", "rc=%s\n%s" % (rc, out[-800:]))
    want("连内容层判据都跑不了" in out, "未覆盖理由可见", out[-800:])

    print("[5] 端到端：空输入树 ⇒ rc=1（审计 G1-9：不许退化成'对空集比较'）")
    empty = tmp / "empty_tree"
    empty.mkdir(parents=True, exist_ok=True)
    rc, out = run_script(["--tree", empty, "--work", tmp / "work_e", "--id", "crit-empty"],
                         timeout=600)
    want(rc == 1 and "对空集比较" in out,
         "空输入树判红（不是 PASS、不是 0 次比较）", "rc=%s\n%s" % (rc, out[-800:]))

    if PROBLEMS:
        print("SQFS-PACK-CRITERION fail (checks=%d failed=%d)；夹具留在 %s"
              % (CHECKS, len(PROBLEMS), tmp))
        return 1
    print("SQFS-PACK-CRITERION ok (checks=%d failed=0)" % CHECKS)
    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
