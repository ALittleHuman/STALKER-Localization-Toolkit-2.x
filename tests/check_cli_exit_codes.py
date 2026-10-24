# -*- coding: utf-8 -*-
r"""判据 C-06：CLI 退出码 3 的**双语义**必须被枚举、钉住、并写进文案（第三方审计 C-06）。

**事实（本判据现场枚举，不引审计、不引旧批）**
  `tools/cli/main.cpp` 里 `return kExitUsage;`（＝3）有 **59** 处（`return 3;` 字面量另 1 处），
  但它同时承担**两种语义**：

  | 家族 | 数量 | 例子 | 语义 |
  | --- | --- | --- | --- |
  | `USAGE` 真·用法错误 | 48 | 缺操作数、未知参数、未知格式名、未知动作 | "你命令行写错了" |
  | `GUARD_EXISTS` 写盘守卫 | 10 | `--extract`/`--pack-db`/`--decode-text`/`--codec-lossy`/`--decl-fix`/`--sniff`/`--font-render`/`--resample-dump`/`--extract-chars`/`--lzhuf` 的"输出已存在且没 `--force`" | "**我什么都没写**，确认后再来" |
  | `GUARD_CONFIRM` 待用户确认 | 1 | `--convert-batch` 的 `needs_confirm`（P17-87） | "**我一个文件都没转**，由你显式指定后重跑" |

  ★ 用户 2026-10-16 的口径：「我立了 9 个写盘动作的『守卫 ＝ rc 3』，若别处也用 3，
    **先枚举所有 `kExitUsage` 语义再定，别直接改**」。
    枚举结果：守卫是 **10** 处（不是 9；`--lzhuf` 与 `--decode-text` 各有一条），
    另有 `--convert-batch` 一处**待确认**；其余 48 处是真·用法错误（T-07 加过一处 `--compressor` 缺值；决策 1 删 `--plugin-init-rollback` 又少一处）。
  ★ 结论（本批采用）：**rc 3 的真实含义是「我什么都没做，读我的理由行」** —— 代码一行不动，
    该改的是**文案**（`--help` 与 `README.md` 只写了「3=用法错误」）。

**断言**
  A **枚举**：每一处 `return kExitUsage;`／`return 3;` 都必须能找到**理由文案**（同函数内、
    往前 ≤ `LOOKBACK` 行），并按文案归进三族之一；归不进去 ⇒ 红（新加一处守卫/用法错误必须
    同时归族，否则这张表就过期了）；
  B **定值**：三族的处数等于冻结值 `{USAGE:49, GUARD_EXISTS:10, GUARD_CONFIRM:1}`；
    任何一处增删（含"把守卫改回 kExitFail"）⇒ 红，并在消息里说清该改哪里；
  C **字面量**：`return 3;` 只许出现在 `--convert-batch` 的待确认分支（其余一律走 `kExitUsage`）；
  D **文案**：`--help` 与 `README.md` 必须同时写出「3=」的**两种语义**（用法错误 ＋ 守卫/待确认），
    只写一种 ⇒ 红（这就是本批要修的漂移）；
  E **黑盒**（有 CLI 产物时才跑，否则 rc=2）：
    · 守卫命中（`--extract` 输出已存在、无 `--force`）⇒ rc=**3**、文案含「输出已存在」与
      「拒绝」（"我什么都没写"必须说出来）；
    · 真·用法错误（`--dump` 少参数）⇒ rc=**3**；
    · **失败**（`--dump` 指向不存在的文件）⇒ rc=**1**（证明 1≠3，别把失败也读成"没做"）。

**退出码**：0=绿 / 1=判据不成立 / 2=缺输入（源码或 README 缺失；或 E 那半缺 CLI 产物）。
`--cli` / `--readme` 只为**取证**存在（造一份带新站点的副本跑红证），默认永远走仓库路径。
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI_SRC = ROOT / "tools" / "cli" / "main.cpp"
README = ROOT / "README.md"
CLI_EXE = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

LOOKBACK = 25
# ★ 2026-10-24（Q2）：新增 `--sha256 <文件>` 动作 ⇒ 多一处真·用法错误（缺操作数）
#   ⇒ USAGE 48 → 49。**按本判据自己的规矩**：增删一处就必须重估口径并改本表，
#   不是把判据放宽 —— 族的划分没变（缺操作数 = 用法错误）。
FROZEN = {"USAGE": 49, "GUARD_EXISTS": 10, "GUARD_CONFIRM": 1}

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


def classify(message: str) -> str:
    """按**理由文案**归族。归不进去返回空串（判据要红）。"""
    if "输出已存在" in message:
        return "GUARD_EXISTS"
    if "没有任何文件被转换" in message or "needs-confirm" in message:
        return "GUARD_CONFIRM"
    if message == "print_usage()":
        return "USAGE"   # 没有动作 ⇒ 打用法 ⇒ 3（与"缺操作数"同类）
    if re.search(r"用法：|未知参数|未知格式名|未知子命令|未知动作|不认识|missing-path|未知模式", message):
        return "USAGE"
    return ""


def action_blocks(lines) -> list[tuple[int, str]]:
    """把 `main.cpp` 切成**动作块**：`if (action == "--xxx")` 到下一个动作/文件尾。

    ★ C0.5（2026-10-24）：本判据原来用**行距启发式**（`LOOKBACK` 往前找最近一条文案），
      而一次机械替换（11 处写盘点收进 helper，每处缩短 4~5 行）就会把行距全挪掉 ——
      于是这条门禁会因为"行号变了"而假红/假绿，**和改动对错无关**。
      现在改成**按动作块**找：同一个 `action == "--X"` 块里的理由文案才算这一处的理由。
      块边界不随代码行数漂移 ⇒ sweep 之后仍然有效。
    """
    blocks = []
    for i, line in enumerate(lines):
        if 'action == "' in line and "if" in line:
            blocks.append((i, line.strip()))
    if not blocks:
        return [(0, "<无动作块>")]
    return blocks


def enclosing_block(blocks, i: int) -> tuple:
    """返回行 i 所属的 (块起点, 块终点, 动作行)。"""
    cur = blocks[0]
    for k, (start, label) in enumerate(blocks):
        if start <= i:
            cur = (start, label)
            nxt = blocks[k + 1][0] if k + 1 < len(blocks) else 10 ** 9
    start, label = cur
    end = 10 ** 9
    for s2, _l2 in blocks:
        if s2 > start:
            end = min(end, s2)
    return (start, end, label)


def find_message(lines, i, blocks) -> str:
    """找 return 那处 rc=3 的**理由文案** —— 只在**同一个动作块内**找（不再按行距）。"""
    start, end, _label = enclosing_block(blocks, i)
    ret_indent = len(lines[i]) - len(lines[i].lstrip())
    # ① 同块内往上找最近一条"像理由"的行（缩进 ≤ return，避免抓到更深层的分支）
    for j in range(i - 1, start - 1, -1):
        raw = lines[j]
        s = raw.strip()
        if not s:
            continue
        if "std::cerr <<" in s or ("std::cout <<" in s and ("reason=" in s or "hint=" in s)):
            if len(raw) - len(raw.lstrip()) <= ret_indent:
                return s
    # ② 同块内没有理由行、但有 print_usage ⇒ 仍算用法错误
    for j in range(i - 1, start - 1, -1):
        if "print_usage()" in lines[j]:
            return "print_usage()"
    # ③ 不在任何动作块里（`action.empty()` 那一族）⇒ 往上有限找（这一段没有 sweep 风险）
    for j in range(i - 1, max(-1, i - 1 - LOOKBACK), -1):
        raw = lines[j]
        s = raw.strip()
        if not s:
            continue
        if "std::cerr <<" in s or ("std::cout <<" in s and ("reason=" in s or "hint=" in s)):
            if len(raw) - len(raw.lstrip()) <= ret_indent:
                return s
        if "print_usage()" in lines[j]:
            return "print_usage()"
    return ""


def enumerate_sites(text: str):
    """→ [(行号, 原文行, 最近理由文案, 族)]。"""
    lines = text.splitlines()
    blocks = action_blocks(lines)
    sites = []
    for i, line in enumerate(lines):
        s = line.strip()
        if s not in ("return kExitUsage;", "return 3;"):
            continue
        message = find_message(lines, i, blocks)
        sites.append((i + 1, s, message, classify(message) if message else ""))
    return sites


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--cli", default=str(CLI_SRC))
    ap.add_argument("--readme", default=str(README))
    args = ap.parse_args()
    src_path = Path(args.cli)
    readme_path = Path(args.readme)

    print("== CLI 退出码 3 的双语义（C-06）：枚举 ＋ 钉住 ＋ 文案一致 ==")
    for p in (src_path, readme_path):
        if not p.is_file():
            print("CLI-EXIT-CODES skip 缺 %s" % p)
            return 2
    text = src_path.read_text(encoding="utf-8", errors="replace")
    readme = readme_path.read_text(encoding="utf-8", errors="replace")

    sites = enumerate_sites(text)
    counts = {"USAGE": 0, "GUARD_EXISTS": 0, "GUARD_CONFIRM": 0}
    unknown = []
    for ln, raw, message, fam in sites:
        if fam:
            counts[fam] += 1
        else:
            unknown.append("%d: %s（最近文案：%s）" % (ln, raw, message[:60] or "<找不到理由文案>"))
    print("   枚举结果：%d 处（usage=%d / guard_exists=%d / guard_confirm=%d）" %
          (len(sites), counts["USAGE"], counts["GUARD_EXISTS"], counts["GUARD_CONFIRM"]))

    want(not unknown, "A 每一处 rc=3 都能按**理由文案**归族（新加的必须归族，否则这张表过期）",
         ("未归族 %d 处：%s" % (len(unknown), unknown[:3])) if unknown else "")
    want(counts == FROZEN, "B 三族处数等于冻结值（增删一处就必须重估口径并改本表）",
         "实得 %s，冻结 %s" % (counts, FROZEN))

    # C：字面量 `return 3;` 只许在 --convert-batch 的待确认分支
    literal = [(ln, raw) for ln, raw, _m, _f in sites if raw == "return 3;"]
    want(len(literal) == 1, "C1 `return 3;` 字面量只许一处（其余一律 `kExitUsage`）",
         "实得 %d 处：%s" % (len(literal), [ln for ln, _ in literal]))
    if literal:
        ln = literal[0][0]
        near = "\n".join(text.splitlines()[max(0, ln - 30):ln])
        want("needs_confirm" in near or "CONVERTBATCH stopped" in near,
             "C2 那一处字面量属于 `--convert-batch` 的待确认分支", "行 %d" % ln)

    # D：文案必须写出两种语义（★ 必须在**写着 `3=` 的那一行**上写 —— 别处提一句"守卫"不算）
    GUARD_WORDS = ("守卫", "待确认", "拒绝覆盖", "什么都没做", "什么都没写")

    def doc_ok(doc: str) -> tuple:
        for line in doc.splitlines():
            if re.search(r"3\s*=\s*", line):
                return (True, "用法错误" in line, any(w in line for w in GUARD_WORDS))
        return False, False, False

    # `print_usage` 的**全部** `3=` 行（帮助串 ＋ 它头上那段注释）都要写全 ——
    #   ★ 第一版只判"第一条含 3= 的行"，结果命中了**早已过期的注释**
    #     （`// … 3 = 用法错误。`），而真正印给用户的帮助串反而没被读到。
    #   ★ 第二版按**字符**切窗口（±9000 字符）也错了：本文件行很长（中文注释），
    #     9000 字符到不了帮助串 ⇒ 窗口里一条 `3=` 都没有。改成按**行**切。
    src_lines = text.splitlines()
    i_usage = next((k for k, ln in enumerate(src_lines) if "void print_usage(" in ln), -1)
    usage_region = "\n".join(src_lines[max(0, i_usage - 40):i_usage + 400]) if i_usage >= 0 else ""
    h3, hu, hg = doc_ok(usage_region)
    want(h3 and hu and hg, "D1 `--help` 的退出码那一行（含 `print_usage` 的注释）同时写出 3 的两种语义",
         "3=%s 用法错误=%s 守卫/待确认=%s" % (h3, hu, hg))
    stale = [ln.strip()[:80] for ln in usage_region.splitlines()
             if re.search(r"3\s*=\s*", ln) and not any(w in ln for w in GUARD_WORDS)]
    want(not stale, "D1b `print_usage` 附近**没有**只写「3=用法错误」的旧口径（注释也算）",
         ("仍有 %d 行：%s" % (len(stale), stale[:2])) if stale else "")
    r3, ru, rg = doc_ok(readme)
    want(r3 and ru and rg, "D2 `README.md` 的退出码那一行同上（它也是给用户看的表）",
         "3=%s 用法错误=%s 守卫/待确认=%s" % (r3, ru, rg))

    # E：黑盒三条（rc 3 的两种用法 ＋ rc 1 的失败）
    if not CLI_EXE.is_file():
        print("   · 缺 CLI 构建产物 ⇒ E 那三条**未跑**（本判据不假装它通过）")
        print("CLI-EXIT-CODES skip 缺 CLI（静态 %d 项已判，黑盒未验）" % CASES)
        return 2
    with tempfile.TemporaryDirectory(prefix="cliexit_") as tmp:
        base = Path(tmp)
        tree = base / "src"
        tree.mkdir()
        (tree / "a.txt").write_bytes(b"x")
        pkg = base / "t.db0"
        subprocess.run([str(CLI_EXE), "--pack-db", str(pkg), "xdb", str(tree)],
                       capture_output=True, timeout=300)
        exists = base / "already.txt"
        exists.write_bytes(b"keep me")
        p = subprocess.run([str(CLI_EXE), "--extract", str(pkg), "a.txt", str(exists)],
                           capture_output=True, timeout=300)
        out = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
        want(p.returncode == 3 and "输出已存在" in out and "拒绝" in out,
             "E1 写盘守卫命中 ⇒ rc=3，且**说出来**「输出已存在／拒绝」",
             "rc=%d out=%s" % (p.returncode, out.strip()[:80]))
        want(exists.read_bytes() == b"keep me",
             "E2 守卫命中时**一个字节都没写**（这才是 rc 3 的真实含义）")

        q = subprocess.run([str(CLI_EXE), "--dump"], capture_output=True, timeout=300)
        qout = ((q.stdout or b"") + (q.stderr or b"")).decode("utf-8", "replace")
        want(q.returncode == 3 and "missing-path" in qout,
             "E3 真·用法错误也是 rc=3（同一码、不同语义；靠理由行区分）",
             "rc=%d out=%s" % (q.returncode, qout.strip()[:80]))

        ghost = base / "no_such_package.db0"
        r = subprocess.run([str(CLI_EXE), "--dump", str(ghost)], capture_output=True, timeout=300)
        rout = ((r.stdout or b"") + (r.stderr or b"")).decode("utf-8", "replace")
        want(r.returncode == 1, "E4 **失败**是 rc=1（别把失败也读成「没做」）",
             "rc=%d out=%s" % (r.returncode, rout.strip()[:80]))

    print("-" * 66)
    if PROBLEMS:
        print("CLI-EXIT-CODES fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("CLI-EXIT-CODES ok %d 项（3 的两种语义已枚举并写进 --help/README；守卫拒不写盘）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
