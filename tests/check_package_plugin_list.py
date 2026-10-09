# -*- coding: utf-8 -*-
r"""P16-2a 静态锁：**随包插件清单**必须是显式的，且包内自检必须同时报 dll 与 py。

为什么要有它（"切打包口径"最容易悄悄退回去的地方）
--------------------------------------------------
修前 `package.ps1` 只把一枚 `hello.py` 夹具拷进包、`smoke.cpp` 的 #8 note **只数 `*.dll`**，
`package.ps1` 的判据也只按 `插件：(\d+)\s*个\s*dll` 解析 ⇒ "六页插件版 + 三插件同页进包"
这件事**根本没有判据**：把 `.py` 那一段删掉、或把 note 改回只报 dll，门禁一声不响。
无端到端打包就无法在门禁里验（打包要几分钟）⇒ 这里锁**三件事的结构**：

  ① `package.ps1` 必须显式列出**随包的 7 枚 `.py`**（六页插件版 + `toolchain_page.py`），
     且这 7 枚与 `plugins/` 目录里的实际文件**逐名一致**（少一枚/多一枚都红）；
  ② `package.ps1` 的解析必须要求**新格式**（同一行里同时出现 `个文件`、`个 dll`、`个 py`）；
  ③ `src/shell/smoke.cpp` 的 `#8 包内插件：` 那一行必须**同时**报出 dll 与 py 两组计数。

输出：`PKG-PLUGINS ok …` / `fail …`；退出码 0=绿 / 1=有断言不成立。
自带反例自证：合成"缺一枚 py / note 只报 dll"两种样本，必须都报红。
"""

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "scripts" / "package.ps1"
SMOKE = ROOT / "src" / "shell" / "smoke.cpp"
PLUGINS = ROOT / "plugins"

EXPECTED_PAGES = ["fs_page.py", "convert_page.py", "text_page.py",
                  "xml_page.py", "video_page.py", "fontpack_page.py",
                  "toolchain_page.py"]


def quoted_names(source: str) -> set:
    return set(re.findall(r"'([A-Za-z0-9_]+\.py)'", source))


def audit_package(source: str, expected: list) -> list:
    problems = []
    if ".py" not in source:
        problems.append("package.ps1 里看不到任何 `.py` —— 随包插件那一段没了？")
    if "plugins" not in source:
        problems.append("package.ps1 里看不到 `plugins` 目录")
    listed = quoted_names(source)
    missing = [n for n in expected if n not in listed]
    if missing:
        problems.append("package.ps1 的显式清单里缺：%s" % missing)
    # ② 解析必须要求新格式
    for token in ("个文件", "个 dll", "个 py"):
        if token not in source:
            problems.append("package.ps1 的解析里看不到 `%s` —— 还在按旧格式（只数 dll）判？" % token)
    return problems


def audit_smoke(source: str) -> list:
    problems = []
    if "#8 包内插件：" not in source:
        problems.append("smoke.cpp 里没有 `#8 包内插件：` 那一行（包内自检的口径行）")
        return problems
    # 取那一行附近的窗口来判（note 是一条 QStringLiteral，可能跨行拼接）
    idx = source.find("#8 包内插件：")
    window = source[max(0, idx - 600): idx + 600]
    for token in ("个 dll", "个 py"):
        if token not in window:
            problems.append("smoke.cpp 的 #8 note 里看不到 `%s`（要同时报 dll 与 py）" % token)
    return problems


def selfcheck():
    good_pkg = ("$stage = Join-Path $root 'build\\package_stage'\n"
                "$dst = Join-Path $stage 'plugins\\'\n"
                "Copy-Item -LiteralPath $srcPlugin -Destination $dst\n"
                + "".join("'%s'\n" % n for n in EXPECTED_PAGES)
                + "if ($pluginText -match 'plugins：(\\d+)\\s*个文件') {}\n"
                + "'（(\\d+)\\s*个 dll'\n'(\\d+)\\s*个 py）'\n")
    good_smoke = ('#8 包内插件：%1 个文件（%2 个 dll / %3 个 py）（%4）')
    cases = [
        ("好形状", good_pkg, good_smoke, 0),
        ("缺一枚 py", good_pkg.replace("'toolchain_page.py'\n", ""), good_smoke, 1),
        ("只报 dll", good_pkg, "#8 包内插件：%1 个 dll（%2）", 1),
        ("没有那行 note", good_pkg, "别的 note", 1),
    ]
    results, ok = [], True
    for label, pkg, smoke, want_min in cases:
        problems = audit_package(pkg, EXPECTED_PAGES) + audit_smoke(smoke)
        good = len(problems) >= want_min if want_min else not problems
        ok &= good
        results.append((label, want_min, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P16-2a：随包插件清单 + 包内自检口径 ==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_min, got, good, problems in results:
        print("   %s %-14s 期望≥%d 实得=%d" % ("✓" if good else "✗", label, want_min, got))
        if not good:
            for p in problems[:2]:
                print("        · %s" % p)
    if not self_ok:
        print()
        print("PKG-PLUGINS fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (PACKAGE, SMOKE):
        if not path.exists():
            print("PKG-PLUGINS fail 仓库自带文件缺失：%s" % path)
            return 1

    actual_pages = sorted(p.name for p in PLUGINS.glob("*_page.py"))
    print("   plugins/ 里的页文件（%d 枚）：%s" % (len(actual_pages), ", ".join(actual_pages)))
    problems = audit_package(PACKAGE.read_text(encoding="utf-8-sig"), actual_pages)
    problems += audit_smoke(SMOKE.read_text(encoding="utf-8", errors="replace"))
    print()
    if problems:
        for p in problems:
            print("   ✗ %s" % p)
        print()
        print("PKG-PLUGINS fail %d 项不成立" % len(problems))
        return 1
    print("PKG-PLUGINS ok 随包页清单显式（%d 枚）＋ 包内自检同时报 dll 与 py" % len(actual_pages))
    return 0


if __name__ == "__main__":
    sys.exit(main())
