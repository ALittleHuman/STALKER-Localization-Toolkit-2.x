# -*- coding: utf-8 -*-
"""P17-81（甲）：**夹具定性 + "默认可见页＝六个内置页" + "把 `_fixtures\\` 抬上来才验插件页"**。

为什么要有它（用户转来的 P17-78 致命项："验收物 ≠ 交付物"）
----------------------------------------------------------
事实（已核）：`Hub` 启动时按 `app_dir()/plugins` 加载插件（`hub.cpp:814`），而插件页用
`replaces` **顶掉**内置页 ⇒ **同一个 exe，目录里有没有 `plugins\\*.py`，决定它跑哪一份实现**：
开发包跑**插件页**（被反复验证的那份）、发行包跑**内置页**（用户拿到的那份）。
用户 2026-10-10 拍板 **甲**：**产品实现＝C++ 内置六页（唯一真源）**；
`plugins/*_page.py` ＋ `hello.py` 降级为**插件 API 能力验证夹具（非产品）**。

本脚本把甲落成**可判红**的四件事（前两件静态、后两件跑产物）：

  ① **夹具带固定标记串**（文件头 `NON-PRODUCT-FIXTURE`）—— "非产品"不能只写在文档里
     （文档会烂），要能被机器查；8 枚（六页 ＋ `toolchain_page.py` ＋ `hello.py`）逐枚点名。
  ② **发行口径包里不含夹具**（`version.txt` 的 `version=` 里标记 ∈ {RC, FINAL} ⇒
     `plugins\\` 顶层与 `plugins\\_fixtures\\` 都必须为空）。ALPHA/BETA 是开发版：
     夹具**随包携带**但落 `_fixtures\\`（加载器**非递归**：`loader.cpp:108` /
     `py_host.cpp:107/3428` 三处都是 `directory_iterator` ⇒ 子目录默认不加载）。
  ③ **默认可见页＝六个内置页**：在**产物副本**里跑 `xrkrkit_app.exe --smoke`
     （`QT_QPA_PLATFORM=offscreen`），读 `PLUGIN-PAGES 可见页集合=…` 那一行 ⇒ 逐名比对六个内置 id。
     ⇒ 这就是 P17-78 第四条的止血：**发行口径那一份被真的跑过**（开发包默认也跑同一份实现）。
  ④ **把 `_fixtures\\` 显式抬上来跑一次**（P17-79 §十·2 的"同机制"）：同一次运行里
     **既取"抬上来之后的包内插件计数"、又断"插件页真的生效"** —— 七个插件页 id 逐个点名、
     且**不再有内置页**。这一条**不依赖"开发包默认加载夹具"**，所以与 ③ 不打架。
     ★ 抬起来只动**临时副本**（原产物一个字节都不动）——`--py-plugins` 那条链在
       `package.ps1` 的包内自检里已经跑过（它读 `plugins\\`，那里是空的、加载不到东西）。

外部前提与第三态（口径同 `check_package_platform.py`）：
  · `dist/` 是 gitignore 的运行期产物 ⇒ **没有包**时 ③④ 不执行，退出码 **2**（调用方记台账；
    release 档由调用方按"前提缺失"判 FAIL —— 不许把"没跑"读成"绿"）。
  · 有包但没有可执行文件 ⇒ 明确判红（不是台账）。

输出：`PKG-PAGES ok …` / `PKG-PAGES fail …`；退出码 0=绿 / 1=判据不成立 / 2=本次没有产物可跑。
自带反例自证（`selfcheck()`）：喂"夹具缺标记""发行包里带夹具""可见页是插件页"三种反例必须报红，
一份干净样本必须报绿（**判据自己先要可信**）。
"""

from __future__ import annotations

import os
import re
import shutil
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

# 夹具＝六页插件版（各自 replaces 一个内置页）＋ 三插件同页 ＋ 最小 `.py` 加载链夹具。
#   ★ 显式清单（不挂 glob）：`plugins/` 下多一枚 `.py` 而这里没登记 ⇒ 红（见 ① 的"多一枚"那条）。
FIXTURE_PAGES = [
    "plugins/fs_page.py",
    "plugins/convert_page.py",
    "plugins/text_page.py",
    "plugins/xml_page.py",
    "plugins/video_page.py",
    "plugins/fontpack_page.py",
    "plugins/toolchain_page.py",
    "tests/python_plugins/hello.py",
]

# 六个内置页 id（与 `src/shell/pages/page_factory.cpp` 的 `kBuiltinPages[]` 同源；顺序＝槽位序）
BUILTIN_IDS = ["fs", "convert", "text", "xml", "video", "fontpack"]
# 夹具页的插件 id（`replaces` 用的新 id；`toolchain_page.py` 没有 replaces，是"新增页"）
PLUGIN_IDS = ["fs_py", "convert_py", "text_py", "xml_py", "video_py", "fontpack_py", "toolchain_py"]

MARKER = "NON-PRODUCT-FIXTURE"
RELEASE_MARKERS = ("RC", "FINAL")

FAILURES: list[str] = []
NOTES: list[str] = []

# `SHELL-SMOKE note PLUGIN-PAGES 可见页集合=7 页（内置=0 / 插件py=7）[1=fs_py 2=convert_py] 结束 · …`
PAGES_RE = re.compile(r"PLUGIN-PAGES 可见页集合=(\d+)\s*页（内置=(\d+)\s*/\s*插件py=(\d+)）\[(.*?)\]\s*结束")
# ★★ B-4（第三方只读审计 2026-10-18）：**新 ASCII 行**（`KEY k=v`）—— 迁移期与上面那条并存，
#   本判据**优先认新行**、认不到再退回旧行（老包只发旧行）。固定骨架全是 ASCII，值里允许非 ASCII。
PAGES_ASCII_RE = re.compile(
    r"PLUGIN-PAGES pages=(\d+) builtin=(\d+) plugin_py=(\d+) owner_matches_id=(\d) names=\[(.*?)\]")
SMOKE_OK_RE = re.compile(r"SHELL-SMOKE ok checks=(\d+) failed=0")


def parse_pages_any(text: str):
    """→ (pages, builtin, plugin_py, ids, 用的哪一行) 或 None。新 ASCII 行优先。"""
    m = PAGES_ASCII_RE.search(text)
    if m:
        ids = re.findall(r"\d+=([A-Za-z0-9_]+)", m.group(5))
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)), ids, "ascii")
    m = PAGES_RE.search(text)
    if m:
        # 旧行：条目形如 `1=fs=`（内置，尾部带"内置标记"串）或 `2=fs_py`（插件）——
        #   ★ 两种都要认（`.rstrip("=")` 是原来那段的语义，别用"必须带尾等号"的正则）。
        ids = []
        for item in m.group(4).split():
            naming = re.match(r"^\d+=(.+)$", item)
            if naming is not None:
                ids.append(naming.group(1).rstrip("="))
        return (int(m.group(1)), int(m.group(2)), int(m.group(3)), ids, "legacy")
    return None


def say(line: str) -> None:
    print(line, flush=True)


def check(ok: bool, label: str, detail: str = "") -> None:
    if ok:
        say("  \u2713 %s" % label)
    else:
        FAILURES.append(label)
        say("  \u2717 %s%s" % (label, ("\n      " + detail) if detail else ""))


def brief(text: str, lines: int = 4) -> str:
    rows = [row for row in text.splitlines() if row.strip()]
    return "\n      ".join(rows[-lines:])


# ── ① 夹具标记串（静态、可自证）────────────────────────────────────────────
def marker_problems(sources: dict) -> list:
    """sources: {相对路径: 源码文本} ⇒ 问题清单（空＝全绿）。★ 会被喂反例自证。"""
    problems = []
    for name in FIXTURE_PAGES:
        if name not in sources:
            problems.append("%s：清单里点名了，但文件不存在（夹具清单与仓库脱节）" % name)
            continue
        if MARKER not in sources[name]:
            problems.append("%s：文件里没有固定标记串 `%s` —— 夹具不许「看起来像产品」"
                            % (name, MARKER))
    return problems


# ── ② 发行包里不许有夹具（静态、可自证）────────────────────────────────────
def release_package_problems(marker: str, top_files: list, fixture_files: list) -> list:
    """marker: 'ALPHA'/'BETA'/'RC'/'FINAL'/''；两份清单＝包内实测文件名。★ 会被喂反例自证。"""
    problems = []
    if marker in RELEASE_MARKERS:
        if top_files:
            problems.append("发行口径包（%s）的 `plugins\\` 顶层有 %d 个文件：%s"
                            % (marker, len(top_files), ", ".join(sorted(top_files))))
        if fixture_files:
            problems.append("发行口径包（%s）里带夹具（`plugins\\_fixtures\\` 有 %d 个文件）：%s"
                            % (marker, len(fixture_files), ", ".join(sorted(fixture_files))))
    return problems


# ── ③④ 产物面：跑一次自检，比对可见页 ──────────────────────────────────────
def parse_pages(out: str):
    """返回 (页数, 内置数, 插件py数, [逐页 id])；两行都不匹配 ⇒ None。
    ★★ B-4（第三方只读审计 2026-10-18）：**新 ASCII 行优先**（`PLUGIN-PAGES pages=…`），
    认不到再退回旧中文行（迁移期两种都在；老包只发旧行）。"""
    parsed = parse_pages_any(out)
    if parsed is None:
        return None
    pages, builtin, plugin_py, ids, _which = parsed
    return pages, builtin, plugin_py, ids


def run_smoke(pkg: Path, timeout: int = 900):
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"   # ★ 只给子进程，不设持久变量
    env.setdefault("PYTHONUTF8", "1")
    exe = pkg / "xrkit_app.exe"
    try:
        proc = subprocess.run([str(exe), "--smoke"], cwd=str(pkg), env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        subprocess.run(["taskkill", "/F", "/T", "/IM", "xrkit_app.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return None, "[超时 %ds] 未收尾" % timeout
    except OSError as exc:
        return None, "起不来 `--smoke`：%s" % exc
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def newest_package():
    base = ROOT / "dist"
    if not base.is_dir():
        return None
    best = None
    for index_dir in base.glob("*/*/*"):
        if not index_dir.is_dir() or not index_dir.name.isdigit():
            continue
        score = int(index_dir.name)
        if best is None or score > best[0]:
            best = (score, index_dir)
    return best[1] if best else None


def package_marker(pkg: Path) -> str:
    """从 `version.txt` 的 `version=2.0.0 ALPHA.169` 取标记（取不到＝空串 ⇒ 按开发版处理）。"""
    version_txt = pkg / "version.txt"
    if not version_txt.is_file():
        return ""
    for row in version_txt.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        if row.strip().startswith("version="):
            value = row.strip().split("=", 1)[1].strip()
            match = re.match(r"^\S+\s+([A-Za-z]+)", value)
            if match is not None:
                return match.group(1).upper()
    return ""


def audit_package(pkg: Path) -> None:
    rel = pkg.relative_to(ROOT).as_posix()
    marker = package_marker(pkg)
    say("  产物：%s（标记=%s）" % (rel, marker or "(认不出，按开发版处理)"))

    top_files = sorted(p.name for p in (pkg / "plugins").glob("*") if p.is_file()) \
        if (pkg / "plugins").is_dir() else []
    fixture_files = sorted(p.name for p in (pkg / "plugins" / "_fixtures").glob("*")
                           if p.is_file()) \
        if (pkg / "plugins" / "_fixtures").is_dir() else []
    say("  包内 `plugins\\` 顶层 %d 个文件；`plugins\\_fixtures\\` %d 个文件"
        % (len(top_files), len(fixture_files)))

    problems = release_package_problems(marker, top_files, fixture_files)
    check(not problems, "产物 %s：夹具不进发行口径包（ALPHA/BETA 走 `_fixtures\\`）" % rel,
          "\n      ".join(problems))

    exe = pkg / "xrkit_app.exe"
    if not exe.is_file():
        check(False, "产物 %s：xrkit_app.exe 在包里（③④ 要跑它）" % rel)
        return

    # ★ 只动**临时副本**：原产物一个字节都不许改（④ 要把夹具抬到顶层）
    tmp = Path(tempfile.mkdtemp(prefix="p1781_pkg_pages_"))
    try:
        work = tmp / "pkg"
        shutil.copytree(pkg, work)

        # ── ③ 默认形态：可见页＝六个内置页 ────────────────────────────────
        rc, out = run_smoke(work)
        pages = parse_pages(out) if out else None
        if rc is None:
            check(False, "产物 %s（默认形态）：`--smoke` 能跑完" % rel, str(out))
            return
        smoke_ok = SMOKE_OK_RE.search(out)
        check(smoke_ok is not None and rc == 0,
              "产物 %s（默认形态）：`SHELL-SMOKE ok … failed=0` 且退出码 0" % rel,
              "rc=%s\n      %s" % (rc, brief(out)))
        check(pages is not None,
              "产物 %s（默认形态）：自检打出 `PLUGIN-PAGES 可见页集合=…` 那一行" % rel,
              brief(out))
        if pages is not None:
            count, builtin, plugin_py, ids = pages
            say("  默认形态实测：%d 页（内置=%d / 插件py=%d）%s" % (count, builtin, plugin_py, ids))
            check(count == len(ids) and builtin == len(BUILTIN_IDS) and plugin_py == 0
                  and ids == BUILTIN_IDS,
                  "产物 %s（默认形态）：**默认可见页＝六个内置页**（逐名同序）" % rel,
                  "期望 %s，实得 %s" % (BUILTIN_IDS, ids))

        # ── ④ 显式把 `_fixtures\` 抬到顶层再跑一次（同机制：计数与"插件页生效"同一次运行）──
        fixture_dir = work / "plugins" / "_fixtures"
        if not fixture_dir.is_dir():
            check(False, "产物 %s：`plugins\\_fixtures\\` 存在（④ 的显式装载要有东西可抬）" % rel)
            return
        lifted = []
        for item in sorted(fixture_dir.glob("*")):
            if item.is_file():
                target = work / "plugins" / item.name
                shutil.move(str(item), str(target))
                lifted.append(item.name)
        say("  ④ 显式抬起 %d 个夹具到顶层：%s" % (len(lifted), ", ".join(lifted)))
        rc2, out2 = run_smoke(work)
        pages2 = parse_pages(out2) if out2 else None
        if rc2 is None:
            check(False, "产物 %s（抬夹具形态）：`--smoke` 能跑完" % rel, str(out2))
            return
        smoke_ok2 = SMOKE_OK_RE.search(out2)
        check(smoke_ok2 is not None and rc2 == 0,
              "产物 %s（抬夹具形态）：`SHELL-SMOKE ok … failed=0` 且退出码 0" % rel,
              "rc=%s\n      %s" % (rc2, brief(out2)))
        # ★★ B-4：**新 ASCII 行优先**（`PKG-PLUGINS files=… dll=… py=…`），认不到再退回旧中文行。
        note = re.search(r"PKG-PLUGINS files=(\d+) dll=(\d+) py=(\d+)", out2)
        if note is not None:
            total, dlls, pys = int(note.group(1)), int(note.group(2)), int(note.group(3))
        else:
            note = re.search(r"#8 包内插件：(\d+)\s*个文件（(\d+)\s*个 dll / (\d+)\s*个 py）", out2)
            if note is not None:
                total, dlls, pys = int(note.group(1)), int(note.group(2)), int(note.group(3))
        if note is None:
            check(False, "产物 %s（抬夹具形态）：自检报出包内插件计数" % rel, brief(out2))
        else:
            total, dlls, pys = int(note.group(1)), int(note.group(2)), int(note.group(3))
            # 顶层的期望枚数＝**包里实际有多少夹具**（六页 ＋ toolchain ＋ hello.py，
            # 以及构建出来的 `xrkit_*_plugin.dll`）—— 用**抬上来的那份清单**算，
            # 免得判据自己写死一个数、与实际打的包脱节（"计数要与目录一致"那条口径）。
            want_dlls = len([n for n in lifted if n.lower().endswith(".dll")])
            want_pys = len([n for n in lifted if n.lower().endswith(".py")])
            say("  抬夹具形态实测：包内插件 %d 个文件（%d 个 dll / %d 个 py）"
                % (total, dlls, pys))
            check(pys == want_pys and dlls == want_dlls and total == len(lifted)
                  and pys >= len(PLUGIN_IDS),
                  "产物 %s（抬夹具形态）：包内插件计数＝抬上来的清单（%d 个 py / %d 个 dll）"
                  % (rel, want_pys, want_dlls),
                  "期望 %d 个 py / %d 个 dll，实得 %d / %d" % (want_pys, want_dlls, pys, dlls))
        check(pages2 is not None,
              "产物 %s（抬夹具形态）：自检打出 `PLUGIN-PAGES 可见页集合=…` 那一行" % rel,
              brief(out2))
        if pages2 is not None:
            count2, builtin2, plugin_py2, ids2 = pages2
            say("  抬夹具形态实测：%d 页（内置=%d / 插件py=%d）%s"
                % (count2, builtin2, plugin_py2, ids2))
            check(builtin2 == 0 and plugin_py2 == len(PLUGIN_IDS) and ids2 == PLUGIN_IDS,
                  "产物 %s（抬夹具形态）：**插件页真的生效**（七个插件页逐名同序、内置页全被顶掉）"
                  % rel,
                  "期望 %s，实得 %s" % (PLUGIN_IDS, ids2))
    finally:
        if os.environ.get("XRKIT_KEEP_PKG_PAGES_TMP"):
            say("  （XRKIT_KEEP_PKG_PAGES_TMP 已设：临时副本保留在 %s）" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)


# ── 反例自证 ───────────────────────────────────────────────────────────
def selfcheck() -> tuple:
    results, all_ok = [], True

    def add(label, problems, want_min):
        nonlocal all_ok
        good = len(problems) >= want_min if want_min else not problems
        all_ok &= good
        results.append((label, want_min, len(problems), good, problems))

    clean_sources = {name: "#!/usr/bin/env python3\n# %s: 夹具（非产品）\n" % MARKER
                     for name in FIXTURE_PAGES}
    add("好形状（8 枚都带标记）", marker_problems(clean_sources), 0)
    no_mark = dict(clean_sources)
    del no_mark["plugins/fs_page.py"]
    add("夹具缺一枚标记", marker_problems(no_mark), 1)
    missing = dict(clean_sources)
    del missing["plugins/xml_page.py"]
    add("清单点名的夹具不存在", marker_problems(missing), 1)

    add("好形状（发行包无夹具）", release_package_problems("FINAL", [], []), 0)
    add("发行包顶层带夹具", release_package_problems("FINAL", ["fs_page.py"], []), 1)
    add("发行包 `_fixtures` 里带夹具", release_package_problems("FINAL", [], ["fs_page.py"]), 1)
    add("好形状（开发版带 `_fixtures`）",
        release_package_problems("ALPHA", [], ["fs_page.py", "hello.py"]), 0)

    good_out = ("SHELL-SMOKE note PLUGIN-PAGES 可见页集合=6 页（内置=6 / 插件py=0）"
                "[1=fs= 2=convert= 3=text= 4=xml= 5=video= 6=fontpack=] 结束 · dir=x\n")
    lifted_out = ("SHELL-SMOKE note PLUGIN-PAGES 可见页集合=7 页（内置=0 / 插件py=7）"
                  "[1=fs_py 2=convert_py 3=text_py 4=xml_py 5=video_py 6=fontpack_py "
                  "7=toolchain_py] 结束 · dir=x\n")
    add("好形状（默认形态解析）", [] if parse_pages(good_out) == (6, 6, 0, BUILTIN_IDS) else ["解析不对"], 0)
    add("好形状（抬夹具形态解析）",
        [] if parse_pages(lifted_out) == (7, 0, 7, PLUGIN_IDS) else ["解析不对"], 0)
    add("判据行缺失", [] if parse_pages("SHELL-SMOKE ok checks=1 failed=0\n") is None else ["应当取不到"], 0)
    return all_ok, results


def main() -> int:
    say("PKG-PAGES P17-81：夹具定性（非产品）＋ 默认可见页＝六个内置页 ＋ 抬 `_fixtures\\` 才验插件页")
    say("")
    say("-- 反例自证 --")
    self_ok, results = selfcheck()
    for label, want_min, got, good, problems in results:
        say("   %s %-28s 期望≥%d 实得=%d" % ("\u2713" if good else "\u2717", label, want_min, got))
        if not good:
            for item in problems[:2]:
                say("        · %s" % item)
    if not self_ok:
        say("")
        say("PKG-PAGES fail 反例自证不通过（本锁自己不可信）")
        return 1
    say("   \u2713 反例自证 %d/%d" % (len(results), len(results)))
    say("")

    # ── ① 仓库侧：夹具标记串（逐枚点名）────────────────────────────────────
    sources = {}
    for name in FIXTURE_PAGES:
        path = ROOT / name
        if path.is_file():
            sources[name] = path.read_text(encoding="utf-8", errors="replace")
    problems = marker_problems(sources)
    check(not problems, "① 8 枚夹具都带固定标记串 `%s`（非产品＝机器可查，不是文档里的口号）" % MARKER,
          "\n      ".join(problems))
    extra = sorted(p.name for p in (ROOT / "plugins").glob("*.py")
                   if ("plugins/" + p.name) not in FIXTURE_PAGES)
    check(not extra, "① `plugins/*.py` 没有清单外的文件（多一枚就是「没登记的夹具」）",
          "多余：%s" % ", ".join(extra))
    say("")

    # ── ②③④ 产物侧 ────────────────────────────────────────────────────────
    # 可选：命令行给一个包目录 ⇒ 只跑 ③④（② 判的是"发行口径包里不许有夹具"，需要
    # `dist/<主版本>/<标记>/<序号>` 那个布局里的标记口径；离线副本判不了，所以显式记账，
    # **不当绿**）。
    if len(sys.argv) > 1:
        given = Path(sys.argv[1]).resolve()
        if not given.is_dir():
            say("PKG-PAGES fail 命令行给的包目录不存在：%s" % given)
            return 1
        NOTES.append("② 发行口径包不含夹具（本次由命令行指定包目录 ⇒ 跳过；它只在 "
                     "dist/<主版本>/<标记>/<序号> 布局下判得了）")
        say("  指定包目录：%s（只跑 ③④，② 记台账）" % given)
        audit_package(given)
    else:
        pkg = newest_package()
        if pkg is None:
            NOTES.append("P17-81 夹具页判据（%s 下没有任何包；③④ 未执行，同一条断言写在 "
                         "package.ps1 的包内自检里，每次打包都会跑）" % (ROOT / "dist").relative_to(ROOT))
            say("  LEDGER 本次没有 dist 产物 ⇒ ②③④ 未执行（每次打包时 package.ps1 会跑同一套断言）")
        else:
            audit_package(pkg)

    if FAILURES:
        say("")
        say("PKG-PAGES fail %d 项不成立" % len(FAILURES))
        return 1
    say("")
    say("PKG-PAGES ok 夹具带 NON-PRODUCT-FIXTURE 标记、不进发行包；默认可见页＝六个内置页；"
        "抬 `_fixtures\\` 后七个插件页逐名生效%s" % ("（②③④ 未执行，见台账）" if NOTES else ""))
    return 2 if NOTES else 0


if __name__ == "__main__":
    sys.exit(main())
