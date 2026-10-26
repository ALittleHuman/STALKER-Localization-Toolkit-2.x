# -*- coding: utf-8 -*-
"""**XML 目录对比：mod 覆盖树对齐** ＋ **只在一侧的文件也要报真实计数**（2026-10-26 用户实测）。

两个 bug（都在真实语料上撞到的）
------------------------------------------------
1. **零交集**：B 侧是 mod 覆盖树（`<mod>/configs/...`），A 侧是 `configs/...` ⇒ 按相对路径配对
   得到 **186 行全判「仅一侧」**（实测），整份报告无信息。修法：`--align-mod-dirs`
   （两侧都"剥到已知根 configs/scripts/…"，按根之后的 key 配对，**每个 mod 各出一行**）。
2. **ID 个数恒为 0**：只在一侧的行直接短路，`lines_*/ids_*` 全是 0 ⇒ 报告看起来"这些文件是空的"。
   修法：仍标 `only_one_side`，但把**存在那一侧**的行数/ID 数如实读出来。

判据（不依赖真实语料，全部现造）
--------------------------------
A: `configs/text/rus/a.xml`（3 个 id）
B: `mod1/configs/text/rus/a.xml`（同内容）/ `mod2/configs/text/rus/a.xml`（多一个 id）
   / `mod1/configs/text/rus/only.xml`（只在 B）
① 不对齐 ⇒ 行数 = 3，全部 only_a/only_b（证明 bug 的前提成立）；
② 对齐 ⇒ `mod1/a.xml` 一致、`mod2/a.xml` 不一致、`mod1/only.xml` 仅 B，且 A 的 a.xml **不再是 only_a**；
③ 只在一侧的行 `ids_b > 0`（ID 个数修好了；修前恒为 0）。

退出码：0=绿 / 1=红 / 2=缺 CLI。
"""
from __future__ import annotations

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

TEMPLATE = (
    '<?xml version="1.0" encoding="utf-8" ?>\n<string_table>\n{body}</string_table>\n'
)


def xml(ids: list[str]) -> str:
    body = "".join('  <string id="%s"><text>v</text></string>\n' % i for i in ids)
    return TEMPLATE.format(body=body)


def check(ok: bool, label: str, detail: str = "") -> None:
    print(("  ✓ " if ok else "  ✗ ") + label)
    if not ok:
        if detail:
            print("      " + detail)
        problems.append(label)


def run(args: list[str]) -> tuple[int, str]:
    proc = subprocess.run([str(CLI)] + args, cwd=str(ROOT), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=300)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def rows_of(out: str) -> list[dict]:
    rows = []
    for line in out.splitlines():
        if not line.startswith("XMLDIRS row "):
            continue
        row = {}
        for token in line.split("XMLDIRS row ", 1)[1].split(" "):
            if "=" in token:
                key, value = token.split("=", 1)
                row[key] = value
        rows.append(row)
    return rows


def main() -> int:
    print("== XML 目录对比：mod 覆盖树对齐 + 仅一侧行的真实计数 ==")
    if not CLI.is_file():
        print("XML-MOD-ALIGN skip 缺 build/msvc-release 的 xrkit_cli.exe")
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="xmlalign_", dir=str(ROOT / "build")))
    try:
        a = tmp / "A" / "configs" / "text" / "rus"
        b1 = tmp / "B" / "mod1" / "configs" / "text" / "rus"
        b2 = tmp / "B" / "mod2" / "configs" / "text" / "rus"
        for d in (a, b1, b2):
            d.mkdir(parents=True, exist_ok=True)
        (a / "a.xml").write_text(xml(["i1", "i2", "i3"]), encoding="utf-8")
        (b1 / "a.xml").write_text(xml(["i1", "i2", "i3"]), encoding="utf-8")
        (b2 / "a.xml").write_text(xml(["i1", "i2", "i3", "i4"]), encoding="utf-8")
        (b1 / "only.xml").write_text(xml(["x1"]), encoding="utf-8")

        rc, out = run(["--xml-compare-dirs", str(tmp / "A"), str(tmp / "B")])
        plain = rows_of(out)
        check(rc == 0 and len(plain) == 4,
              "① 不对齐：4 行（A/a.xml、B/mod1/a.xml、B/mod2/a.xml、B/mod1/only.xml 各算一个）",
              "rc=%d 行数=%d" % (rc, len(plain)))
        check(all(r.get("kind") in ("only_a", "only_b") for r in plain),
              "① 不对齐：全部判「仅一侧」（证明修前那个前提真的成立）",
              "；".join(r.get("kind", "?") for r in plain))

        rc2, out2 = run(["--xml-compare-dirs", str(tmp / "A"), str(tmp / "B"), "--align-mod-dirs"])
        align = rows_of(out2)
        by_rel = {r.get("rel", ""): r for r in align}
        check(rc2 == 0 and len(align) == 3, "② 对齐：3 行（mod1/a、mod2/a、mod1/only）",
              "rc=%d 行数=%d rel=%s" % (rc2, len(align), sorted(by_rel)))
        m1 = by_rel.get("mod1/configs/text/rus/a.xml")
        m2 = by_rel.get("mod2/configs/text/rus/a.xml")
        only = by_rel.get("mod1/configs/text/rus/only.xml")
        check(m1 is not None and m1.get("kind") == "same",
              "② `mod1/a.xml` 与 A 的 a.xml 配成一对且**一致**（修前这里只可能是 only_b）",
              str(m1))
        check(m2 is not None and m2.get("kind") == "diff",
              "② `mod2/a.xml` 配成一对且**不一致**（id 多一个）", str(m2))
        check(not any(r.get("kind") == "only_a" for r in align),
              "② A 的 a.xml 不再被判「仅一侧」", "；".join(f"{r.get('rel')}={r.get('kind')}" for r in align))
        check(only is not None and only.get("kind") == "only_b",
              "② 只在 B 的文件仍是「仅一侧」", str(only))
        # ③ ID 个数：只在一侧的行要报真实计数（修前恒为 0）
        check(only is not None and int(only.get("ids_b", "0")) == 1,
              "③ 仅 B 的行：ids_b=1（真实计数，修前是 0）", str(only))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if problems:
        print("XML-MOD-ALIGN fail %d 项不成立：" % len(problems))
        for item in problems:
            print("  · " + item)
        return 1
    print("XML-MOD-ALIGN ok mod 覆盖树能按根对齐配对（每个 mod 一行）＋ 仅一侧行给出真实 ID 数")
    return 0


if __name__ == "__main__":
    sys.exit(main())
