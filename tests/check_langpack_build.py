# -*- coding: utf-8 -*-
"""标准语言包装配（`--langpack-build`）判据 —— XML 校对第三模式「提取文本」的落点。

口径（用户 2026-10-26 逐条）：
  ① 文本形式的 XML，**空表也要写**成 `<string_table></string_table>`；
  ② 改写后的 `scripts/`、`configs/gameplay/` 按标准语言包布局摆；
  ③ 每个语言目录一份（`<out>/<lang>/…`）；
  ④ 同名文件已存在 ⇒ **默认拒绝并报告**（`--overwrite` 才覆盖）；
  ⑤ 两个产物至少勾一个（都不勾 ⇒ 退 1 并说理由）。
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

CONTENT = '<?xml version="1.0" encoding="utf-8"?><string_table><string id="x"><text>你好</text></string></string_table>'
EMPTY_SELFCLOSE = '<?xml version="1.0" encoding="utf-8"?><string_table/>'


def fail(msg: str) -> None:
    print("  ✗ " + msg)


def ok(msg: str) -> None:
    print("  ✓ " + msg)


def run(args: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--langpack-build"] + args,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def main() -> int:
    print("== 标准语言包装配（--langpack-build）：布局 / 空表 / 拒绝覆盖 ==")
    if not CLI.exists():
        print("!! 缺 %s" % CLI)
        return 1
    problems = 0
    with tempfile.TemporaryDirectory(prefix="lpbuild_") as tmp:
        tmp = Path(tmp)
        src = tmp / "src" / "rus"
        src.mkdir(parents=True)
        (src / "a.xml").write_text(CONTENT, encoding="utf-8")
        (src / "empty.xml").write_text(EMPTY_SELFCLOSE, encoding="utf-8")
        rew = tmp / "rew"
        (rew / "scripts").mkdir(parents=True)
        (rew / "gameplay").mkdir(parents=True)
        (rew / "scripts" / "a.script").write_text('local x = "你好"', encoding="utf-8")
        (rew / "gameplay" / "x.xml").write_text(CONTENT, encoding="utf-8")
        out = tmp / "out"

        rc, stdout = run([str(tmp / "src"), str(rew), str(out)])
        if rc == 0 and "LANGPACK-BUILD ok" in stdout:
            ok("装配成功：rc=0")
        else:
            problems += 1
            fail("装配失败：rc=%d\n%s" % (rc, stdout[-400:]))

        want = [
            out / "rus" / "configs" / "text" / "rus" / "a.xml",
            out / "rus" / "configs" / "text" / "rus" / "empty.xml",
            out / "rus" / "scripts" / "a.script",
            out / "rus" / "gameplay" / "x.xml",
        ]
        missing = [p for p in want if not p.is_file()]
        if not missing:
            ok("标准语言包布局：configs/text/rus + scripts + gameplay 都在")
        else:
            problems += 1
            fail("产物缺：%s" % ", ".join(str(p.relative_to(out)) for p in missing))

        empty_text = (out / "rus" / "configs" / "text" / "rus" / "empty.xml").read_text(encoding="utf-8")
        if "<string_table></string_table>" in empty_text:
            ok("空表被写成 <string_table></string_table>（不是删掉）")
        else:
            problems += 1
            fail("空表没被归一：%r" % empty_text)

        content_bytes = (out / "rus" / "configs" / "text" / "rus" / "a.xml").read_bytes()
        if content_bytes == CONTENT.encode("utf-8"):
            ok("有内容的 XML 逐字节未改")
        else:
            problems += 1
            fail("有内容的 XML 被改了")

        # ④ 二次运行：默认拒绝覆盖
        rc2, stdout2 = run([str(tmp / "src"), str(rew), str(out)])
        if rc2 == 0 and "skipped_existing=4" in stdout2 and "拒绝覆盖" in stdout2:
            ok("二次运行：已存在的文件被拒绝并报告（skipped_existing=4：2 个文本 + 2 个改写产物）")
        else:
            problems += 1
            fail("二次运行没按拒绝处理：rc=%d\n%s" % (rc2, stdout2[-400:]))

        # ⑤ --overwrite：允许覆盖
        rc3, stdout3 = run([str(tmp / "src"), str(rew), str(out), "--overwrite"])
        if rc3 == 0 and "skipped_existing=0" in stdout3:
            ok("--overwrite：不再拒绝（skipped_existing=0）")
        else:
            problems += 1
            fail("--overwrite 没生效：rc=%d\n%s" % (rc3, stdout3[-300:]))

        # ⑥ 两个产物都不勾 ⇒ 退 1 且说理由
        rc4, stdout4 = run([str(tmp / "src"), str(rew), str(tmp / "out2"),
                            "--no-texts", "--no-rewritten"])
        if rc4 == 1 and "至少勾一个" in stdout4:
            ok("两个产物都不勾：退 1 且说理由")
        else:
            problems += 1
            fail("两个产物都不勾的行为不对：rc=%d\n%s" % (rc4, stdout4[-300:]))

        # ⑦ --extract：从"非空一侧"真抽（scripts/gameplay 两分支）再摆盘
        src2 = tmp / "src_game"
        (src2 / "gameplay").mkdir(parents=True)
        (src2 / "gameplay" / "x.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?><gameplay><text>Привет мир</text></gameplay>',
            encoding="utf-8")
        out2 = tmp / "out_game"
        rc6, stdout6 = run([str(src2), "-", str(out2), "--extract"])
        game_ok = rc6 == 0 and "extracted_files=" in stdout6
        rewritten = out2 / "rus" / "gameplay" / "x.xml"
        texts = list((out2 / "rus" / "configs" / "text" / "rus").glob("*.xml"))
        if game_ok and rewritten.is_file() and "sgtat_" in rewritten.read_text(encoding="utf-8"):
            ok("--extract：gameplay 被改写成 ID 引用（sgtat_…）")
        else:
            problems += 1
            fail("--extract 没改写 gameplay：rc=%d\n%s" % (rc6, stdout6[-400:]))
        if texts and "Привет" in texts[0].read_text(encoding="utf-8"):
            ok("--extract：文本 XML 落在标准布局里且含原文")
        else:
            problems += 1
            fail("--extract 的文本 XML 不对：%s" % [p.name for p in texts])

        # ⑧ 源目录不存在 ⇒ 退 1
        rc5, stdout5 = run([str(tmp / "nope"), "-", str(tmp / "out3")])
        if rc5 == 1 and "不存在" in stdout5:
            ok("源目录不存在：退 1 且说理由")
        else:
            problems += 1
            fail("源目录不存在的行为不对：rc=%d" % rc5)

    if problems:
        print("LANGPACK-BUILD: FAIL（%d 处）" % problems)
        return 1
    print("LANGPACK-BUILD: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
