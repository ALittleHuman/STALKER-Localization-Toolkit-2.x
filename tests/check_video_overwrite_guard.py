#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""视频输出目标守卫（审计 2026-10-06 补充 F1-2 / F1-3）。

**为什么要独立成步**：`plan_transcode` 照抄 1.x，argv 里**恒有 `-y`** ⇒ 覆盖是静默的。
修前的事实（审计原文，逐条已复核）：

  · `src/engine/video/transcode.cpp` 推 `-y`，而 `video_page.cpp` 的起跑路径
    （`request_transcode` → `start_transcode`）**零存在性检查**（全文件对输出的
    `QFileInfo::exists` 只在取消/析构两处）⇒ **静默覆盖既有 OGM**；
  · 唯一的"防自毁"守卫只作用于**默认名**、且只跟**参考**比 ⇒ 源本身是 `.ogm`
    且参考与它同目录不同名时，默认输出**＝源文件**，叠上 `-y` 就是**截断输入**；
    手打的输出框同理没有任何校验。

判据（每条都能判红）：
  ① 引擎四态：`--video-output-check` 对 4 种输入必须分别给出
     `same-as-source` / `same-as-reference` / `exists` / `none`
     —— 路径按 `core::path_equals` 归一（大小写、正反斜杠、`.`/`..` 段都算同一个）；
  ② 页面接线：`request_transcode()` 的函数体里必须在 `start_transcode(` **之前**
     出现 `check_output_conflict(`（否则守卫根本没接上）；
  ③ 页面口径：`Exists` 必须**问一次**（`confirm_overwrite`），两个 `Same*` 必须**直接拒**；
  ④ 静态判据**自证**：把一段"没有守卫就开跑"的假函数体喂给 ② 用的同一个分析器，
     必须报红（防止"删掉守卫、判据照样绿"）。

退出码：0 = 全绿；1 = 有判据不成立；2 = 外部前提缺失（没构建 CLI）。
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _cpp_source import function_body, strip_comments  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PAGE = ROOT / "src" / "shell" / "pages" / "video_page.cpp"

FAILURES: list[str] = []


def say(line: str) -> None:
    print(line, flush=True)


def brief(text: str, lines: int = 2) -> str:
    """CLI 打错动作时会把整张帮助吐出来 —— 证据只留前几行，别把门禁日志淹掉。"""
    rows = [row for row in text.splitlines() if row.strip()]
    return "\n      ".join(rows[:lines])


def check(ok: bool, label: str, detail: str = "") -> None:
    if ok:
        say("  \u2713 %s" % label)
    else:
        FAILURES.append(label)
        say("  \u2717 %s%s" % (label, ("\n      " + detail) if detail else ""))


# ── ① 引擎四态（真跑 CLI，夹具一律 %TEMP%）──────────────────────────────────────

def cli_conflict(src: Path, ref: str, out: Path) -> tuple[int, str]:
    proc = subprocess.run(
        [str(CLI), "--video-output-check", str(src), ref, str(out)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
    )
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def parse_conflict(text: str) -> str:
    match = re.search(r"VIDEOOUTPUT conflict=(\S+)", text)
    return match.group(1) if match else ""


def run_engine_cases() -> None:
    if not CLI.is_file():
        say("  !! 缺构建产物：%s" % CLI)
        say("VIDEO-OVERWRITE: SKIP（未构建 CLI）")
        sys.exit(2)
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_video_guard_"))
    try:
        src_ogm = tmp / "intro.ogm"          # 源本身就是 .ogm（默认输出＝源的那个现场）
        src_avi = tmp / "intro.avi"
        ref_ogm = tmp / "ref.ogm"
        other = tmp / "other.ogm"
        for path in (src_ogm, src_avi, ref_ogm, other):
            path.write_bytes(b"X")
        missing = tmp / "new.ogm"            # 故意不创建

        cases = [
            # label, 源, 参考（- = 没给）, 输出, 期望
            ("输出＝源（同一路径）", src_ogm, str(ref_ogm), src_ogm, "same-as-source"),
            # ★ 路径归一：`./` 段 + 大小写差异也算"同一个文件"
            ("输出＝源（带 ./ 段 + 大小写差异）", src_ogm, str(ref_ogm),
             tmp / "." / "INTRO.OGM", "same-as-source"),
            ("输出＝参考（大小写不同）", src_avi, str(ref_ogm), tmp / "REF.OGM",
             "same-as-reference"),
            ("输出是别的已存在文件", src_avi, str(ref_ogm), other, "exists"),
            ("输出撞源但参考没给", src_ogm, "-", src_ogm, "same-as-source"),
            ("输出不存在、也不是任何输入", src_avi, str(ref_ogm), missing, "none"),
        ]
        for label, src, ref, out, want in cases:
            rc, text = cli_conflict(Path(src), ref, Path(out))
            got = parse_conflict(text)
            check(rc == 0 and got == want,
                  "引擎：%s ⇒ %s" % (label, want),
                  "rc=%d got=%r\n      %s" % (rc, got, brief(text)))
            if want != "none":
                check("reason=" in text and len(text.split("reason=", 1)[1].strip()) > 10,
                      "引擎：%s ⇒ 必须带一句人能看的理由" % label,
                      brief(text))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ── ②/④ 页面接线（静态，且**自证**）────────────────────────────────────────────

# ── ⑤ F1-1：置空子进程成员只许出现在"放弃对象"的唯一实现里（纯文本分析器，可自证）──

PROCESS_MEMBERS = ("process_", "probe_process_", "encoder_process_", "install_process_")
NULL_OUT = re.compile(r"\b(%s)\s*=\s*nullptr\s*;" % "|".join(PROCESS_MEMBERS))
DISCARD = "VideoPage::discard_process"


def null_out_problems(source: str) -> list[str]:
    """判据：**放弃一个子进程对象**只有一份实现（`VideoPage::discard_process`），
    而它必须"先置空 → `disconnect()` → `deleteLater()`"；**别处**一个 `X = nullptr;` 都不许有。

    为什么不是"每个置空点前 8 行里要有 disconnect"：那样会把
    · 析构里 `stop()`（它已经 disconnect）之后的那几行、
    · `finished` 回调自身（对象已经跑完，本来就是安全的）
    一起误判。把"放弃"这件事**收成一个函数**之后，判据变成结构性的：
      ① 这个函数存在，且函数体里 `slot = nullptr;` 在 `disconnect()` 之前（先置空 ⇒ 回调
         里的守卫立刻生效）；
      ② 全文件其余地方**没有**这四个成员的 `= nullptr;`（所以"取消超时只置空不断开"
         这种写法在结构上不可能再出现）；
      ③ 四个成员**每个都被它调用过至少一次**（否则判据可能挂在空集上 —— 把调用整段删掉
         就永绿）。
    """
    problems: list[str] = []
    start = source.find(DISCARD)
    if start < 0:
        return ["找不到 `%s`（放弃子进程对象的唯一实现）—— 判据无从下手" % DISCARD]
    body = function_body(source, DISCARD)
    if not body:
        return ["`%s` 的函数体切不出来（花括号不配对？）" % DISCARD]
    order_null = body.find("slot = nullptr;")
    order_disconnect = body.find("disconnect()")
    order_delete = body.find("deleteLater()")
    if order_null < 0 or order_disconnect < 0 or order_delete < 0:
        problems.append("`%s` 里必须同时有 `slot = nullptr;` / `.disconnect()` / `.deleteLater()`"
                        "（实测 %d/%d/%d）" % (DISCARD, order_null, order_disconnect, order_delete))
    elif not (order_null < order_disconnect < order_delete):
        problems.append("`%s` 里的顺序必须是 **置空 → disconnect → deleteLater**"
                        "（实测偏移 %d/%d/%d）" % (DISCARD, order_null, order_disconnect,
                                                   order_delete))
    # ② 其余地方（把 helper 的函数体挖掉）一个置空都不许有
    body_at = source.find(body, start)
    rest = source[:body_at] + source[body_at + len(body):]
    for match in NULL_OUT.finditer(rest):
        line = rest[:match.start()].count("\n") + 1
        problems.append("第 %d 行（在 `%s` 之外）直接给 `%s` 置空：这条路上的回调会读"
                        "已置空的**成员**指针（nullptr 解引用 / 删掉新对象）—— 必须走 helper"
                        % (line, DISCARD, match.group(1)))
    # ③ 每个成员都必须真的被 helper 处理过
    for name in PROCESS_MEMBERS:
        if "discard_process(%s)" % name not in source:
            problems.append("`discard_process(%s)` 一次都没被调用（判据挂空：把调用删掉就永绿）"
                            % name)
    return problems


def guard_wiring_problem(body: str) -> str:
    """守卫接线分析器（纯文本 → 问题描述；空串 = 没问题）。

    ★ 同一个分析器在下面被**喂一段坏样本**，所以它不是"文件里字符串出现过没有"
    那种一删就绿的形态 —— 分析器自己会被自证。
    """
    if not body:
        return "找不到函数体"
    call = body.find("start_transcode(")
    guard = body.find("check_output_conflict(")
    if call < 0:
        return "函数体里没有 start_transcode(（接线变了？）"
    if guard < 0:
        return "起跑前**没有**调 engine::video::check_output_conflict(...)（-y 会静默覆盖）"
    if guard > call:
        return "守卫出现在 start_transcode( **之后**（等于没拦）"
    return ""


def run_page_wiring() -> None:
    if not PAGE.is_file():
        say("  !! 缺源文件：%s" % PAGE)
        say("VIDEO-OVERWRITE: FAIL")
        sys.exit(1)
    source = strip_comments(PAGE.read_text(encoding="utf-8"))

    body = function_body(source, "VideoPage::request_transcode")
    problem = guard_wiring_problem(body)
    check(not problem, "页面：request_transcode 在 start_transcode 之前调引擎守卫", problem)

    # 两个 Same* ⇒ 直接拒（不许问、不许跑）；Exists ⇒ 必须问一次
    check("confirm_overwrite" in body,
          "页面：Exists 走 `confirm_overwrite`（唯一的确认点，零静默）")
    check("OutputConflict::Exists" in body and "OutputConflict::None" in body,
          "页面：按引擎的三态分支（Same*/Exists/None）",
          brief(body, 3))

    # ★ 自证：同一分析器吃"删掉守卫"的假函数体必须报红
    bad = ("void VideoPage::request_transcode() { log_to_host(\"开始\"); "
           "QString error; if (!start_transcode(build_request(paths), &error)) {} }")
    check(guard_wiring_problem(bad) != "",
          "自证：没有守卫的假函数体必须被判红（分析器不是恒真式）",
          guard_wiring_problem(bad))

    # ── F1-1：进程对象置空之前**必须先 disconnect** ──────────────────────────
    #   事实（修前）：`cancel_transcode` 的取消超时支走 `deleteLater(); process_ = nullptr;`
    #   却**没有断开**，而 finished 回调读的是**成员** `process_`（此刻 nullptr）⇒
    #     ① 对 nullptr 调 `deleteLater()`；② 取消超时后立刻再点转换 ⇒ 旧对象的回调
    #        把**新**的进程对象删掉。同形还有 `probe_process_` / `encoder_process_`
    #        （`waitForStarted` 失败支）与 python 自动安装的 `install_process_`。
    problems = null_out_problems(source)
    check(not problems, "页面：放弃子进程对象的路径唯一（先置空 → disconnect → deleteLater）",
          "\n      ".join(problems))
    # ★ 自证：置空却不断开的假实现必须被判红
    bad_src = ("void VideoPage::discard_process(QProcess*& slot) {\n"
               "    slot = nullptr;\n    // 忘了 disconnect\n}\n"
               "void VideoPage::cancel_transcode() {\n    process_->deleteLater();\n"
               "    process_ = nullptr;\n}\n"
               "void VideoPage::start_probe() { discard_process(probe_process_); }\n"
               "void VideoPage::f() { discard_process(encoder_process_); "
               "discard_process(install_process_); }\n")
    check(bool(null_out_problems(bad_src)),
          "自证：置空却不断开 / helper 之外直接置空 必须被判红", str(null_out_problems(bad_src)))


def main() -> int:
    say("VIDEO-OVERWRITE 视频输出目标守卫（-y 不许静默覆盖 / 输出不许等于输入）")
    run_engine_cases()
    run_page_wiring()
    if FAILURES:
        say("VIDEO-OVERWRITE: FAIL（%d 条不成立）" % len(FAILURES))
        return 1
    say("VIDEO-OVERWRITE: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
