# -*- coding: utf-8 -*-
"""P17-110 判据：散装层条目表**不许跟着软链接/junction 走，且必须有上限**（复审计 `01 F-07`）。

事实（修前）
------------
`src/engine/fs/overlay.cpp:loose_entries` 用 `std::filesystem::is_directory(full, ec)` ——
它**跟随**链接（Windows 上 junction 也算目录）⇒ 两件事同时成立：
  ① **走出根**：`<root>/out -> D:\\别处` 被当普通子目录递归进去，条目表里出现**不属于这个根**的东西；
  ② **自指成环 = 不终止**：`<root>/self -> <root>` 反复递归，而修前**没有任何深度/条数上限**
     ⇒ 内存吃满、界面挂死（`loose_entries` 是在 UI 线程被调的）。

判据（**真造 junction 试**，形状与 `check_extract_symlink_guard.py` 同一套）
--------------------------------------------------------------------------
① 基线：普通目录 ⇒ 条目数/内容正确（改了不许把正常情况弄坏）；
② `<root>/self` 做成**指向 root 自己**的 junction ⇒
   · `--loose-entries` 必须**在超时内返回**（修前不终止 ⇒ 这一格就是"卡住"，判据按**超时**判红）；
   · 条目数**必须是有限小**（不爆炸）；
   · `self` **作为条目出现**（可见）；
   · **不许**出现 `self/…`（递归进去的痕迹）；
③ 链接指向**根外**的目录 ⇒ 同样不许把外部内容列进来（走出根那一半）。
非 Windows / 造不出 junction ⇒ 如实 skip（rc=2），不假装判过。

输出：`LOOSE-GUARD ok …` / `… fail …`；退出码 0=绿 / 1=红 / 2=skip。
"""

from __future__ import annotations

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
DEFAULT_CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
TIMEOUT_S = 25

PROBLEMS: list = []
CASES = 0


def want(cond: bool, label: str, detail: str = "") -> bool:
    global CASES
    CASES += 1
    if cond:
        print("   ✓ %s%s" % (label, ("（%s）" % detail) if detail else ""))
        return True
    PROBLEMS.append("%s%s" % (label, ("（%s）" % detail) if detail else ""))
    print("   ✗ %s%s" % (label, ("（%s）" % detail) if detail else ""))
    return False


def make_junction(link: Path, target: Path) -> bool:
    """Windows：`cmd /c mklink /J`（junction **不需要管理员**，符号链接才需要）。"""
    if sys.platform != "win32":
        return False
    proc = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)],
                          capture_output=True, timeout=60)
    return proc.returncode == 0 and link.exists()


def probe(cli: Path, root: Path) -> tuple:
    """跑探针：返回 (rc, 条目路径列表, 首行, 是否超时)。"""
    try:
        proc = subprocess.run([str(cli), "--loose-entries", str(root)],
                              capture_output=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return (None, [], "TIMEOUT", True)
    text = ((proc.stdout or b"") + (proc.stderr or b"")).decode("utf-8", "replace")
    lines = [l.strip() for l in text.splitlines() if l.strip().startswith("LOOSE")]
    entries = [l.split(" ", 2)[2].split(" dir=")[0] for l in lines if l.startswith("LOOSE entry ")]
    return (proc.returncode, entries, (lines[0] if lines else ""), False)


def main() -> int:
    cli = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_CLI
    print("== P17-110 散装层条目表：不进链接 ＋ 有上限（复审计 `01 F-07`）==")
    if not cli.is_file():
        print("!! 缺构建产物：%s" % cli)
        print("LOOSE-GUARD skip 缺 CLI")
        return 2

    with tempfile.TemporaryDirectory(prefix="loose_guard_") as tmp:
        base = Path(tmp) / "root"
        (base / "sub").mkdir(parents=True)
        (base / "empty").mkdir()
        (base / "a.txt").write_text("A", encoding="utf-8")
        (base / "sub" / "b.txt").write_text("B", encoding="utf-8")

        # ① 基线
        rc, entries, first, timed_out = probe(cli, base)
        want(not timed_out and rc == 0, "① 基线：普通目录能在超时内跑完", first[:80])
        want(entries == ["a.txt", "empty", "sub", "sub/b.txt"],
             "① 基线条目正确（与既有口径一致）", ",".join(entries))

        # ② 自指 junction
        self_link = base / "self"
        if not make_junction(self_link, base):
            print("   ⚠ 造不出 junction（非 Windows / 无权限）⇒ 如实 skip")
            print("LOOSE-GUARD skip 造不出 junction")
            return 2
        rc, entries, first, timed_out = probe(cli, base)
        want(not timed_out,
             "② 自指 junction 下**能在超时内返回**（修前不终止 ⇒ 界面挂死）",
             "TIMEOUT(%ds)" % TIMEOUT_S if timed_out else "")
        if timed_out:
            print()
            print("LOOSE-GUARD fail 跟着自指链接递归下去了（修前形状：不终止）")
            return 1
        want(len(entries) < 20, "② 条目数不爆炸", "%d 条" % len(entries))
        want("self" in entries, "② 链接**本身**作为条目列出（可见）")
        escaped = [e for e in entries if e.startswith("self/")]
        want(not escaped, "② 链接**不许**被递归进去", "逃逸条目 %d 条" % len(escaped))
        self_link.rmdir()

        # ③ 指向根外的 junction
        outside = Path(tmp) / "outside"
        outside.mkdir()
        (outside / "secret.txt").write_text("X", encoding="utf-8")
        out_link = base / "out"
        if make_junction(out_link, outside):
            rc, entries, first, timed_out = probe(cli, base)
            want(not timed_out and rc == 0, "③ 指向根外的 junction 下能跑完", first[:80])
            leaked = [e for e in entries if e.startswith("out/")]
            want(not leaked, "③ 根外目录的内容**不许**被列进来（走出根）",
                 "泄漏条目 " + ",".join(leaked) if leaked else "")
            want("out" in entries, "③ 根外链接本身仍作为条目列出（可见）")
        else:
            print("   ⚠ 造不出第二个 junction ⇒ ③ 格如实跳过")

        # ④ ★★ P17-119（复审计 `P2-9`）：**退出码契约** —— 探针不许"出错也返回 0"。
        #   事实（修前）：`--loose-entries` 无论 `error` 空不空都 `return kExitOk`，
        #   而 `--sniff` 在同类情形下返回 `kExitFail` ⇒ 只看退出码的脚本会把
        #   "散装层根不存在"当成成功（又一个假绿入口）。
        #   口径：正常目录 → 0；**不存在的根 → 非 0**；缺操作数 → 用法码（3）。
        rc_ok, _, _, _ = probe(cli, base)
        want(rc_ok == 0, "④ 正常目录 ⇒ 退出码 0", "rc=%s" % rc_ok)
        rc_missing, _, first_missing, _ = probe(cli, base / "definitely-not-here")
        want(rc_missing is not None and rc_missing != 0,
             "④ **不存在的根 ⇒ 非 0 退出**（修前是 0 ⇒ 假绿）", "rc=%s" % rc_missing)
        want("不存在" in first_missing or "散装层" in first_missing,
             "④ 且理由可见（零静默）", first_missing[:90])
        # 缺操作数：直接手跑（`probe` 会塞一个路径）
        usage = subprocess.run([str(cli), "--loose-entries"], capture_output=True, timeout=60)
        want(usage.returncode != 0, "④ 缺操作数 ⇒ 用法码（非 0）", "rc=%s" % usage.returncode)

        # ⑤ ★★ P17-120（复审计 `P2-5`，用户点名的"会烂的耦合"）：**截断必须是结构化标志**。
        #   为什么这一格只能**静态查**：上限是编译期常量（深度 32 / 条数 20 万）⇒ 造不出夹具来触发。
        #   但"截断有没有和 error 分开"是**源码形状**问题，可以查得很硬：
        #     · 引擎**不许**再把 `条目表已截断` 写进 `error`（只许写进 `LooseInfo`）；
        #     · **CLI 不许**再用中文前缀判语义（`rfind("条目表已截断"` 必须消失）；
        #     · **GUI 必须**读 `loose_info.truncated`（而不是"非空 error 就拒载"）。
        engine_src = (ROOT / "src" / "engine" / "fs" / "overlay.cpp").read_text(
            encoding="utf-8", errors="replace")
        cli_src = (ROOT / "tools" / "cli" / "main.cpp").read_text(
            encoding="utf-8", errors="replace")
        gui_src = (ROOT / "src" / "shell" / "pages" / "fs_page.cpp").read_text(
            encoding="utf-8", errors="replace")
        want('error = "条目表已截断' not in engine_src,
             "⑤ 引擎**不许**把「截断」写进 `error`（那是失败通道）",
             "还在写" if 'error = "条目表已截断' in engine_src else "")
        want("truncated_reason" in engine_src and "LooseInfo" in engine_src,
             "⑤ 引擎提供 `LooseInfo.truncated` / `truncated_reason`（结构化）")
        want('rfind("条目表已截断"' not in cli_src,
             "⑤ CLI **退役**了中文前缀判语义（`rfind(...)` 已消失）",
             "还在用前缀" if 'rfind("条目表已截断"' in cli_src else "")
        want("truncated=" in cli_src,
             "⑤ CLI 把标志**报出来**（`truncated=<0|1>`）")
        want("loose_info.truncated" in gui_src,
             "⑤ GUI 读的是**同一个标志**（不是「非空 error 就拒载」）",
             "GUI 没读标志" if "loose_info.truncated" not in gui_src else "")

        # ⑥ ★★ P17-121（复审计 `P2-4`）：**"会不会跟出去"要判位，不是列两个标签的白名单**。
        #   事实（修前）：`is_link_or_junction` 只认 `MOUNT_POINT`/`SYMLINK` 两个值 ⇒
        #   其它"名字代理型"重解析点（复审计点名的 **WSL `LX_*` 族**）**全部漏网**被跟随。
        #   口径：判**名字代理位**（`0x20000000`）—— 置位=路径解析会跟着走；不置位=过滤器/元数据型
        #   （WOF/Dedup/AppExecLink/Cloud）。★ 新出现的名字代理标签**自动被覆盖**，不必补清单。
        #   ★ 实测边界（如实）：本机**没装 WSL** ⇒ `LX_*` 那类**量不到**，它被归为 follow 是
        #     按官方位定义推出来的。判据只能查"用的是位判据"＋"注释里写清了这条边界"。
        link_src = (ROOT / "include" / "xrkit" / "engine" / "fs" / "link_check.h").read_text(
            encoding="utf-8", errors="replace")
        want("0x20000000" in link_src and "kNameSurrogate" in link_src,
             "⑥ `is_link_or_junction` 用**名字代理位**判（不是两个标签的白名单）")
        want("== IO_REPARSE_TAG_MOUNT_POINT" not in link_src
             and "== IO_REPARSE_TAG_SYMLINK" not in link_src,
             "⑥ 两个标签的**白名单比较已退役**（替换，不并存）",
             "还在列标签" if ("== IO_REPARSE_TAG_MOUNT_POINT" in link_src
                             or "== IO_REPARSE_TAG_SYMLINK" in link_src) else "")
        want(("LX" in link_src or "WSL" in link_src),
             "⑥ 注释点名了这条要覆盖的那族（WSL `LX_*`）")
        want("没装 WSL" in link_src or "无法实测" in link_src,
             "⑥ 并**如实标注**了「本机量不到 `LX_*`」这条边界",
             "没写边界" if ("没装 WSL" not in link_src and "无法实测" not in link_src) else "")

    print("-" * 66)
    if PROBLEMS:
        print("LOOSE-GUARD fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("LOOSE-GUARD ok %d 项全绿（基线 ＋ 自指不终止已消除 ＋ 不递归进链接 ＋ 不走出根；"
          "复审计 `01 F-07`）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
