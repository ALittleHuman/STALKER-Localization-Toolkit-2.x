#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：.ltx 文本提取的替换**不许**动注释、也不许切碎更长标识符，且跳过必须**可见**。

为什么要有这个脚本 —— 1.x 的替换是 `text.replace(old, new)` 的**裸全局替换**
（`_replace_from_text`），本工程第一版照抄了它。实测（拿 1.x 自己的 `_extract_scripts`
跑同一份夹具）它会把两处**不该动**的地方一起改，而且**全是静默**：

  ① 注释：`; Пистолет тут в комментарии` → `; game.translate_string("…") тут …`
     —— .ltx 的注释是给编辑器/后续维护看的，改写它只破坏语料、零收益。
     （★ 注意口径：1.x **确实**改写注释；本批按"零静默 + 不切碎语料"处理成**跳过**，
       并把它列进台账。判决依据见本批报告。）
  ② 更长标识符的子串：源里同时有 `inv_name = точто` 与 `unknown_key = точто_больше`
     时，`точто` 那次替换会把 key 切成 `game.translate_string("…")_больше = что-то`
     —— **产物已经不是合法 .ltx**（这就是"裸值替换会切碎长标识符"）。

判据（改前红 / 改后绿，逐条断言，缺一条都不算过）：
  · **注释原样**：三行注释里的目标串一个字都不许动；
  · **长标识符原样**：`точто_больше`（key 与注释里的各一处）不许被切碎；
  · **正文照换**：`inv_name` / `descr` 的裸值照旧换成 `game.translate_string("<id>")`，
    且**行结构不变**（每行仍然只是一个 `key = value`）；
  · **台账可见**：`TEXT repl-skip` 逐条打印 + `TEXT ok … replskip=N` 计数，
    N 必须 == "被跳过的最少处数"（零静默：跳过了就一定要报出来）；
  · **不误伤**：没有任何"本文该换却没换"的正文行（用"替换后的行数 == 期望"钉住）。

输出：`✓` / `✗` 行 + 收尾行 `LTX-REPLACE ok …` / `LTX-REPLACE fail …`；
退出码 0=全绿、1=有断言不成立、2=构建产物缺失（外部前提，按档位判定）。

★ 夹具一律现造在 `%TEMP%`，**不碰真机语料、不碰游戏目录**。
"""
import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS = []

# ── 夹具：**行号固定**（台账里断言行号，所以别随意加行/挪行）──────────────────
#   1 [wpn_akm]
#   2 inv_name = точто                  ← 候选、裸值：**必须**被替换
#   3 ; точто тут в КОММЕНТАРИИ         ← 注释：**必须**原样
#   4 unknown_key = точто_больше        ← 更长标识符（key 也是候选形态）：**必须**原样
#   5 ; точто_больше в комментарии      ← 注释里的更长标识符：**必须**原样
#   6 descr = другой                    ← 候选、裸值：**必须**被替换
#   7 'inv_name' = точто                ← 同一裸值第二处：**必须**被替换（且 id 与第 2 行相同）
#   8 inv_name = "Строка"               ← 带引号字面量：**必须**整体替换
#   9 ; inv_name = "Строка" в коммент.  ← 注释里的**带引号**字面量：**必须**原样
#  10 ; только комментарий              ← 行内没目标串：原样
FIXTURE = (
    "[wpn_akm]\n"
    "inv_name = точто\n"
    "; точто тут в КОММЕНТАРИИ\n"
    "unknown_key = точто_больше\n"
    "; точто_больше в комментарии\n"
    "descr = другой\n"
    "'inv_name' = точто\n"
    'inv_name = "Строка"\n'
    '; inv_name = "Строка" в комментарии\n'
    "; только комментарий\n"
)

LINE_BODY_BARE = 2
LINE_COMMENT_TARGET = 3
LINE_LONG_ID_KEY = 4
LINE_COMMENT_LONG_ID = 5
LINE_BODY_BARE_2 = 6
LINE_BODY_BARE_AGAIN = 7
LINE_BODY_QUOTED = 8
LINE_COMMENT_QUOTED = 9
LINE_COMMENT_PLAIN = 10

TRANSLATE_RE = re.compile(r'game\.translate_string\("([^"]+)"\)')


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


def main():
    parser = argparse.ArgumentParser(description=".ltx 替换的注释/词边界判红锁")
    parser.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("LTX-REPLACE: SKIP (cli-missing)")
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="xrkit_ltx_lock_"))
    try:
        src = tmp / "scripts"
        src.mkdir(parents=True, exist_ok=True)
        (src / "probe.ltx").write_bytes(FIXTURE.encode("cp1251"))
        out = tmp / "out"
        out.mkdir(parents=True, exist_ok=True)
        rc, text = run([str(CLI), "--extract-text", str(src), str(out),
                        "--prefix", "ltxlock", "--id-prefix", "sgtat_ltxlock_sc_0"])
        want(rc == 0, "夹具跑得通（--extract-text exit=0）",
             "exit=%d 输出=%r" % (rc, text.strip()[:300]))
        if rc != 0:
            return 1

        got_path = out / "scripts" / "probe.ltx"
        got = got_path.read_text(encoding="utf-8") if got_path.exists() else ""
        lines = got.split("\n")
        src_lines = FIXTURE.split("\n")
        want(got_path.exists() and len(lines) >= len(FIXTURE.split("\n")),
             "产物存在且行数与源一致（源 %d 行 / 产物 %d 行）"
             % (len(FIXTURE.split("\n")), len(lines)))
        print("    --- 产物 ---")
        for i, ln in enumerate(lines, 1):
            print("    %2d| %s" % (i, ln))
        print("    --- CLI 台账 ---")
        for ln in text.splitlines():
            if ln.startswith("TEXT repl-skip") or ln.startswith("TEXT ok"):
                print("      " + ln)

        def line(n):
            return lines[n - 1] if n - 1 < len(lines) else ""

        # ① 注释原样（四行：3 / 5 / 9 / 10）
        want(line(LINE_COMMENT_TARGET) == "; точто тут в КОММЕНТАРИИ",
             "① 注释第 %d 行原样（不含 translate_string）" % LINE_COMMENT_TARGET,
             "实得 %r" % line(LINE_COMMENT_TARGET))
        want(line(LINE_COMMENT_LONG_ID) == "; точто_больше в комментарии",
             "① 注释第 %d 行原样（长标识符没被切）" % LINE_COMMENT_LONG_ID,
             "实得 %r" % line(LINE_COMMENT_LONG_ID))
        want(line(LINE_COMMENT_QUOTED) == '; inv_name = "Строка" в комментарии',
             "① 注释第 %d 行的**带引号**字面量原样" % LINE_COMMENT_QUOTED,
             "实得 %r" % line(LINE_COMMENT_QUOTED))
        want(line(LINE_COMMENT_PLAIN) == "; только комментарий",
             "① 无目标串的注释第 %d 行原样" % LINE_COMMENT_PLAIN,
             "实得 %r" % line(LINE_COMMENT_PLAIN))

        # ② 更长标识符原样（key 那处）
        want(line(LINE_LONG_ID_KEY) == "unknown_key = точто_больше",
             "② 更长标识符第 %d 行原样（裸值不许切进别人的名字里）" % LINE_LONG_ID_KEY,
             "实得 %r" % line(LINE_LONG_ID_KEY))
        want("translate_string" not in line(LINE_LONG_ID_KEY),
             "② 长标识符行里没有 translate_string 插入")

        # ③ 正文照换（裸值 ×2 + 同一裸值的第二处 + 带引号字面量）
        m2 = TRANSLATE_RE.search(line(LINE_BODY_BARE))
        want(m2 is not None and line(LINE_BODY_BARE).startswith("inv_name = "),
             "③ 正文裸值第 %d 行照换（inv_name = game.translate_string(...)）" % LINE_BODY_BARE,
             "实得 %r" % line(LINE_BODY_BARE))
        m6 = TRANSLATE_RE.search(line(LINE_BODY_BARE_2))
        want(m6 is not None, "③ 正文裸值第 %d 行照换（descr）" % LINE_BODY_BARE_2,
             "实得 %r" % line(LINE_BODY_BARE_2))
        m7 = TRANSLATE_RE.search(line(LINE_BODY_BARE_AGAIN))
        want(m7 is not None,
             "③ 同一裸值的第二处第 %d 行照换（--prefix 的 key 不参与判定）"
             % LINE_BODY_BARE_AGAIN,
             "实得 %r" % line(LINE_BODY_BARE_AGAIN))
        m8 = TRANSLATE_RE.search(line(LINE_BODY_QUOTED))
        want(m8 is not None and line(LINE_BODY_QUOTED) == "inv_name = %s" % m8.group(0),
             "③ 带引号字面量第 %d 行**整体**替换（只剩一个 value）" % LINE_BODY_QUOTED,
             "实得 %r" % line(LINE_BODY_QUOTED))
        # 行结构：每行仍然只有一个 '='
        want(all(ln.count("=") <= 1 for ln in lines if ln.strip()),
             "③ 没有一行出现两个 '='（切碎会产生 `... = ... = ...` 这种坏行）",
             "坏行 %r" % [ln for ln in lines if ln.strip() and ln.count("=") > 1][:3])
        # 同一裸值的两处必须拿到**同一个** id（候选是按值去重的）；两个不同值必须不同 id
        want(m2 is not None and m7 is not None and m2.group(1) == m7.group(1),
             "③ 同一裸值的两处拿到同一个 id（%s）"
             % (m2.group(1) if m2 else "?"),
             "实得 %s / %s" % (m2.group(1) if m2 else None, m7.group(1) if m7 else None))
        want(m2 is not None and m6 is not None and m2.group(1) != m6.group(1),
             "③ 两个不同裸值拿到不同 id（%s vs %s）"
             % (m2.group(1) if m2 else "?", m6.group(1) if m6 else "?"))
        want(m8 is not None and (m2 is None or m8.group(1) != m2.group(1)),
             "③ 带引号字面量与裸值是两条不同候选")

        # ④ 台账可见 + 计数 + **行号与理由一致**（行号是"源文件里的那一行"）
        skip_lines = [ln for ln in text.splitlines() if ln.startswith("TEXT repl-skip")]
        want(len(skip_lines) >= 4,
             "④ 跳过台账逐条打印（实得 %d 条；至少 4 条：注释目标串/注释长标识符/"
             "长标识符 key/注释里的带引号字面量）" % len(skip_lines))
        want(any("命中在注释里" in ln for ln in skip_lines),
             "④ 台账里有『命中在注释里』这一条")
        want(any("命中在更长标识符内部" in ln for ln in skip_lines),
             "④ 台账里有『命中在更长标识符内部』这一条")
        # 台账每一行都必须"**在它自己报的那一行上**真的成立"（理由与位置对得上）。
        # 这条抓的是行号算错：修前拿**替换后**的坐标算行号，第 3 行的命中会被报成第 2 行，
        # 而第 2 行是正文（不是注释）⇒ 理由与位置对不上 ⇒ 红。
        def reason_holds(ln_no, why, needle):
            if not (1 <= ln_no <= len(src_lines)):
                return False
            raw = src_lines[ln_no - 1]
            if needle not in raw:
                return False
            if why == "命中在注释里":
                return raw.lstrip().startswith(";")
            if why == "命中在更长标识符内部":
                return not raw.lstrip().startswith(";")
            return False

        led = re.compile(r"：(?P<needle>.+?) → 未替换（(?P<why>.+?)，行 (?P<line>\d+)）")
        rows = []
        for ln in skip_lines:
            m = led.search(ln)
            if m:
                rows.append((m.group("needle"), m.group("why"), int(m.group("line"))))
        bad_rows = [r for r in rows if not reason_holds(r[2], r[1], r[0])]
        want(bool(rows) and not bad_rows,
             "④ 台账每一行的『理由』都在它自己报的**那一行**上成立（%s）"
             % [(r[1], r[2]) for r in rows],
             "对不上的台账 %s" % bad_rows[:3])
        triples = [(w, n) for _, w, n in rows]      # (理由, 行号)
        want(("命中在注释里", LINE_COMMENT_TARGET) in triples,
             "④ 台账里有（行 %d, 命中在注释里）" % LINE_COMMENT_TARGET, "实得 %s" % triples)
        want(("命中在更长标识符内部", LINE_LONG_ID_KEY) in triples,
             "④ 台账里有（行 %d, 命中在更长标识符内部）" % LINE_LONG_ID_KEY, "实得 %s" % triples)
        want(("命中在注释里", LINE_COMMENT_QUOTED) in triples,
             "④ 台账里有（行 %d, 命中在注释里）—— 带引号字面量也受注释保护"
             % LINE_COMMENT_QUOTED, "实得 %s" % triples)
        want(not any(n in (LINE_BODY_BARE, LINE_BODY_BARE_2, LINE_BODY_BARE_AGAIN,
                           LINE_BODY_QUOTED) for _, _, n in rows),
             "④ 正文行（%d/%d/%d/%d）**不在**跳过台账里（它们该被替换）"
             % (LINE_BODY_BARE, LINE_BODY_BARE_2, LINE_BODY_BARE_AGAIN, LINE_BODY_QUOTED),
             "实得 %s" % triples)
        okline = [ln for ln in text.splitlines() if ln.startswith("TEXT ok")]
        want(bool(okline) and "replskip=" in okline[-1],
             "④ ASCII 收尾行带 `replskip=` 计数", "实得 %r" % (okline[-1] if okline else ""))
        if okline and "replskip=" in okline[-1]:
            n = int(okline[-1].split("replskip=")[1].split()[0])
            want(n == len(skip_lines),
                 "④ replskip 计数 == 台账条数（%d vs %d）" % (n, len(skip_lines)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if PROBLEMS:
        print("LTX-REPLACE fail %d 处：%s" % (len(PROBLEMS), "; ".join(PROBLEMS[:5])))
        return 1
    print("LTX-REPLACE ok 注释与长标识符零误伤、正文照换、跳过台账逐条可见")
    return 0


if __name__ == "__main__":
    sys.exit(main())
