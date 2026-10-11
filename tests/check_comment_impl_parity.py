# -*- coding: utf-8 -*-
"""P17-94 判据：**注释/标注必须与实现同源**（不许各说各话）。

背景（本批修的审计四条，都是"文档/注释漂了"这一类）
--------------------------------------------------
| 审计 | 事实 | 处置 |
| --- | --- | --- |
| `02 F-10` | `encoding.cpp` 的 `kBig5Cols = 0xFE - 0x40 + 1;  // 190` —— **算出来是 191**（`big5_tables.inc` 的 `[16999]=89×191` 也证实） | 注释改 191 |
| `03 F-10` | `theme.h` 写"scale: 0.85/1.0/1.25/1.5"，实现 `theme.cpp` 的 `kChoices` 是 **0.9/1.0/1.1/1.2** | 注释改成**指向唯一事实来源**（不再抄一遍） |
| `03 F-11` | `hub.h` 的 `Q_INVOKABLE void log_line(...)` —— 本类**不用 `Q_OBJECT`** ⇒ 那是**空标**（`invokeMethod` 只会静默 false）；功能坑早已用**回调**绕开 | 删掉空标，把口径留在注释里 |
| `03 F-09` | `screen_scale.h` 注释宣称"用**逻辑 DPI**"，而实现用 `devicePixelRatio()` ＋ `physicalDotsPerInch()`（真机实测 `logicalDotsPerInch()` 恒 96） | 删掉/改掉那句（`★ 口径更正` 早就写明正确规则） |

判据（四组，都要有牙）
----------------------
① `kBig5Cols` 那行的注释值必须 **等于算出来的值**（不信注释、也不信我，**现场算**）；
② `theme.h` 里**不许再抄**档位数值（`0.85`/`1.25`/`1.5` 都不许出现），必须指向 `ui_size_choices()`；
③ `hub.h` 的 `log_line` 声明上**不许有 `Q_INVOKABLE`**（本类没有 `Q_OBJECT`），且必须留下
   "改用回调"的口径（`services.log` / `log_to_host` 任一提及即可）；
④ `screen_scale.h` 的**文件头注释**里不许再出现"用逻辑 DPI"这种主张（"不是逻辑 DPI"这类**否定句**允许）。

★ 为什么值得一条判据：这四条全是"改实现时忘了改旁边那句字"的产物 —— 而**判据里没有一条读它们** ⇒
漂了几个月没人发现（本轮审计实查才发现）。清零不够，得**上锁**。

输出：`COMMENT-PARITY ok …` / `COMMENT-PARITY fail …`；退出码 0=绿 / 1=有断言不成立。自带反例自证。
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
ENCODING = ROOT / "src" / "engine" / "text" / "encoding.cpp"
THEME_H = ROOT / "src" / "shell" / "theme.h"
THEME_CPP = ROOT / "src" / "shell" / "theme.cpp"
HUB_H = ROOT / "src" / "shell" / "hub.h"
SCREEN_SCALE_H = ROOT / "src" / "shell" / "screen_scale.h"
SNIFF_CPP = ROOT / "src" / "engine" / "text" / "sniff.cpp"
TEXT_SPEC = ROOT / "docs" / "2.0.0-文本模块规格.md"


def audit_big5_cols(text: str) -> list:
    """① 注释值必须等于表达式算出来的值（**现场算**，不信任任何一边）。"""
    problems = []
    m = re.search(r"constexpr\s+uint16_t\s+kBig5Cols\s*=\s*([^;]+);\s*//\s*(\d+)", text)
    if m is None:
        return ["encoding.cpp：找不到 `kBig5Cols = <表达式>;  // <数>` 这个形状（判据与源码脱节？）"]
    expr, claimed = m.group(1).strip(), int(m.group(2))
    parts = re.findall(r"0x[0-9A-Fa-f]+|\d+", expr)
    if len(parts) != 3:
        return ["encoding.cpp：`kBig5Cols` 的表达式形状变了（%s）—— 判据要跟着改" % expr]
    values = [int(p, 16) if p.lower().startswith("0x") else int(p) for p in parts]
    real = values[0] - values[1] + values[2]
    if claimed != real:
        problems.append("encoding.cpp：`kBig5Cols` 注释写 %d，**算出来是 %d**" % (claimed, real))
    return problems


def audit_theme_scale(theme_h: str, theme_cpp: str) -> list:
    """② 不许在头文件里抄一份档位数值（抄了就必然过期）；必须指向唯一事实来源。"""
    problems = []
    head = theme_h[:theme_h.find("struct UiSizeChoice")]
    for num in ("0.85", "1.25", "1.5"):
        if num in head:
            problems.append("theme.h：档位注释里又出现了抄来的值 `%s`（应当指向 `ui_size_choices()`）" % num)
    if "ui_size_choices()" not in head:
        problems.append("theme.h：档位注释没有指向 `ui_size_choices()`（唯一事实来源）")
    if "kChoices" not in theme_cpp:
        problems.append("theme.cpp：找不到 `kChoices`（实现搬走了？判据要跟着改）")
    return problems


def audit_hub_invokable(hub_h: str) -> list:
    """③ `log_line` 不许带 `Q_INVOKABLE`（本类无 `Q_OBJECT` ⇒ 空标），且要留"改用回调"的口径。"""
    problems = []
    if re.search(r"Q_INVOKABLE\s+void\s+log_line", hub_h):
        problems.append("hub.h：`log_line` 又带上了 `Q_INVOKABLE` —— 本类**没有 `Q_OBJECT`**，"
                        "那是空标（`invokeMethod` 只会静默 false）")
    if "Q_OBJECT" not in hub_h:
        # 前提变了（有人加了 Q_OBJECT）⇒ 这条判据的口径要重估，不能默默绿
        problems.append("hub.h：找不到 `Q_OBJECT` 的说明 —— 若真的加了 `Q_OBJECT`，本判据的前提就变了，"
                        "请重估并改判据")
    if "改用**回调**" not in hub_h and "改用回调" not in hub_h:
        problems.append("hub.h：`log_line` 旁边没留下「改用回调」的口径（后人会以为是漏了 `Q_INVOKABLE`）")
    return problems


def audit_screen_scale(header: str) -> list:
    """④ 文件头注释不许再宣称"用逻辑 DPI"（**否定句**允许：「不是逻辑 DPI」）。"""
    problems = []
    head = header[:header.find("namespace xrkit::shell")]
    for match in re.finditer(r"用\s*\*{0,2}逻辑\s*DPI", head):
        line_start = head.rfind("\n", 0, match.start()) + 1
        line = head[line_start:head.find("\n", match.start())]
        if "不" in line.split("用")[0][-6:] or "不要" in line:
            continue   # 否定句（"不要宣称用逻辑 DPI"）允许
        problems.append("screen_scale.h：文件头注释又在说「用逻辑 DPI」了（实现是 dpr ＋ 物理 DPI）：%s"
                        % line.strip()[:80])
    return problems


def audit_sniff_thresholds(sniff_cpp: str, spec: str) -> list:
    """⑤ ★★ P17-96（审计 `02 F-13`）：**sniff 的门槛常量与规格必须同值**。

    事实（审计原文）：门槛「只靠注释互指」—— `sniff.cpp` 里 `kStatMinEvidence = 2` / `kStatMargin = 0.15`，
    规格 `docs/2.0.0-文本模块规格.md` §5 写 `决定性 = 证据 >= 2 且 |score| >= 0.15`。
    ⇒ **改任一边都不会红**（门禁只锁零静默标记，不锁这两个数）—— 本判据把两处**对起来**：
    源码里那两个常量的值 == 规格里那两个数；任一边改了而另一边没跟上 ⇒ 红。
    """
    problems = []
    m_ev = re.search(r"kStatMinEvidence\s*=\s*(\d+)", sniff_cpp)
    m_mg = re.search(r"kStatMargin\s*=\s*([0-9.]+)", sniff_cpp)
    if m_ev is None or m_mg is None:
        return ["sniff.cpp：找不到 `kStatMinEvidence` / `kStatMargin` 两个常量（判据与源码脱节？）"]
    ev, mg = int(m_ev.group(1)), m_mg.group(1)
    m_spec = re.search(r"证据[^\n]{0,20}>=\s*(\d+)[^\n]{0,30}\|\s*score\s*\|\s*>=\s*([0-9.]+)", spec)
    if m_spec is None:
        return ["规格里找不到「证据 >= N 且 |score| >= M」那一行（判据与文档脱节？）"]
    spec_ev, spec_mg = m_spec.group(1), m_spec.group(2)
    if str(ev) != spec_ev:
        problems.append("sniff.cpp 的 `kStatMinEvidence=%d` 与规格的 `>= %s` **不同值**（02 F-13）"
                        % (ev, spec_ev))
    if mg.rstrip("0").rstrip(".") != spec_mg.rstrip("0").rstrip("."):
        problems.append("sniff.cpp 的 `kStatMargin=%s` 与规格的 `>= %s` **不同值**（02 F-13）"
                        % (mg, spec_mg))
    return problems


def selfcheck() -> tuple:
    good_enc = "constexpr uint16_t kBig5Cols = 0xFE - 0x40 + 1;      // 191\n"
    good_theme_h = "//   scale: 见 `ui_size_choices()`（当前是 0.9 小 / 1.0 默认）\nstruct UiSizeChoice {"
    good_theme_cpp = "static const std::vector<UiSizeChoice> kChoices = {{...}};\n"
    good_hub = "// 改用**回调**：Hub 造页时把它的 log_line 接进来\n// 本类**不用 Q_OBJECT / 不用 signals**\nvoid log_line(const QString& text);\n"
    good_scale = "//   ② **不要再宣称\"用逻辑 DPI\"** —— 真机实测它恒为 96\nnamespace xrkit::shell {"
    cases = [
        ("好形状", 0,
         audit_big5_cols(good_enc) + audit_theme_scale(good_theme_h, good_theme_cpp)
         + audit_hub_invokable(good_hub) + audit_screen_scale(good_scale)),
        ("注释值算错（修前形状）", 1,
         audit_big5_cols("constexpr uint16_t kBig5Cols = 0xFE - 0x40 + 1;      // 190\n")),
        ("头文件里抄了一份档位值", 1,
         audit_theme_scale("//   scale: 0.85 小 / 1.0 默认 / 1.25 大 / 1.5 特大\nstruct UiSizeChoice {",
                           good_theme_cpp)),
        ("注释没指向唯一事实来源", 1,
         audit_theme_scale("//   scale: 见下面的表\nstruct UiSizeChoice {", good_theme_cpp)),
        ("Q_INVOKABLE 又回来了", 1,
         audit_hub_invokable("// 本类**不用 Q_OBJECT**\n// 改用回调\nQ_INVOKABLE void log_line(const QString& text);\n")),
        ("screen_scale 又宣称用逻辑 DPI", 1,
         audit_screen_scale("//   ② 用 **逻辑 DPI**（QScreen::logicalDotsPerInch()）\nnamespace xrkit::shell {")),
        ("screen_scale 的否定句不算违规", 0,
         audit_screen_scale("//   ② 不要宣称用逻辑 DPI —— 实测它恒为 96\nnamespace xrkit::shell {")),
        ("sniff 门槛与规格同值", 0,
         audit_sniff_thresholds("constexpr size_t kStatMinEvidence = 2;\nconstexpr double kStatMargin = 0.15;\n",
                                "决定性 = 证据(高位字节数) >= 2  且  |score| >= 0.15")),
        ("门槛常量被改而规格没跟上", 1,
         audit_sniff_thresholds("constexpr size_t kStatMinEvidence = 3;\nconstexpr double kStatMargin = 0.15;\n",
                                "决定性 = 证据(高位字节数) >= 2  且  |score| >= 0.15")),
        ("规格被改而常量没跟上", 1,
         audit_sniff_thresholds("constexpr size_t kStatMinEvidence = 2;\nconstexpr double kStatMargin = 0.20;\n",
                                "决定性 = 证据(高位字节数) >= 2  且  |score| >= 0.15")),
    ]
    results, ok = [], True
    for label, want, problems in cases:
        good = (len(problems) > 0) if want else (not problems)
        ok &= good
        results.append((label, want, len(problems), good, problems))
    return ok, results


def main() -> int:
    print("== P17-94：注释/标注 ↔ 实现 同源核对（审计 02 F-10 / 03 F-09 / 03 F-10 / 03 F-11）==")
    print()
    print("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want, got, good, problems in results:
        print("   %s %-28s 期望%s 实得 %d 项" % ("✓" if good else "✗", label,
                                                "有错" if want else "无错", got))
        if not good:
            for item in problems[:2]:
                print("        · %s" % item)
    if not self_ok:
        print()
        print("COMMENT-PARITY fail 反例自证不通过（本锁自己不可信）")
        return 1
    print("   ✓ 反例自证 %d/%d" % (len(results), len(results)))
    print()

    for path in (ENCODING, THEME_H, THEME_CPP, HUB_H, SCREEN_SCALE_H, SNIFF_CPP, TEXT_SPEC):
        if not path.is_file():
            print("COMMENT-PARITY fail 仓库自带文件缺失：%s" % path)
            return 1
    enc = ENCODING.read_text(encoding="utf-8", errors="replace")
    th = THEME_H.read_text(encoding="utf-8", errors="replace")
    tc = THEME_CPP.read_text(encoding="utf-8", errors="replace")
    hh = HUB_H.read_text(encoding="utf-8", errors="replace")
    ss = SCREEN_SCALE_H.read_text(encoding="utf-8", errors="replace")
    sn = SNIFF_CPP.read_text(encoding="utf-8", errors="replace")
    spec = TEXT_SPEC.read_text(encoding="utf-8", errors="replace")

    groups = [
        ("① `kBig5Cols` 注释值 == 算出来的值（02 F-10）", audit_big5_cols(enc)),
        ("② 档位值只许有一处（指向 `ui_size_choices()`）（03 F-10）", audit_theme_scale(th, tc)),
        ("③ `log_line` 不带空标 `Q_INVOKABLE` ＋ 留下回调口径（03 F-11）", audit_hub_invokable(hh)),
        ("④ `screen_scale.h` 文件头不再宣称「用逻辑 DPI」（03 F-09）", audit_screen_scale(ss)),
        ("⑤ sniff 门槛常量 == 规格里那两个数（02 F-13）", audit_sniff_thresholds(sn, spec)),
    ]
    print("-- 实测 --")
    problems = []
    for label, items in groups:
        print("   %s %s" % ("✓" if not items else "✗", label))
        for item in items:
            print("        · %s" % item)
        problems += items
    print()
    if problems:
        print("COMMENT-PARITY fail %d 项不成立" % len(problems))
        return 1
    print("COMMENT-PARITY ok 五处「字据 ↔ 实现」都同源（02 F-10 / 03 F-09 / 03 F-10 / 03 F-11 / 02 F-13 已被判据钉住）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
