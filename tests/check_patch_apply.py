# -*- coding: utf-8 -*-
"""P16-1b 写盘半判据：`--patch-apply` 的产物与**逐字节**要求 + 四条拒绝路径。

判据（每一条都要能判红）
------------------------
① **产物逐字节等于 v4 参照**：产物 sha256 == 金标准里 `v4_correct` 样本的 sha256
   （整个文件的 sha256 相等 ⇒ 逐字节相同），且字节数相等；
② **输入永不被改动**：跑完所有用例后，原件 sha256 仍等于金标准里 `orig` 的 sha256；
③ `refused=exists`：输出已存在且未 `--force` ⇒ 拒绝，理由里要有"已存在"；
④ `--force` 才允许覆盖：同一条命令加 `--force` ⇒ 成功；
⑤ `refused=input`：输出 == 输入 ⇒ 拒绝（**哪怕给了 `--force`**），理由里要有"永不覆盖输入"；
⑥ **缺陷输入必须被守卫拦下**：拿归档 v2 样本（thunk 被覆盖）当输入 ⇒ 拒绝，
   理由里要有「preserve 守卫不通过」且点出 `keep.thunk`，并且**不产出任何文件**（零静默 + 不留半成品）；
⑦ 已打过的 v4 参照当输入 ⇒ 拒绝，理由里要有「已是终态」。

★ 产物写到 `%TEMP%`（**绝不在仓库里写产物**）；`written_bytes` 只打印、不参与判定。
输出：`PATCH-APPLY ok cases=N` / `PATCH-APPLY fail …`；退出码 0=绿 / 1=不成立 / 2=样本缺件。
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "golden_patch" / "identify_golden.json"
SITES = ROOT / "tests" / "golden_patch" / "sites_engine_utf8.txt"
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

MIN_CASES = 7


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_apply(cli: Path, src: Path, out: Path, force: bool = False, timeout=600):
    cmd = [str(cli), "--patch-apply", str(src), str(out), str(SITES)]
    if force:
        cmd.append("--force")
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", timeout=timeout)
    text = r.stdout + r.stderr
    ok = "PATCH-APPLY ok" in text
    refused = ""
    reason = ""
    for line in text.splitlines():
        if line.startswith("PATCH-APPLY fail"):
            for token in line.split():
                if token.startswith("refused="):
                    refused = token.split("=", 1)[1]
            if "reason=" in line:
                reason = line.split("reason=", 1)[1]
        if "PATCH-APPLY ok" in line:
            reason = line.strip()
    return r.returncode, ok, refused, reason, text


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P16-1b 写盘半：产物逐字节 + 四条拒绝路径 ==")
    for path in (GOLDEN, SITES):
        if not path.exists():
            print("PATCH-APPLY fail 仓库自带文件缺失：%s" % path)
            return 1
    if not cli.exists():
        print("PATCH-APPLY fail 仓库自带产物缺失：%s（先构建）" % cli)
        return 1

    golden = json.loads(GOLDEN.read_text(encoding="utf-8"))
    by_name = {s["name"]: s for s in golden["samples"]}
    orig = Path(by_name["orig"]["path"])
    v2 = Path(by_name["v2_defect"]["path"])
    v4 = Path(by_name["v4_correct"]["path"])
    expected_product = by_name["v4_correct"]["sha256"]
    expected_size = by_name["v4_correct"]["size"]
    orig_hash = by_name["orig"]["sha256"]

    if not orig.exists():
        print("PATCH-APPLY skip 原始样本缺件（外部前提）：%s" % orig)
        return 2

    problems = []
    cases = 0
    with tempfile.TemporaryDirectory(prefix="p16b1b_") as tmpdir:
        tmp = Path(tmpdir)
        out = tmp / "out.exe"

        # ① + ② 生成 → 逐字节等于参照
        rc, ok, refused, reason, text = run_apply(cli, orig, out)
        cases += 1
        if not ok or not out.exists():
            problems.append("① 生成失败：rc=%d reason=%s" % (rc, reason[:160]))
        else:
            got = sha256_file(out)
            cases += 1
            if got != expected_product:
                problems.append("① 产物 sha256 不等于 v4 参照：实得 %s" % got)
            cases += 1
            if out.stat().st_size != expected_size:
                problems.append("① 产物字节数不等：实得 %d，期望 %d"
                                % (out.stat().st_size, expected_size))
        cases += 1
        if sha256_file(orig) != orig_hash:
            problems.append("② 输入被改动了！（原件 sha256 变了）")
        print("   %s ① 产物 == v4 参照（sha256 全等）＋ 输入未被改动"
              % ("✓" if not problems else "✗"))

        # ③ 输出已存在（未 --force）
        rc, ok, refused, reason, text = run_apply(cli, orig, out)
        cases += 1
        if ok or refused != "exists" or "已存在" not in reason:
            problems.append("③ 输出已存在时未按预期拒绝：ok=%s refused=%s reason=%s"
                            % (ok, refused, reason[:120]))
        else:
            print("   ✓ ③ 输出已存在 ⇒ refused=exists")

        # ④ --force 才允许覆盖
        rc, ok, refused, reason, text = run_apply(cli, orig, out, force=True)
        cases += 1
        if not ok:
            problems.append("④ --force 覆盖没成功：reason=%s" % reason[:120])
        else:
            print("   ✓ ④ --force 覆盖成功")

        # ⑤ 输出 == 输入（哪怕 --force）
        rc, ok, refused, reason, text = run_apply(cli, orig, orig, force=True)
        cases += 1
        if ok or refused != "input" or "永不覆盖输入" not in reason:
            problems.append("⑤ 输出==输入 未按预期拒绝：ok=%s refused=%s reason=%s"
                            % (ok, refused, reason[:120]))
        else:
            print("   ✓ ⑤ 输出==输入 ⇒ refused=input（永不覆盖输入）")
        cases += 1
        if sha256_file(orig) != orig_hash:
            problems.append("⑤ 跑了「输出==输入」之后输入被改动了！")

        # ⑤-b ★★ P17-95（审计 `01 F-05`）：「同一个文件」必须按**真实文件系统**判，不是按**字面**比。
        #   修前 `patch.cpp` 只做 `absolute(src) == absolute(out)` —— MSVC 的 `path::operator==`
        #   **大小写敏感**，于是"同一路径只换大小写"会被判成两个不同文件 ⇒ **真的写盘**（覆盖掉输入）。
        #   判据：把输入路径的**文件名**换成大写（Windows 同一文件）当输出 ⇒ 必须 `refused=input`，
        #   且**输入字节不变**。★ 非 Windows（大小写敏感）上这个探针无意义 ⇒ 先探一下跳不跳。
        probe_lower = tmp / "p1795_probe"
        probe_lower.write_bytes(b"x")
        case_sensitive = not (tmp / "P1795_PROBE").exists()
        probe_lower.unlink()
        if case_sensitive:
            print("   ⚠ ⑤-b 跳过：本文件系统**大小写敏感**，构造不出「同一文件、两种写法」的探针")
        elif orig.name.upper() == orig.name:
            print("   ⚠ ⑤-b 跳过：样本文件名本身就是全大写，换不了大小写")
        else:
            alt = orig.with_name(orig.name.upper())
            rc, ok, refused, reason, text = run_apply(cli, orig, alt, force=True)
            cases += 1
            if ok or refused != "input" or "永不覆盖输入" not in reason:
                problems.append("⑤-b 只换**大小写**的同一文件未被拒（词法比较漏判）："
                                "ok=%s refused=%s reason=%s" % (ok, refused, reason[:120]))
            elif sha256_file(orig) != orig_hash:
                problems.append("⑤-b 「同一文件换大小写」的探针**真的写盘了** —— 输入被覆盖！")
            else:
                print("   ✓ ⑤-b 同一文件换大小写 ⇒ refused=input（按真实文件系统判）")

        # ⑥ 缺陷输入必须被 preserve 守卫拦下，且不产出文件
        if v2.exists():
            out2 = tmp / "from_v2.exe"
            rc, ok, refused, reason, text = run_apply(cli, v2, out2)
            cases += 1
            if ok or "preserve 守卫不通过" not in reason or "keep.thunk" not in reason:
                problems.append("⑥ 缺陷输入未被 preserve 守卫拦下：ok=%s reason=%s"
                                % (ok, reason[:160]))
            else:
                print("   ✓ ⑥ 缺陷输入 ⇒ preserve 守卫拦下（点名 keep.thunk）")
            cases += 1
            if out2.exists():
                problems.append("⑥ 守卫失败却产出了文件（不许留半成品）")
        else:
            print("   ⚠ ⑥ 缺陷样本缺件，跳过：%s" % v2)

        # ⑦ 已打过的产物当输入 ⇒ "已是终态"
        if v4.exists():
            out3 = tmp / "from_v4.exe"
            rc, ok, refused, reason, text = run_apply(cli, v4, out3)
            cases += 1
            if ok or "已是终态" not in reason:
                problems.append("⑦ 已打过的输入未被拦下：ok=%s reason=%s" % (ok, reason[:160]))
            else:
                print("   ✓ ⑦ 已打过的输入 ⇒ 拦下并点出「已是终态」")
        else:
            print("   ⚠ ⑦ v4 参照缺件，跳过：%s" % v4)

    print()
    if cases < MIN_CASES:
        problems.append("用例数 %d < 下限 %d（空集/少跑不算通过）" % (cases, MIN_CASES))
    if problems:
        for p in problems:
            print("   ✗ %s" % p)
        print()
        print("PATCH-APPLY fail %d 项不成立" % len(problems))
        return 1
    print("PATCH-APPLY ok cases=%d（产物逐字节等于 v4 参照；四条拒绝路径 + 输入不可变都成立）" % cases)
    return 0


if __name__ == "__main__":
    sys.exit(main())
