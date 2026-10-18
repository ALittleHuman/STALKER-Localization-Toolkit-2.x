# -*- coding: utf-8 -*-
"""判据 H-04：打包规格的**正文**必须与实现同源（第三方只读审计 H-04）。

修前事实（审计原话）
--------------------
`docs/2.0.0-打包与发版规格.md` 的正文 §2/§4 仍写「把 `qoffscreen.dll` **临时**铺进舞台…**跑完
删掉那个插件**再压包」，而实现早已是「**只出目录、不压 zip**」「`platforms\\` 同时带
`qwindows.dll` 与 `qoffscreen.dll`，**随包常驻**」（用户 2026-10-07 裁决 + 2026-09-29 产物形态）。
靠文件头的 banner 打补丁 —— 读者读正文会读到与实现相反的做法。

口径
----
* **banner（`<!-- … -->`）算"历史更正记录"，不算正文**：本判据先把注释块剥掉再查，
  否则文件头那段"修前是 X、现在以本节为准"的更正自己就会被判成违规；
* 正文必须写清三件事实，且都不是从横幅抄来的：① qoffscreen **随包常驻**；② **只出目录、不压 zip**；
  ③ 落点形状 `dist\\<主版本>\\<标记>\\<序号>\\`（**只留序号**）；
* **同源锁**：这三件事实在 `scripts/package.ps1` 里必须有对应物（`platform_plugins=…`、
  没有 `Compress-Archive`、`$pkgName` 只取序号），文档说的事实现场可查；
* 正文**不许**再出现旧做法的断言句（四条，见 `STALE`）。

退出码：0=绿 / 1=红 / 2=缺输入。`--spec` 只为取证存在（拿 `git show <修前>:<文件>` 的旧版跑红证）。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / "docs" / "2.0.0-打包与发版规格.md"
PS1 = ROOT / "scripts" / "package.ps1"

# 旧做法的**断言句**（正文里一条都不许留；banner 里提到它们属于历史，不算）
STALE = ["铺进舞台", "跑完删掉", "再压包", "STALKER_Toolkit_2.0.0_", "`<标记>.<序号>`"]

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


def body_of(spec: str) -> str:
    """剥掉 HTML 注释块（banner 是**历史更正记录**，不是正文）。"""
    return re.sub(r"<!--.*?-->", "", spec, flags=re.S)


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--spec", default=str(SPEC))
    args = ap.parse_args()
    spec_path = Path(args.spec)

    print("== H-04：打包规格正文必须与实现同源 ==")
    for p in (spec_path, PS1):
        if not p.is_file():
            print("PACK-SPEC skip 缺 %s" % p)
            return 2
    raw = spec_path.read_text(encoding="utf-8", errors="replace")
    body = body_of(raw)
    ps1 = PS1.read_text(encoding="utf-8", errors="replace")

    want(body.strip() != "", "A0 剥掉 banner 之后还有正文（剥法把正文也剥了＝判据失效）",
         "%d 字符" % len(body))

    hits = [w for w in STALE if w in body]
    want(not hits, "A1 正文里**不再**出现旧做法的断言句",
         ("仍出现：" + "、".join(hits)) if hits else "")

    want("qoffscreen" in body and "随包常驻" in body,
         "B1 正文写清 `qoffscreen.dll` **随包常驻**（不是临时铺了再撤）",
         "qoffscreen=%s 随包常驻=%s" % ("qoffscreen" in body, "随包常驻" in body))
    want("只出目录" in body and "不压 zip" in body,
         "B2 正文写清**只出目录、不压 zip**",
         "只出目录=%s 不压 zip=%s" % ("只出目录" in body, "不压 zip" in body))
    want(re.search(r"dist\\<主版本>\\<标记>\\<序号>\\", body) is not None,
         "B3 正文写清落点形状 `dist\\<主版本>\\<标记>\\<序号>\\`（**只留序号**）")
    want("SHELL-SMOKE ok checks=" in body,
         "B4 正文写清包内自检的**结论行**（判据要能指出看什么）")

    want("platform_plugins=qwindows,qoffscreen" in ps1,
         "C1 同源锁：`package.ps1` 里真的有 `platform_plugins=qwindows,qoffscreen` 那行声明")
    want("Compress-Archive" not in ps1,
         "C2 同源锁：`package.ps1` **没有** `Compress-Archive`（＝真的不压 zip）")
    want(re.search(r'\$pkgName\s*=\s*"\{0\}"\s*-f\s*\$idx', ps1) is not None,
         'C3 同源锁：落点名只取序号（`$pkgName = "{0}" -f $idx`）')
    want('Move-Item -LiteralPath $stage -Destination $pkgDir' in ps1,
         "C4 同源锁：产物是**目录移动**（`Move-Item … -Destination $pkgDir`），不是打包")

    print("-" * 66)
    if PROBLEMS:
        print("PACK-SPEC fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PACK-SPEC ok %d 项（正文与 `package.ps1` 的现状同源：qoffscreen 常驻／只出目录不压 zip／"
          "落点只留序号；旧做法的断言句已清）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
