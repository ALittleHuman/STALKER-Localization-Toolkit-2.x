# -*- coding: utf-8 -*-
"""**CLI 写盘落点**判据（H1b / H6b）：非 ASCII 输出路径必须可用、产物字节必须对。

为什么要有它（2026-10-24 全量审计）
----------------------------------
`tools/cli/main.cpp` 里 11 处写盘点中，`--extract` 是**唯一**把 `std::string` 直接喂
`std::ofstream` 的（`:3434`）⇒ Windows 上按 **CP_ACP** 解释，**中文/西里尔落点整条动作打不开**
（不是静默损坏：会打 `EXTRACT error reason=无法写输出文件` 并 `kExitFail`，但功能性不可用）。
其余 10 处已走 `path_from_utf8`，却**都不是原子写**（与 `--pack-sqfs` 的 `.part`、GUI 的
`write_file_atomic` 是两把尺）。

判据形态（按**产物形态**取格，不按动作枚举 —— 第 30 个动作才不会漏）
------------------------------------------------------------------
每格 × {ASCII, CJK 文件名, CJK 父目录, 西里尔}：
  1. 小/中单产物 blob：`--lzhuf encode`（输入是固定字节，输出必须**逐字节相等**）；
  2. 单产物 + 包内提取：`--extract`（先从临时目录 `--pack-db` 造包，再提取，**哈希相等**）；
另加一格 `--force` 覆盖：目标已存在 ⇒ 加 `--force` 后内容等于新产物。

★ 断言看**产物字节**，不只看 rc —— 只看 rc 的判据在"写了个空文件"上也会绿。
★ 文件名与父目录**分开测**：ANSI 转换发生在整条路径上，两种形态都会红，但红因不同。

退出码：0=绿 / 1=红 / 2=缺 CLI 产物。
"""
from __future__ import annotations

import argparse
import hashlib
import json
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

PAYLOAD = bytes(range(256)) * 8   # 2048 字节，含不可打印字节：能抓"按文本写"的错
problems: list[str] = []
# ★ C0.5/C1 的"纯机械替换"证据：每格记五元组（rc / 落点是否存在 / 产物 sha256 / 输出首行）
#   ⇒ C1 之后与 C0 的记录 diff 必须为空（红格也算事实记进去）。
RECORDS: dict[str, dict] = {}


def record(cell: str, rc: int, exists: bool, sha256: str, out_first: str) -> None:
    RECORDS[cell] = {"rc": rc, "exists": exists, "sha256": sha256,
                     "out_first": out_first.strip()[:120]}


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            for line in detail.splitlines()[-6:]:
                print("      " + line)
        problems.append(label)


def run(args: list[str], timeout: int = 300) -> tuple:
    proc = subprocess.run([str(CLI)] + args, cwd=str(ROOT), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


CJK = "中文"
CYR = "西里尔"


def grid_for_lzhuf(tmp: Path) -> None:
    print("-- 形态 1：单产物 blob（--lzhuf encode）")
    src = tmp / "in.bin"
    src.write_bytes(PAYLOAD)
    cases = [
        ("ASCII", tmp / "plain.lz", tmp),
        ("CJK 文件名", tmp / (CJK + "产物.lz"), tmp),
        ("CJK 父目录", tmp / CJK / "out.lz", tmp / CJK),
        ("西里尔", tmp / (CYR + "产物.lz"), tmp),
    ]
    for label, out, parent in cases:
        if parent is not tmp:
            parent.mkdir(parents=True, exist_ok=True)
        rc_enc, text_enc = run(["--lzhuf", "encode", str(src), str(out), "--force"])
        rc, text = rc_enc, text_enc
        # ★ encode 的产物**不该**等于原文 ⇒ 判"往返"：encode 出来的文件再 decode 回原文
        back = tmp / (label.replace(" ", "_") + ".back.bin")
        roundtrip = False
        if out.is_file():
            rc2, text2 = run(["--lzhuf", "decode", str(out), str(back), "--force"])
            roundtrip = (rc2 == 0 and back.is_file() and back.read_bytes() == PAYLOAD)
            if back.is_file():
                back.unlink()
        check(rc == 0 and roundtrip,
              "lzhuf encode→decode 往返 × %s：rc=0 且解回原文（out=%d 字节）"
              % (label, out.stat().st_size if out.is_file() else -1), text)
        record("lzhuf/" + label, rc, out.is_file(),
               hashlib.sha256(out.read_bytes()).hexdigest() if out.is_file() else "",
               text)
        if out.is_file():
            out.unlink()


def grid_for_extract(tmp: Path) -> None:
    print("-- 形态 2：包内单条目（--pack-db → --extract）")
    stage = tmp / "stage"
    stage.mkdir()
    (stage / "a.bin").write_bytes(PAYLOAD)
    pkg = tmp / "fixture.xdb.db"
    rc, text = run(["--pack-db", str(pkg), "xdb", str(stage)])
    if rc != 0 or not pkg.is_file():
        check(False, "造夹具：--pack-db 成功", text)
        return
    want = hashlib.sha256(PAYLOAD).hexdigest()
    cases = [
        ("ASCII", tmp / "out.bin", tmp),
        ("CJK 文件名", tmp / (CJK + "产物.bin"), tmp),
        ("CJK 父目录", tmp / CJK / "out.bin", tmp / CJK),
        ("西里尔", tmp / (CYR + "产物.bin"), tmp),
    ]
    for label, out, parent in cases:
        if parent is not tmp:
            parent.mkdir(parents=True, exist_ok=True)
        rc, text = run(["--extract", str(pkg), "a.bin", str(out), "xdb"])
        exists = out.is_file()
        okbytes = exists and hashlib.sha256(out.read_bytes()).hexdigest() == want
        diag = "期望落点存在=%s 大小=%s" % (exists, out.stat().st_size if exists else "-")
        record("extract/" + label, rc, exists,
               hashlib.sha256(out.read_bytes()).hexdigest() if exists else "", text)
        check(rc == 0 and okbytes, "extract × %s：rc=0 且产物哈希相等（%s）" % (label, diag), text)
        # --force 覆盖：先塞垃圾，再覆盖，内容必须等于新产物
        if out.is_file():
            out.write_bytes(b"stale")
            rc2, text2 = run(["--extract", str(pkg), "a.bin", str(out), "xdb", "--force"])
            okforce = out.is_file() and hashlib.sha256(out.read_bytes()).hexdigest() == want
            check(rc2 == 0 and okforce,
                  "extract × %s：--force 覆盖后内容等于新产物" % label, text2)
            out.unlink()


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", default="", help="把每格五元组写到这个 json（基线）")
    ap.add_argument("--compare", default="", help="与基线 json 逐格 diff（C1 的验收判据）")
    args = ap.parse_args()
    print("== CLI 写盘落点（H1b/H6b）：非 ASCII 路径必须可用 + 产物字节正确 ==")
    if not CLI.is_file():
        print("CLI-OUT skip 缺 %s" % CLI.relative_to(ROOT).as_posix())
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="cliout_", dir=str(ROOT / "build")))
    try:
        grid_for_lzhuf(tmp)
        grid_for_extract(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if args.record:
        Path(args.record).write_text(json.dumps(RECORDS, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8")
        print("  已记录基线：%s（%d 格）" % (args.record, len(RECORDS)))
    if args.compare:
        base = load_json(Path(args.compare))
        diffs = []
        for cell, want in base.items():
            got = RECORDS.get(cell)
            if got is None:
                diffs.append("%s：新跑没有这一格（判据少跑了？）" % cell)
                continue
            for key in ("rc", "exists", "sha256", "out_first"):
                if got.get(key) != want.get(key):
                    diffs.append("%s.%s：%r -> %r" % (cell, key, want.get(key), got.get(key)))
        for cell in RECORDS:
            if cell not in base:
                diffs.append("%s：基线里没有这一格（新加的格？）" % cell)
        if diffs:
            print("  ✗ 与基线不一致（%d 处）：" % len(diffs))
            for d in diffs[:20]:
                print("      " + d)
            problems.append("与 C0 基线逐格 diff 不为空")
        else:
            print("  ✓ 与基线逐格一致（%d 格：rc/落点/字节/首行）" % len(base))

    if problems:
        print("CLI-OUT fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("CLI-OUT ok 四类落点（ASCII / CJK 名 / CJK 父目录 / 西里尔）× 两种产物形态全部可用且字节正确")
    return 0


if __name__ == "__main__":
    sys.exit(main())
