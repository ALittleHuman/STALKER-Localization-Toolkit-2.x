# -*- coding: utf-8 -*-
"""Lua 片段词法判据（2026-11-21，用户口径 B）：看引号在前，认注释在后。

钉死四件事（修前每一条都错）：
  ① 引号里的 `--` 是内容 ⇒ `"a -- b"` 必须被提取（修前整条丢）；
  ② 注释里的引号是普通字符 ⇒ `-- don't` 不能把下一行吞成注释；
  ③ `--[[ ]]` 块注释（含跨行）里的文本**不许**被提取；
  ④ 改写只在引号字面量内部生效 ⇒ 注释里的同名字面量一个字节都不动。

用法：python tests/check_script_lexer.py
退出码：0=全过 / 1=有断言不成立。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = Path(os.environ.get("XRKIT_CLI", ROOT / "build" / "msvc-release" / "tools" / "cli" /
                          "xrkit_cli.exe"))

FAILURES: list[str] = []


def check(cond: bool, label: str, detail: str = "") -> None:
    if cond:
        print("  \u2713 " + label, flush=True)
    else:
        print("  \u2717 " + label + (("  —— " + detail) if detail else ""), flush=True)
        FAILURES.append(label)


def run_case(work: Path, body: str) -> tuple[str, str]:
    """写一份 gamedata（scripts + 一个语言包标记），跑 --extract-pack，返回 (改写脚本, 文本 XML)。"""
    gamedata = work / "gamedata"
    scripts = gamedata / "configs" / "scripts"
    lang = gamedata / "configs" / "text" / "rus"
    scripts.mkdir(parents=True, exist_ok=True)
    lang.mkdir(parents=True, exist_ok=True)
    (scripts / "t.script").write_text(body, encoding="utf-8", newline="\n")
    (lang / "x.xml").write_text(
        '<string_table><string id="k"><text>Привет</text></string></string_table>',
        encoding="utf-8", newline="\n")
    out = work / "out"
    proc = subprocess.run([str(CLI), "--extract-pack", str(gamedata), str(out)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    text = proc.stdout.decode("utf-8", "replace")
    assert "EXTRACTPACK ok" in text, text
    # 没有可提取文本时，工具**不复制/不改写**该文件（设计如此）⇒ 这里返回空串
    rewritten_path = out / "configs" / "scripts" / "t.script"
    rewritten = rewritten_path.read_text(encoding="utf-8") if rewritten_path.is_file() else ""
    xml = ""
    for p in out.rglob("*_texts.xml"):
        xml += p.read_text(encoding="utf-8")
    return rewritten, xml


def main() -> int:
    if not CLI.is_file():
        print("SKIP cli-missing: %s" % CLI)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="lua_lex_", dir=str(ROOT / "build")))
    try:
        print("== ① 引号里的 -- 是内容 ==")
        rewritten, xml = run_case(tmp / "c1", 'local a = "dash -- inside"\n')
        check("dash -- inside" in xml, "① 「dash -- inside」被提取（修前整条丢）", xml[:200])

        print("== ② 注释里的引号不开串 ==")
        rewritten, xml = run_case(tmp / "c2", "-- don't break\nlocal b = \"after comment\"\n")
        check("after comment" in xml, "② 注释里的单引号不吞掉下一行", xml[:200])

        print("== ③ 块注释里的文本不提取（含跨行） ==")
        rewritten, xml = run_case(
            tmp / "c3", 'local a = "keep me"\n--[[ hidden "must not" ]]\n')
        check("keep me" in xml, "③ 块注释外的文本照常提取", xml[:200])
        check("must not" not in xml, "③ 块注释里的文本**没有**被提取", xml[:200])
        rewritten, xml = run_case(
            tmp / "c4", '--[[ line1\n line2 "hidden two" ]]\nlocal c = "keep this"\n')
        check("keep this" in xml and "hidden two" not in xml, "③ 跨行块注释同样成立", xml[:200])

        print("== ④ 改写只动引号字面量 ==")
        rewritten, xml = run_case(
            tmp / "c5",
            'local a = "after comment"\n-- local z = "after comment"\n')
        check('game.translate_string("' in rewritten, "④ 代码里的字面量被改写", rewritten)
        check('-- local z = "after comment"' in rewritten,
              "④ 注释里的同名字面量原样保留（修前会被改）", rewritten)

        print("== ⑥ 注释里的文本长相**不会**被误提取 ==")
        rewritten, xml = run_case(tmp / "c7", '-- "--[[hello big world]]"' + "\n")
        check("hello big world" not in xml, "⑥ 行注释里的文本长得像正文也不提取", xml[:200])
        rewritten, xml = run_case(
            tmp / "c8",
            'local s = "real text here" -- "--[[fake text here]]"' + "\n")
        check("real text here" in xml and "fake text here" not in xml,
              "⑥ 同一行：代码里的提取、注释里的不提取", xml[:200])
        check("fake text here" in rewritten,
              "⑥ 改写也不动注释里的那段（原样保留）", rewritten)

        print("== ⑤ 长字符串 [[ ]] 不参与提取/改写 ==")
        rewritten, xml = run_case(
            tmp / "c6", 'local t = [[ raw "not text" ]]\nlocal b = "real text"\n')
        check("real text" in xml and "not text" not in xml, "⑤ 长字符串内容不提取", xml[:200])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILURES:
        print("SCRIPT-LEXER: FAIL（%d 条）" % len(FAILURES), flush=True)
        return 1
    print("SCRIPT-LEXER: PASS", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
