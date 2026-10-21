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

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
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


def _kill_leftover_smoke() -> int:
    """★ 隔离：先清掉上一轮残留的 `xrkit_app.exe`（P17-176 的教训：残留进程会占用文件/目录）。"""
    try:
        r = subprocess.run(["taskkill", "/F", "/T", "/IM", "xrkit_app.exe"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        txt = (r.stdout or b"").decode("utf-8", "replace")
        return txt.count("SUCCESS") + txt.count("成功")
    except (OSError, subprocess.SubprocessError):
        return 0


PHASE = ["default"]


def _prune_kept_dirs(keep: int = 3) -> None:
    """★ 2026-10-20 现场教训：`XRKIT_KEEP_PKG_PAGES_TMP` 一开，失败的临时副本就**不删**；
    连跑几十次后把 C 盘撑到 0.1 GB ⇒ 之后每次 `copytree` 都 `[WinError 112] 磁盘空间不足`，
    表现为"判据几乎全红"。这里只保留最新 `keep` 份现场，其余删掉（现场仍在日志里）。"""
    try:
        dirs = []
        for base in (ROOT / "build", Path(tempfile.gettempdir())):
            dirs += [p for p in base.glob("p1781_pkg_pages_*") if p.is_dir()]
        dirs = sorted(dirs,
                      key=lambda p: p.stat().st_mtime, reverse=True)
        for old in dirs[keep:]:
            shutil.rmtree(old, ignore_errors=True)
    except OSError:
        pass


def _free_gb(path: Path) -> float:
    try:
        return shutil.disk_usage(str(path)).free / (1024.0 ** 3)
    except OSError:
        return -1.0


def disk_space_ok(need_gb: float = 2.0) -> bool:
    """★ 2026-10-20 现场教训：本判据要往 %TEMP% 复制整包（~250 MB/次）。C 盘被"保留现场"
    撑到 0.1 GB 之后，每次 `copytree` 都 `[WinError 112] 磁盘空间不足` ⇒ **判据几乎全红**，
    而那是环境问题、不是产品问题。这里先做**前置检查**：不够就按"未覆盖"如实说，不产出假红。"""
    free = _free_gb(Path(tempfile.gettempdir()))
    if free < 0:
        return True
    if free < need_gb:
        print("   !! 临时盘余量 %.2f GB < %.1f GB ⇒ 本判据**未覆盖**（不是产品红；先清盘）"
              % (free, need_gb))
        return False
    return True


LAST_SMOKE_TEMP = [None]      # 本轮 smoke 的私有 TEMP（失败取证要从这里拷现场）
PHASE4_ONLY = [False]         # `--phase4-only`：只跑 ④（定向复测，省时间也少噪声）


def run_smoke(pkg: Path, timeout: int = 900):
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"   # ★ 只给子进程，不设持久变量
    env.setdefault("PYTHONUTF8", "1")
    # ★★ 用户 2026-10-19/20：间歇红先查"是不是和共享目录撞车"。
    #   包内 `--smoke` 会往 `%TEMP%\xrkit_smoke_*` 写日志与插件夹具；整档门禁里前面几步也在用
    #   同一批 `%TEMP%` 目录，一旦有残留进程/残留文件就会撞。⇒ 这里给**本次自检一个独占的 TEMP**，
    #   并在起跑前清掉残留进程。这是**测试隔离**，不是"重试把它改绿"。
    killed = _kill_leftover_smoke()
    if killed:
        print("   [隔离] 起跑前清掉 %d 个残留 xrkit_app.exe" % killed)
    # ★ 私有 TEMP 要**短**：放进包副本里会把路径拉长（`…\p1781_pkg_pages_xxxx\pkg\_smoke_temp\…`），
    #   插件夹具那条链在长路径上会出问题（实测：取消探针页的按钮找不到）。
    #   用系统 TEMP 下的短随机目录 ⇒ 既隔离、路径长度又与以前一致。
    # ★★ 用户 2026-10-20 的取证要求：「查干扰 —— 杀软扫描」。实测线索：③ 的偶发红全是
    #   **插件页没建出来**（P17-1 取消探针页 / P5 插件页视口为 0），而插件是**刚写进 %TEMP% 的 .py**。
    #   今天 360 把仓库加白之后，把私有 TEMP 也挪进**仓库内**（= 白名单路径）做对照实验：
    #   若失败率从 8/20 掉到 0/20，就说明是"杀软扫刚写的 .py 导致 import 偶发失败"。
    # ★ 2026-10-21：私有 TEMP 的位置可以换（默认在 `build/`＝仓库内＝360 白名单内）。
    #   用 `XRKIT_PKTMP_DIR=system` 可切到 `%TEMP%`，用来做"路径/杀软"对照实验。
    _pktmp_parent = str(ROOT / "build")
    if os.environ.get("XRKIT_PKTMP_DIR") == "system":
        _pktmp_parent = None
    private_temp = Path(tempfile.mkdtemp(prefix="pktmp_", dir=_pktmp_parent))
    LAST_SMOKE_TEMP[0] = private_temp
    env["TEMP"] = str(private_temp)
    env["TMP"] = str(private_temp)
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
    out = proc.stdout.decode("utf-8", errors="replace")
    # ★★ 用户 2026-10-19/20：间歇红要**先抓证据**（不许先用重试把它改绿）。
    #   失败时把"跑了哪个包 / 退出码 / 失败的具体检查名"一起落到文件，便于事后定性。
    if proc.returncode != 0:
        fails = [l.strip() for l in out.splitlines() if "SHELL-SMOKE fail" in l]
        diag = ROOT / "build" / "logs" / "pkg_pages_fail.txt"
        try:
            diag.parent.mkdir(parents=True, exist_ok=True)
            diag.write_text(
                "pkg=%s\nrc=%d\n\n== SHELL-SMOKE fail 行（%d 条）==\n%s\n\n== 末 40 行 ==\n%s\n"
                % (pkg, proc.returncode, len(fails), "\n".join(fails),
                   "\n".join(out.splitlines()[-40:])),
                encoding="utf-8")
            print("   [诊断] 失败细节已落盘：%s（fail 行 %d 条）" % (diag, len(fails)))
        except OSError as exc:
            print("   [诊断] 落盘失败：%s" % exc)
    return proc.returncode, out


def package_looks_complete(pkg: Path) -> str:
    """★ 判据的**前提**：这个包是"打完了的"，不是被打包过程改写了一半/被中断的残包。

    来由（P17-181 的取证）：`build/logs/pkg_pages_fail.txt` 显示抬夹具形态的 6~9 条失败全是
    "门面控件 0 个 / entry 关键字没生效" —— 那次 `package-pages` 判的是**上一次被中断的
    `_pkg.py` 留在 `dist/` 里的半成品**（版本已 bump、包目录已建、但打包流程没跑完）。
    判据本身没错，是**输入**不完整 ⇒ 这里先验完整性，不完整返回 rc=2（未覆盖），
    **不判红、也不重试**。
    """
    if not (pkg / "xrkit_app.exe").is_file():
        return "缺 xrkit_app.exe"
    exe = pkg / "xrkit_app.exe"
    if exe.stat().st_size < 512 * 1024:
        return "xrkit_app.exe 体积异常（%d B）" % exe.stat().st_size
    vt = pkg / "version.txt"
    if not vt.is_file():
        return "缺 version.txt"
    try:
        text = vt.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return "读不出 version.txt：%s" % exc
    if "version=" not in text:
        return "version.txt 里没有 `version=` 行"
    for rel in ("python", "bin"):
        if not (pkg / rel).is_dir():
            return "缺 `%s\\` 子目录（分类布局没落地 ⇒ 大概率是半成品）" % rel
    return ""


def newest_package():
    """★ 用户 2026-10-20：**输入要钉死** —— 按 `version.json` 的 `current.index` 取包，
    不再"rglob 里挑最大的"（那会把开发包/旧包混进同一条判据的输入，红绿就随目录内容漂）。
    取不到唯一一个就报**歧义**（返回 None + 理由），由调用方按"未覆盖"处理。"""
    base = ROOT / "dist"
    if not base.is_dir():
        return None
    # ★★ 用户 2026-10-20「包选择要钉死」＋ 一条现场教训：`current.index` 会被门禁里的
    #   `shell-smoke` 临时改写（它跑打包脚本、脚本会写 version.json，随后门禁逐字节恢复）。
    #   所以**先看 `tree`**（只在"成功发包"时写、稳定）里最大的序号；没有才退回 `current.index`。
    idx = ""
    try:
        import json
        data = json.loads((ROOT / "version.json").read_text(encoding="utf-8"))
        best = -1
        for ver, marks in (data.get("tree") or {}).items():
            for mark, builds in (marks or {}).items():
                for num in (builds or {}):
                    if str(num).isdigit():
                        best = max(best, int(num))
        if best > 0:
            idx = str(best)
        else:
            idx = str(data["current"]["index"])
    except (OSError, ValueError, KeyError, TypeError):
        idx = ""
    if not idx:
        return None
    cands = [p for p in base.glob("*/*/*") if p.is_dir() and p.name == idx]
    if len(cands) == 1:
        return cands[0]
    if len(cands) > 1:
        print("   !! 包候选歧义（序号 %s 匹配到 %d 个目录）：%s" % (idx, len(cands), cands))
        return None
    return None


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


# ★ 依赖插件页夹具的检查（默认形态必然为 0）——见本文件头与用户 2026-10-20 的定性。
FIXTURE_DEPENDENT = ("P17-82", "P16-3b-2", "P16-3c", "P4 虚拟列表", "P4 拖入", "P4 任务壳",
                     "P4 worker")


def fixture_only_failures(out: str) -> tuple:
    """返回 (夹具依赖的失败行, 其余失败行)。"""
    fails = [ln.strip() for ln in out.splitlines() if "SHELL-SMOKE fail" in ln]
    only = [f for f in fails if any(k in f for k in FIXTURE_DEPENDENT)]
    rest = [f for f in fails if f not in only]
    return only, rest


def capture_phase4_artifacts(pkg: Path, out: str) -> Path:
    """④ 失败时把现场**拷出来**（在删 pktmp 之前调用）。

    用户 2026-10-21 口径：不只两个文件 —— 还要 plugin_errors / 宿主页诊断 / 日志尾部 /
    文件大小·mtime·sha256；`l2_trace.txt` 单独留（判"builder 没跑完"还是"判据读太早"）。
    **缺文件也要写一行"缺失"**（缺失本身就是证据），单文件截断 64 KB。
    """
    ts = time.strftime("%Y%m%d_%H%M%S")
    dest = ROOT / "build" / "logs" / ("pkg_pages_phase4_artifacts_" + ts)
    dest.mkdir(parents=True, exist_ok=True)
    private = LAST_SMOKE_TEMP[0]
    names = ["page_trace.txt", "l2_trace.txt", "build_error.txt", "l2_marker.txt",
             "cancel_marker.txt", "cancel_mark_error.txt", "cancel_probe.py", "l2_demo.py"]
    manifest = []
    src_dirs = []
    if private is not None:
        src_dirs = [private / "xrkit_smoke_uifacade_l2", private / "xrkit_smoke_cancel_probe",
                    private]
    for name in names:
        found = None
        for d in src_dirs:
            cand = d / name
            if cand.is_file():
                found = cand
                break
        if found is None:
            manifest.append("%-24s 缺失（找过：%s）" % (name, ", ".join(str(d) for d in src_dirs)))
            continue
        dst = dest / name
        try:
            data = found.read_bytes()[:65536]
            dst.write_bytes(data)
            digest = hashlib.sha256(found.read_bytes()).hexdigest()
            st = found.stat()
            manifest.append("%-24s size=%d mtime=%s sha256=%s%s"
                            % (name, st.st_size,
                               time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
                               digest, "（已截断 64KB）" if st.st_size > 65536 else ""))
        except OSError as exc:
            manifest.append("%-24s 拷贝失败：%s" % (name, exc))
    # ★ 2026-10-21：把 smoke 里所有**诊断类**行单独抽出来（`诊断` / `plugin_errors` / `插件问题`），
    #   这样"页没注册"那类红不用去 200 行尾巴里翻（下一轮就看这个文件）。
    try:
        diag_lines = [ln for ln in out.splitlines()
                      if ("诊断" in ln or "plugin_errors" in ln or "插件问题" in ln
                          or "已用占位页顶替" in ln)]
        (dest / "diag_lines.txt").write_text("\n".join(diag_lines[-200:]),
                                             encoding="utf-8", errors="replace")
        manifest.append("diag_lines.txt             %d 行" % len(diag_lines[-200:]))
    except OSError:
        manifest.append("diag_lines.txt             写入失败")

    # stdout 尾部 200 行
    try:
        tail = "\n".join(out.splitlines()[-200:])
        (dest / "stdout_tail.txt").write_text(tail, encoding="utf-8", errors="replace")
        manifest.append("stdout_tail.txt            %d 行（末 200）" % len(out.splitlines()[-200:]))
    except OSError:
        manifest.append("stdout_tail.txt            写入失败")
    (dest / "manifest.txt").write_text(
        "phase=4\npkg=%s\nwhen=%s\n\n%s\n" % (pkg, ts, "\n".join(manifest)),
        encoding="utf-8")
    return dest


def repo_write_allowed() -> bool:
    """★ 用户 2026-10-21 的前置探针：**先证明仓库路径能写 DLL**，再跑判据。

    为什么必须探：本机 360「主动防御」会按"文件名＋仓库路径"拦 `vcruntime140.dll`
    （实测：改名片/换路径/先写 `.tmp` 再改名都被拒，`%TEMP%` 可写）。它一旦生效，
    打包/自检会出现**一片红**，而那是环境问题、不是产品问题。
    探针不过 ⇒ 判据按**未覆盖**记账（return 2），绝不产出假红。
    """
    probe_dir = ROOT / "build" / "probe_av"
    src = ROOT / "third_party" / "python-embed" / "vcruntime140.dll"
    try:
        probe_dir.mkdir(parents=True, exist_ok=True)
        dst = probe_dir / "vcruntime140.dll"
        shutil.copy2(src, dst)
        with open(dst, "rb") as fh:
            ok = len(fh.read(1)) == 1
        dst.unlink(missing_ok=True)
        shutil.rmtree(probe_dir, ignore_errors=True)
        return ok
    except OSError as exc:
        print("   !! 仓库路径写 DLL 被拒（%s）⇒ 安全软件没放行这个目录" % exc)
        try:
            shutil.rmtree(probe_dir, ignore_errors=True)
        except OSError:
            pass
        return False


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
    # ★★ 用户 2026-10-21：包副本**落在仓库里**（`build/`），这样只要仓库目录在安全软件里放行，
    #   复制品就不会再被单独扫（不用把整个 `%TEMP%` 放开）。
    tmp = Path(tempfile.mkdtemp(prefix="p1781_pkg_pages_", dir=str(ROOT / "build")))
    try:
        work = tmp / "pkg"
        shutil.copytree(pkg, work)

        # ── ③ 默认形态：可见页＝六个内置页 ────────────────────────────────
        if PHASE4_ONLY[0]:
            say("   [--phase4-only] 跳过 ③（定向复测只跑 ④，少噪声也省时间）")
            rc, out = 0, ""
        else:
            rc, out = run_smoke(work)
        if rc != 0:
            # ★★ 用户 2026-10-20：「失败时能读出是哪个页、childCount 多少、等了多久」——
            #   ③ 也要留**全量输出**与现场（原来只留末 40 行，读不出页/控件细节）。
            diag3 = ROOT / "build" / "logs" / "pkg_pages_phase3_fail.txt"
            try:
                fails3 = [l for l in out.splitlines() if "SHELL-SMOKE fail" in l]
                notes3 = [l for l in out.splitlines() if "SHELL-SMOKE note" in l and "诊断" in l]
                diag3.parent.mkdir(parents=True, exist_ok=True)
                diag3.write_text(
                    "phase=3\npkg=%s\nrc=%s\nwhen=%s\nwork_dir=%s（故意保留）\n"
                    "== fail（%d 条）==\n%s\n\n== 就绪诊断 notes ==\n%s\n\n== 全量输出 ==\n%s\n"
                    % (pkg, rc, time.strftime("%Y-%m-%d %H:%M:%S"), work,
                       len(fails3), "\n".join(fails3), "\n".join(notes3), out),
                    encoding="utf-8")
                say("   [诊断] ③ 失败现场：%s" % diag3)
                try:
                    art3 = capture_phase4_artifacts(pkg, out)
                    say("   [诊断] ③ 现场 artifacts：%s" % art3)
                except OSError as exc:
                    say("   [诊断] artifacts 落盘失败：%s" % exc)
                os.environ["XRKIT_KEEP_PKG_PAGES_TMP"] = "1"
                _prune_kept_dirs(3)
            except OSError as exc:
                say("   [诊断] ③ 落盘失败：%s" % exc)
        pages = parse_pages(out) if out else None
        if rc is None:
            check(False, "产物 %s（默认形态）：`--smoke` 能跑完" % rel, str(out))
            return
        smoke_ok = SMOKE_OK_RE.search(out)
        only_fx, rest_fx = fixture_only_failures(out)
        if smoke_ok is not None and rc == 0:
            check(True, "产物 %s（默认形态）：`SHELL-SMOKE ok … failed=0` 且退出码 0" % rel)
        elif not rest_fx and only_fx:
            # ★ 用户 2026-10-20 定性：这些检查**需要插件页夹具**，默认形态拿不到控件 ⇒ 必然 0。
            #   记 **未覆盖**（NOTE），不是产品红；④ 抬夹具后照旧全跑。
            check(True, "产物 %s（默认形态）：%d 条**夹具依赖**检查按未覆盖记账（④ 抬夹具后验）"
                  % (rel, len(only_fx)), "例：" + only_fx[0][:80])
            NOTES.append("%s 默认形态：%d 条夹具依赖检查未覆盖（%s）"
                         % (rel, len(only_fx), ", ".join(sorted({f.split(" ")[2] if len(f.split(" ")) > 2 else "?" for f in only_fx}))))
        else:
            check(False,
                  "产物 %s（默认形态）：`SHELL-SMOKE ok … failed=0` 且退出码 0" % rel,
                  "rc=%s 夹具依赖 %d 条 / **其余 %d 条**\n      %s"
                  % (rc, len(only_fx), len(rest_fx), brief(out)))
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
        _t_lift = time.time()
        # ★ 数量必须在**搬之前**数（我第一版放在搬之后 ⇒ 目录已空 ⇒ 恒 0 ⇒ 假红）
        fixture_total = len([p for p in fixture_dir.glob("*") if p.is_file()])
        for item in sorted(fixture_dir.glob("*")):
            if item.is_file():
                target = work / "plugins" / item.name
                shutil.move(str(item), str(target))
                lifted.append(item.name)
        lift_seconds = time.time() - _t_lift
        say("  ④ 显式抬起 %d/%d 个夹具到顶层（%.2fs）：%s"
            % (len(lifted), fixture_total, lift_seconds, ", ".join(lifted)))
        # ★ lift 必须搬完：少一个 ⇒ 现场就不对，别把它当"smoke 飘"
        check(len(lifted) == fixture_total,
              "产物 %s：夹具**全部**抬到顶层（lifted=%d / 目录 %d）" % (rel, len(lifted), fixture_total),
              "少搬 %d 个" % (fixture_total - len(lifted)))
        PHASE[0] = "lifted"
        _t_smoke = time.time()
        rc2, out2 = run_smoke(work)
        smoke_seconds = time.time() - _t_smoke
        pages2 = parse_pages(out2) if out2 else None
        if rc2 is None:
            check(False, "产物 %s（抬夹具形态）：`--smoke` 能跑完" % rel, str(out2))
            return
        smoke_ok2 = SMOKE_OK_RE.search(out2)
        if smoke_ok2 is None or rc2 != 0:
            # ★★ 用户 2026-10-20：④ 失败必须**留完整现场**（不只末 40 行），并保留临时目录。
            diag = ROOT / "build" / "logs" / "pkg_pages_phase4_fail.txt"
            try:
                diag.parent.mkdir(parents=True, exist_ok=True)
                plug_line = next((l for l in out2.splitlines() if "PKG-PLUGINS" in l), "<无>")
                pages_lines = [l for l in out2.splitlines() if "PLUGIN-PAGES" in l]
                fails4 = [l for l in out2.splitlines() if "SHELL-SMOKE fail" in l]
                diag.write_text(
                    "phase=4\npkg=%s\nlifted=%d/%d\nfixtures=%s\nrc=%s\nwhen=%s\n"
                    "lift_seconds=%.2f\nsmoke_seconds=%.2f\nwork_dir=%s（故意保留）\n"
                    "%s\n%s\n== SHELL-SMOKE fail（%d 条）==\n%s\n\n== 全量输出 ==\n%s\n"
                    % (pkg, len(lifted), fixture_total, ", ".join(lifted), rc2,
                       time.strftime("%Y-%m-%d %H:%M:%S"), lift_seconds, smoke_seconds, work,
                       plug_line, "\n".join(pages_lines), len(fails4),
                       "\n".join(fails4), out2),
                    encoding="utf-8")
                say("   [诊断] ④ 失败现场：%s（work 保留：%s）" % (diag, work))
                try:
                    art = capture_phase4_artifacts(pkg, out2)
                    say("   [诊断] ④ 现场 artifacts：%s" % art)
                except OSError as exc:
                    say("   [诊断] artifacts 落盘失败：%s" % exc)
                os.environ["XRKIT_KEEP_PKG_PAGES_TMP"] = "1"
                _prune_kept_dirs(3)
            except OSError as exc:
                say("   [诊断] 落盘失败：%s" % exc)
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
    if "--phase4-only" in sys.argv:
        PHASE4_ONLY[0] = True
        sys.argv.remove("--phase4-only")
    if not disk_space_ok(2.0):
        print("PKG-PAGES 未覆盖：临时盘余量不足（清盘后重跑）")
        return 2
    if not repo_write_allowed():
        print("PKG-PAGES 未覆盖：仓库路径仍被安全软件拦截（把仓库目录加入信任区后重跑）")
        return 2
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
            # ★★ 前提检查（P17-181 的取证）：包必须是"打完了的"。
            #   那次 `package-pages` 判的是**上一次被中断的 `_pkg.py` 留在 dist/ 里的半成品**
            #   （版本已 bump、目录已建、流程没跑完）⇒ 抬夹具形态 9 条假红（门面控件 0 个）。
            #   输入不完整 ⇒ rc=2（未覆盖），**不判红、也不重试**。
            why = package_looks_complete(pkg)
            if why:
                say("  PKG-PAGES skip 包不完整（%s）：%s" % (why, pkg.relative_to(ROOT)))
                say("     ↑ 不是判据缺陷：`dist/` 里那份被打包过程改写了一半或被中断 ⇒ 重打一次包再判")
                NOTES.append("包不完整（%s）" % why)
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
