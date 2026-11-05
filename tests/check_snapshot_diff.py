# -*- coding: utf-8 -*-
"""追更 CLI 第 1 步：`--snapshot` / `--diff-snapshot`（文件级 manifest）。

判据（合成样本，真值已知）：
  · 改 1 个文件 + 加 1 个 + 删 1 个 ⇒ added=1 removed=1 modified=1；
  · 内容相同 ⇒ 不进 modified；
  · 两次 snapshot 逐字节一致（无时间戳，确定性）；
  · JSGME 行序变化 ⇒ jsgme_changed=1；
  · **有变化也返回 0**（退出码 2 是全仓的"跳过"语义，不许拿来表示"有变化"）。
"""
from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []


def ok(m):
    print("  \u2713 " + m)


def fail(m):
    print("  \u2717 " + m)
    PROBLEMS.append(m)


def run(*args):
    p = subprocess.run([str(CLI)] + [str(a) for a in args],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def sha_of(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main() -> int:
    print("== 追更 CLI：--snapshot / --diff-snapshot ==")
    if not CLI.is_file():
        print("SNAPSHOT-DIFF skip 缺 CLI")
        return 2
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        old = td / "old" / "gamedata" / "configs"
        old.mkdir(parents=True)
        (old / "a.xml").write_text("<string_table><string id=\"a\"><text>A</text></string></string_table>", encoding="utf-8")
        (old / "same.txt").write_text("same", encoding="utf-8")
        (old / "gone.txt").write_text("gone", encoding="utf-8")
        (td / "old" / "JSGME.ini").write_text("mod_a=1\nmod_b=1\n", encoding="utf-8")
        import shutil
        shutil.copytree(td / "old", td / "new")
        new = td / "new" / "gamedata" / "configs"
        (new / "a.xml").write_text("<string_table><string id=\"a\"><text>A CHANGED</text></string></string_table>", encoding="utf-8")
        (new / "added.txt").write_text("added", encoding="utf-8")
        (new / "gone.txt").unlink()
        (td / "new" / "JSGME.ini").write_text("mod_b=1\nmod_a=1\n", encoding="utf-8")

        rc1, out1 = run("--snapshot", td / "old", td / "old.json")
        rc2, out2 = run("--snapshot", td / "new", td / "new.json")
        (ok if "SNAPSHOT ok" in out1 and "SNAPSHOT ok" in out2 else fail)(
            "两次 snapshot 都成功")
        rc3, out3 = run("--diff-snapshot", td / "old.json", td / "new.json")
        want = "added=1 removed=1 modified=1 jsgme_changed=1 changed=1"
        (ok if want in out3 else fail)("diff 计数（%s）" % want)
        (ok if rc1 == 0 and rc2 == 0 and rc3 == 0 else fail)(
            "三个命令都返回 0（有变化也不返回 2 —— 2 是「跳过」）")

        run("--snapshot", td / "old", td / "old2.json")
        if sha_of(td / "old.json") == sha_of(td / "old2.json"):
            ok("同输入两次 snapshot 逐字节一致（确定性）")
        else:
            fail("两次 snapshot 不一致（有时间戳/顺序漂移？）")

        # 只改缩进/空白 ⇒ 文件 sha 变、文件级必须报 modified（这是 manifest 的职责；
        # 文本级"是否算变更"由 compare_dirs --mode text 判）
        (new / "same.txt").write_text("same  ", encoding="utf-8")
        run("--snapshot", td / "new", td / "new2.json")
        _, out4 = run("--diff-snapshot", td / "old.json", td / "new2.json")
        (ok if "modified=2" in out4 else fail)("空白改动在文件级也算 modified（%s）" % out4.strip().splitlines()[-1])

    # DSH 封测防漏：默认 stats 必须明说"只比 ID 集合，不比文本"
    with tempfile.TemporaryDirectory(prefix="xmlnote_") as td2s:
        td2 = Path(td2s)
        (td2 / "a").mkdir(); (td2 / "b").mkdir()
        (td2 / "a" / "x.xml").write_text('<string_table><string id="a"><text>A</text></string></table>', encoding="utf-8")
        (td2 / "b" / "x.xml").write_text('<string_table><string id="a"><text>B</text></string></table>', encoding="utf-8")
        rc, out = run("--xml-compare-dirs", td2 / "a", td2 / "b")
        (ok if "只比 ID 集合" in out and "ID 文本对比" in out else fail)(
            "stats 模式有防漏提示（只比 ID / 请切文本对比）")

    if PROBLEMS:
        print("SNAPSHOT-DIFF: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("SNAPSHOT-DIFF: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
