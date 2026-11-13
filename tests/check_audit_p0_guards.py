# -*- coding: utf-8 -*-
"""审计 P0 回归守卫（2026-11-13）。

这些是源码级防回退锁：行为判据已在 golden_text_extract / --smoke / 手工夹具覆盖；
本 checker 专门锁住五条 P0 修法的关键结构，避免后续重构悄悄退回旧写法。
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKS = []


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="replace")


def need(name: str, cond: bool, detail: str) -> None:
    CHECKS.append((name, cond, detail))


def main() -> int:
    font = read("src/engine/font/package.cpp")
    need("F-061 临时 gamedata", "gamedata.xrkit_part" in font and
         "commit_staged_gamedata" in font, "缺 staged 目录/提交函数")
    need("F-061 失败不删最终目录",
         "remove_tree(out_gd)" in font and
         ".xrkit_part" in font, "失败清理仍可能指向最终 gamedata")

    facade = read("src/engine/plugin/engine_facade_impl.cpp")
    need("F-076 备份状态 ec fail-closed",
         "exists_ec" in facade and "备份状态不可读" in facade,
         "pkg_restore 仍把读失败当不存在")

    update = read("src/shell/host_update.cpp")
    need("F-152 future 只消费一次", "consumed" in update and
         "futures[index].get()" in update, "未见 consumed 标记/单次 get")
    need("F-152 启动异常兜底", "catch (const std::exception" in update,
         "std::async 异常仍可能穿出")

    extract = read("src/engine/text/extract.cpp")
    need("F-040 old_text 取源码切片", "old_text = raw;" in extract,
         "仍可能二次 escape_literal_text")
    need("F-040 转义原样保留", "is_escape" in extract and
         "原样保留" in extract, "解析器未见原样保留注释")
    need("F-227 行过滤器使用原始字面量", "const std::string needle = lm;" in extract,
         "sac_line 仍重转义 needle")

    video = read("src/shell/pages/video_page.cpp")
    need("F-135 取消定时器绑定被取消进程",
         "cancelled_process" in video and "process_ != cancelled_process" in video,
         "定时器仍读成员 process_")
    need("F-135 运行序号", "run_seq_" in video, "仍只靠指针身份")
    need("F-061 合并失败回滚", "roll_back" in font and "backups" in font,
         "逐文件覆盖无回滚")

    patch = read("src/engine/patch/patch.cpp")
    need("F-034 patched/expect 等长", "patched.size() != expect.size()" in patch,
         "缺等长不变量")

    convert = read("plugins/convert_page.py")
    need("F-210 暂停检查取消", "ui.task_cancel_requested(panel)" in convert and
         "while paused[0]" in convert, "暂停循环不查取消位")

    failed = 0
    for name, ok, detail in CHECKS:
        print(("OK   " if ok else "FAIL ") + name + ("" if ok else " :: " + detail))
        failed += 0 if ok else 1
    print("AUDIT-P0-GUARDS: %s (%d/%d)" % ("PASS" if failed == 0 else "FAIL",
                                           len(CHECKS) - failed, len(CHECKS)))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
