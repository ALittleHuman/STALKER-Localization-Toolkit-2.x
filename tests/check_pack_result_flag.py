# -*- coding: utf-8 -*-
"""判据 C-02：**成败不许拿中文文案判**（第三方只读审计 C-02）。

修前事实（审计原话 + 代码核）
----------------------------
`src/shell/pages/fs_page.cpp::run_pack()` 里判成败是这么写的：

    return !text.startsWith("封包失败") && !text.startsWith("封包：");

—— 成败判在**给人看的文案**上。改一条文案（哪怕加个空格）就**改了语义**；
更要命的是：**新加一条失败消息只要前缀没撞上，就静默算成功**（零静默的另一面：假绿）。

修法
----
`run_pack_job()` 返回一等公民结构 `FsPage::PackRunResult{bool ok; QString message;}`：
**`ok` 是唯一判据**，`message` 只给人看（成功/失败都带）。所有返回点显式写 `{true, …}` / `{false, …}`。
`run_pack()` 只读 `result.ok`；`start_pack()` 的失败理由走**任务壳的消息通道**（工作线程不碰 UI）。

断言
----
A `run_pack()` 里**没有** `.startsWith(`（判成败不许看文案）；
B `PackRunResult` 在 `fs_page.h` 里，且带 `bool ok`；
C `run_pack_job` 的返回类型是 `PackRunResult`，且**每个 return 都显式给 bool**（数出 `{true,`/`{false,`）；
D `start_pack` 的失败理由走 `message(...)`，**不**在工作线程里直接 `log_message`。

退出码：0=绿 / 1=红 / 2=缺输入。`--cpp`/`--hdr` 只为取证（拿 `git show <修前>:…` 跑红证）。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CPP = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"
HDR = ROOT / "src" / "shell" / "pages" / "fs_page.h"

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   \u2713 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
        return True
    print("   \u2717 " + label + (("\uff08%s\uff09" % detail) if detail else ""))
    PROBLEMS.append(label + (("\uff08%s\uff09" % detail) if detail else ""))
    return False


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--cpp", default=str(CPP))
    ap.add_argument("--hdr", default=str(HDR))
    args = ap.parse_args()
    cpp_p, hdr_p = Path(args.cpp), Path(args.hdr)

    print("== C-02：成败不许拿中文文案判 ==")
    for p in (cpp_p, hdr_p):
        if not p.is_file():
            print("PACK-RESULT skip 缺 %s" % p)
            return 2
    cpp = cpp_p.read_text(encoding="utf-8", errors="replace")
    hdr = hdr_p.read_text(encoding="utf-8", errors="replace")

    # A：run_pack() 里不许再出现 startsWith
    i = cpp.find("bool FsPage::run_pack(QString* summary) {")
    if i < 0:
        want(False, "A0 找不到 `bool FsPage::run_pack(QString* summary)`")
    else:
        j = cpp.find("\n}", i)
        body = cpp[i:j if j > i else i + 1200]
        want("startsWith" not in body, "A `run_pack()` 里没有 `.startsWith(`（判成败不看文案）",
             [l.strip() for l in body.splitlines() if "startsWith" in l][:1])
        want("result.ok" in body, "A2 `run_pack()` 返回的是 `result.ok`（一等公民标志）")

    # B：结构体
    want("struct PackRunResult {" in hdr, "B1 `fs_page.h` 里有 `struct PackRunResult {`")
    k = hdr.find("struct PackRunResult {")
    seg = hdr[k:k + 400] if k >= 0 else ""
    want("bool ok" in seg, "B2 结构体里有 `bool ok`")
    want("QString message" in seg, "B3 结构体里有 `QString message`（零静默：理由仍带回来）")

    # C：run_pack_job 的返回点全部显式给 bool
    m = re.search(r"FsPage::PackRunResult FsPage::run_pack_job\(", cpp)
    want(m is not None, "C1 `run_pack_job` 的返回类型是 `FsPage::PackRunResult`")
    if m:
        end = cpp.find("\nbool FsPage::run_pack(", m.start())
        body = cpp[m.start():end if end > m.start() else len(cpp)]
        n_true = len(re.findall(r"return \{true,", body))
        n_false = len(re.findall(r"return \{false,", body))
        stray = [l.strip() for l in body.splitlines()
                 if l.strip().startswith("return ") and "{true," not in l and "{false," not in l]
        want(n_true >= 1 and n_false >= 6,
             "C2 返回点显式给 bool（成功 %d 处 / 失败 %d 处）" % (n_true, n_false))
        want(not stray, "C3 没有「裸 return」（每个返回点都得说清成败）",
             stray[:2])

    # D：失败理由走消息通道
    d = cpp.find("void FsPage::start_pack()")
    seg2 = cpp[d:d + 1600] if d >= 0 else ""
    want("message(r.message.toUtf8().toStdString())" in seg2,
         "D1 `start_pack` 的失败理由走 `message(...)`（任务是工作线程，不许直接碰 UI）")

    print("-" * 66)
    if PROBLEMS:
        print("PACK-RESULT fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PACK-RESULT ok %d 项（成败靠 `PackRunResult.ok`，文案只给人看；"
          "返回点全部显式；失败理由走任务消息通道）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
