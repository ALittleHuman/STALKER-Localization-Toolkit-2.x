#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：`--extract-text` 必须**走 7.5 守卫**（不许原地覆盖源文件 / 不许 `--clean` 删源目录），
并且**产物写不出去时不许静默报成功**。

为什么要有这个脚本 —— 守卫（`etxt::validate_request`）与理由文案**早就写好了**，
GUI 页面（`text_page.cpp`）与 `--text-plan`（干跑）都在调它，**唯独真跑那条路 `extract_dir`
入口不调**（`tools/cli/main.cpp` 的 `--extract-text` 直透传）。后果是用户点得出来的：

  · 输出目录选成**源目录的父目录**（＝模组根目录，很自然的选择）⇒ `dst == 源目录`：
      - 不带 `--clean`：改写产物**逐字节覆盖源文件本身**（原文不可恢复，实测 a.xml/b.xml
        sha256 全变、退出码 0、还打印 `TEXT ok files=2`）；
      - 带 `--clean`：先 `remove_all(dst)` ⇒ **整棵源目录消失**，之后 `collect(src)` 为空
        ⇒ `files=0`、`st.ok=true`、**退出码 0**（"目录没了却报成功"）。
  · 附带的零静默洞：产物**写不出去**时只进 `st.skipped`、**不置 `ok=false`** ⇒
    产物一个都没落盘也照样退 0（下面用例 D 就是这一条）。

判据（改前红 / 改后绿，逐条断言，缺一条都不算过）：

  A  `--extract-text <t>\src\gameplay <t>\src`（输出＝源目录父目录）
     ⇒ ① 退出码**非 0**；② `TEXT error reason=` 非空且**写明"会覆盖源目录中的原文件"**
        （不是"源目录不存在"这类别的理由糊过去）；③ 源树**逐文件 sha256 + 目录清单全等**。
  B  同 A 但带 `--clean`
     ⇒ 同上三条 **＋** 源目录**仍然存在**且树形完整（防 `remove_all` 先跑）。
  C  【反向锁】`--extract-text <t>\src\gameplay <t>\src\gameplay`（target == source）
     ⇒ **不许**被判红：1.x 口径下产物落在 `source/gameplay\…` 子目录里，**不覆盖源文件**
        （判据见 `extract.cpp` validate_request 的 7.5 注释）。断言：退出码 0 +
        源文件字节**未变** + 产物确实落在子目录里。
        ★ 没有这条，"修法"很容易写成"target 一律拒"⇒ 与 1.x 行为分叉。
  D  【零静默】输出目录里先把产物要落的**文件位置占成目录** ⇒ 两个产物都写不出去
     ⇒ 退出码**非 0** + 台账 `TEXT skip` 逐条可见 + **不许**打印 `TEXT ok`。

输出：`✓` / `✗` 行 + 收尾行 `EXTRACT-GUARD ok …` / `EXTRACT-GUARD fail …`；
退出码 0=全绿、1=有断言不成立、2=构建产物缺失（外部前提，按档位判定）。

★ 夹具一律现造在 `%TEMP%`，**不碰真机语料、不碰游戏目录**；红的那一趟会**真的**破坏夹具，
  所以每个用例都**现建一份新夹具**（快照在跑之前算好）。
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS = []

# 覆盖理由的**关键子串**（与 `validate_request` 的文案同源；只断子串，不断整句 ——
# 整句断言会让"文案润色"变成红，那不是这条判据要锁的东西）
COVER_REASON = "会覆盖源目录中的原文件"

# ── 夹具：cp1251 的 gameplay XML（带 XML 声明 ⇒ 走"改写后按检测到的编码写回"那条分支）
XML_A = (
    '<?xml version="1.0" encoding="windows-1251"?>\r\n'
    "<string_table>\r\n"
    '\t<string id="a1"><text>Привет мир</text></string>\r\n'
    "</string_table>\r\n"
)
XML_B = (
    '<?xml version="1.0" encoding="windows-1251"?>\r\n'
    "<string_table>\r\n"
    '\t<string id="b1"><text>Второй файл</text></string>\r\n'
    "</string_table>\r\n"
)
# 非目标扩展名：**不该被动**（源树逐字节判据里它是一份"没被碰过"的见证）
NOT_TARGET = "не целевой расширение\r\n"


def want(cond, label, detail=""):
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def run(argv, timeout=300):
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def make_fixture(tag):
    """建一份新夹具：`<tmp>\\src\\gameplay\\{a.xml, c.txt, sub\\b.xml}`。"""
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_extract_guard_%s_" % tag))
    play = tmp / "src" / "gameplay"
    (play / "sub").mkdir(parents=True, exist_ok=True)
    (play / "a.xml").write_bytes(XML_A.encode("cp1251"))
    (play / "sub" / "b.xml").write_bytes(XML_B.encode("cp1251"))
    (play / "c.txt").write_bytes(NOT_TARGET.encode("cp1251"))
    return tmp


def snapshot(root):
    """源树快照：{相对路径: sha256} + 目录清单。"""
    files = {}
    dirs = []
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root).as_posix()
        if p.is_dir():
            dirs.append(rel)
        elif p.is_file():
            files[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return files, dirs


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_guard_rejected(tag, extra_args):
    """A/B：必须**拒绝**，且源树逐字节未变（B 另断源目录仍在）。"""
    tmp = make_fixture(tag)
    try:
        play = tmp / "src" / "gameplay"
        before_files, before_dirs = snapshot(tmp / "src")
        before_a = sha256_of(play / "a.xml")
        rc, text = run([str(CLI), "--extract-text", str(play), str(tmp / "src")] + extra_args)
        after_files, after_dirs = snapshot(tmp / "src")

        want(rc != 0, "%s ① 退出码非 0（拒绝必须体现在退出码上）" % tag,
             "exit=0，输出=%r" % text.strip()[:300])
        want(("TEXT error reason=" in text) and (COVER_REASON in text),
             "%s ② 理由写明『%s』" % (tag, COVER_REASON),
             "实得=%r" % text.strip()[:300])
        want(after_files == before_files and after_dirs == before_dirs,
             "%s ③ 源树逐文件 sha256 + 目录清单全等（逐字节未变）" % tag,
             "变了的文件=%s / 少了的目录=%s"
             % (sorted(k for k in set(before_files) | set(after_files)
                       if before_files.get(k) != after_files.get(k)),
                sorted(set(before_dirs) - set(after_dirs))))
        if extra_args:  # B：`--clean` 绝不许先把源目录删掉
            want((play / "a.xml").is_file() and sha256_of(play / "a.xml") == before_a and
                 (play / "sub" / "b.xml").is_file() and (play / "c.txt").is_file(),
                 "%s ④【--clean 专用】源目录仍然存在且树形完整" % tag,
                 "源目录在=%s" % play.is_dir())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_reverse_lock():
    """C：target == source **不许**被拒（1.x 口径：产物落在子目录里，不覆盖源文件）。"""
    tmp = make_fixture("C")
    try:
        play = tmp / "src" / "gameplay"
        before = {rel: h for rel, h in snapshot(play)[0].items()}
        # 只取源文件那三条（产物会新增，所以只比原文件）
        source_before = {rel: before[rel] for rel in ("a.xml", "c.txt", "sub/b.xml")}
        rc, text = run([str(CLI), "--extract-text", str(play), str(play)])
        want(rc == 0, "C ① 反向锁：target == source **不许**被拒（exit=0）",
             "exit=%d，输出=%r" % (rc, text.strip()[:300]))
        after = {rel: h for rel, h in snapshot(play)[0].items()}
        want(all(after.get(rel) == h for rel, h in source_before.items()),
             "C ② 源文件字节未变（产物落在 source/gameplay\\ 子目录里）",
             "实得=%s" % {rel: (after.get(rel) == h) for rel, h in source_before.items()})
        want((play / "gameplay" / "a.xml").is_file() and
             (play / "gameplay" / "sub" / "b.xml").is_file(),
             "C ③ 产物确实落在 source/gameplay\\ 子目录里",
             "目录清单=%s" % sorted(after))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_write_failure_not_silent():
    """D：产物写不出去 ⇒ 必须非 0 退出，且台账可见（零静默）。"""
    tmp = make_fixture("D")
    try:
        play = tmp / "src" / "gameplay"
        out = tmp / "out"
        # 把**产物要落的文件位置**占成目录 ⇒ `write_bytes` 必然失败（不需要改权限）
        (out / "gameplay" / "sub").mkdir(parents=True, exist_ok=True)
        (out / "gameplay" / "a.xml").mkdir()
        (out / "gameplay" / "sub" / "b.xml").mkdir()
        rc, text = run([str(CLI), "--extract-text", str(play), str(out)])
        want(rc != 0, "D ① 产物写不出去 ⇒ 退出码非 0（不许静默报成功）",
             "exit=0，输出=%r" % text.strip()[:300])
        want(text.count("TEXT skip ") >= 2, "D ② 写失败的台账逐条可见（TEXT skip）",
             "输出=%r" % text.strip()[:300])
        want("TEXT ok" not in text, "D ③ 产物缺失时**不许**打印 TEXT ok",
             "输出=%r" % text.strip()[:300])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description="--extract-text 的 7.5 守卫判红锁")
    parser.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("EXTRACT-GUARD: SKIP (cli-missing)")
        return 2

    print("[extract-guard] 夹具现造在 %%TEMP%%（不碰真机语料）")
    check_guard_rejected("A", [])
    check_guard_rejected("B", ["--clean"])
    check_reverse_lock()
    check_write_failure_not_silent()

    print("-" * 66)
    if PROBLEMS:
        print("EXTRACT-GUARD fail 不成立的断言 %d 条：%s" % (len(PROBLEMS), "；".join(PROBLEMS)))
        return 1
    print("EXTRACT-GUARD ok 覆盖=拒绝原地覆盖+拒绝删源+反向锁+写失败非静默")
    return 0


if __name__ == "__main__":
    sys.exit(main())
