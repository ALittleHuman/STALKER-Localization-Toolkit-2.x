#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：`only`（"解包选中"）**语义只许一份** + 命中不了必须可见（审计 1-4 / S1-7）。

病根（审计原文 §1 第 4 条 / 判据 §7 的 1-4）：
  · 页面（`fs_page.cpp:1493-1496`）把 `tree_.flatten()` 的 `node->path` 塞进 `only`，而那棵树是
    `preview_entries_in_layout()` 的产物 ⇒ 那是**落点相对路径**（带包名壳）；
  · 引擎比的是 `listed.entries` 的**镜像内路径** ⇒ **两条不同源的路径在比大小**；
  · 更糟的是 DB 分支命中不了时 `continue` 而**不计 failures、不计 written** ⇒
    `files_written=0`、`failures` 空、`ok=true` —— 界面"解包：0 个文件"、CLI 退 0，一个字不说。

判据（每条都断言，缺一条不算过）：
  ① 真解一次：勾选 N 项的落点路径 ⇒ `files_written == N`（修前是 0）；
  ② 镜像内路径写法仍然可用（1.x `--cat` 与既有 CLI 调用点的写法）；
  ③ 对不上的路径 ⇒ **必须说出理由**（`failures` 非空 / CLI 打 `UNPACK-PKG fail`），
     不许"静默 0 产物 + ok"；
  ④ 落点内容与源逐字节一致（不是"写了 N 个空文件"）。

输出：`✓`/`✗` 行 + 收尾行 `UNPACK-ONLY ok …` / `UNPACK-ONLY fail …`；退出码 0/1/2。
夹具一律建在 `%TEMP%`；**不碰真机语料/游戏目录**。
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS = []
CHECKS = 0


def want(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def run(argv):
    p = subprocess.run([str(CLI)] + argv, cwd=str(ROOT), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=300)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def field(out, key):
    """从 `k=v` 串里取值（收尾行的值不带空格）。"""
    for tok in out.replace("\n", " ").split():
        if tok.startswith(key + "="):
            return tok[len(key) + 1:]
    return None


def main() -> int:
    if not CLI.exists():
        print("UNPACK-ONLY fail reason=构建产物缺失（先跑门禁的 build 步骤）：%s" % CLI)
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="xrkit_unpack_only_"))
    try:
        # ── 夹具：**不自带壳**的包（包内路径就是 `a.txt` / `sub/b.txt`）──────────
        src = work / "src"
        (src / "sub").mkdir(parents=True, exist_ok=True)
        (src / "a.txt").write_bytes(b"AAAA\n")
        (src / "sub" / "b.txt").write_bytes(b"BBBBBB\n")
        pack = work / "bare.db0"
        rc, out = run(["--pack-db", str(pack), "xdb", str(src)])
        want(rc == 0 and pack.exists(), "夹具包造出来了（--pack-db xdb）", out.strip()[:200])
        if not pack.exists():
            return 1
        rc, out = run(["--dump", str(pack)])
        want(rc == 0 and "a.txt" in out, "--dump 能列出条目", out.strip()[:200])

        # 落点：`--unpack-package` 把包内路径摊到 <out>/<包名去扩展名>/
        out_root = work / "out_shell"
        out_root.mkdir()
        only = work / "only_shell.txt"
        # 落点路径 = 包名壳 + 镜像内路径（正是页面 `preview_entries_in_layout` 产出的形状）
        shell = "bare"
        only.write_text(shell + "/a.txt\n" + shell + "/sub/b.txt\n", encoding="utf-8")
        rc, out = run(["--unpack-package", str(pack), str(out_root), "--only", str(only)])
        written = field(out, "files")
        want(written == "2",
             "\u2460 勾 2 项落点路径 ⇒ files_written == 2（修前 = 0：静默 0 产物）",
             "实测 files=%s；输出=%s" % (written, out.strip()[:300]))
        got_a = out_root / shell / "a.txt"
        got_b = out_root / shell / "sub" / "b.txt"
        want(got_a.exists() and got_a.read_bytes() == b"AAAA\n",
             "\u2462 落点产物 a.txt 与源逐字节一致", str(got_a))
        want(got_b.exists() and got_b.read_bytes() == b"BBBBBB\n",
             "\u2462 落点产物 sub/b.txt 与源逐字节一致", str(got_b))

        # ── ② 镜像内路径写法（1.x --cat / 既有 CLI 调用点）也要能用 ──────────────
        out_root2 = work / "out_inner"
        out_root2.mkdir()
        only2 = work / "only_inner.txt"
        only2.write_text("a.txt\n", encoding="utf-8")
        rc, out = run(["--unpack-package", str(pack), str(out_root2), "--only", str(only2)])
        want(field(out, "files") == "1",
             "\u2461 镜像内路径写法（a.txt）仍然命中 1 项",
             "实测 files=%s；输出=%s" % (field(out, "files"), out.strip()[:300]))

        # ── ③ 对不上的路径必须**可见**（零静默）────────────────────────────────
        #   ★ 引擎侧硬判据：清单里对不上的**逐条记 failures**（命令见下），
        #     这里走 CLI 的生产路径验"不会看起来成功却一个字不说"。
        out_root3 = work / "out_bad"
        out_root3.mkdir()
        only3 = work / "only_bad.txt"
        only3.write_text("nope/zzz.txt\n" + shell + "/no_such_entry.txt\n", encoding="utf-8")
        rc, out = run(["--unpack-package", str(pack), str(out_root3), "--only", str(only3)])
        want("一条都没命中" in out,
             "\u2462 对不上的清单 ⇒ 打出台账原因（零静默；修前只有 files=0 一行）",
             "输出=%s" % out.strip()[:300])
        want(field(out, "files") == "0",
             "\u2462 对不上的清单 ⇒ 产物数如实为 0（不谎报）",
             "files=%s" % field(out, "files"))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    verdict = "ok" if not PROBLEMS else "fail"
    print("UNPACK-ONLY %s (checks=%d failed=%d)" % (verdict, CHECKS, len(PROBLEMS)))
    return 0 if not PROBLEMS else 1


if __name__ == "__main__":
    sys.exit(main())
