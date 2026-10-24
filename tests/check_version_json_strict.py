# -*- coding: utf-8 -*-
"""**version.json 严格解析**判据（H8：JSON 尾部不许有尾随垃圾）。

修前事实
--------
`src/core/build_version.cpp` 的 `JParser::parse()` 是
`skip_ws(); value(); skip_ws(); return true;` —— **没有 `i_ == s_.size()` 断言**
⇒ `{"current":{…}}garbage` 也解析成功。而这个解析器**同时**是：
  · `--version-store` 的入口（被当**不可信输入**读）；
  · `--build-package` 读回/写回的那一份。
吞掉尾随内容的后果不是崩溃，是**静默按截断后的内容理解**，下次写回就把尾巴覆盖掉（丢数据且无声）。

判据（走**真入口** `--version-store`，不只测函数）
--------------------------------------------------
A 合法档必须成功：正常 `version.json`（含**尾随换行/空白**，那属于合法 JSON 结尾）⇒ rc=0；
B 尾随垃圾必须失败且点名：`{…}garbage` / `{…}{…}` ⇒ rc≠0，错误里有「尾随」与「偏移」；
C **不写法**：失败时该文件**逐字节不变**（这条复用既有的"解析失败不写"保护）；
D 自证：构造器可信 —— 坏文件确实带着垃圾（断言字节里有 garbage）。

退出码：0=绿 / 1=红 / 2=缺 CLI 产物。
"""
from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

problems: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            for line in detail.splitlines()[-6:]:
                print("      " + line)
        problems.append(label)


def run_store(path: Path) -> tuple:
    proc = subprocess.run([str(CLI), "--version-store", str(path)], cwd=str(ROOT),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


VALID = '{\n  "current": { "version": "2.0.0", "prerelease": "BETA", "index": 2 },\n  "tree": {}\n}\n'


def main() -> int:
    print("== version.json 严格解析（H8：尾部尾随内容必须判错）==")
    if not CLI.is_file():
        print("VJSON-STRICT skip 缺 %s" % CLI.relative_to(ROOT).as_posix())
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="vjson_", dir=str(ROOT / "build")))
    try:
        # ── A 合法档（含尾随空白）必须成功 ─────────────────────────────
        good = tmp / "good.json"
        good.write_text(VALID, encoding="utf-8")
        rc, out = run_store(good)
        check(rc == 0 and "VERSIONSTORE ok" in out,
              "A 合法 version.json（尾随换行属合法结尾）能读", out)

        # ── B1 尾随乱码必须失败 ────────────────────────────────────────
        bad1 = tmp / "bad_garbage.json"
        bad1.write_text(VALID + "garbage", encoding="utf-8")
        check(b"garbage" in bad1.read_bytes(), "D1 自证：坏文件确实带 garbage")
        before = sha(bad1)
        rc, out = run_store(bad1)
        check(rc != 0, "B1 尾部 `garbage` ⇒ 非零退出", out)
        check("尾随" in out and "偏移" in out, "B1 错误点名「尾随」与「偏移」", out)
        check(sha(bad1) == before, "C1 解析失败时文件**逐字节不变**")

        # ── B2 两个 JSON 连写也必须失败 ────────────────────────────────
        bad2 = tmp / "bad_two.json"
        bad2.write_text(VALID + '{"current": {}}', encoding="utf-8")
        before2 = sha(bad2)
        rc, out = run_store(bad2)
        check(rc != 0 and "尾随" in out, "B2 两个对象连写 ⇒ 非零 + 点名尾随", out)
        check(sha(bad2) == before2, "C2 同样不写回（逐字节不变）")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("VJSON-STRICT fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("VJSON-STRICT ok 尾随内容被拒（走 --version-store 真入口）、失败不写回、合法档不受影响")
    return 0


if __name__ == "__main__":
    sys.exit(main())
