#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""STALKER 工具集 2.x 门禁（对齐 1.x 的"两级门禁"口径，2026-09-12 用户裁定）。

档位：
    --fast     公开 CI：跳过需要外部前提的步骤，跳过项一律记入「本次未覆盖」
    默认       local 全量：能跑的都跑，缺外件记 SKIP + 清单
    --release  发版：任何 SKIP 一律 FAIL，且「未覆盖清单」非空即整体 FAIL

判据分类（1.x 的血账，别搞反）：
    · 仓库自带的门禁文件缺件          → FAIL（"把门禁文件删掉就算绿灯"是最危险的假绿）
    · 外部前提缺件（MSVC / 真机样本） → SKIP，并记入未覆盖清单

用法：
    python tests/run_ci.py [--fast|--release] [--only 子串] [--verbose]
"""

from __future__ import annotations

import argparse
import os
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build" / "msvc-release"
CLI_EXE = BUILD_DIR / "tools" / "cli" / "xrkit_cli.exe"
# Qt6 外壳是**另一个构建目录**：CLI 侧那条"core/engine 脱离 Qt 也能编"的约束因此仍被真实检验
SHELL_BUILD_DIR = ROOT / "build" / "msvc-shell"
SHELL_EXE = SHELL_BUILD_DIR / "src" / "shell" / "xrkit_app.exe"
ENV_CACHE = ROOT / "build" / "msvc_env.txt"

VS_ROOT = Path(os.environ.get("XRKIT_VS", r"E:\Software\Tools\Visual Studio\2022\BuildTools"))
QT_ROOT = Path(os.environ.get("XRKIT_QT", r"E:\Software\Tools\Qt\6.8.3\msvc2022_64"))
VCVARS = VS_ROOT / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
CMAKE = VS_ROOT / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
NINJA_DIR = VS_ROOT / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja"

# 1.x 参照仓库（字库数据表、黄金语料的生成方）。与 tests/golden_*.py 的路径口径一致。
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")

UNCOVERED: list[str] = []
LEDGERS: list[str] = []
FAILURES = 0
_MSVC_ENV: dict[str, str] | None = None


# ── 基础设施 ────────────────────────────────────────────────────────────
# ★★ 2026-10-06（审计处理批·追加 / 假绿批的照见 D3）：修前只给**子进程**打了
#   `PYTHONUTF8/PYTHONIOENCODING`（`run()` 里），**自己**没重编码 stdout ⇒
#   stdout 是管道 + 本地编码不是 UTF-8 时，打印 `✓/✗` 直接 `UnicodeEncodeError`，
#   退 1 且**没有 RESULT 行**（看起来"红但不说哪一步"）。口径：脚本要**自己**保证可跑，
#   不许依赖外部环境变量。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:   # 老 Python / 已重定向的自定义流：退回原行为，不影响判定
    pass


def say(msg: str) -> None:
    print(msg, flush=True)


def load_msvc_env() -> dict[str, str] | None:
    """导出并缓存 MSVC 环境。

    做法与 scripts/build.ps1 一致：`call vcvars64.bat && set > 文件`。
    不在批处理里对同一日志文件反复 `>>` 追加 —— 实测会报"文件被占用"。
    """
    global _MSVC_ENV
    if _MSVC_ENV is not None:
        return _MSVC_ENV
    if not VCVARS.is_file():
        return None
    ENV_CACHE.parent.mkdir(parents=True, exist_ok=True)
    if ENV_CACHE.exists():
        ENV_CACHE.unlink()
    # ★ 必须用 shell=True（即 cmd /c <原样字符串>）：走 list 形式时 subprocess 的
    #   list2cmdline 会给整串再套一层引号，cmd 解析失败 —— 实测 exit=1、无输出、
    #   目标文件根本没生成（而且被 >nul 吞掉，看起来像"vcvars 不存在"）。
    subprocess.run(
        'call "%s" >nul 2>&1 && set > "%s"' % (VCVARS, ENV_CACHE),
        shell=True,
        check=False,
    )
    if not ENV_CACHE.exists():
        return None
    env = dict(os.environ)
    text = ENV_CACHE.read_bytes().decode("mbcs", errors="replace")
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            # ★ 键要**归一成大写**：`set` 输出的是原始大小写（实测是 `Path=` 而不是
            #   `PATH=`），而 `os.environ` 在 Windows 上本身就是大写键 ⇒ 不归一化就会
            #   同时存在 `PATH`（继承来的，**没有** MSVC）与 `Path`（真正的 MSVC 环境）
            #   两个键，`env.get("PATH")` 拿到的是前者 → 找不到 cl.exe（2026-09-24 踩到）。
            env[key.upper()] = value
    env["PATH"] = os.pathsep.join([str(NINJA_DIR), str(QT_ROOT / "bin"), env.get("PATH", "")])
    _MSVC_ENV = env
    return env


def run(cmd: list[str], env: dict[str, str] | None = None, timeout: int = 600) -> tuple[int, str]:
    # ★ 子进程的 Python 必须能打印 ✓/✗ 这类字符：控制台可能是 cp1252/GBK，
    #   裸跑会以 UnicodeEncodeError 把整步打断（1.x 的 CI 踩过同一个坑）。
    child_env = dict(os.environ if env is None else env)
    child_env.setdefault("PYTHONUTF8", "1")
    child_env.setdefault("PYTHONIOENCODING", "utf-8")
    try:
        proc = subprocess.run(
            cmd,
            cwd=str(ROOT),
            env=child_env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as exc:
        partial = (exc.stdout or b"").decode("utf-8", errors="replace")
        return 124, partial + "\n[超时 %ss]" % timeout


def fail(msg: str) -> None:
    global FAILURES
    FAILURES += 1
    say("  FAIL " + msg)


def ok(msg: str) -> None:
    say("  ok   " + msg)


def prereq(what: str, why: str, release: bool) -> bool:
    """外部前提缺件：release 档 FAIL，其余档记 SKIP + 未覆盖清单。"""
    if release:
        fail("缺少外部前提（release 档零容忍）：%s —— %s" % (what, why))
        return False
    UNCOVERED.append("%s（%s）" % (what, why))
    say("  SKIP %s —— %s" % (what, why))
    return True


def ledger(what: str, why: str) -> None:
    """**差异台账**（已知分歧 / 能力缺口）：既不是失败，也**不是"未覆盖"**。

    与 `prereq()` 的分工（★ T-H1 定这条口径时想清楚的）：
      · `prereq` = **外部前提**缺件（参照工具/真机样本不在本机）⇒ release 零容忍；
      · `ledger` = 我们自己**已知且被冻结**的能力缺口（例如判定链第 5 步没移植 chardet 的
        GB2312 探针）—— 同一条分歧在门禁 [16] 判定链那边也是"能力缺口台账"，
        所以这里沿用同一口径，别把两处写成两种性质。
    台账**必须可见**（逐条打印 + 收尾汇总），且**参与脚本自己的判定**：脚本的第三态是
    非零退码，分歧一旦消失或换了形状都会红（见 `tests/golden_font_charset.py` 的
    `KNOWN_DIVERGENCES` 与它的"减法抵消"回归锁）。
    """
    LEDGERS.append("%s（%s）" % (what, why))
    say("  LEDGER %s —— %s" % (what, why))


def package_fact(what: str, why: str, release: bool) -> None:
    """★ 2026-10-10（P17-2，审计 04 F-05 Medium）：**包内端到端事实不许在 release 档退化为台账**。

    事实（修前）：三条包内端到端判据 —— ①「包内平台插件齐备并**从产物真跑**自检」、
    ②「包内 exe 的版本身份」、③「自带 CPython 能起宿主」—— 在没有"带契约声明的新序号包"时
    都走 `ledger()`；而 `LEDGERS` **不进 `UNCOVERED`**、`release` 收尾只判 `UNCOVERED`
    ⇒ **干净检出**（或 `--only` 组合）跑 `run_ci.py --release` 可以打 `RESULT: PASS`，
    而这三条**一次都没真跑**。判据本体确实写在打包链里（每次打包会 throw），但
    "门禁绿"与"这三条成立"之间**没有必然关系** —— 那是又一种假绿。
    现在：**release 档**按"前提缺失"处理（`prereq` ⇒ release 零容忍、FAIL）；
    fast/local 档照旧记**可见的**差异台账（本地开发不该因为"这台机器没打过包"就全红）。
    """
    if release:
        prereq(what, why + "（release 档：包内端到端事实不许退化为台账）", True)
    else:
        ledger(what, why)


def require_repo_file(path: Path, what: str) -> bool:
    """仓库自带的门禁文件缺件 = FAIL，绝不 SKIP。"""
    if path.exists():
        return True
    fail("仓库自带文件缺失（不许静默通过）：%s（%s）" % (path.relative_to(ROOT), what))
    return False


# ── 步骤 ────────────────────────────────────────────────────────────────
def showincludes_prefix() -> str:
    """量出 cl 的 /showIncludes 前缀（理由见 CMakeLists.txt 顶部）。

    只去掉行尾换行：前缀**本身以空格结尾**（"注意: 包含文件: "），不能 strip()。
    """
    rc, out = run([sys.executable, str(ROOT / "tools" / "probe_showincludes_prefix.py"),
                   "--env-file", str(ENV_CACHE)])
    prefix = out.rstrip("\r\n").splitlines()[-1] if out.strip() else ""
    if rc != 0 or not prefix:
        say("  注意：量不到 /showIncludes 前缀，本次不传（头文件依赖自检会判红）")
        return ""
    return prefix


def configure_and_build(build_dir: Path, env: dict, shell: bool) -> bool:
    """configure + build + **头文件依赖效果自检**；返回是否通过（失败已 fail()）。

    `shell=True` 时开 Qt 外壳（`XRKIT_BUILD_SHELL=ON` + Qt 前缀路径）——外壳是**另一个
    构建目录**，这样 CLI 侧那条"core/engine 脱离 Qt 也能编"的约束仍然每次被真实检验。
    """
    args = [str(CMAKE), "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DXRKIT_SHOWINCLUDES_PREFIX=%s" % showincludes_prefix()]
    if shell:
        args += ["-DXRKIT_BUILD_SHELL=ON", "-DCMAKE_PREFIX_PATH=%s" % QT_ROOT.as_posix()]
    rc, out = run(args, env=env)
    if rc != 0:
        fail("configure 失败（%s，退出码 %d）\n%s"
             % (build_dir.name, rc, tail(out, 20)))
        return False
    rc, out = run([str(CMAKE), "--build", str(build_dir)], env=env)
    if rc != 0:
        fail("build 失败（%s，退出码 %d）\n%s" % (build_dir.name, rc, tail(out, 30)))
        return False

    # ★ 头文件依赖的**效果自检**（2026-09-24 实测踩到，别再删）：
    #   机制检查（看 rules.ninja 里前缀对不对）会被码页骗；这里只看**效果** ——
    #   `.ninja_deps` 里到底有没有记下头文件与生成表。没有 ⇒ 改了 .h/.inc 不会重编，
    #   门禁会用旧二进制判绿，这是最危险的一类假绿，必须在这一步就拦住。
    deps = build_dir / ".ninja_deps"
    if not deps.is_file():
        fail("没有 %s：ninja 一条依赖都没记下（头文件/表改动不会触发重编）" % deps)
        return False
    blob = deps.read_bytes()
    need = [b"metrics.h", b"adv_tables.inc", b"eaw_tables.inc", b"encoding.h",
            b"squashfs.h"]
    if shell:
        need += [b"hub.h", b"theme.h"]
    missing = [name for name in need if name not in blob]
    if missing:
        fail("头文件依赖追踪失效（%s）：.ninja_deps 里找不到 %s —— 改了 .h/.inc 不会重编，"
             "门禁会拿旧二进制判绿。检查 tools/probe_showincludes_prefix.py 是否量到前缀、"
             "以及 CMakeLists.txt 顶部那段是否生效。"
             % (build_dir.name, b", ".join(missing).decode()))
        return False
    return True


def step_build(release: bool) -> None:
    say("[1] 构建 core/engine/cli")
    env = load_msvc_env()
    if env is None:
        prereq("MSVC 工具链", "找不到 vcvars64.bat：%s" % VCVARS, release)
        return
    if not require_repo_file(CMAKE, "VS 自带 CMake"):
        return

    # README 约定 #1：`.ps1` 必须带 UTF-8 BOM（PowerShell 5.1 会把**无 BOM** 的脚本按 GBK
    # 读，中文注释与字符串直接读坏、脚本语法报错）。这不是洁癖 —— 编辑工具很容易把 BOM
    # 悄悄抹掉（2026-09-24 实测被抹过一次），所以放在门禁里，别只写在文档里。
    #
    # ★★ 2026-10-06（审计处理批·0-6）：修前这里是
    #   `for ps1 in sorted((ROOT/"scripts").glob("*.ps1")): ...` —— 判据**挂在 glob 上**：
    #   `scripts/` 被删掉、或脚本下沉到子目录，glob 结果为空 ⇒ 循环零次 ⇒
    #   没有 fail、没有 ok、也没进未覆盖清单，判据**静默消失**、门禁照旧全绿
    #   （"把判据文件挪走/删掉就没人管"正是门禁最该拦的那类假绿）。
    #   现在：① 脚本目录本身先校验（缺 = FAIL，不许 SKIP）；
    #         ② glob 结果**显式断言非空**（空集 = 明确红/明确未覆盖，不是"零次循环"）；
    #         ③ release 档按档位判定（其它档记未覆盖，降级仍然可见）。
    scripts_dir = ROOT / "scripts"
    if not scripts_dir.is_dir():
        fail("仓库自带目录缺失：%s（.ps1 的 BOM 判据挂在这里，目录没了判据就消失了）"
             % scripts_dir.relative_to(ROOT))
        return
    ps1_files = sorted(scripts_dir.glob("*.ps1"))
    if not ps1_files:
        if release:
            fail(".ps1 的 UTF-8 BOM 判据没被满足：%s 下一个 *.ps1 都没有（release 零容忍）"
                 % scripts_dir.relative_to(ROOT))
        else:
            UNCOVERED.append(".ps1 UTF-8 BOM 判据（%s 下没有 *.ps1，判据未覆盖）"
                             % scripts_dir.relative_to(ROOT))
            say("  SKIP .ps1 BOM 判据 —— %s 下没有 *.ps1" % scripts_dir.relative_to(ROOT))
        return
    for ps1 in ps1_files:
        if ps1.read_bytes()[:3] != b"\xef\xbb\xbf":
            fail("%s 丢了 UTF-8 BOM（PowerShell 5.1 会按 GBK 读，中文会被读坏）"
                 % ps1.relative_to(ROOT))
            return
    say("  ok  .ps1 UTF-8 BOM 判据覆盖 %d 个脚本（%s）"
        % (len(ps1_files), "、".join(p.name for p in ps1_files)))

    # ★★ B 批（审计 §5.5）：`.rc` 与 `.ps1` 同族 —— 资源脚本里的**中文注释**只有在带 BOM 时
    #   才被 cl/rc 按 UTF-8 读。`assets/app.rc` 修前是 UTF-8 **无 BOM**（首 3 字节 `2f 2f 20`），
    #   却编进 `xrkit_app` 与 `build_gui`；仓库自己在 `tools/gen_winres.py` 里把这条写成**硬断言**
    #   （"无 BOM ⇒ cl.exe 按 ANSI(936) 解码 ⇒ C2001/C1018"），而 app.rc 只是**字节奇偶的巧合**
    #   才编得过，门禁不覆盖它。判据用**显式清单**（不挂 glob：0-6 的教训是"文件没了判据就消失"）。
    for rc_name in ("assets/app.rc", "assets/xrkit_winres.rc"):
        rc_file = ROOT / rc_name
        if not require_repo_file(rc_file, "资源脚本（.rc）"):
            return
        if rc_file.read_bytes()[:3] != b"\xef\xbb\xbf":
            fail("%s 丢了 UTF-8 BOM（cl/rc 链会把中文注释按 GBK 解码 ⇒ C2001/C1018；"
                 "tools/gen_winres.py 已把这条口径写成硬断言）" % rc_name)
            return
    say("  ok  .rc UTF-8 BOM 判据覆盖 2 个资源脚本（app.rc、xrkit_winres.rc）")

    if configure_and_build(BUILD_DIR, env, shell=False):
        ok("configure + build 通过（含头文件依赖追踪自检）")


def step_no_qt_lock(release: bool) -> None:
    say("[2] J5 无 Qt 依赖锁（core/engine 不许 include Qt）")
    script = ROOT / "cmake" / "check_no_qt.cmake"
    if not require_repo_file(script, "J5 锁脚本"):
        return
    env = load_msvc_env() or dict(os.environ)
    rc, out = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)], env=env)
    if rc != 0:
        fail("J5 检查未通过：\n%s" % tail(out, 15))
        return
    ok(tail(out, 1).strip() or "J5 通过")


def step_no_qt_injection(release: bool) -> None:
    say("[3] J5 注入必红（先证这条锁抓得住违规，再信它的绿）")
    probe = ROOT / "src" / "core" / "_j5_probe.cpp"
    script = ROOT / "cmake" / "check_no_qt.cmake"
    env = load_msvc_env() or dict(os.environ)

    rc_before, _ = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)], env=env)
    if rc_before != 0:
        fail("注入前基线就不通过 —— 这条锁现在无论红绿都不可信")
        return
    # ★★ 补充 H/§5.7（B 批）：**逐形态**自证，三种都注入一遍 ——
    #   修前只有前两种能红，`<qstring.h>` 这类**小写私名头整类漏网**
    #   （实测：注入后照打 "J5 OK：74 个文件均无 Qt 依赖"、rc=0）。
    #   "注入必红"这条锁自己也有洞：只注入一种形态，就锁不住第二种形态。
    forms = [
        ("#include <QtCore/QString>\n", "模块式 <QtCore/QString>"),
        ("#include <QString>\n", "类式 <QString>"),
        ("#include <qstring.h>\n", "小写私名式 <qstring.h>（§5.7 修前漏网）"),
    ]
    for text, label in forms:
        try:
            probe.write_text(text, encoding="utf-8")
            rc_inject, out_inject = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)],
                                        env=env)
        finally:
            if probe.exists():
                probe.unlink()
        rc_after, _ = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)], env=env)
        if rc_inject == 0:
            fail("往 core 里塞了 %s 却没变红 —— 这是「永远为真的锁」\n%s"
                 % (label, tail(out_inject, 5)))
            return
        if rc_after != 0:
            fail("删掉注入文件后没恢复绿灯 —— 锁有残留状态（形态 %s）" % label)
            return
        ok("%s：基线绿 → 注入红 → 还原绿" % label)


def step_selftest(release: bool) -> None:
    say("[4] CLI --selftest")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "上一步没产出", release)
        return
    rc, out = run([str(CLI_EXE), "--selftest", "--verbose"])
    summary = [ln for ln in out.splitlines() if ln.startswith("SELFTEST ")]
    if not summary:
        fail("找不到 ASCII 收尾行 SELFTEST（1.x 的教训：别的进程只能解析 ASCII 行）\n%s" % tail(out, 15))
        return
    if rc != 0 or "fail=0" not in summary[-1]:
        fail("自检未全绿：%s\n%s" % (summary[-1], tail(out, 15)))
        return
    ok(summary[-1])


def step_cli_contract(release: bool) -> None:
    """退出码本身就是判据：0=通过 2=显式跳过 3=用法错误。

    `--bench` 的**能力**在测试侧（[28] bench.py，口径见 docs/2.0.0-bench口径.md）。
    ★★ 2026-10-10（P17-19，审计 04 F-10 Low）：**这段说法以前是假的** —— 原文写
    "CLI 里**不再保留**那个只会 SKIP 的占位动作"，而 `tools/cli/main.cpp` 里
    `action == "--bench"` **仍在**（打印 `BENCH skip reason=engine-not-implemented` 后返回 2），
    `--help` 也照实写着"未实现时输出 BENCH skip ... 并返回 2"。现状与原文矛盾，属"文档比代码响"。
    现在的口径（三件事分清，别再混）：
      · CLI 的 `--bench` = **显式 SKIP 占位**（rc=2，理由可见）—— 保留它是**有意的**：CLI 是能力全集，
        占位能让"没这个能力"这件事**说出来**而不是静默少一条；
      · 但**绝不许**把它塞进 `UNCOVERED`（那会把 release 档顶住 ⇒ 发版永远过不了，
        这正是原文想防的事）——下面那条判据就是钉这个；
      · bench 的**真实能力**判据在 [28] bench.py（真跑三条 I/O 路径计时）。
    绝不允许把"没实现"混成 0。
    """
    say("[5] CLI 退出码契约")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return

    expectations = [
        (["--version"], 0, "", "--version"),
        (["--selftest"], 0, "SELFTEST", "--selftest"),
        # 主题令牌已实现（M2 前置）：必须是**真检查**（0）而不是 skip —— 两套模式 × 颜色/
        # 字体/px/user.ltx 语义都在里面逐条验
        (["--themecheck"], 0, "THEMECHECK ok", "--themecheck"),
        (["--dump"], 3, "DUMP error", "--dump 缺路径 → 用法错误"),
        # --dump 已实现：文件不存在必须是**失败并报错**，不许退化成 skip（那会假装"没做"）
        (["--dump", "__not_exist__.sq"], 1, "DUMP error",
         "--dump 不存在的文件 → 失败并报错"),
        (["--pack-sqfs"], 3, "PACK error",
         "--pack-sqfs 缺参数 → 用法错误"),
        (["--decode-text"], 3, "DECODE error",
         "--decode-text 缺参数 → 用法错误"),
        (["--extract-text"], 3, "TEXT error",
         "--extract-text 缺参数 → 用法错误"),
        (["--sniff"], 3, "SNIFF error",
         "--sniff 缺参数 → 用法错误"),
        (["--glyph-dump"], 3, "GLYPH error",
         "--glyph-dump 缺参数 → 用法错误"),
        # ★★ 补充 G3【S1】（B 批）：守卫判的是**过滤前**的 `operands`，而真正取下标的是
        #   过滤掉 `--no-full-cyrillic` 之后的 `args` ⇒ 只剩那个开关时 `args[0]` 空取下标
        #   （修前实测 exit=0xC0000005 访问违例 ×3/3）。帮助行把 `<xml目录>` 与
        #   `[--no-full-cyrillic]` 并列展示，很容易这么敲 ⇒ 必须判**用法错误**。
        (["--extract-chars", "--no-full-cyrillic"], 3, "CHARSET error",
         "--extract-chars 只剩 --no-full-cyrillic → 用法错误（不许空取下标）"),
    ]
    for argv, want, marker, label in expectations:
        rc, out = run([str(CLI_EXE)] + argv)
        if rc != want or (marker and marker not in out):
            fail("%s 期望 exit=%d 且输出含 %r，实际 exit=%d 输出=%r"
                 % (label, want, marker, rc, out.strip()[:120]))
        else:
            ok("%s → exit=%d" % (label, rc))

    # 尚未实现的动作：必须显式 SKIP（2），并且被记入未覆盖清单
    for argv, marker, label in (
    ):
        rc, out = run([str(CLI_EXE)] + argv)
        if rc != 2 or marker not in out:
            fail("%s 期望 exit=2 且输出含 %r，实际 exit=%d 输出=%r" % (label, marker, rc, out.strip()))
        else:
            UNCOVERED.append("%s（尚未实现，CLI 显式 SKIP）" % label)
            ok("%s → exit=2（已记入未覆盖）" % label)

    # ── ★★ P17-19（2026-10-10，审计 04 F-10 Low）：**占位动作不许顶住 release** ──────────
    #   事实：CLI 至今保留 `--bench` 占位（rc=2 + `BENCH skip`），而这段文档原来说它"已移除"。
    #   保留占位是**有意**的（CLI 是能力全集，"没这个能力"要说出来）；真正要防的是
    #   "把占位动作塞进 UNCOVERED" —— 那会让 release 档永远过不了（本轮之前的注释记的正是这个坑）。
    #   判据两半：① `--bench` 必须仍然**如实**给出 skip（rc=2 ＋ `BENCH skip` 行，不是 0、不是崩）；
    #             ② 它**不许**出现在 UNCOVERED 里（占位动作不是"未覆盖的能力"——
    #                能力判据在 [28] bench.py）。判红办法：在收尾处 `UNCOVERED.append("bench …")` ⇒ 立刻红。
    rc_bench, out_bench = run([str(CLI_EXE), "--bench"])
    bench_in_uncovered = [u for u in UNCOVERED if "bench" in u.lower()]
    if rc_bench != 2 or "BENCH skip" not in out_bench:
        fail("`--bench` 占位必须如实 SKIP（04 F-10）：期望 rc=2 ＋ `BENCH skip`，实际 rc=%d 输出=%r"
             % (rc_bench, out_bench.strip()[:120]))
    elif bench_in_uncovered:
        fail("占位动作顶住了未覆盖清单（04 F-10）：UNCOVERED 里出现 %r —— 那会让 release 永远过不了；"
             "bench 的能力判据在 [28] bench.py" % bench_in_uncovered)
    else:
        ok("`--bench` 占位如实 SKIP（rc=2）且**不**进未覆盖清单（04 F-10：能力判据在 [28] bench.py）")

    # ── ★★ P17-26（2026-10-10，审计 04 F-06 Low-Medium）：**写盘动作不许静默截断已有文件** ──
    #   口径：输出已存在 ⇒ **拒绝 + 可读理由 + 退出码 3（用法错误）**；确认要覆盖请加 `--force`
    #   （与 `--patch-apply` 同款语义）。
    #   为什么第一波只做 `--lzhuf`：P17-25/P26 的"动作 → 写盘点"映射显示，**只有 `--lzhuf` 与
    #   `--decl-fix` 没有被任何门禁步骤使用** ⇒ 先在这两处落口径，不会打到自检；其余动作
    #   （--extract/--pack-db/--font-render/--extract-chars/--resample-dump/--sniff/--decode-text/
    #   --codec-lossy）都有门禁在用，要连带改测试，下一波逐个来。
    #   判据三段：① 首次写 ⇒ rc=0；② 同路径再写 ⇒ **rc=3** 且理由含 `--force`；③ 带 `--force` ⇒ rc=0。
    guard_dir = ROOT / "build" / "logs" / "cli_overwrite_guard"
    if guard_dir.exists():
        shutil.rmtree(guard_dir)
    guard_dir.mkdir(parents=True, exist_ok=True)
    g_in = guard_dir / "in.bin"
    g_out = guard_dir / "out.lzh"
    g_in.write_bytes(b"hello-guard")
    rc1, out1 = run([str(CLI_EXE), "--lzhuf", "encode", str(g_in), str(g_out)])
    rc2, out2 = run([str(CLI_EXE), "--lzhuf", "encode", str(g_in), str(g_out)])
    rc3, out3 = run([str(CLI_EXE), "--lzhuf", "encode", str(g_in), str(g_out), "--force"])
    _, help_text = run([str(CLI_EXE), "--help"])
    if rc1 != 0 or "LZHUF ok" not in out1:
        fail("写盘守卫的**前提**不成立（04 F-06）：首次写 --lzhuf 期望 rc=0 ＋ `LZHUF ok`，实际 rc=%d %r"
             % (rc1, out1.strip()[:100]))
    elif rc2 != 3 or "--force" not in out2:
        fail("写盘动作静默截断（04 F-06）：同路径再写期望 rc=3 ＋ 理由含 `--force`，实际 rc=%d %r"
             % (rc2, out2.strip()[:120]))
    elif rc3 != 0:
        fail("`--force` 没能覆盖（04 F-06）：期望 rc=0，实际 rc=%d %r" % (rc3, out3.strip()[:120]))
    elif "--force" not in help_text.split("--lzhuf", 1)[-1].split("\n")[0:4].__str__():
        fail("`--force` 没写进 `--help`（04 F-06）：受影响动作的帮助行必须提到它（否则用户只知道被拒、"
             "不知道怎么办）")
    else:
        ok("写盘动作不静默截断（04 F-06 第一波 `--lzhuf`：已存在⇒rc=3；`--force`⇒覆盖；帮助行也写了）")

    # ── ★★ P17-27（04 F-06 第一波收尾）：把同一口径搬到 `--decl-fix` ──────────────────────
    #   为什么是它：P17-26 的"动作 → 写盘点"映射显示 `--decl-fix` 是**第二个没被任何门禁步骤使用**
    #   的动作（另一个是 `--lzhuf`）⇒ 先落口径不会打到自检。判据与 `--lzhuf` 逐段同形。
    d_in = guard_dir / "decl_in.xml"
    d_out = guard_dir / "decl_out.xml"
    d_in.write_bytes(b'<?xml version="1.0" encoding="windows-1251"?>\n<a>\u041f</a>\n')
    d1, do1 = run([str(CLI_EXE), "--decl-fix", str(d_in), "utf-8", str(d_out)])
    d2, do2 = run([str(CLI_EXE), "--decl-fix", str(d_in), "utf-8", str(d_out)])
    d3, do3 = run([str(CLI_EXE), "--decl-fix", str(d_in), "utf-8", str(d_out), "--force"])
    if d1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --decl-fix）：首次写期望 rc=0，实际 rc=%d %r"
             % (d1, do1.strip()[:100]))
    elif d2 != 3 or "--force" not in do2:
        fail("写盘动作静默截断（04 F-06 / --decl-fix）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (d2, do2.strip()[:120]))
    elif d3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --decl-fix）：期望 rc=0，实际 rc=%d %r"
             % (d3, do3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第一波收尾 `--decl-fix`，与 `--lzhuf` 同款）")

    # ── ★★ P17-29（04 F-06 第二波第一个动作）：`--resample-dump` 同款守卫 ────────────────
    #   先查（P17-28 扫描 + 实测）：它**只有 1 个真使用点**（`tests/golden_font_resample.py:86`，
    #   另一处是注释假阳性），但那一处在**同一路径上循环复用** ⇒ 已连带给它补 `--force`。
    #   判据与 `--lzhuf`/`--decl-fix` 逐段同形。
    r_src = guard_dir / "rs_src.raw"
    r_dst = guard_dir / "rs_dst.raw"
    r_src.write_bytes(bytes(4 * 4))          # 4x4 全 0
    ra, roa = run([str(CLI_EXE), "--resample-dump", str(r_src), "4", "4", "2", "2", str(r_dst)])
    rb, rob = run([str(CLI_EXE), "--resample-dump", str(r_src), "4", "4", "2", "2", str(r_dst)])
    rc_, roc = run([str(CLI_EXE), "--resample-dump", str(r_src), "4", "4", "2", "2", str(r_dst),
                    "--force"])
    if ra != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --resample-dump）：首次写期望 rc=0，实际 rc=%d %r"
             % (ra, roa.strip()[:100]))
    elif rb != 3 or "--force" not in rob:
        fail("写盘动作静默截断（04 F-06 / --resample-dump）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (rb, rob.strip()[:120]))
    elif rc_ != 0:
        fail("`--force` 没能覆盖（04 F-06 / --resample-dump）：期望 rc=0，实际 rc=%d %r"
             % (rc_, roc.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--resample-dump`：已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-30（04 F-06 第二波）：`--font-render`（**两个产物**）同款守卫 ──────────────
    #   它一次写 DDS + ini ⇒ 判据要证明**两份都判**（先在 DDS 上撞、再在 ini 上撞）。
    fr_chars = guard_dir / "fr_chars.txt"
    fr_chars.write_text("0041\n", encoding="ascii")
    fr_prefix = str(guard_dir / "fr_out")
    fr_font_path = "C:/Windows/Fonts/simsun.ttc"
    fr_argv = [str(CLI_EXE), "--font-render", fr_font_path, "16", "0", "8888", str(fr_chars),
               fr_prefix]
    f1, fo1 = run(fr_argv)
    f2, fo2 = run(fr_argv)
    f3, fo3 = run(fr_argv + ["--force"])
    if f1 != 0:
        # 字体/环境前置不满足：只记台账（别的步骤也这么处理），不判红
        ledger("`--font-render` 守卫判据", "前置不满足：%s" % fo1.strip()[:100])
    elif f2 != 3 or "输出已存在" not in fo2:
        fail("写盘动作静默截断（04 F-06 / --font-render）：产物已存在时期望 rc=3 ＋ `输出已存在`，"
             "实际 rc=%d %r" % (f2, fo2.strip()[:120]))
    elif f3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --font-render）：期望 rc=0，实际 rc=%d %r"
             % (f3, fo3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--font-render`：DDS+ini 两份都判；`--force`⇒覆盖）")

    # ── ★★ P17-31（04 F-06 第二波）：`--sniff` 同款守卫 ──────────────────────────────────
    #   ★ 改件提示：本动作与 `--decode-text` 的写盘点**文本完全相同**（都从 `operands[1]` 截断打开），
    #     所以改件/判据都要带动作名做锚点（第一版脚本就是在这里被断言拦下的）。
    sn_in = guard_dir / "sniff_in.bin"
    sn_out = guard_dir / "sniff_out.bin"
    sn_in.write_bytes("привет".encode("cp1251"))
    s1, so1 = run([str(CLI_EXE), "--sniff", str(sn_in), str(sn_out)])
    s2, so2 = run([str(CLI_EXE), "--sniff", str(sn_in), str(sn_out)])
    s3, so3 = run([str(CLI_EXE), "--sniff", str(sn_in), str(sn_out), "--force"])
    if s1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --sniff）：首次写期望 rc=0，实际 rc=%d %r"
             % (s1, so1.strip()[:100]))
    elif s2 != 3 or "--force" not in so2:
        fail("写盘动作静默截断（04 F-06 / --sniff）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (s2, so2.strip()[:120]))
    elif s3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --sniff）：期望 rc=0，实际 rc=%d %r"
             % (s3, so3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--sniff`：已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-32（04 F-06 第二波）：`--decode-text` 同款守卫 ───────────────────────────
    #   ★ 它的写盘点与 `--sniff` **文本完全相同** ⇒ 改件/判据都带动作名（DECODE/SNIFF）锚定。
    #   受影响的门禁步骤：`golden-text-decode`（在同一 `decoded.txt` 上反复写）已连带补 `--force`；
    #   `codec-strict` 与 `bench` 实测**不受影响**（各自用不同输出路径）。
    de_in = guard_dir / "decode_in.bin"
    de_out = guard_dir / "decode_out.txt"
    de_in.write_bytes("привет".encode("cp1251"))
    e1, eo1 = run([str(CLI_EXE), "--decode-text", str(de_in), str(de_out)])
    e2, eo2 = run([str(CLI_EXE), "--decode-text", str(de_in), str(de_out)])
    e3, eo3 = run([str(CLI_EXE), "--decode-text", str(de_in), str(de_out), "--force"])
    if e1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --decode-text）：首次写期望 rc=0，实际 rc=%d %r"
             % (e1, eo1.strip()[:100]))
    elif e2 != 3 or "--force" not in eo2:
        fail("写盘动作静默截断（04 F-06 / --decode-text）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (e2, eo2.strip()[:120]))
    elif e3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --decode-text）：期望 rc=0，实际 rc=%d %r"
             % (e3, eo3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--decode-text`：已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-33（04 F-06 第二波）：`--codec-lossy` 同款守卫（输出下标是**动态**的）────────
    #   `--codec-lossy <decode|encode> <编码> <输入> [strict] <输出>`：`strict` 时输出是第 5 个操作数、
    #   否则第 4 个 ⇒ 守卫用同一个 `out_arg` 下标（别写死）。受影响的门禁步骤 `codec-strict`
    #   （在同一个 out.bin 上反复写各用例）已连带补 `--force`（4 处调用）。
    cl_in = guard_dir / "codec_in.bin"
    cl_out = guard_dir / "codec_out.bin"
    cl_in.write_bytes("привет".encode("cp1251"))
    c1, co1 = run([str(CLI_EXE), "--codec-lossy", "decode", "windows-1251", str(cl_in),
                   str(cl_out)])
    c2, co2 = run([str(CLI_EXE), "--codec-lossy", "decode", "windows-1251", str(cl_in),
                   str(cl_out)])
    c3, co3 = run([str(CLI_EXE), "--codec-lossy", "decode", "windows-1251", str(cl_in),
                   str(cl_out), "--force"])
    c4, co4 = run([str(CLI_EXE), "--codec-lossy", "decode", "windows-1251", str(cl_in), "strict",
                   str(cl_out)])
    if c1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --codec-lossy）：首次写期望 rc=0，实际 rc=%d %r"
             % (c1, co1.strip()[:100]))
    elif c2 != 3 or "--force" not in co2:
        fail("写盘动作静默截断（04 F-06 / --codec-lossy）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (c2, co2.strip()[:120]))
    elif c3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --codec-lossy）：期望 rc=0，实际 rc=%d %r"
             % (c3, co3.strip()[:120]))
    elif c4 != 3:
        fail("`--codec-lossy` 的**动态下标**没判对（04 F-06）：带 `strict` 时输出是第 5 个操作数、"
             "那份已存在 ⇒ 期望 rc=3，实际 rc=%d %r" % (c4, co4.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--codec-lossy`：已存在⇒rc=3（含 strict 的动态下标）；"
           "`--force`⇒覆盖）")

    # ── ★★ P17-34（04 F-06 第二波）：`--extract-chars` 同款守卫 ────────────────────────
    #   ★ 本动作的写盘点用的是 `args[1]`（`args` 与 `operands` 的过滤规则不同，别混）。
    #   受影响的步骤 `golden-font-charset`（在固定前缀 `ours_*.txt` 上反复写）已连带补 `--force`（2 处）。
    ec_dir = guard_dir / "chars_dir"
    ec_dir.mkdir(parents=True, exist_ok=True)
    (ec_dir / "a.xml").write_text("<config><text>Привет</text></config>\n", encoding="cp1251")
    ec_out = guard_dir / "chars_out.txt"
    x1, xo1 = run([str(CLI_EXE), "--extract-chars", str(ec_dir), str(ec_out)])
    x2, xo2 = run([str(CLI_EXE), "--extract-chars", str(ec_dir), str(ec_out)])
    x3, xo3 = run([str(CLI_EXE), "--extract-chars", str(ec_dir), str(ec_out), "--force"])
    if x1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --extract-chars）：首次写期望 rc=0，实际 rc=%d %r"
             % (x1, xo1.strip()[:100]))
    elif x2 != 3 or "--force" not in xo2:
        fail("写盘动作静默截断（04 F-06 / --extract-chars）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (x2, xo2.strip()[:120]))
    elif x3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --extract-chars）：期望 rc=0，实际 rc=%d %r"
             % (x3, xo3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--extract-chars`：已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-35（04 F-06 口径件）：`--convert-batch` **原地改写没开 `--backup` 要吵一句** ──
    #   本动作**不套**"输出已存在即拒"（它就是批量写产物/原地转换，一刀切会把功能打死）；
    #   `--backup` 才是它的保护手段（默认 on，且**只在原地转换时生效** —— 见 batch.h:175）。
    #   判据两半：① 原地 + `--no-backup` ⇒ 必须打 warn；② 原地 + 默认(`--backup`) ⇒ 不许打。
    #   ★ 口径说明与两种更硬的候选（要求显式 `--force` / 直接拒绝）见
    #     `P17-35先查_convert-batch口径_20261010.md`，等拍板再动。
    cb_dir = guard_dir / "cb_dir"
    cb_dir.mkdir(parents=True, exist_ok=True)
    (cb_dir / "a.txt").write_text("hello", encoding="ascii")
    cb1, cbo1 = run([str(CLI_EXE), "--convert-batch", str(cb_dir), str(cb_dir),
                       "utf-8", "utf-8", "--ext", ".txt", "--no-backup"])
    _, cbo2 = run([str(CLI_EXE), "--convert-batch", str(cb_dir), str(cb_dir),
                      "utf-8", "utf-8", "--ext", ".txt"])
    if "CONVERTBATCH warn=原地改写且未开 --backup" not in cbo1:
        fail("原地改写没开 `--backup` 必须吵一句（04 F-06 / --convert-batch）：没看到 warn 行；"
             "实际输出 %r" % cbo1.strip()[:140])
    elif "CONVERTBATCH warn=" in cbo2:
        fail("默认（`--backup` 开）时不该吵（04 F-06 / --convert-batch）：实际 %r"
             % cbo2.strip()[:140])
    else:
        ok("原地改写无备份会被点名（04 F-06 口径件 `--convert-batch`：--no-backup⇒warn；"
           "默认有备份⇒不吵）")

    # ── ★★ P17-36（2026-10-10，审计 04 F-12 Info）：**注释里不许写死构建计数的当前值** ──
    #   事实：`version.h` 的注释曾写「（M0 尚为 0）」，而 `kBuildCount` 早就是三位数 ⇒
    #   「写死当前值」的注释必然过期。判据两半：① 不许再出现那种写死；
    #   ② 必须留下「由打包器就地改写」与「历史断号（17/107）别补」这两层信息（防后人乱补号）。
    vk_src = (ROOT / "include" / "xrkit" / "core" / "version.h").read_text(
        encoding="utf-8", errors="replace")
    if "尚为 0" in vk_src:
        fail("version.h 注释又把构建计数的当前值写死了（04 F-12）：这种注释必然过期 ⇒ "
             "改成「由打包器就地改写」的说法，别写数字")
    elif "就地改写" not in vk_src or "历史断号" not in vk_src:
        fail("version.h 注释缺关键信息（04 F-12）：要同时说明「由打包器就地改写」与「历史断号」"
             "（后者防后人去补 17/107 两个号）")
    else:
        ok("构建计数注释不写死当前值（04 F-12：改说「由打包器就地改写」，并注明 17/107 是历史断号别补）")

    # ── ★★ P17-38（2026-10-10，审计 05 PLG-09 ②） ──
    #   事实（修前）：`loader.cpp` 的目录扫描里写的是 `if (!it->is_regular_file(ec)) continue;`
    #   —— stat 失败会把 `ec` 置位，`continue` 之后循环头先 `it.increment(ec)`、紧接着
    #   `if (ec) break;` 就**提前结束整次扫描** ⇒ 后续插件文件**静默漏载**（一次 stat 失败＝后面全不扫）。
    #   判据：该扫描必须用**独立的 stat_ec**，不许把迭代器的 `ec` 交给 `is_regular_file`。
    loader_src = (ROOT / "src" / "engine" / "plugin" / "loader.cpp").read_text(
        encoding="utf-8", errors="replace")
    # ★ 先剥注释再查：本判据自己的说明里就引用了那段旧代码（`is_regular_file(ec)`），
    #   不剥注释会把注释里的引用当成"代码还在"（P17-8 的 --help 锁踩过同款）。
    loader_code = active_code(loader_src)
    if "is_regular_file(ec)" in loader_code:
        fail("插件目录扫描会因一次 stat 失败就静默停扫（05 PLG-09 ②）：还在把迭代器的 ec "
             "交给 is_regular_file ⇒ 必须换成独立的 stat_ec")
    elif "stat_ec" not in loader_code:
        fail("插件目录扫描缺独立的 stat_ec（05 PLG-09 ②）")
    else:
        ok("插件扫描不因 stat 失败停扫（05 PLG-09 ②：stat 用独立 stat_ec，迭代器的 ec 只反映迭代本身）")

    # ── ★★ P17-39（2026-10-10，审计 05 PLG-09 ③④）：两处纪律/概念区分必须在源码现场 ──
    #   ③ `unload_all()` 只 FreeLibrary、不清注册表 ⇒ 误用会回调悬垂；
    #   ④ `*_facade_available()`（整体）与 `available(method)`（per-method）同名不同义。
    ldr2 = (ROOT / "src" / "engine" / "plugin" / "loader.cpp").read_text(
        encoding="utf-8", errors="replace")
    efc = (ROOT / "src" / "engine" / "plugin" / "engine_facade.cpp").read_text(
        encoding="utf-8", errors="replace")
    if '回调悬垂' not in ldr2:
        fail("unload_all 的纪律说明没了（05 PLG-09 ③）：必须写清只 FreeLibrary、不清注册表与误用会回调悬垂")
    elif '两个 available* 不是一回事' not in efc:
        fail("两个 available* 概念的区分说明没了（05 PLG-09 ④）：整体可用性 vs per-method 必须写清")
    else:
        ok("插件纪律与概念区分都在现场（05 PLG-09 ③④：unload_all 的回调悬垂风险 + 两个 available* 的取舍）")

    # ── ★★ P17-41（2026-10-10，审计 03 F-05）：插件槽位重渲染必须**真销毁**上一轮的下拉 ──
    #   事实（修前）：`PageBase::clear_plugin_option_combos()` 只有一行 `plugin_option_combos_.clear();`
    #   —— **只摘登记、不删控件**；而 Hub 重渲染在「该 area 没有下拉」时会新建并 addWidget 到同一布局
    #   ⇒ 每重渲染一次多留一套控件（线性累积）。判据：该函数体里必须有 `removeWidget(` 与 `deleteLater()`。
    pb_cpp = (ROOT / "src" / "shell" / "pages" / "page_base.cpp").read_text(
        encoding="utf-8", errors="replace")
    fn_at = pb_cpp.find("void PageBase::clear_plugin_option_combos()")
    if fn_at < 0:
        fail("找不到 clear_plugin_option_combos 的定义（03 F-05）：判据失效，先修判据本身")
    else:
        fn_body = pb_cpp[fn_at:pb_cpp.find(chr(10) + "}", fn_at)]
        if "removeWidget(" not in fn_body or "deleteLater()" not in fn_body:
            fail("重渲染只摘登记不删控件（03 F-05）：clear_plugin_option_combos 里缺 removeWidget/deleteLater"
                 " ⇒ 每渲染一次就多留一套下拉")
        else:
            # ★★ P17-42（03 F-05 第二条累积路径）：**按钮**那条也要真销毁（且只动 plugin_ 前缀的）。
            ab_at = pb_cpp.find("void PageBase::clear_plugin_action_buttons()")
            hub_src = (ROOT / "src" / "shell" / "hub.cpp").read_text(encoding="utf-8",
                                                                   errors="replace")
            if ab_at < 0:
                fail("缺少 clear_plugin_action_buttons（03 F-05 第二条）：Hub 重渲染前必须先销毁"
                     "上一轮的插件按钮")
            else:
                ab_body = pb_cpp[ab_at:pb_cpp.find(chr(10) + "}", ab_at)]
                if "removeWidget(" not in ab_body or "deleteLater()" not in ab_body:
                    fail("插件按钮重渲染只加不清（03 F-05 第二条）：clear_plugin_action_buttons "
                         "里缺 removeWidget/deleteLater")
                elif "plugin_" not in ab_body:
                    fail("插件按钮清理必须以 objectName 前缀 plugin_ 为界（03 F-05 第二条）："
                         "不许连内置动作一起删")
                elif "clear_plugin_action_buttons()" not in hub_src:
                    fail("Hub 重渲染前没调 clear_plugin_action_buttons（03 F-05 第二条）："
                         "按钮会一轮轮累积")
                else:
                    ok("插件槽位重渲染两条都不累积（03 F-05：下拉与按钮都在重渲染前真销毁，"
                       "且只动 plugin_ 前缀的那些）")

    # ── ★★ P17-45（2026-10-10，零碎批）：hub.cpp 的插件页 builder 必须被 try/catch 兜住 ──
    #   事实（修前）：hub.cpp **全文无 try/catch** —— 原生 builder 一抛异常就穿过 Qt 的页面构造
    #   路径往上冒（Qt 不接异常 ⇒ 半成品页面甚至进程终止）。判据：builder 那一调必须在 try 里。
    hub_src2 = (ROOT / "src" / "shell" / "hub.cpp").read_text(encoding="utf-8", errors="replace")
    mk = hub_src2.find("return reinterpret_cast<QWidget*>(builder(parent));")
    if mk < 0:
        fail("找不到插件页 builder 的调用点（零碎批）：判据失效，先修判据本身")
    elif hub_src2.rfind("try {", 0, mk) < 0 or hub_src2.rfind("catch", 0, mk) < 0:
        fail("插件页 builder 的异常会穿进 UI（零碎批）：那一调前面没有 try/catch")
    else:
        ok("插件页 builder 异常被兜住（零碎批：hub.cpp 的 builder 调用在 try 里，异常只记账、不穿进 UI）")

    # ── ★★ P17-47（零碎批）：`stk_api` 的「声明 ↔ 赋值」对账（审计：10 个注册点无表对账）──
    #   做法：从 abi.h 的 stk_api 结构体抽 `(*name)` 成员，逐个查宿主里有没有赋值；
    #   抽到的数量 < 8 也判红（解析器与源码脱节 = 这条锁变成永远绿的摆设）。解析前先剥注释。
    abi_txt = (ROOT / "include" / "xrkit" / "plugin" / "abi.h").read_text(
        encoding="utf-8", errors="replace")
    api_at = abi_txt.find("typedef struct stk_api {")
    if api_at < 0:
        fail("abi.h 里找不到 stk_api 定义（零碎批）：判据失效，先修判据本身")
    else:
        api_body = active_code(abi_txt[api_at:abi_txt.find("} stk_api;", api_at)])
        members = re.findall(r"\(\*(\w+)\)\s*\(", api_body)
        hosts = ""
        for rel in ("src/engine/plugin/abi_host.cpp", "src/engine/plugin/loader.cpp",
                    "tools/cli/main.cpp"):
            hosts += (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        if len(members) < 8:
            fail("stk_api 只解析出 %d 个注册点（零碎批）：与预期（≥8）不符" % len(members))
        else:
            missing = [m for m in members if not re.search(r"\.\s*" + re.escape(m) + r"\s*=", hosts)]
            if missing:
                fail("stk_api 有声明却没人赋值（零碎批）：%s" % "、".join(missing))
            else:
                ok("stk_api 声明与赋值对账（零碎批：%d 个注册点全部在宿主里赋值）" % len(members))

    # ── ★★ P17-51（接口契约批「标签换行」）：插件 `ui_label` 必须能换行 ──
    #   修前没有 setWordWrap ⇒ 长文案被截断成一行（1.x 的 TkUiFactory.label 按宽度折行）。
    pui = (ROOT / "src" / "shell" / "plugin_ui.cpp").read_text(encoding="utf-8", errors="replace")
    lab_at = pui.find("stk_widget* ui_label(")
    if lab_at < 0:
        fail("找不到 ui_label 的实现（接口契约批）：判据失效，先修判据本身")
    else:
        lab_body = pui[lab_at:pui.find(chr(10) + "}", lab_at)]
        if "setWordWrap(true)" not in lab_body:
            fail("插件标签不会换行（接口契约批）：ui_label 里缺 setWordWrap(true) ⇒ 长文案被截断成一行")
        else:
            ok("插件标签能换行（接口契约批：ui_label 设了 setWordWrap(true)，与 hub 的提示标签一致）")

    # ── ★★ P17-55（04 F-06 第二波·收官）：`--extract` 同款写盘守卫 ────────────────
    #   受影响的 6 步已逐个处理（golden-extract / golden-db / db-11xx-dirlock /
    #   golden-sqfs-pack / golden-sqfs-pack-real / sqfs-pack-criterion ⇒ 逐处精确补了 --force）。
    #   ★ 教训：这 6 步**不能**靠通配批量脚本乱补（P17-50 试过、5 步仍红）——
    #     必须按**真实调用文本**逐处替换（本批这么做，一次全绿）。
    ex_src = guard_dir / "ex_src"
    ex_src.mkdir(parents=True, exist_ok=True)
    (ex_src / "a.txt").write_text("hello", encoding="ascii")
    ex_pack = guard_dir / "ex_pack.db"
    run([str(CLI_EXE), "--pack-db", str(ex_pack), "xdb", str(ex_src), "--force"])
    ex_out = guard_dir / "ex_out.bin"
    e1, eo1 = run([str(CLI_EXE), "--extract", str(ex_pack), "a.txt", str(ex_out), "xdb", "--force"])
    e2, eo2 = run([str(CLI_EXE), "--extract", str(ex_pack), "a.txt", str(ex_out), "xdb"])
    e3, eo3 = run([str(CLI_EXE), "--extract", str(ex_pack), "a.txt", str(ex_out), "xdb", "--force"])
    if e1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --extract）：首次写期望 rc=0，实际 rc=%d %r"
             % (e1, eo1.strip()[:100]))
    elif e2 != 3 or "--force" not in eo2:
        fail("写盘动作静默截断（04 F-06 / --extract）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (e2, eo2.strip()[:120]))
    elif e3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --extract）：期望 rc=0，实际 rc=%d %r"
             % (e3, eo3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--extract`：已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-56（接口契约批·签名交叉禁令）：**插件侧只许走 `api.*`**（反向那一半）──
    #   已有：`check_official_pages_contract.py` 管**官方页 → 引擎头**方向的棘轮。
    #   本锁补**反向**：`plugins/**/*.py` 不许出现 `PageServices` / `xrkit/engine` / `import xrkit`
    #   （插件只能经宿主给的 `api.*` 表；一旦直连引擎头，C ABI 表就形同虚设）。
    #   ★ 数量下限自检：扫到的插件文件数 < 3 或 `api.` 用法 < 20 也判红 ——
    #     否则插件目录一改名/清空，这条锁就变成「永远绿」的摆设。
    plugin_pys = sorted((ROOT / "plugins").rglob("*.py"))
    if len(plugin_pys) < 3:
        fail("插件侧交叉禁令锁的前提不成立（接口契约批）：只扫到 %d 个 plugins/**/*.py"
             % len(plugin_pys))
    else:
        bad = []
        api_uses = 0
        for py in plugin_pys:
            src = py.read_text(encoding="utf-8", errors="replace")
            api_uses += src.count("api.")
            for needle in ("PageServices", "xrkit/engine", "import xrkit", "from xrkit"):
                if needle in src:
                    bad.append("%s → %s" % (py.name, needle))
        if bad:
            fail("插件侧出现跨用（接口契约批）：%s —— 插件只许走 api.* 表" % "；".join(bad))
        elif api_uses < 20:
            fail("插件侧 api.* 用法只数到 %d（< 20）⇒ 判据与现状脱节，先修判据本身" % api_uses)
        else:
            ok("插件侧只许走 api（接口契约批：%d 个插件文件、%d 处 api.* 用法，无 PageServices/引擎头交叉）"
               % (len(plugin_pys), api_uses))

    # ── ★★ P17-57（接口契约批）：`TreeNodeSpec` 必须带上 `tooltip` 与 `matched` ──
    #   口径（用户 2026-10-09）：**接口归地基、呈现归表层** —— 字段先运到（工具提示、命中标记），
    #   "命中怎么画（加粗等）"留给 UI 批。三处都要在：结构体声明 / Python 绑定 / 宿主落 item。
    wf = (ROOT / "include" / "xrkit" / "engine" / "plugin" / "widget_facade.h").read_text(
        encoding="utf-8", errors="replace")
    ph = (ROOT / "src" / "engine" / "plugin" / "py_host.cpp").read_text(
        encoding="utf-8", errors="replace")
    ft = (ROOT / "src" / "shell" / "facade_tree.cpp").read_text(
        encoding="utf-8", errors="replace")
    ts_at = wf.find("struct TreeNodeSpec {")
    ts_body = active_code(wf[ts_at:wf.find("};", ts_at)]) if ts_at >= 0 else ""
    # ★ 查**声明形状**（`std::string tooltip;` / `bool matched = ...;`），不是查词 ——
    #   光查 "matched" 会被说明注释里的那个词骗过（本判据第一版就是这么假绿的，红证当场拆穿）。
    tn_missing = []
    if not re.search(r"std::string\s+tooltip\b", ts_body):
        tn_missing.append("tooltip")
    if not re.search(r"bool\s+matched\b", ts_body):
        tn_missing.append("matched")
    if tn_missing:
        fail("TreeNodeSpec 缺字段（接口契约批）：%s —— 接口要先运到，呈现留给 UI 批"
             % "、".join(tn_missing))
    elif 'node["tooltip"]' not in ph or 'node["matched"]' not in ph:
        fail("Python 侧给不了 tooltip/matched（接口契约批）：tree_nodes_from_python 没读这两个键")
    elif "setToolTip" not in ft or "Qt::UserRole + 2" not in ft:
        fail("宿主没把 tooltip/matched 落到 item（接口契约批）：facade_tree 缺 setToolTip / UserRole+2")
    else:
        ok("TreeNodeSpec 带上 tooltip 与 matched（接口契约批：声明＋Python 绑定＋宿主落 item 三处齐全）")

    # ── ★★ P17-59（审计 01 F-19 Info）：`lzhuf_ref.c` 不许再依赖**非对齐/端序**读写 ──
    #   修前：`*(unsigned*)src`（读侧两处）与 `*(unsigned*)dest = …`（写侧一处）——
    #   `src`/`dest` 指向 `std::vector<uint8_t>::data()`，**不保证 4 字节对齐**；x86 容忍，
    #   换架构即未定义行为，而且把**端序**也一起依赖掉了。
    #   现在按字节显式组装/拆分**小端 32 位**（读 `lzhuf_read_u32_le`、写侧逐字节）。
    #   ★ 查代码前先 `active_code()` 剥注释 —— 说明注释里正引用了那段旧写法（会假红）。
    lz_code = active_code((ROOT / "src" / "engine" / "fs" / "lzhuf_ref.c").read_text(
        encoding="utf-8", errors="replace"))
    lz_cast = re.findall(r"\*\(\s*(?:unsigned|uint32_t|int|uint16_t|short)\s*\*\s*\)", lz_code)
    if lz_cast:
        fail("lzhuf_ref.c 又出现非对齐/端序读写（01 F-19）：%d 处指针强转（如 %s）"
             % (len(lz_cast), lz_cast[0]))
    elif "lzhuf_read_u32_le" not in lz_code:
        fail("lzhuf_ref.c 缺小端读助手（01 F-19）：判据与实现脱节，先修判据本身")
    else:
        ok("lzhuf 不依赖非对齐/端序（01 F-19：读写两侧都按字节显式组小端 32 位）")

    # ── ★★ P17-60（审计 01 F-20 Info）：解压器内存预算**只许一个口径** ──
    #   事实（修前）：xz 走 `decode_mem_limit(out_capacity)`（P17-2 的闸），
    #   而 zstd 直接 `ZSTD_decompress(...)` ⇒ 吃默认窗口上限 27（**128 MiB**），比 xz 的闸还松
    #   ⇒ 同一个文件里两种预算口径。本批要求：zstd 分支也必须**用同一个 decode_mem_limit**
    #   反推 `windowLogMax` 并走显式 DCtx（超限即报错，零静默）。
    dec_src = active_code((ROOT / "src" / "engine" / "fs" / "decompress.cpp").read_text(
        encoding="utf-8", errors="replace"))
    if "ZSTD_decompress(" in dec_src:
        fail("zstd 分支又走了默认窗口（01 F-20）：必须用 decode_mem_limit 反推 windowLogMax")
    elif "ZSTD_d_windowLogMax" not in dec_src or "decode_mem_limit(out_capacity)" not in dec_src:
        fail("zstd 的内存预算没跟 xz 对齐（01 F-20）：缺 ZSTD_d_windowLogMax 或没用 decode_mem_limit")
    elif "LZMA_MEMLIMIT_ERROR" not in dec_src:
        fail("xz 的内存闸不见了（01 F-20/01 F-02 回归）：LZMA_MEMLIMIT_ERROR 那条明确报错没了")
    else:
        ok("解压器内存预算一个口径（01 F-20：xz 与 zstd 共用 decode_mem_limit；超限都明确报错）")

    # ── ★★ P17-65（审计 01 F-13）：`pack_db` 的 4 GiB 窄化检测**必须是 opt-in** ──
    #   事实（修前）：`build_header` 里 `static_cast<uint32_t>(it.data.size())` 单条目 ≥4 GiB 静默回绕；
    #   `uint32_t off` 累计越过 4 GiB 静默回绕 ⇒ 产物"能打开但条目大小/偏移全错"。
    #   本批：给 `pack_db` 加**默认 nullptr** 的 `std::string* error` 出参 ⇒ 只有给了它才检测；
    #   命中返回空 vector ＋ 置 error；CLI（`--pack-db`）已接上（非空即 `PACKDB error reason=` ＋ 非零退码、
    #   **且不写盘**）。★ 判据三条，其中 ② 是**守住 opt-in 语义**（默认必须是 nullptr，
    #   否则未接线的三处（fs 页 / 门面 / py_host）会被动变成"空包落盘"）。
    dbh = (ROOT / "include" / "xrkit" / "engine" / "fs" / "db.h").read_text(
        encoding="utf-8", errors="replace")
    dbc = active_code((ROOT / "src" / "engine" / "fs" / "db.cpp").read_text(
        encoding="utf-8", errors="replace"))
    mnc = active_code((ROOT / "tools" / "cli" / "main.cpp").read_text(
        encoding="utf-8", errors="replace"))
    if "0xFFFFFFFFull" not in dbc or dbc.count("0xFFFFFFFFull") < 2:
        fail("pack_db 缺 4 GiB 窄化检查（01 F-13）：单条目与累计偏移两条都要有")
    elif (dbc.find("if (error != nullptr)") >= 0 and
          dbc.find("if (error != nullptr)") < dbc.find("0xFFFFFFFFull")):
        # ★★ P17-68（01 F-13 收官）：检查**不许再被 opt-in 门控** —— 判法是**看位置**：
        #   `0xFFFFFFFFull`（检查）必须出现在任何 `if (error != nullptr)`（门控/报理由）**之前**。
        #   修前的写法正是"先 `if (error != nullptr) {` 再把两条检查包进去" ⇒ 命中本判据。
        fail("4 GiB 检查又被 opt-in 门控了（01 F-13）：四处调用点都已接线 ⇒ 检查必须无条件")
    elif not re.search(r"std::string\*\s*error\s*=\s*nullptr", dbh):
        fail("pack_db 的 error 出参默认值不是 nullptr（01 F-13）：会破坏既有调用方的源码兼容")
    elif "&packdb_err" not in mnc:
        fail("CLI 没接上 pack_db 的出错理由（01 F-13）：--pack-db 该先算内容、算不出来就不落盘")
    else:
        ok("pack_db 会拒绝 4 GiB 窄化（01 F-13：两条检查 ＋ opt-in 出参 ＋ CLI 接线并明确报错）")
        # ★★ P17-66（01 F-13 继续接线）：**官方页那条也不许把空包写出去** ——
        #   拿到 pack 的出错理由就报错返回、绝不写盘（P17-62 先查件点名的"更糟那条路"）。
        fsp = active_code((ROOT / "src" / "shell" / "pages" / "fs_page.cpp").read_text(
            encoding="utf-8", errors="replace"))
        if "&pack_err" not in fsp or "封包失败（%1）" not in fsp:
            fail("官方页仍会把空包写出去（01 F-13 接线）：fs_page 的 pack_db 调用没接出错理由")
        else:
            ok("官方页封包不写空包（01 F-13 接线：fs_page 拿到 pack 出错理由即报错返回，不落盘）")
        # ★★ P17-67（01 F-13 接线③④）：**门面那一处同时覆盖两条链**（门面自己 + py_host 的绑定）。
        #   命中窄化就把理由塞进既有的 out.error 通道直接返回 ⇒ 不 write_file_atomic。
        efi = active_code((ROOT / "src" / "engine" / "plugin" / "engine_facade_impl.cpp").read_text(
            encoding="utf-8", errors="replace"))
        if "&pack_err" not in efi or "out.error = pack_err;" not in efi:
            fail("门面仍会把空包写出去（01 F-13 接线）：engine_facade_impl 的 pack_db 调用没接理由")
        else:
            ok("门面封包不写空包（01 F-13 接线：engine_facade_impl 命中窄化即填 out.error 返回，覆盖 py_host）")
        # ★★ P17-70（01 F-09 C 步的引擎半）：**退回整包读必须带理由**（字节数 + 为什么）。
        #   零静默：只报"读了多少"不够，还得说清"为什么不得不整包读"，否则用户无法判断。
        dbh2 = (ROOT / "include" / "xrkit" / "engine" / "fs" / "db.h").read_text(
            encoding="utf-8", errors="replace")
        dbc2 = active_code((ROOT / "src" / "engine" / "fs" / "db.cpp").read_text(
            encoding="utf-8", errors="replace"))
        if "whole_file_reason" not in dbh2:
            fail("DbListing 缺 whole_file_reason（01 F-09）：整包读只报字节数、不说原因 = 半静默")
        elif "res.whole_file_reason" not in dbc2:
            fail("退回整包读的地方没写理由（01 F-09）：db.cpp 的退回点必须置 whole_file_reason")
        else:
            ok("整包读带理由（01 F-09：DbListing 有 whole_file_reason，退回点写清为什么）")

    # ── ★★ P17-48（04 F-06 第二波）：`--pack-db` 同款写盘守卫 ──────────────────────
    #   ★ 判的是**引擎解析后的真实落点**（`resolve_pack_target` 会补扩展名）——判 operands[0] 会判错对象。
    #   受影响步骤：`golden-db`（在同一 ours.<key>.db 上逐格式反复写）已连带补 `--force`。
    pd_dir = guard_dir / "packdb_src"
    pd_dir.mkdir(parents=True, exist_ok=True)
    (pd_dir / "a.txt").write_text("hello", encoding="ascii")
    pd_out = guard_dir / "packdb_out.db"
    p1, po1 = run([str(CLI_EXE), "--pack-db", str(pd_out), "xdb", str(pd_dir)])
    p2, po2 = run([str(CLI_EXE), "--pack-db", str(pd_out), "xdb", str(pd_dir)])
    p3, po3 = run([str(CLI_EXE), "--pack-db", str(pd_out), "xdb", str(pd_dir), "--force"])
    if p1 != 0:
        fail("写盘守卫的**前提**不成立（04 F-06 / --pack-db）：首次写期望 rc=0，实际 rc=%d %r"
             % (p1, po1.strip()[:100]))
    elif p2 != 3 or "--force" not in po2:
        fail("写盘动作静默截断（04 F-06 / --pack-db）：同路径再写期望 rc=3 ＋ 理由含 `--force`，"
             "实际 rc=%d %r" % (p2, po2.strip()[:120]))
    elif p3 != 0:
        fail("`--force` 没能覆盖（04 F-06 / --pack-db）：期望 rc=0，实际 rc=%d %r"
             % (p3, po3.strip()[:120]))
    else:
        ok("写盘动作不静默截断（04 F-06 第二波 `--pack-db`：按解析后落点判；已存在⇒rc=3；`--force`⇒覆盖）")

    # ── ★★ P17-8（2026-10-10，审计 04 F-09 Low）：**`--help` 必须列全"已实现的动作"** ──────
    #   事实（修前）：`print_usage()` 漏列 10 个**确实实现了**（`run_cli()` 里有分派）的动作 ——
    #   `--version-store` / `--exe-version` / `--lzhuf` / `--xml-stats` / `--xml-compare-dirs` /
    #   `--xml-compare` / `--xml-text` / `--xml-csv-probe` / `--patch-identify` / `--patch-apply`
    #   ⇒ 按 `--help` 盘点能力会漏掉二进制补丁、XML 对比、LZHUF、版本号读写一整族入口。
    #   口径出处：`main.cpp` 自己在 `--build-package` 那段写着「能力清单里漏一条就是
    #   '这功能好像没有'」（CLI 被定位成"能力全集"）。
    #   判据两半（**都从源码对账，再拿 --help 的实际输出验**）：
    #     ① 每一个 `action == "--xxx"` 的动作名都必须出现在 `--help` 的输出里；
    #     ② `--help` 里列出的动作名也必须在 `main.cpp` 里有对应字符串（不许写不存在的动作）。
    #   判红办法：把 `print_usage()` 里任一动作行删掉 ⇒ ① 立刻红。
    main_src = (ROOT / "tools" / "cli" / "main.cpp").read_text(encoding="utf-8", errors="replace")
    # ★ 先剥注释再解析：本判据自己的说明里就写着 `action == "--xxx"` 这种占位串，
    #   不剥注释会把占位符当成"实现了但没列"的动作（实测踩过一次）。
    # ★ 分派有两种写法：主分派用 `action == "--xxx"`，前面那层选项/短动作用的是 `a == "--xxx"`
    #   （实测 `--version`/`--only`/`--pack-dirs` 走的是后者）⇒ 两种都要收。
    # ★ `option_only` 是"只作选项/说明、不是独立动作"的短名（两个方向都豁免它）。
    option_only = {"--help", "--module-subset", "--plugins", "--python-home", "--verbose",
                   "--verify-readback", "--set", "--list", "--force", "--encoding",
                   "--prefix", "--id-prefix", "--clean", "--ext", "--no-recursive",
                   "--backup", "--exclude", "--mode", "--suffix", "--offset",
                   "--no-full-cyrillic", "--only", "--pack-dirs"}
    dispatched_all = set(re.findall(r'(?:action|a)\s*==\s*"(--[a-z0-9-]+)"', active_code(main_src)))
    # ★ 正向（"漏列"）只认**主分派**（`action == "--xxx"`）—— 那才是"动作表"的成员；
    #   `a == "--xxx"` 那一族是某个动作内部的**子选项**（实测 `--all`/`--only`/`--pack-dirs`），
    #   它们本来就不该在动作表里单列。反向（"幽灵动作"）用 `dispatched_all` 宽容判。
    dispatched_main = set(re.findall(r'action\s*==\s*"(--[a-z0-9-]+)"', active_code(main_src)))
    dispatched = {name for name in dispatched_main if name not in option_only}
    if not dispatched_all:
        fail("--help 动作表判据（04 F-09）：从 main.cpp 里一个 `action == \"--xxx\"` 都没解析到"
             "（解析器与源码脱节 ⇒ 本判据会永不生效）")
    else:
        rc_help, out_help = run([str(CLI_EXE), "--help"])
        # 只认**行首**的动作表条目（正文里提到的 `[--pack-dirs]` 之类不算"列了一条动作"）。
        listing = set(re.findall(r"(?m)^\s+(--[a-z0-9-]+)", out_help))
        missing = sorted(dispatched - listing)
        if rc_help != 0:
            fail("`--help` 期望 exit=0，实际 %d" % rc_help)
        elif missing:
            fail("--help 漏列已实现的动作（04 F-09）：%s —— 能力清单里漏一条就是"
                 "'这功能好像没有'（共解析到 %d 个动作）" % ("、".join(missing), len(dispatched)))
        else:
            # 反向锁：动作表里出现、但 main.cpp 里没有对应分派的名字 ——
            # 防止"帮助里写了一堆做不到的事"。
            phantom = sorted(name for name in listing
                             if name not in dispatched_all and name not in option_only)
            if phantom:
                fail("--help 列了 main.cpp 里没有分派动作的名字（04 F-09 反向锁）：%s"
                     % "、".join(phantom))
            else:
                ok("--help 列全 %d 个已实现动作（无漏列、无幽灵动作）" % len(dispatched))

    # ── ★★ P17-10（2026-10-10，审计 02 F-02 / F-05 Medium）：**遍历出错不许静默** ──────────
    #   事实（修前）：`extract.cpp::collect` 与 `xml/dirdiff.cpp::list_xml_files` 里都是
    #   `if (ec) break;` —— 遍历中途出错就静默跳出：不计数、不上报，调用方按 `ok` 报"完成"，
    #   而候选文件已被截断（校对工具最不能忍的"漏"）。两半判据：
    #     ① **静态**：这两处不许再出现**裸** `if (ec) break;`；`collect` 必须带错误出参；
    #        `DirDiffReport` 必须有 `errors` 字段。
    #     ② **运行期（CLI）**：`--xml-compare-dirs <不存在的目录> <真目录>` ⇒
    #        收尾行 `errors=1` ＋ 一条 `XMLDIRS error reason=` 到 stderr ＋ **非零退出**
    #        （修前：静默当作"该侧一个文件都没有"，rc=0）。
    ext_src = (ROOT / "src" / "engine" / "text" / "extract.cpp").read_text(
        encoding="utf-8", errors="replace")
    diff_src = (ROOT / "src" / "engine" / "xml" / "dirdiff.cpp").read_text(
        encoding="utf-8", errors="replace")
    dirdiff_h = (ROOT / "include" / "xrkit" / "engine" / "xml" / "dirdiff.h").read_text(
        encoding="utf-8", errors="replace")
    silent = []
    if re.search(r"if\s*\(\s*ec\s*\)\s*break\s*;", active_code(ext_src)):
        silent.append("text/extract.cpp 的 collect")
    if re.search(r"if\s*\(\s*ec\s*\)\s*break\s*;", active_code(diff_src)):
        silent.append("xml/dirdiff.cpp 的 list_xml_files")
    if silent:
        fail("遍历出错静默 break（02 F-02/F-05）：%s 里还有裸 `if (ec) break;`" % "、".join(silent))
    elif "std::string* error" not in ext_src or "std::vector<std::string> errors" not in dirdiff_h:
        fail("遍历出错静默 break（02 F-02/F-05）：错误通道缺失 —— `collect` 要有 error 出参、"
             "`DirDiffReport` 要有 errors 字段")
    else:
        ok("遍历出错有通道（02 F-02/F-05：无裸 `if (ec) break;`；collect 带 error、"
           "DirDiffReport 带 errors）")
        rc_x, out_x = run([str(CLI_EXE), "--xml-compare-dirs", "__not_exist_dir__", "."])
        if (rc_x == 0 or "errors=1" not in out_x or "XMLDIRS error reason=" not in out_x
                or "目录不可用" not in out_x):
            fail("遍历出错要判红（02 F-05）：`--xml-compare-dirs <不存在> <真目录>` 期望 rc≠0 ＋ "
                 "`errors=1` ＋ `XMLDIRS error reason=目录不可用…`，实际 rc=%d 输出=%r"
                 % (rc_x, out_x.strip()[-160:]))
        else:
            ok("A 侧目录不存在 ⇒ 明确报错并非零退出（02 F-05 运行期那一半）")

    # ★★ 2026-10-06（④）：FS 的「auto」= **不给格式** —— CLI 侧两种走法必须**逐值等价**
    #   （同一枚包：`--dump <包>` vs `--dump <包> auto`，条目表逐字节相同）。
    #   为什么在门禁里造夹具：这条判据要"同一枚包两种走法"，而仓库自带语料里没有
    #   恰好能自洽判定的合成包；用引擎自己的写侧造一枚，正是 GUI 拖进来的那种包。
    if CLI_EXE.exists():
        with tempfile.TemporaryDirectory(prefix="xrkit_auto_lock_") as tmp:
            src = Path(tmp) / "src"
            (src / "gamedata").mkdir(parents=True, exist_ok=True)
            (src / "gamedata" / "a.txt").write_bytes(b"AAAA")
            (src / "readme.md").write_bytes(b"root\n")
            pack = Path(tmp) / "auto.db0"
            rc, out = run([str(CLI_EXE), "--pack-db", str(pack), "xdb", str(src)])
            if rc != 0 or not pack.exists():
                fail("auto 等价锁：夹具包没造出来（exit=%d，输出 %r）" % (rc, out.strip()[:120]))
            else:
                rc_plain, plain = run([str(CLI_EXE), "--dump", str(pack)])
                rc_auto, auto = run([str(CLI_EXE), "--dump", str(pack), "auto"])

                def entries_only(text: str) -> str:
                    """只留**条目表**行，丢掉收尾行 `DUMP ok …`。

                    ★ 2026-10-05（③ 批实测踩到）：旧写法直接比整段输出，而收尾行里有
                    `ms=`（引擎内耗时，1~2 ms 随系统负载跳）。于是这条"条目表逐值等价"的锁
                    会因为**一个与本判据无关的计时字段**随机判红（实测 6 次里红 1 次）。
                    锁的意图是"两种走法算出同一张条目表"—— 那就只比那张表。
                    """
                    return "\n".join(
                        ln for ln in text.splitlines()
                        if ln.strip() and not ln.startswith("DUMP ")).strip()

                if rc_plain != 0 or rc_auto != 0:
                    fail("auto 等价锁：`--dump` 两种走法都要 exit=0（实得 %d / %d）"
                         % (rc_plain, rc_auto))
                elif entries_only(plain) != entries_only(auto):
                    fail("auto 等价锁：`--dump <包>` 与 `--dump <包> auto` 的**条目表**不一致 —— "
                         "auto 必须**等价于不传格式**（同一条自洽判定）\n"
                         "  plain=%r\n  auto =%r" % (entries_only(plain)[:300], entries_only(auto)[:300]))
                else:
                    ok("auto 等价锁（④：`--dump <包>` ≡ `--dump <包> auto`，条目表逐值相同）")


def step_corpus(release: bool) -> None:
    """黄金语料基准：验证"我们到底拿哪几个字节当基准"没变。

    缺件分类：清单文件本身缺 = FAIL（仓库自带）；样本缺 = 外部前提（按档位 FAIL/SKIP）。
    退出码：0=全对，1=基准变了，2=样本缺失（部分）。
    """
    say("[6] 黄金语料基准")
    script = ROOT / "tests" / "collect_golden.py"
    if not require_repo_file(script, "语料收集脚本"):
        return
    rc, out = run([sys.executable, str(script)])
    for line in out.splitlines():
        if line.startswith("CORPUS:") or line.strip().startswith("!!"):
            say("  " + line.strip())
    if rc == 0:
        ok("语料基准与清单完全一致")
    elif rc == 2:
        prereq("真机语料样本", "有样本缺失（见上）", release)
    else:
        fail("语料基准校验失败（退出码 %d）：大小或 sha256 与清单不符" % rc)


def step_golden_sqfs(release: bool) -> None:
    """黄金语料差分：SquashFS 条目表，2.x（C++）逐条对齐 1.x 的输出。

    退出码：0=全部样本逐条一致，1=有不一致，2=有样本被跳过（按外部前提处理）。
    """
    say("[7] 黄金语料差分：SquashFS 条目（2.x vs 1.x）")
    script = ROOT / "tests" / "golden_sqfs.py"
    if not require_repo_file(script, "SquashFS 基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-SQFS"):
            say("  " + s)
    if rc == 0:
        ok("SquashFS 条目与 1.x 基线逐条一致")
    elif rc == 2:
        prereq("SquashFS 语料样本", "有样本缺失或未生成基线（见上）", release)
    else:
        fail("SquashFS 条目与 1.x 基线不一致（见上）")


def step_golden_extract(release: bool) -> None:
    """黄金语料差分：文件提取（2.x 进程内解压 vs 1.x 的 rdsquashfs 抽取结果）。

    覆盖三类路径：整文件在 fragment 的小文件、多数据块的大文件、中间尺寸。
    退出码：0=全部逐字节一致，1=有差异，2=有样本被跳过。
    """
    say("[8] 黄金语料差分：文件提取（2.x vs 1.x）")
    script = ROOT / "tests" / "golden_extract.py"
    if not require_repo_file(script, "提取基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-EXTRACT"):
            say("  " + s)
    if rc == 0:
        ok("提取内容与 1.x 逐字节一致")
    elif rc == 2:
        prereq("提取语料样本", "有样本缺失或未生成基线（见上）", release)
    else:
        fail("提取内容与 1.x 不一致（见上）")


def step_golden_db(release: bool) -> None:
    """黄金语料差分：DB 六格式（pack 产物字节 + 条目表 + 提取内容，全部对齐 1.x）。"""
    say("[9] 黄金语料差分：DB 六格式（2.x vs 1.x）")
    script = ROOT / "tests" / "golden_db.py"
    if not require_repo_file(script, "DB 基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-DB"):
            say("  " + s)
    if rc == 0:
        ok("DB 六格式：pack 产物/条目表/提取内容均与 1.x 一致")
    elif rc == 2:
        prereq("DB 语料样本", "有样本缺失或未生成基线（见上）", release)
    else:
        fail("DB 六格式与 1.x 不一致（见上）")


def step_db_11xx_dirlock(release: bool) -> None:
    """11xx **写侧目录条目**的三道锁（2026-10-05 已查实的真缺陷，官方 cv 判据）。

    判据本体在 `tests/check_db_11xx_dirlock.py`：①官方 `converter.exe -unpack -11xx`
    **退出码必须 == 0**（修前是栈溢出 `0xC00000FD`）；②不依赖 cv 的**字节级锁**（打包产物
    HEADER 明文里目录条目字段必须全零，自带 LZHUF 解码 + 判据自证）；③反向锁（2215/2945
    目录条目仍全零 + 三格式自往返仍过）。

    ★ 为什么独立成步、且**缺 cv 时不判红**（与其它步的 prereq 口径不同）：cv 是**外部参照工具**，
    不是仓库自带件；而"没有 cv 就整条锁失效"是最坏的形态（锁会悄悄消失）。所以脚本里
    cv 缺失只打 `!!` note、退码仍是 0 —— ②③ 两条不依赖 cv 的锁照跑照判红。
    退出码：0=全绿（cv 缺失只记 note），1=有断言不成立，2=CLI 缺失（外部前提，按档位判定）。
    """
    say("[db-11xx] 11xx 写侧目录条目：cv 判据锁 + 字节级锁 + 反向锁")
    script = ROOT / "tests" / "check_db_11xx_dirlock.py"
    if not require_repo_file(script, "11xx 目录条目判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("DB-11XX-DIRLOCK") \
                or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("11xx 目录条目写侧：cv 判据 / 字节级 / 反向三组锁全绿")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("11xx 目录条目写侧有断言不成立（见上）")


def step_db_name_cp1251_lock(release: bool) -> None:
    """DB 写侧 **cp1251 名编码**的锁（2026-10-06 用官方 cv 查实的真缺陷）。

    判据本体在 `tests/check_db_name_cp1251_lock.py`：①官方 `converter.exe -unpack -2215`
    **退出码必须 == 0**（修前是**栈溢出** `0xC00000FD`：`utf8_to_cp1251` 遇到 cp1251
    不可表示的码点就 `return false`，调用点忽略返回值 ⇒ 名字被静默截断成空名/撞名）；
    ②不依赖 cv 的**字节级锁**（自己解产物 HEADER 明文：不许有空名/重名，不可表示名按 `?`
    逐码点顶替且一一对应，cp1251 西里尔名原样，自带判据自证）；③反向锁（全 ASCII 名必须
    逐字节原样 —— 防"靠一刀切改名把锁混绿"）。

    ★ 与 `db-11xx-dirlock` 同款：cv 缺失只打 `!!` note、退码仍 0（②③ 不依赖 cv 照跑）。
    退出码：0=全绿（cv 缺失只记 note），1=有断言不成立，2=CLI 缺失（外部前提，按档位判定）。
    """
    say("[db-name1251] DB 写侧 cp1251 名编码：cv 判据锁 + 字节级锁 + 反向锁")
    script = ROOT / "tests" / "check_db_name_cp1251_lock.py"
    if not require_repo_file(script, "cp1251 名编码判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("DB-NAME1251-LOCK") \
                or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("DB 写侧 cp1251 名编码：cv 判据 / 字节级 / 反向三组锁全绿")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("DB 写侧 cp1251 名编码有断言不成立（见上）")


def step_unpack_rules(release: bool) -> None:
    """真机判红锁：解包**落点**（包名壳：VESSEL 不叠第二层壳 + 反向两条仍叠壳）与
    **报数口径**（`IWP\\database\\maps\\fake_start.db` 报数==盘上真实；`--dump` 仍报包内记录数）。

    为什么单独一步（而不是塞进 golden-db）：这两条是 2026-10-04 用户口径下**已查实**的缺陷，
    现场在真机包上（VESSEL 的 `readme.md` 掀翻量词、maps 的条目表重复登记），合成夹具造不出来
    （`pack_db` 不会自己写重复记录，smoke 里那条是**手工塞两条同路径记录**才复现的）。
    退出码：0=三组锁全绿，1=有断言不成立，2=真机样本缺失（外部前提，按档位判定）。
    """
    say("[unpack-rules] 真机锁：解包落点（自带壳判定）与报数口径（最终产物）")
    script = ROOT / "tests" / "check_unpack_rules.py"
    if not require_repo_file(script, "解包落点/报数真机锁脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("UNPACK-LOCK") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("解包落点与报数口径三组真机锁全绿")
    elif rc == 2:
        prereq("真机解包样本（VESSEL / IWP maps / Golden Sphere）", "缺样本（见上）", release)
    else:
        fail("解包落点/报数口径的真机锁不成立（见上）")


def step_fs_hardening(release: bool) -> None:
    """判红锁：文件系统层的两条**不可信输入**边界（F-1 路径逃逸判定 / H-7 解压炸弹闸）。

    夹具是**自造的最小 SquashFS**（`tests/check_fs_hardening.py` 里现造，不依赖真机语料、
    不碰游戏目录）：条目名里放 `.. ` / `.. .` 这两条"Windows 落盘时会剥掉尾随空格/点"的
    绕过形态，外加一条尾随点的正常名与一条正常哨兵；另一枚镜像声明 1 TiB 文件尺寸。
    判据＝"**判定与理由一起**断言 + 域外零文件 + 超限明确报错（不 OOM）+ 正常镜像不变"。
    为什么单独一步：这两条都是"校验确实在、但边界条件下不成立"，真机语料里没有这种条目，
    只能现造夹具；而"判定错"本身必须能变红（不许靠"反正写不出去"蒙过）。
    退出码：0=全绿，1=有断言不成立，2=构建产物缺失（外部前提，按档位判定）。
    """
    say("[fs-hardening] 判红锁：`.. ` / `.. .` 路径逃逸判定 + SquashFS 解压炸弹闸")
    script = ROOT / "tests" / "check_fs_hardening.py"
    if not require_repo_file(script, "文件系统判红锁脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") or s.startswith("✓") or s.startswith("✗") \
                or s.startswith("FS-HARDEN") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("路径逃逸判定与解压炸弹闸两组判红锁全绿")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("文件系统判红锁有断言不成立（见上）")


def step_sqfs_lreg_layout(release: bool) -> None:
    """判红锁：SquashFS **LREG（FILE_X）inode 的字段偏移**必须与权威布局逐字段相等。

    为什么要有这一步：2026-10-06 有一份审计把 `squashfs.cpp:365-366` 报成"LREG 偏移全错"。
    **复核结论＝误报**（那份报告把权威偏移列对了之后又错标一位：把 fragment/offset 说成
    40/44），而本工程读侧写的就是权威布局。既然"靠肉眼列偏移"已经误报过一次，就把它**钉成
    判据**：写侧产物的每个字段必须落在权威偏移上、读侧必须逐字节读回一致 ——
    任一处改错立刻红（已实测：把读侧 `fragment/offset` 挪到 40/44 ⇒ 本步红 3 条）。
    夹具＝现造的稀疏/带数据稀疏镜像 + 一枚**手工按权威布局摆的**最小 LREG 镜像
    （writer 造不出非平凡 fragment/offset 的 LREG，所以那一半只能手工造），
    全部落在 `%TEMP%`，不碰真机语料。
    退出码：0=全绿，1=有断言不成立，2=构建产物缺失（外部前提，按档位判定）。
    """
    say("[sqfs-lreg] 判红锁：LREG inode 字段偏移 == 权威布局（写侧逐字段 + 读侧逐字节）")
    script = ROOT / "tests" / "check_sqfs_lreg_layout.py"
    if not require_repo_file(script, "LREG 偏移判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") or s.startswith("✓") or s.startswith("✗") \
                or s.startswith("SQFS-LREG") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("LREG 字段偏移与权威布局逐字段一致、读侧逐字节读回一致")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("LREG 字段偏移判据不成立（见上）")


def step_ltx_replace_lock(release: bool) -> None:
    """判红锁：.ltx 替换**不许**动注释、不许切碎更长标识符，跳过必须**可见**。

    为什么要有这一步：1.x 的替换是裸 `text.replace`（本工程第一版照抄），实测它会把
    注释里的目标串一起改写、还会把"更长标识符"（甚至 .ltx 的**键名**）切碎成
    `game.translate_string("…")_delen` 这种**非法 .ltx**，而且**全静默**。
    夹具＝现造的一条 .ltx（注释里含目标串、一个把目标串当子串的长标识符、
    `unknown` 这类非候选键、带引号字面量），断言：
      · 注释与长标识符**零误伤**；正文（裸值与带引号字面量）**照换**；
      · `TEXT repl-skip` 逐条打印 + `replskip=N` 计数，且**台账行号真的指在那一行上**。
    已实测：改回裸 `replace_all` ⇒ 本步红 12 条（注释被改写、键名被切碎、台账 0 条）。
    退出码：0=全绿，1=有断言不成立，2=构建产物缺失（外部前提，按档位判定）。
    """
    say("[ltx-replace] 判红锁：注释/词边界保护 + 跳过台账可见")
    script = ROOT / "tests" / "check_ltx_replace_lock.py"
    if not require_repo_file(script, ".ltx 替换判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") or s.startswith("✓") or s.startswith("✗") \
                or s.startswith("LTX-REPLACE") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("注释与长标识符零误伤、正文照换、跳过台账逐条可见")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail(".ltx 替换判据不成立（见上）")


def step_extract_guard(release: bool) -> None:
    """判红锁：`--extract-text` 必须走 **7.5 守卫**（S1-1）＋ 产物写不出去**不许静默报成功**。

    为什么要有这一步：守卫与文案早就写好了、GUI 页面与 `--text-plan`（干跑）都在调它，
    **唯独真跑那条路 `extract_dir` 入口不调** ⇒ 输出目录选成源目录的**父目录**（很自然的选择）时：
      · 不带 `--clean`：改写产物**逐字节覆盖源文件**（原文不可恢复，实测退出码还是 0）；
      · 带 `--clean`：先 `remove_all` ⇒ **整棵源目录消失**，之后 `files=0` 也退 0。
    另有一条零静默洞：产物**写不出去**只进 `skipped`、不置 `ok=false` ⇒ 一个文件都没落盘也退 0。
    判据＝A（拒绝原地覆盖）/ B（拒绝删源，且拒绝发生在 `--clean` 之前）/ C 反向锁
    （`target == source` **不许**拒 —— 1.x 口径产物落在子目录里）/ D（写失败非静默），
    每条都断"退出码 + 理由原文 + 源树逐字节未变"。
    夹具现造在 `%TEMP%`（**不碰真机语料/游戏目录**）。
    退出码：0=全绿，1=有断言不成立，2=构建产物缺失（外部前提，按档位判定）。
    """
    say("[extract-guard] 判红锁：--extract-text 的 7.5 守卫 + 写失败非静默（S1-1）")
    script = ROOT / "tests" / "check_extract_guard.py"
    if not require_repo_file(script, "文本提取守卫判红锁脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") or s.startswith("✓") or s.startswith("✗") \
                or s.startswith("EXTRACT-GUARD") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("7.5 守卫在真跑那条路上生效（拒绝原地覆盖/删源 + 反向锁）＋ 写失败非静默")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("文本提取守卫判红锁有断言不成立（见上）")


def step_plugin_init_rollback(release: bool) -> None:
    """判红锁：插件 `init` 返回 0 ⇒ 必须**按 owner 回滚**它这次 init 期间的注册（S1-2）。

    为什么要有这一步：插件在 init 期间是"成功一条进一条"地写注册表的（官方夹具的写法就是
    每条失败 `return 0`）；修前宿主拿到 0 只 `FreeLibrary` ⇒ 表里留着**指向已 unmap 代码段**
    的裸函数指针 ⇒ 用户点到它注册的按钮就是 AV（`Registry::invoke` 只 catch C++ 异常）。
    夹具＝`xrkit_test_plugin_badinit.dll`（与官方夹具同一份写法，只有最后一句
    `register_compare_mode` 缺 compare ⇒ 前 6 条注册成功、入口返回 0）。
    判据＝① 夹具真的触发（宿主如实报"入口返回 0"）② ★主判据：按 **owner 名**逐容器断言
    表里**零残留** ＋ `registration_count()` 回到 mark ③ 失败留痕仍在（零静默）。
    ★ "之后调它不崩"**不作判据**（`FreeLibrary` 未必立刻 unmap，"不崩≠干净"）——
      红取证里单独实测过：真调用 ⇒ `0xC0000005`。
    退出码：0=全绿，1=有断言不成立，2=构建产物缺失（外部前提，按档位判定）。
    """
    say("[plugin-init-rollback] 判红锁：插件 init 返回 0 必须按 owner 回滚注册（S1-2）")
    script = ROOT / "tests" / "check_plugin_init_rollback.py"
    if not require_repo_file(script, "插件 init 回滚判红锁脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("[") or s.startswith("✓") or s.startswith("✗") or s.startswith("    ") \
                or s.startswith("PLUGIN-INIT-ROLLBACK") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("init 返回 0 的注册按 owner 逐容器回滚干净（留痕仍在）")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe / xrkit_test_plugin_badinit.dll", "缺失（见上）", release)
    else:
        fail("插件 init 回滚判红锁有断言不成立（见上）")


def step_golden_sqfs_pack(release: bool) -> None:
    """黄金语料差分：SquashFS **封包**产物（2.x 自写 writer vs 1.x 的 sqfs_pack/tar2sqfs）。

    判据＝用户拍板的"产物逐字节一致"：同一份输入，两边产物 sha256 必须相等。
    退出码：0=全部一致，1=有不一致，2=有样本/参照工具被跳过（按外部前提处理）。
    """
    say("[10] 黄金语料差分：SquashFS 封包（2.x writer vs 1.x）")
    script = ROOT / "tests" / "golden_sqfs_pack.py"
    if not require_repo_file(script, "封包基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=3600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-SQFS-PACK") \
                or s.startswith("!!") or s.startswith("（"):
            say("  " + s)
    if rc == 0:
        ok("封包产物与 1.x 逐字节一致")
    elif rc == 2:
        prereq("封包语料样本 / tar2sqfs", "缺参照工具或未生成基线（见上）", release)
    else:
        fail("封包产物与 1.x 不一致（见上）")


def step_golden_sqfs_pack_real(release: bool) -> None:
    """真机样本的封包对拍：把真实镜像里的文件抽出来当输入树，两套链路各打一次。

    为什么合成夹具之外还要这一条：夹具只覆盖"想得到的组合"。这一轮真机抽验抓到过一个
    合成夹具漏掉的**真缺陷**（去重截断把刚写的 fragment 块删掉），所以它是一等判据。
    耗时口径：local/fast 抽 200 个文件（≈60 s），release 跑整棵树（≈4 min）。
    ★ A 批（2026-10-06）判据**先验选择**，三态全可见：
      · `compared=`    与 1.x 参照**逐字节**（两边 sha256 相等，headline）；
      · `equivalent=`  **等价判据**：官方第三方 `rdsquashfs` 读回（条目表 + **类型**）+
                       **整树解包逐文件 sha256**。**不是"降级"** —— 它不依赖参照同源，
                       独立性比"1.x 参照"更高；脚本会**逐样本打印理由**（树多大/内存多少）。
      · `uncovered=`   连内容层都跑不了 ⇒ 进「本次未覆盖」。
    ★ `--release` 档**只**把 `uncovered` 当失败：这样门禁不再红在"参照侧这一次有没有扛住"
    （1.x `sqfs_pack` 把整棵树 read_bytes 进内存 + 给 tar2sqfs 挂了硬 120 s 超时 ⇒
    同一棵树忙时超时、闲时跑完）。口径与理由见 `docs/2.0.0-行为口径对照表.md` ⑯。
    """
    say("[11] 真机样本：SquashFS 封包对拍（判据先验选择：compared / equivalent）")
    script = ROOT / "tests" / "golden_sqfs_pack_real.py"
    if not require_repo_file(script, "真机封包脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    argv = [sys.executable, str(script), "--id", "nlc-sqz-autumn"]
    if not release:
        argv += ["--sample", "200"]
    rc, out = run(argv, timeout=3600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("GOLDEN-SQFS-PACK-REAL") or s.startswith("✗") or s.startswith("!!") \
                or s.startswith("ok 逐字节一致") or s.startswith("输入树") \
                or s.startswith("未覆盖") or s.startswith("判据：") or s.startswith("理由：") \
                or s.startswith("ok rdsquashfs") or s.startswith("ok 等价判据") \
                or s.startswith("· ") or s.startswith("equivalent 逐样本理由"):
            say("  " + s)
    verdict = ""
    for s in out.splitlines():
        if s.strip().startswith("GOLDEN-SQFS-PACK-REAL:"):
            verdict = s.strip()

    def _num(key: str) -> int:
        marker = key + "="
        i = verdict.find(marker)
        if i < 0:
            return 0
        j = i + len(marker)
        k = j
        while k < len(verdict) and verdict[k].isdigit():
            k += 1
        return int(verdict[j:k] or "0")

    if rc == 0:
        if _num("equivalent"):
            ok("真机样本封包产物一致（compared=%d equivalent=%d）：equivalent 走**等价判据**"
               "（官方第三方 rdsquashfs 整树读回 + 逐文件 sha256），逐样本理由见上"
               % (_num("compared"), _num("equivalent")))
        else:
            ok("真机样本封包产物逐字节一致（headline 判据真跑到了）")
    elif rc == 2 and _num("uncovered"):
        prereq("真机封包：连内容层判据都跑不了（%d 个样本）" % _num("uncovered"),
               verdict or "见上", release)
    else:
        fail("真机样本封包产物不一致（见上）")


def step_sqfs_pack_criterion(release: bool) -> None:
    """真机封包判据的自检锁（A 批）：先验选择 + 两条判据都真跑 + 三态退码契约。

    为什么独立成步：它锁的是"判据怎么选"这件事本身（`choose_criterion()` 的先验结论、
    `equivalent` 真跑到整树逐文件 sha256、`uncovered` 有活写入者、`判据：` 行在任何封包
    输出之前）。塞进 selftest 就没人知道它到底跑没跑；独立成步后，被 `--only` 过滤时
    按假绿批的记账进 UNCOVERED（`--release` 档于是整体 FAIL，不会假装覆盖了）。
    """
    say("[11b] 真机封包判据：先验选择 + 等价判据真跑 + 三态退码")
    script = ROOT / "tests" / "check_sqfs_pack_criterion.py"
    if not require_repo_file(script, "封包判据自检脚本"):
        return
    rc, out = run([sys.executable, str(script)], timeout=3600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("SQFS-PACK-CRITERION") \
                or s.startswith("[") or s.startswith("fixtures:"):
            say("  " + s)
    if rc == 0:
        ok("判据先验选择 + 等价判据真跑 + 三态退码契约（见上）")
    elif rc == 2:
        prereq("封包判据自检的外部前提", "缺 1.x 参照实现（见上）", release)
    else:
        fail("封包判据自检不成立（见上）")



def step_text_tables(release: bool) -> None:
    """编码表锁：磁盘上的 .inc 必须与 Python 同名 codec 一致。

    这是"仓库自带门禁文件"性质：表被手改、或换了 Python 版本导致 codec 变了 → FAIL。
    表错了的后果就是整篇乱码，而且是**逐字节判据**才能发现的那种错。
    """
    say("[12] 编码表与 Python codec 一致（cp1251 / cp1252 / gb18030 / gbk / gb2312 / "
        "big5 / shift_jis / euc-kr）")
    script = ROOT / "tools" / "gen_encoding_tables.py"
    if not require_repo_file(script, "编码表生成器"):
        return
    for inc in ("cp1251_tables.inc", "cp1252_tables.inc", "gb18030_tables.inc",
                "gbk_tables.inc", "gb2312_tables.inc",
                # ★ D3 批：三张东亚表也必须**在仓库里**（缺件即失败，不是跳过）
                "big5_tables.inc", "shift_jis_tables.inc", "euc_kr_tables.inc"):
        if not require_repo_file(ROOT / "src" / "engine" / "text" / inc, "编码表生成物"):
            return
    # ★★ 2026-10-07（H1-1）：fs 版 cp1251 表**也必须存在**，而且现在同样是本生成器的产物
    #   （`--check` 会逐字节校验它）。删掉/改名这份 ⇒ `db.cpp` 会**静默**改用 0xFFFF 那份
    #   （0x98 的 replace 语义就此改变）而编译不报错 —— 所以缺件必须是 FAIL，不是跳过。
    if not require_repo_file(ROOT / "src" / "engine" / "fs" / "cp1251_tables.inc",
                             "fs 版 cp1251 表（DB 归档文件名用）"):
        return
    rc, out = run([sys.executable, str(script), "--check"])
    if rc != 0:
        fail("编码表与 codec 不一致：\n%s" % tail(out, 10))
        return
    ok("八张表与 codec 完全一致")

    # ★ 2026-10-06：判定链"鲁棒"步的单字节家族统计剖面（`sniff_profiles.inc`）——
    #   同样**只能由生成器产出**。它是"哪一边更像俄语/西欧"的唯一依据，手改一个权重
    #   就足以让真俄文被推去 cp1252（或反过来），而这种错**看着像对**。
    prof_gen = ROOT / "tools" / "gen_sniff_profiles.py"
    if not require_repo_file(prof_gen, "判定链剖面生成器"):
        return
    if not require_repo_file(ROOT / "src" / "engine" / "text" / "sniff_profiles.inc",
                             "判定链剖面生成物"):
        return
    rc, out = run([sys.executable, str(prof_gen), "--check"])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("!!") or s.startswith("GEN-SNIFF-PROFILES"):
            say("  " + s)
    if rc != 0:
        fail("判定链统计剖面与生成器不一致（见上）")
        return
    ok("判定链统计剖面与生成器完全一致")

    # ★★ 2026-10-07（审计补充 H1-4 余条）：上面那条"一致"必须是**逐字节**的 ——
    #   修前 `gen_sniff_profiles.py --check` 用 `read_text`（universal newlines），
    #   把盘上的 `\r\n` 读成 `\n` ⇒ **CRLF 化照样 PASS**（其余 6 个生成器都是 `read_bytes`）。
    #   做法（★ 注入必红，与 J5 注入同一姿势）：把文件行尾改成 CRLF ⇒ `--check` **必须红**；
    #   再按**注入前的原字节**还原 ⇒ 必须绿，并断言还原后**逐字节相同**
    #   （来源是本进程读到的原字节，不是"再生成一遍" —— 门禁不许把源码树改坏）。
    sniff = ROOT / "src" / "engine" / "text" / "sniff_profiles.inc"
    original = sniff.read_bytes()
    try:
        sniff.write_bytes(original.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        rc_inj, _ = run([sys.executable, str(prof_gen), "--check"])
    finally:
        sniff.write_bytes(original)
    if sniff.read_bytes() != original:
        fail("注入后还原失败：%s 与注入前**不逐字节相同**（门禁把源码树改坏了）"
             % sniff.relative_to(ROOT))
        return
    if rc_inj == 0:
        fail("注入必红没红：把 %s 改成 CRLF 之后 `--check` 仍然通过 ⇒ 判据不是逐字节"
             % sniff.relative_to(ROOT))
        return
    say("  ok  注入必红：CRLF 化 %s ⇒ --check 退 %d；还原后逐字节一致"
        % (sniff.relative_to(ROOT), rc_inj))
    rc, out = run([sys.executable, str(prof_gen), "--check"])
    if rc != 0:
        fail("还原之后 --check 仍不通过（见上）")
        return
    ok("判定链统计剖面逐字节一致（含「CRLF 化必红」的注入自证）")

    # 表对了还不够：编码转换页用的 `errors="replace"`（有损编解码）**也要逐字节等于
    # Python 同名 codec** —— 替换符个数差一个就是产物错，而它错得"看着像对"。
    lossy = ROOT / "tests" / "golden_lossy_codec.py"
    if not require_repo_file(lossy, "有损编解码对拍"):
        return
    rc, out = run([sys.executable, str(lossy)], timeout=600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✗") or s.startswith("GOLDEN-LOSSY-CODEC"):
            say("  " + s)
    if rc != 0:
        fail("有损编解码与 Python codec 不一致（见上）")
        return
    ok("有损编解码（errors=replace）与 Python 逐字节一致")


def step_font_tables(release: bool) -> None:
    """字库数据表锁：磁盘上的 .inc 必须与 1.x 的 JSON / Python unicodedata 一致。

    与编码表是同一性质：表被手改、1.x 的 JSON 换了内容、或 Python 换了 Unicode 版本 → FAIL。
    表错了的后果同样是"产物看着像、逐字节比对全是错"（推进宽错 1px 就是整张图集挪位）。

    1.x 的 `font_pack/*.json` 是**外部前提**（不随 2.x 仓库走）→ 缺件记 SKIP/未覆盖；
    而 `--check` 本身还会拿"原版工具 汉化包生成工具2024 的成品 ini"交叉核对推进宽表
    （重叠 1020 条逐条相等），原版成品缺失时只报 SKIP 行、不算失败。
    """
    say("[13] 生成物锁：字库数据表 / 分组模板 / 主题令牌 与 1.x 一致")
    script = ROOT / "tools" / "gen_font_tables.py"
    if not require_repo_file(script, "字库表生成器"):
        return
    for inc in ("adv_tables.inc", "st_fallback.inc", "eaw_tables.inc"):
        if not require_repo_file(ROOT / "src" / "engine" / "font" / inc, "字库表生成物"):
            return
    adv = TOOLKIT_1X / "font_pack" / "adv_tables.json"
    if not adv.is_file():
        prereq("1.x 参照仓库（font_pack/adv_tables.json）", "未找到 %s" % adv, release)
        return
    rc, out = run([sys.executable, str(script), "--check"])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("!!") or "交叉核对" in s or s.startswith("GEN-FONT-TABLES"):
            say("  " + s)
    if rc != 0:
        fail("字库数据表与数据源不一致（见上）")
        return
    ok("三张表与 1.x 数据 / unicodedata 完全一致")

    # 同一性质的另外两个生成器（字库分组/ltx 模板、主题令牌）——之前漏接门禁，
    # 2026-09-24 补上：它们驱动出包的 ltx 文本与字符集分组，漂移了只有 [22] 才会红，
    # 而且报的是"整棵树不一致"，定位成本高得多。
    for script_name, marker, what, srcs in (
        ("gen_font_templates.py", "GEN-FONT-TEMPLATES", "字库分组/ltx 模板",
         [ROOT / "src" / "engine" / "font" / "charset_groups.inc",
          ROOT / "src" / "engine" / "font" / "ltx_templates.inc"]),
        ("gen_theme_tables.py", "GEN-THEME-TABLES", "主题令牌",
         [ROOT / "src" / "core" / "theme_tables.inc"]),
    ):
        gen = ROOT / "tools" / script_name
        if not require_repo_file(gen, "%s生成器" % what):
            return
        for inc in srcs:
            if not require_repo_file(inc, "%s生成物" % what):
                return
        if not (TOOLKIT_1X / "font_pack" / "font_pack.py").is_file() and marker != "GEN-THEME-TABLES":
            prereq("1.x 参照仓库", "未找到 font_pack/font_pack.py", release)
            return
        if marker == "GEN-THEME-TABLES" and not (TOOLKIT_1X / "toolkit_theme.py").is_file():
            prereq("1.x 参照仓库", "未找到 toolkit_theme.py", release)
            return
        rc, out = run([sys.executable, str(gen), "--check"])
        for line in out.splitlines():
            s = line.strip()
            if s.startswith("!!") or s.startswith(marker):
                say("  " + s)
        if rc != 0:
            fail("%s与 1.x 不一致（见上）" % what)
            return
        ok("%s与 1.x 一致" % what)

    # 插件契约表（槽位 HOSTS/LOCATIONS/AREA_* + api.ui 的 16 方法及其参数名）——
    # ⑫12.3 要求"契约清单从源码 AST 读，读不到就报错、不许静默降级"，
    # 所以它是**生成物 + --check 锁**，不是手抄的一份表。
    gen = ROOT / "tools" / "gen_plugin_contract.py"
    if not require_repo_file(gen, "插件契约生成器"):
        return
    if not require_repo_file(ROOT / "src" / "engine" / "plugin" / "contract_tables.inc",
                             "插件契约生成物"):
        return
    for src in ("toolkit_plugins.py", "toolkit_plugin_ui.py"):
        if not (TOOLKIT_1X / src).is_file():
            prereq("1.x 参照仓库（%s）" % src, "未找到", release)
            return
    rc, out = run([sys.executable, str(gen), "--check"])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("插件契约表") or "失败" in s:
            say("  " + s)
    if rc != 0:
        fail("插件契约表与 1.x 源码不一致（见上）")
        return
    ok("插件契约表（槽位 + api.ui 16 方法）与 1.x 源码一致")


def step_golden_text_decode(release: bool) -> None:
    """黄金语料差分：文本**解码层**（六编码链）2.x vs 1.x 的 read_text_file。

    退出码：0=全部逐字节一致，1=有不一致，2=有样本变动/不可达（按外部前提处理）。
    """
    say("[14] 黄金语料差分：文本解码（2.x vs 1.x 的 read_text_file）")
    script = ROOT / "tests" / "golden_text_decode.py"
    if not require_repo_file(script, "解码基线脚本"):
        return
    if not require_repo_file(ROOT / "tests" / "golden" / "text_decode.json", "解码基线数据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("GOLDEN-TEXT-DECODE") or s.startswith("✗") or s.startswith("!!") \
                or s.startswith("样本内容变了"):
            say("  " + s)
    if rc == 0:
        ok("文本解码（编码名 + 正文）与 1.x 逐字节一致")
    elif rc == 2:
        prereq("真语料文本样本", "样本缺失或内容变动（见上）", release)
    else:
        fail("文本解码与 1.x 不一致（见上）")


def step_golden_text_extract(release: bool) -> None:
    """黄金语料差分：文本**提取层**（2.x vs 1.x 的 text_extract_app）。

    判据＝**整棵产物树逐字节一致**（改写后的源文件 + 文本 XML），不只是条数。
    退出码：0=全部一致，1=有不一致，2=夹具不可达（按外部前提处理）。
    """
    say("[15] 黄金语料差分：文本提取（2.x vs 1.x）")
    script = ROOT / "tests" / "golden_text_extract.py"
    if not require_repo_file(script, "提取基线脚本"):
        return
    if not require_repo_file(ROOT / "tests" / "golden" / "text_extract.json", "提取基线数据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=3600)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-TEXT-EXTRACT") \
                or s.startswith("!!") or s.startswith("ok strings=") or s.startswith("[取样]"):
            say("  " + s)
    if rc == 0:
        ok("提取产物（改写源文件 + 文本 XML）与 1.x 逐字节一致")
    elif rc == 2:
        prereq("真语料文本树", "夹具不可达（见上）", release)
    else:
        fail("提取产物与 1.x 不一致（见上）")


def step_golden_text_sniff(release: bool) -> None:
    """黄金语料：文本**判定链**（六步）2.x vs 1.x 的 sniff_encoding + decode_bytes。

    判据分**四层**（第 5 步统计探测没移植 chardet，必须如实记账，不许放宽）：
      ① 硬判据 = (used, forced, 码位数, 正文 sha256) 一致；
      ② 命名台账 = 允许 (step, 编码名) 不同，但只允许台账里登记过的那些；
      ③ 能力缺口台账 = 连正文都不同的样本（含 1.x 的 cp1250 误判、GBK/Big5/SJIS 家族）；
      ④ ★ 口径差异台账（2026-10-06）= **有意与 1.x 不同**的那批 —— 用户拍板的单字节家族
         置信度系统让"谎报 windows-1251 的西欧文件"不再被 1251 吃掉。
    另有三条**零静默自证**（rc⟺lowconf、lowconf⟹step6、mism⟺enc≠used）与一条**独立于 1.x**
    的硬判据（用户那份 109 个西语 xml 真语料：不再按 windows-1251 解、不出西里尔乱码）。
    退出码：0=一致，1=不一致，2=样本缺失/变动。
    """
    say("[16] 黄金语料：文本判定链（2.x vs 1.x）")
    script = ROOT / "tests" / "golden_text_sniff.py"
    if not require_repo_file(script, "判定链基线脚本"):
        return
    if not require_repo_file(ROOT / "tests" / "golden" / "text_sniff.json", "判定链基线数据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("GOLDEN-TEXT-SNIFF") or s.startswith("✗") or s.startswith("!!") \
                or s.startswith("台账") or s.startswith("样本内容变了") \
                or s.startswith("西语真语料") or s.startswith("诚实声明") or s.startswith("gunsl_keys"):
            say("  " + s)
    if rc == 0:
        ok("判定链解码结果与 1.x 一致（差异台账已冻结；含 109 个西语真语料硬判据）")
    elif rc == 2:
        prereq("真语料文本样本", "样本缺失或内容变动（见上）", release)
    else:
        fail("判定链与 1.x 不一致（见上）")


def step_golden_font_raster(release: bool) -> None:
    """字库栅格一致性：我们的 FreeType 栅格 vs Pillow 的 `getmask(mode="L")`。

    M1-7 的第一道锁：1.x 的字库产物是 Pillow 12.3.0（捆绑 FreeType 2.14.3）做的，
    与工程 vendored 的 FreeType 同版本。栅格对不上就说明"产物逐字节一致"不可行，
    必须先对齐 flags —— 所以它排在渲染管线前面。
    退出码：0=一致，1=不一致，2=字体/CLI 缺失（按外部前提处理）。
    """
    say("[17] 字库栅格一致性（2.x FreeType vs Pillow getmask）")
    script = ROOT / "tests" / "golden_font_raster.py"
    if not require_repo_file(script, "栅格基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✓") or s.startswith("✗") or s.startswith("GOLDEN-FONT-RASTER") \
                or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("栅格与 Pillow 逐字节一致")
    elif rc == 2:
        prereq("源字体（msyh.ttf）", "字体或 CLI 缺失（见上）", release)
    else:
        fail("栅格与 Pillow 不一致（见上）")


def step_golden_font_metrics(release: bool) -> None:
    """字库**几何/宽度**一致性：2.x 的查表与裁决 vs 1.x 的 `font_pack`。

    产物里每个矩形都出自 `font_pack` 的那几张表与四条裁决函数，所以先把这一层钉死 ——
    这层错了（推进宽差 1px），整张图集一路挪位，到成品 DDS 上表现为"整片都对不上"。
    退出码：0=一致，1=不一致，2=1.x 的 font_pack 缺失（按外部前提处理）。
    """
    say("[18] 字库几何/宽度一致性（2.x vs 1.x font_pack）")
    script = ROOT / "tests" / "golden_font_metrics.py"
    if not require_repo_file(script, "字库几何基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        t = line.strip()
        if t.startswith("✓") or t.startswith("✗") or t.startswith("GOLDEN-FONT-METRICS") \
                or t.startswith("!!"):
            say(line.rstrip())
    if rc == 0:
        ok("槽位/宽度/方块判定/简繁表/框宽裁决与 1.x 完全一致")
    elif rc == 2:
        prereq("1.x 参照仓库（font_pack/font_pack.py）", "未找到（见上）", release)
    else:
        fail("字库几何/宽度与 1.x 不一致（见上）")


def step_golden_font_render(release: bool) -> None:
    """字库**渲染产物**（S=1 与 x2 档）：我们的 DDS + ini vs 1.x `render_font` 逐字节一致。

    M1-7 第④⑤⑥步的判据本体：同字符集 + 同字体 + 同字号 + 同格式 ⇒ 产物逐字节相同。
    S=1 档（11/13/15）不重采样；x2 档（16/17/18）走 Pillow 的 LANCZOS —— 那一步另有
    **孤立**对拍（门禁 [20]）。
    退出码：0=一致，1=不一致，2=字体 / 1.x / CLI 缺失（按外部前提处理）。
    """
    say("[19] 字库渲染产物一致性（S=1 与 x2 档：DDS + ini vs 1.x）")
    script = ROOT / "tests" / "golden_font_render.py"
    if not require_repo_file(script, "字库渲染基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        t = line.strip()
        if t.startswith("✓") or t.startswith("✗") or t.startswith("GOLDEN-FONT-RENDER") \
                or t.startswith("!!") or t.startswith("      "):
            say("  " + t)
    if rc == 0:
        ok("S=1 档 DDS + ini 与 1.x 逐字节一致")
    elif rc == 2:
        prereq("源字体（msyh.ttf）/ 1.x 参照仓库", "缺失（见上）", release)
    else:
        fail("S=1 档产物与 1.x 不一致（见上）")


def step_golden_font_resample(release: bool) -> None:
    """LANCZOS **孤立**锁：我们的实现 vs `PIL.Image.resize(..., LANCZOS)`。

    x2 档（16/17/18）的产物只差这一步，而它是"看着像、实则全错"的高危点（定点系数、
    边界裁剪、两趟顺序、`(int)` 与 `floor` 的差别）。单独锁比在图集产物上排障便宜一个量级。
    退出码：0=一致，1=不一致，2=CLI 缺失（按外部前提处理）。
    """
    say("[20] LANCZOS 重采样一致性（2.x vs Pillow resize）")
    script = ROOT / "tests" / "golden_font_resample.py"
    if not require_repo_file(script, "重采样基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        t = line.strip()
        if t.startswith("✗") or t.startswith("GOLDEN-FONT-RESAMPLE") or t.startswith("!!"):
            say("  " + t)
    if rc == 0:
        ok("86 个用例与 Pillow 逐字节一致")
    elif rc == 2:
        prereq("构建产物 xrkit_cli.exe", "缺失（见上）", release)
    else:
        fail("重采样与 Pillow 不一致（见上）")


def step_golden_font_charset(release: bool) -> None:
    """字符集提取锁（M1-7 第⑦步）：2.x `--extract-chars` vs 1.x `extract_chars`。

    字符集错一个字的后果特别隐蔽：引擎对缺失码位不是留空，而是画成同一个重复字形。
    语料 = 真语料（我们提取出来的翻译 XML）+ 合成边界（单根/多根/畸形/实体/CDATA/注释/
    PI/DOCTYPE/GBK/UTF-16/无声明 cp1251/空文件/子目录递归）× full_cyrillic 两档。
    已知分歧（判定链第 5 步未移植 chardet 探针）**独立进差异台账**：不算通过，**也不许被
    减法抵消** —— 台账非空时脚本走第三态、**退码非零**，这一步按 `ledger()` 记一条**可见的**
    差异台账（与 [16] 判定链那边的"能力缺口台账"同一口径：它是已知能力缺口，不是外部前提
    缺件 ⇒ 不因它判 release FAIL，但必须印在门禁输出里）。
    退出码：0=全一致，1=有非台账差异（或本脚本的"减法抵消"回归锁被触发），
    2=差异台账非空 / CLI / 1.x 缺失。
    """
    say("[21] 字符集提取一致性（2.x vs 1.x extract_chars）")
    script = ROOT / "tests" / "golden_font_charset.py"
    if not require_repo_file(script, "字符集基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        t = line.strip()
        if t.startswith("✓") or t.startswith("✗") or t.startswith("GOLDEN-FONT-CHARSET") \
                or t.startswith("!!") or t.startswith("! ") or t.startswith("gbk.xml") \
                or t.startswith("(跳过") or t.startswith("差异台账") or t.startswith("第 "):
            say("  " + t)
    verdict = ""
    for t in out.splitlines():
        if t.strip().startswith("GOLDEN-FONT-CHARSET:"):
            verdict = t.strip()
    if rc == 0:
        ok("字符集与 1.x 完全一致（差异台账为空）")
    elif rc == 2:
        if "LEDGER" in verdict:
            # ★ T-H1：这是**已知能力缺口**（判定链第 5 步 chardet 探针未移植），
            #   不是外部前提缺件 ⇒ 记**差异台账**（可见、release 不阻断），不用 prereq()。
            ledger("字符集差异台账（判定链第 5 步 chardet GB2312 探针未移植）", verdict)
        else:
            prereq("1.x 参照仓库 / 真语料 XML", "缺失（见上）", release)
    else:
        fail("字符集与 1.x 不一致（见上）")


def step_golden_font_package(release: bool) -> None:
    """出包**端到端**锁（M1-7 第⑦步）：整棵 `gamedata/` 产物树 vs 1.x `build_package`。

    这是 M1-7 的收口判据：10 个字号（S=1 + x2）× DDS/ini + XML 转写（无 BOM UTF-8 + 声明
    改 utf-8）+ `fonts.ltx`/`localization.ltx`，三套游戏版本各跑一遍（顺带验 `GAMES` 的
    目录名与 DDS 格式）。
    退出码：0=一致，1=不一致，2=字体 / 1.x / CLI 缺失（按外部前提处理）。
    """
    say("[22] 出包端到端一致性（整棵 gamedata 树 vs 1.x build_package）")
    script = ROOT / "tests" / "golden_font_package.py"
    if not require_repo_file(script, "出包端到端基线脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=3600)
    for line in out.splitlines():
        t = line.strip()
        if t.startswith("✓") or t.startswith("✗") or t.startswith("GOLDEN-FONT-PACKAGE") \
                or t.startswith("!!") or t.startswith("语料") or t.startswith("只") \
                or t.startswith("不同"):
            say("  " + t)
    if rc == 0:
        ok("整棵产物树与 1.x 逐字节一致（3 套游戏版本）")
    elif rc == 2:
        prereq("源字体 / 1.x 参照仓库", "缺失（见上）", release)
    else:
        fail("出包产物与 1.x 不一致（见上）")


def active_code(text: str) -> str:
    """只留**活代码**：丢掉注释（C++ `//` 行尾与整行、`/* ... */` 块注释）。

    静态锁要拦的是"代码里另造一份"，注释里为了讲清口径写出的调用形态不算违规 ——
    反过来要求注释不许提这个名字，等于把文档从源码里赶出去（这几条锁的出处就是注释）。

    ★★ 2026-10-06（审计处理批·0-5）：修前这里是**按行看首字符**：
        `if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"): continue`
      —— 两个方向都错：
        ① **行首 `*` 的活代码被当注释丢掉**：`*bar = new QScrollBar(Qt::Vertical);`（解引用赋值、
           乘法续行）整行消失 ⇒ 步骤 [34] 那批静态锁（裸 `QScrollBar`、页内视口…）
           可以被这种排版绕过、**判据看不见违规**；
        ② **块注释正文不缩进成 `*` 时又被当活代码**：`/* 1.x 的反例：\n   new QScrollBar(…)\n*/`
           中间那行会被判成违规 ⇒ 假红。
      现在按**词法状态**走：维护 `in_block`（`/* ... */` 的深度），只删真正在注释里的**内容**；
      行首 `*` 只有在块注释内才当装饰符剥掉，在代码里就是**活代码**：
        · 块注释内，行首 `*`（可选前置空白）后若紧跟 `/`（即 `*/`）⇒ 那是闭合符，不能误剥；
        · 代码里以 `*` 开头（`*bar = …` / 乘法续行）⇒ 原样保留。
    """
    kept = []
    in_block = 0
    for line in text.splitlines():
        out = []
        i = 0
        n = len(line)
        # 块注释续行的装饰符：`   * 正文` —— 只在块注释里、且这个 `*` 不是 `*/` 的闭合符时才剥。
        if in_block > 0:
            k = 0
            while k < n and line[k] in " \t":
                k += 1
            if k < n and line[k] == "*" and not (k + 1 < n and line[k + 1] == "/"):
                i = k + 1
        while i < n:
            c = line[i]
            if in_block:
                if c == "/" and i + 1 < n and line[i + 1] == "*":
                    in_block += 1
                    i += 2
                    continue
                if c == "*" and i + 1 < n and line[i + 1] == "/":
                    in_block -= 1
                    i += 2
                    continue
                i += 1
                continue
            if c == "/" and i + 1 < n and line[i + 1] == "/":
                break                                   # 行尾 `//` 注释：本行到此为止
            if c == "/" and i + 1 < n and line[i + 1] == "*":
                in_block = 1
                i += 2
                continue
            if c == '"':                                # 字符串字面量整体保留（里面的 // /* 不是注释）
                j = i + 1
                while j < n:
                    if line[j] == "\\":
                        j += 2
                        continue
                    if line[j] == '"':
                        j += 1
                        break
                    j += 1
                out.append(line[i:j])
                i = j
                continue
            if c == "'":
                j = i + 1
                while j < n:
                    if line[j] == "\\":
                        j += 2
                        continue
                    if line[j] == "'":
                        j += 1
                        break
                    j += 1
                out.append(line[i:j])
                i = j
                continue
            out.append(c)
            i += 1
        kept.append("".join(out))
    return "\n".join(kept)


def step_golden_xml_stats(release: bool) -> None:
    """XML 统计模式的黄金对拍（M2-6e1，⑩10.6）。

    参照是两件真实存在的东西：Python 的 `len(str.splitlines())` 与 1.x
    `xml_compare_app.ID_PATTERN`（含"先剥开头 XML 声明"这一步）。
    1.x 曾对**结尾换行**额外 +1，于是"A 有结尾换行、B 没有"被误判成不一致 ——
    这条对拍就是冲着这个口径来的（合成样本里专门放了成对的两份）。
    """
    say("[24] XML 统计模式：行数/ID 与 Python 口径一致（⑩10.6）")
    script = ROOT / "tests" / "golden_xml_stats.py"
    if not require_repo_file(script, "XML 统计对拍"):
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("✗") or s.startswith("GOLDEN-XML-STATS"):
            say("  " + s)
    if rc == 0:
        ok("行数（splitlines 语义）与 ID 计数与参照完全一致")
    elif rc == 2:
        prereq("XML 统计对拍的外部前提", "CLI 缺失", release)
    else:
        fail("XML 统计模式与参照不一致（见上）")


def step_shell_smoke(release: bool) -> None:
    """Qt6 外壳：构建 + **离屏自检**（M2-1）。

    为什么外壳也要门禁：M2 的判据是"行为口径对照表 15 组逐条重验"，而界面行为在
    `-platform offscreen` 下也能量（能构造真实控件树）。所以外壳从第一天起就带
    `--smoke`（ASCII 收尾行），不靠人眼截图。
    退出码：0=通过，1=自检红 / 构建失败，2=Qt 或构建工具缺失（按外部前提处理）。
    """
    say("[23] Qt6 外壳：构建 + 离屏自检（M2-1）")
    if not (QT_ROOT / "lib" / "cmake" / "Qt6").is_dir():
        prereq("Qt6（%s）" % QT_ROOT, "未安装或路径不对", release)
        return
    env = load_msvc_env()
    if env is None:
        prereq("MSVC 工具链", "找不到 vcvars64.bat：%s" % VCVARS, release)
        return
    if not configure_and_build(SHELL_BUILD_DIR, env, shell=True):
        return
    if not SHELL_EXE.exists():
        fail("构建产物缺失：%s" % SHELL_EXE)
        return
    # 构建 GUI 的**排版判据**（用户 2026-09-27："两个软件 UI 的排版都有很大问题"）——
    # 不是看截图：跑 `--layout-report`（离屏构建窗口 → 量各区高度 → 按口径给结论）。
    #
    # ★★ 2026-10-06（审计处理批·0-7）：修前这里是 `if gui_exe.exists(): ...` —— **没有 else**。
    #   目标没编出来（改名 / 从 CMake 摘掉）就既不打 FAIL 也不进未覆盖清单 ⇒
    #   这条**用户拍板**的排版判据可以在"判据根本不存在"的情况下让 release PASS、输出里毫无痕迹。
    #   现在按档位判定：release 直接 FAIL；其它档记未覆盖 + 明确 SKIP 行（降级必须可见）。
    gui_exe = SHELL_BUILD_DIR / "src" / "shell" / "xrkit_build_gui.exe"
    if not gui_exe.exists():
        if release:
            fail("构建 GUI 排版判据的目标不存在：%s（判据没跑成，不是通过）"
                 % gui_exe.relative_to(ROOT))
        else:
            UNCOVERED.append("构建 GUI 排版判据（%s 不存在，判据未覆盖）"
                             % gui_exe.relative_to(ROOT))
            say("  SKIP 构建 GUI 排版判据 —— %s 不存在" % gui_exe.relative_to(ROOT))
    else:
        env_gui = dict(env)
        env_gui["QT_QPA_PLATFORM"] = "offscreen"
        rc_gui, out_gui = run([str(gui_exe), "--layout-report"], env=env_gui, timeout=180)
        for line in out_gui.splitlines():
            s = line.strip()
            if s.startswith("LAYOUT") or s.startswith("SCALE"):
                say("  " + s)
        if rc_gui == 0 and "LAYOUT PASS" in out_gui:
            ok("构建 GUI 排版口径通过（表单 <=200px / 构建史 >=110px / 日志 <=50%）")
        else:
            fail("构建 GUI 排版口径不通过（退出码 %d，见上）" % rc_gui)

    rc, out = run([str(SHELL_EXE), "--smoke"], env=env, timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("SHELL-SMOKE") or s.startswith("!!"):
            say("  " + s)
    if rc == 0 and "SHELL-SMOKE ok" in out:
        ok("外壳构建 + 离屏自检通过")
    else:
        fail("外壳自检未通过（退出码 %d，见上）" % rc)
        return

    # 对照表 1.6：**活代码里不许有裸滚动条**（1.x 是"不许有裸 `ttk.Scrollbar(`"，唯一例外是
    # 插件的 Tk 回退）。Qt 侧等价判据：`new QScrollBar(` 只允许出现在滚动组件里 ——
    # 别处自己造条子就等于绕过三闸门（这正是 1.x 花钱那一族的入口）。
    offenders = []
    for src in sorted((ROOT / "src" / "shell").rglob("*")):
        if src.suffix not in (".cpp", ".h"):
            continue
        if src.name == "scroll_viewport.cpp":
            continue
        if "new QScrollBar(" in active_code(src.read_text(encoding="utf-8", errors="replace")):
            offenders.append(str(src.relative_to(ROOT)))
    if offenders:
        fail("裸滚动条（1.6）：%s —— 条子只能由 ScrollViewport 造" % ", ".join(offenders))
        return
    ok("没有裸滚动条（1.6：只允许 ScrollViewport 造条子）")

    # 对照表 5.1：**页内滚动只能由 PageBase 造**。各页自己 new 一层视口，就等于
    # 各写各的 `fit`/`minsize_y` —— 1.x 的 `min_y=` 事故正是这么来的（一处写错、
    # 整页 `TclError` 起不来，而且只有打开那一页才看得见）。
    offenders = []
    for src in sorted((ROOT / "src" / "shell" / "pages").rglob("*")):
        if src.suffix not in (".cpp", ".h") or src.name == "page_base.cpp":
            continue
        if "new ScrollViewport(" in active_code(src.read_text(encoding="utf-8", errors="replace")):
            offenders.append(str(src.relative_to(ROOT)))
    if offenders:
        fail("页内视口（5.1）：%s —— 视口只能由 PageBase 造" % ", ".join(offenders))
        return
    ok("页内视口只由 PageBase 造（5.1）")

    # 对照表 5.2：`page_min_h()` 必须是**函数**（`px(430)`）。落到别处的字面量 430
    # 就是把"按 96 DPI 定的值"钉死在别的缩放上 —— 静态锁只认 page_base.cpp 那一处。
    offenders = []
    for src in sorted((ROOT / "src" / "shell").rglob("*")):
        if src.suffix not in (".cpp", ".h") or src.name == "page_base.cpp":
            continue
        if "px(430)" in active_code(src.read_text(encoding="utf-8", errors="replace")):
            offenders.append(str(src.relative_to(ROOT)))
    if offenders:
        fail("页最小高字面量（5.2）：%s —— 一律走 page_min_h()" % ", ".join(offenders))
        return
    ok("页最小高只有 page_min_h() 一处真值（5.2）")

    # 对照表 6.11：**解包只能有一条路**。1.x 是"逐项 `--cat`（300 s/项）"与
    # "整镜像 `--unpack-path`（3600 s）"两条路，成功计数与文案还不同源 ——
    # 页面里再出现第二处导出调用，就等于把那条路又劈开了。
    # ★ 2026-09-28：那一处**入口改名了** —— `extract_tree` 只认 SquashFS，而 1.x 的
    #   "解包"DB 与 SquashFS 都能解（这正是"FS 不如 1.x 方便"的一半）⇒ 引擎加了
    #   `extract_package`（按包类型分派，共用同一套越界判据/原子写/失败台账）。
    #   锁的**意图不变**（页面里只有一处导出调用），指向改成新入口；并要求页面里
    #   **不再直接出现** `extract_tree(`（免得有人绕过统一入口又开一条路）。
    page_src = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"
    page_code = active_code(page_src.read_text(encoding="utf-8", errors="replace"))
    calls = page_code.count("extract_package(")
    direct = page_code.count("extract_tree(")
    if calls != 1 or direct != 0:
        fail("解包入口（6.11）：fs_page.cpp 里 extract_package( 必须恰好 1 处、"
             "extract_tree( 必须 0 处（实测 %d / %d）" % (calls, direct))
        return
    ok("解包只有一条路（6.11：fs_page.cpp 里 extract_package( 恰好 1 处、无旁路）")

    # ── ★★ P17-4（2026-10-10，审计 03 F-03 High）：**同一批只许解一遍** ────────────────
    #   事实（修前）：`FsPage::unpack_selected` / `unpack_all` 先调 `run_unpack_jobs`
    #   —— 它内部**已经**逐 job `extract_job` 并报日志/进度 —— 随后**又**对同一个 jobs 逐条
    #   `extract_job` ⇒ 整批**写盘两遍**（第二遍覆盖第一遍产物、对被占用/只读文件凭空多出一轮
    #   失败、日志里多出一串「覆盖：…」、耗时翻倍）。界面按钮走 `start_unpack`（不双跑）
    #   ⇒ 只有这两个 public 入口（自检与任何内部调用者）与界面行为不一致 ——
    #   "同一件事两套实现"正是本仓红线。
    #   锁的形态：这两个函数体内**不许再出现 `extract_job(`** —— 执行只许走
    #   `run_unpack_jobs` 那一条路（它把计数经出参带回来）。
    for fn in ("unpack_selected", "unpack_all"):
        at = page_code.find("FsPage::%s(" % fn)
        if at < 0:
            fail("同批只解一遍（03 F-03）：fs_page.cpp 里找不到 `FsPage::%s(`" % fn)
            return
        body = page_code[at:]
        end = body.find("\n}\n")
        if end >= 0:
            body = body[:end]
        if "extract_job(" in body:
            fail("同批解包两遍（03 F-03）：`%s` 体内又出现了 `extract_job(` —— "
                 "执行只许走 run_unpack_jobs 一条路（这条会让整批写盘两遍）" % fn)
            return
    ok("同一批解包只跑一遍（03 F-03：unpack_selected/unpack_all 不再直接调 extract_job）")

    # ── ★★ P17-13（2026-10-10，审计 01 F-08 Medium）：**不可信声明尺寸上限只许有一处定义** ──
    #   事实（修前）：同一语义的常量分散在两个 TU 且取值不同 —— `lzhuf.cpp` 内联
    #   `256 * 1024 * 1024`、`db.cpp` 的 `kMaxDecompressBytes = 512ull * 1024 * 1024`，
    #   注释还自称"值必须能对照"（谁改一处另一处不会跟着变）。审计另点名的
    #   `lzhuf_ref.c:360` 那处硬编码**先查后确认已不存在**（给的是旧版本行号）。
    #   判据：① 两处都不许再出现裸字面量；② 两处都必须包含共享头
    #   `xrkit/engine/fs/untrusted_limits.h`；③ 该头必须同时定义两个名字
    #   （取值**刻意不强行改成一样**：改小 db 那条会改真实大包行为，属口径变更要先取证）。
    limits_h = ROOT / "include" / "xrkit" / "engine" / "fs" / "untrusted_limits.h"
    lzhuf_src = (ROOT / "src" / "engine" / "fs" / "lzhuf.cpp").read_text(
        encoding="utf-8", errors="replace")
    db_src = (ROOT / "src" / "engine" / "fs" / "db.cpp").read_text(
        encoding="utf-8", errors="replace")
    limits_txt = limits_h.read_text(encoding="utf-8", errors="replace") if limits_h.exists() else ""
    raw_literals = []
    if re.search(r">\s*256\s*\*\s*1024\s*\*\s*1024", active_code(lzhuf_src)):
        raw_literals.append("lzhuf.cpp 的 256*1024*1024")
    if re.search(r"=\s*512ull\s*\*\s*1024\s*\*\s*1024", active_code(db_src)):
        raw_literals.append("db.cpp 的 512ull*1024*1024")
    if raw_literals:
        fail("不可信尺寸上限又写死了（01 F-08）：%s —— 必须走 untrusted_limits.h 里的具名常量"
             % "、".join(raw_literals))
    elif ("untrusted_limits.h" not in lzhuf_src or "untrusted_limits.h" not in db_src
          or "XRKIT_LZHUF_MAX_DECLARED_BYTES" not in lzhuf_src
          or "XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES" not in db_src
          or "XRKIT_LZHUF_MAX_DECLARED_BYTES" not in limits_txt
          or "XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES" not in limits_txt):
        fail("不可信尺寸上限没收敛到一处（01 F-08）：两处都要 include untrusted_limits.h 并用"
             "具名常量（LZHUF→XRKIT_LZHUF_MAX_DECLARED_BYTES / DB→XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES）")
    else:
        ok("不可信尺寸上限一处定义（01 F-08：LZHUF 256 MiB / DB 512 MiB 都来自 "
           "untrusted_limits.h，含大小关系 static_assert；取值刻意不强行统一）")

    # ── ★★ P17-16（2026-10-10，审计 01 F-12 Low）：**exe 目录不许用固定 MAX_PATH 缓冲** ────
    #   事实（修前）：`wchar_t buffer[MAX_PATH]` + 一次 `GetModuleFileNameW(..., MAX_PATH)`，
    #   再用 `n < MAX_PATH` 判成功 —— 路径长于 260 时该函数**静默截断**并返回**缓冲大小**，
    #   判断不成立 ⇒ 直接 `return "."`（app 目录变成当前工作目录：日志/user.ltx/插件目录全找错，
    #   且没有任何提示）。判据：不许再把 `MAX_PATH` 当 nSize 传进去；且必须存在"翻倍重试"的形态
    #   （`n < size` 判完整 + 循环里 `size *= 2`）。
    #   ★ 本轮实测教训：第一次改成 `GetModuleFileNameW(nullptr, nullptr, 0)` 问长度**在这台机器上
    #     返回 0** ⇒ app_dir() 直接退化成 "."，被 `--smoke` 的两条既有判据当场抓住（默认落盘目录 /
    #     user.ltx 同目录）—— 所以判据锁的是"翻倍重试"，不是某一种两步法写法。
    theme_src = (ROOT / "src" / "core" / "theme.cpp").read_text(encoding="utf-8", errors="replace")
    theme_code = active_code(theme_src)
    if "GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH" in theme_code or \
            "GetModuleFileNameW(nullptr, buffer, MAX_PATH)" in theme_code:
        fail("exe 目录还在把 MAX_PATH 当缓冲大小传给 GetModuleFileNameW（01 F-12）："
             "路径超长会被静默截断 ⇒ app_dir() 悄悄退化成 \".\"")
    elif "n < size" not in theme_code or "size *= 2" not in theme_code:
        fail("exe 目录没有翻倍重试（01 F-12）：必须用 `n < size` 判完整路径、截断时 `size *= 2` 重来")
    else:
        ok("exe 目录翻倍重试取路径（01 F-12：不再用固定 MAX_PATH 缓冲，截断即翻倍重来）")

    # ── ★★ P17-18（2026-10-10，审计 01 F-16 Info）：**封包产物必须原子写** ──────────────────
    #   事实（修前）：`pack_squashfs` 直接 `file.open(out_path)` **流式写目标** —— 中途失败
    #   （读盘错/压缩错/进程被杀/断电）会在目标路径留下一个"看起来正常"的**半截包**
    #   （本仓别处的产物早就走 `write_file_atomic`，封包这条是例外）。
    #   判据：不许再直接打开 `out_path`；且必须出现"写 `<out>.part` + 收尾 rename"两步。
    #   ★ 踩坑登记：第一版把 `OutFile` 声明在外层作用域 ⇒ 析构在函数出口才关句柄，
    #     rename 报"文件被另一个进程占用"（实测），所以锁的是"**同一作用域内**写完再改"的形态。
    psw_src = (ROOT / "src" / "engine" / "fs" / "squashfs_write.cpp").read_text(
        encoding="utf-8", errors="replace")
    if "file.open(out_path, err)" in psw_src:
        fail("封包产物还是直接写目标（01 F-16）：中途失败会留下半截包 ⇒ 必须先写 `<out>.part`、"
             "成功关闭后再 rename")
    elif 'out_path + ".part"' not in psw_src or "std::filesystem::rename(part_path, out_path" not in psw_src:
        fail("封包产物原子写没到位（01 F-16）：必须有 `part_path`（`out_path + \".part\"`）"
             "与收尾的 `std::filesystem::rename(part_path, out_path, ...)`")
    else:
        ok("封包产物原子写（01 F-16：写 `<out>.part` → 关闭句柄 → rename；失败清 .part、目标不动）")

    # ── ★★ P17-23（2026-10-10，审计 04 F-08 Low）：**生成器不许硬编码 1.x 路径** ────────────
    #   事实（修前）：`tools/gen_plugin_contract.py` / `gen_font_templates.py` / `gen_font_tables.py`
    #   都在**模块层**写死本机的 1.x 绝对路径（`E:\\Software\\Games\\...\\STALKER_Toolkit`）
    #   ⇒ 换机器/换检出位置就得改源码。现在三个都改成**环境变量 `XRKIT_TOOLKIT_1X` 优先**，
    #   没有才回落到本机既有位置（`gen_font_tables.py` 另保留 `--toolkit-1x`，命令行优先级最高）。
    #   判据：三个生成器都必须引用 `XRKIT_TOOLKIT_1X`；且不许再出现"裸的、无环境变量开关的"赋值。
    gen_missing = []
    for gen_name in ("gen_plugin_contract.py", "gen_font_templates.py", "gen_font_tables.py"):
        gen_src = (ROOT / "tools" / gen_name).read_text(encoding="utf-8", errors="replace")
        if "XRKIT_TOOLKIT_1X" not in gen_src:
            gen_missing.append(gen_name)
    if gen_missing:
        fail("生成器还硬编码着 1.x 路径（04 F-08）：%s 里没有 `XRKIT_TOOLKIT_1X`（环境变量）开关 ⇒ "
             "换机器就得改源码" % "、".join(gen_missing))
    else:
        ok("生成器不写死 1.x 路径（04 F-08：三个生成器都认 `XRKIT_TOOLKIT_1X`，"
           "命令行 `--toolkit-1x` 优先级更高）")
    #   事实（修前）：`tail_len = static_cast<uint32_t>(nblocks) * 4u` —— 窄化 ×4 不溢出**完全依赖**
    #   上游 `ino.size ≤ 512 MiB` 那条闸（保证 nblocks ≤ 2^29），安全性是"跨两处的隐式耦合"：
    #   谁放宽 block_size 校验或调大那条闸，就会退化成 tail 分配过小而循环按 nblocks 越界读。
    #   判据：这一处必须**自证**（先按 64 位算、超限即判错），不许再出现裸的
    #   `static_cast<uint32_t>(nblocks) * 4`。
    # ── ★★ P17-12（2026-10-10，审计 01 F-18 Info）：**block_sizes 数组长度不许靠上游闸门隐性兜** ──
    #   事实（修前）：`tail_len = static_cast<uint32_t>(nblocks) * 4u` —— 窄化 ×4 不溢出**完全依赖**
    #   上游 `ino.size ≤ 512 MiB` 那条闸（保证 nblocks ≤ 2^29），安全性是"跨两处的隐式耦合"：
    #   谁放宽 block_size 校验或调大那条闸，就会退化成 tail 分配过小而循环按 nblocks 越界读。
    #   判据：这一处必须**自证**（先按 64 位算、超限即判错），不许再出现裸的
    #   `static_cast<uint32_t>(nblocks) * 4`。
    sqfs_src = (ROOT / "src" / "engine" / "fs" / "squashfs.cpp").read_text(
        encoding="utf-8", errors="replace")
    sqfs_code = active_code(sqfs_src)
    if re.search(r"static_cast<uint32_t>\(\s*nblocks\s*\)\s*\*\s*4", sqfs_code):
        fail("block_sizes 长度仍是窄化乘（01 F-18）：`static_cast<uint32_t>(nblocks) * 4` 还在 —— "
             "必须改成先按 64 位算长度、超限即判错")
    elif "kMaxBlockArrayBytes" not in sqfs_code or "tail_len64" not in sqfs_code:
        fail("block_sizes 长度自证缺失（01 F-18）：必须有 `tail_len64` 与 `kMaxBlockArrayBytes` 上限判")
    else:
        ok("block_sizes 长度自证（01 F-18：64 位先算 + 超限即报错，不再靠上游闸门隐性兜底）")

    # ── ★★ P17-7（2026-10-10，审计 03 F-06 Medium）：**起进程不许把主线程焊死** ──────────
    #   事实（修前）：视频页四个调用点都是 `waitForStarted(10000)`，且都跑在**主线程**路径上
    #   （按钮回调 / `apply_file` / `request_transcode`）⇒ 进程启动慢（exe 缺依赖、杀软拦截、
    #   网络盘上的 python）时界面冻结数秒，Windows 上还会被标成"无响应"；
    #   `cancel_transcode` 里还有一句 `waitForFinished(5000)`（ffmpeg 不响应 kill 时同样冻结）。
    #   锁两半（**缺一不可** —— 只缩短等待会让"起不来"变成静默失败）：
    #     ① 长等待必须消失：`video_page.cpp` 里不许出现 `waitForStarted(` 且参数 > 1000 ms，
    #        也不许出现 `waitForFinished(` 的取消路径长等待（析构里那个 2 秒收尾不算）；
    #     ② 信号兜底必须在位：文件里必须出现 `&QProcess::errorOccurred` 接线
    #        （晚一拍的 FailedToStart 要有理由、要收干净），且必须走共用的
    #        `wait_for_start_probed(`（唯一一份探测）。
    video_src = ROOT / "src" / "shell" / "pages" / "video_page.cpp"
    video_code = active_code(video_src.read_text(encoding="utf-8", errors="replace"))
    long_waits = [m for m in re.findall(r"waitForStarted\s*\(\s*(\d+)\s*\)", video_code)
                  if int(m) > 1000]
    long_finish = [m for m in re.findall(r"waitForFinished\s*\(\s*(\d+)\s*\)", video_code)
                   if int(m) > 2000]
    if long_waits or long_finish:
        fail("主线程长等待（03 F-06）：video_page.cpp 里还有 waitForStarted(>1000ms)=%s / "
             "waitForFinished(>2000ms)=%s —— 起进程/取消都不许把主线程焊死"
             % (long_waits or "无", long_finish or "无"))
        return
    if "&QProcess::errorOccurred" not in video_code or "wait_for_start_probed(" not in video_code:
        fail("启动失败兜底缺失（03 F-06）：video_page.cpp 必须有 `&QProcess::errorOccurred` 接线 "
             "与共用的 `wait_for_start_probed(` —— 只缩短等待会把'起不来'变成静默失败")
        return
    ok("起进程不焊主线程（03 F-06：waitForStarted ≤1s + errorOccurred 兜底 + 取消改定时器兜底）")

    # ══════════════════════════════════════════════════════════════════════════
    # ★★ 2026-10-06（审计 §7 第 3 组的**静态面**：3-3 / 3-8）
    #   动行为那半在 `src/shell/smoke.cpp` 的 3-x 锁里（跑得到、判得红）；
    #   这里锁的是"**语义只许一份**"这条形式判据 —— 拆掉它就等于又开了第二份实现。
    # ══════════════════════════════════════════════════════════════════════════

    # 3-3：判重**只许**走 `core::path_equals`。修前 `add_paths`/`scan_dir` 各写一份
    # `QString::compare(..., Qt::CaseInsensitive)`（认不出 `E:\a\x` 与 `E:/a/x` 是同一份）。
    fs_src = ROOT / "src" / "shell" / "pages" / "fs_page.cpp"
    fs_code = active_code(fs_src.read_text(encoding="utf-8", errors="replace"))
    if "core::path_equals(" not in fs_code:
        fail("判重口径（3-3）：fs_page.cpp 必须用 `core::path_equals(` 判重 —— "
             "整串比较认不出正/反斜杠与 `.`/`..` 段，会重复入源")
        return
    if "QString::compare(source.path" in fs_code:
        fail("判重口径（3-3）：fs_page.cpp 里还有整串比较的判重（`QString::compare(source.path…`）"
             "—— 判重只许 `core::path_equals` 一份")
        return
    ok("判重只此一份（3-3：`core::path_equals`，无整串比较）")

    # 3-4：**加载序只此一份实现**，而且每条加源路径都走它（修前只有 `scan_dir` 会排）。
    #   ★ 注意：这条判据放在**独立步 `engine-contract`**（`tests/check_engine_contract.py`）
    #     里，不放在这里 —— 本步会**先跑 `--smoke`**，而"把 `push_source` 里的
    #     `apply_load_order()` 删掉"同时会让 smoke 的 3-4 行为锁红 ⇒ 本步在走到静态锁之前
    #     就已经 FAIL，"这条静态锁自己能红"的取证就出不来（本批实测踩到）。
    if "sort_sources_by_basename" not in fs_code:
        fail("加载序（3-4）：排序必须交给引擎 `sort_sources_by_basename`（页面不许自己排）")
        return
    ok("加载序只此一份（3-4：排序只走引擎 `sort_sources_by_basename`；'每条加源路径都调'"
       "那条在 `engine-contract` 步 —— 这里跑 smoke 会先红，取不出静态锁自己的红）")

    # 3-8：**`SplitPane` 是死代码**（产品路径零实例化）—— 判据不许挂在产品不用的类上。
    #   口径（本批二选一里选的那条，理由写进 `docs/2.0.0-外壳规格.md` 的 §10 表）：
    #   产品分栏用 `QSplitter`（外壳规格 §10 的"拖到底/松手重排"那两条判据随之改写）；
    #   `SplitPane` 只作为**自检夹具**保留（它验的是"面板分配算法"，不是产品控件），
    #   所以：`src/shell` 下**除 smoke 自身**不许出现 `SplitPane` 的实例化/包含。
    split_hits = []
    for src in sorted((ROOT / "src" / "shell").rglob("*")):
        if src.suffix not in (".cpp", ".h"):
            continue
        if src.name in ("split_pane.cpp", "split_pane.h", "smoke.cpp"):
            continue   # 类自己 + 自检夹具（判据的来源）
        text = active_code(src.read_text(encoding="utf-8", errors="replace"))
        if "SplitPane" in text:
            split_hits.append(str(src.relative_to(ROOT)))
    if split_hits:
        fail("死代码（3-8）：`SplitPane` 只许出现在 `split_pane.{h,cpp}` 与 `smoke.cpp`（自检夹具），"
             "产品路径不许引用它 —— 实测：%s" % "、".join(split_hits))
        return
    if "new QSplitter(" not in fs_code:
        fail("死代码（3-8）：FS 页的分栏必须是 `new QSplitter(`（产品真实用的那个）—— "
             "判据不许挂在产品不用的 `SplitPane` 上")
        return
    ok("SplitPane 只作自检夹具、产品用 QSplitter（3-8：判据不挂死代码）")

    # 对照表 ⑨9.7：**页面不许自己拼"定案步骤"标签**。1.x 的事故就是 convert_app 复算了
    # `多字节(...)` / `指定(...)` / `声明(...)` / `统计(...)`，而判定的值来自别处 ⇒
    # 标签与判定自相矛盾。锁的形态取"标签 + 左括号"这一**构造形态**：
    # 页面里可以解释链条（"BOM → 多字节 → …"），但不许把它拼成结论。
    convert_src = ROOT / "src" / "shell" / "pages" / "convert_page.cpp"
    code = active_code(convert_src.read_text(encoding="utf-8", errors="replace"))
    offenders = [label for label in ("多字节(", "指定(", "声明(", "统计(") if label in code]
    if offenders:
        fail("定案步骤标签（9.7）：convert_page.cpp 里出现 %s —— 标签只能由引擎给（sniff_result_label）"
             % "、".join(offenders))
        return
    ok("定案步骤标签只由引擎给（9.7：页面不拼 `多字节(...)` 那类结论）")

    # ★ 2026-10-06 用户口径③「后面的两个才是不确定因素，也就是一般意义上的**置信度系统**」
    #   + 零静默红线：**低置信/否决声明/判定≠实解**必须在四个面上都留痕 ——
    #   引擎置位、CLI 上屏、GUI 逐条与汇总、页面状态栏汇总。拆掉任何一处 ⇒ 红。
    sniff_src = ROOT / "src" / "engine" / "text" / "sniff.cpp"
    sniff_code = active_code(sniff_src.read_text(encoding="utf-8", errors="replace"))
    missing = [name for name in ("res.low_confidence = true;", "res.override_decl = true;")
               if name not in sniff_code]
    if missing:
        fail("置信度系统（零静默）：sniff.cpp 里必须置位 %s —— 低置信/否决声明必须能被看见"
             % "、".join(missing))
        return
    cli_src = ROOT / "tools" / "cli" / "main.cpp"
    cli_code = active_code(cli_src.read_text(encoding="utf-8", errors="replace"))
    missing = [f for f in ("lowconf=", "overdecl=", "statscore=", "mism=") if f not in cli_code]
    if missing:
        fail("置信度系统（零静默）：`--sniff` 的输出行缺少 %s —— CLI 上看不到就等于静默"
             % "、".join(missing))
        return
    batch_src = ROOT / "src" / "engine" / "text" / "batch.cpp"
    batch_code = active_code(batch_src.read_text(encoding="utf-8", errors="replace"))
    missing = [t for t in ("低置信判定：", "统计否决声明：", "判定≠实际使用编码：")
               if t not in batch_code]
    if missing:
        fail("置信度系统（零静默）：batch.cpp 里缺少汇总报数 %s —— 逐条留痕还不够，要能报总数"
             % "、".join(missing))
        return
    conv_code = active_code(convert_src.read_text(encoding="utf-8", errors="replace"))
    missing = [t for t in ("outcome.low_confidence", "outcome.override_decl",
                           "outcome.used_mismatch") if t not in conv_code]
    if missing:
        fail("置信度系统（零静默）：convert_page.cpp 的状态栏缺少 %s —— GUI 上要一眼看到"
             % "、".join(missing))
        return
    ok("置信度系统零静默（引擎置位 + CLI 上屏 + GUI 逐条/汇总 + 状态栏 四处齐全）")

    # 对照表 ⑦7.1/7.5：**页面不许自己写拒绝理由**。判据与文案都在 `validate_request`
    #（引擎），页面只把返回值显示出来 —— 两条入口各写一份理由，迟早会不一样。
    text_src = ROOT / "src" / "shell" / "pages" / "text_page.cpp"
    code = active_code(text_src.read_text(encoding="utf-8", errors="replace"))
    offenders = [phrase for phrase in ("不要选它们的上层目录", "会覆盖源目录中的原文件", "请指定输出目录")
                 if phrase in code]
    if offenders:
        fail("提取守卫文案（7.1/7.5）：text_page.cpp 里出现 %s —— 理由只能由引擎给（validate_request）"
             % "、".join(offenders))
        return
    ok("提取守卫理由只由引擎给（7.1/7.5：页面不自己写那几句话）")

    # 锁的自证：上面这些静态锁都靠 `active_code` 区分"注释"与"活代码"。
    # 两头都得对 —— 既能被一句注释绕过（太松），也会把注释当违规（太紧，等于
    # 禁止源码里写口径出处）。所以这里**故意**喂一份正反样本给它。
    #
    # ★★ 2026-10-06（审计处理批·0-5）：修前的自证只喂了三种"注释形态"，**没覆盖**审计点名的
    #   两个方向，于是"行首 `*` 的活代码被丢掉"这条整整没人管。现在把两个方向都补成样本：
    #     ① 活代码可以以 `*` 开头（解引用赋值 / 乘法续行）—— 修前整行被删 ⇒ 锁漏判；
    #     ② 块注释正文**不缩进成 `*`** —— 修前那行被当活代码 ⇒ 锁假红（反向错误）。
    probe = active_code(
        "// new ScrollViewport(整行注释)\n"
        "/* px(430) 块注释 */\n"
        "new ScrollViewport(真的) // 行内注释后面的代码要留下\n"
        "int h = px(430);\n"
        "/* 1.x 的反例：\n"
        " * px(430) 块注释续行（缩进成星号）\n"
        "   new ScrollViewport(块注释正文不缩进成星号)\n"
        "*/\n"
        "*bar = new ScrollViewport(行首星号的活代码);\n"
        "int w = px(430) /* 行内块注释 */ + 1;\n")
    # ① 活代码必须留下（修前 `*bar = …` 与 `int w = …` 都会被删掉）
    for must in ("new ScrollViewport(真的)", "int h = px(430);",
                 "*bar = new ScrollViewport(行首星号的活代码);"):
        if must not in probe:
            fail("静态锁自证失败：active_code 把活代码滤掉了（锁会漏判）：%s" % must)
            return
    if probe.count("new ScrollViewport(") != 2:
        fail("静态锁自证失败：active_code 留下的 `new ScrollViewport(` 应为 2 处"
             "（一处在行尾注释前、一处在行首星号的活代码里），实测 %d"
             % probe.count("new ScrollViewport("))
        return
    # ② 注释必须被滤掉（含**块注释续行**与**不缩进成星号的块注释正文**；行内块注释只删注释本体）
    if probe.count("px(430)") != 2:
        fail("静态锁自证失败：active_code 没滤掉注释（锁会被注释绕过），"
             "`px(430)` 应剩 2 处（活代码里的两处），实测 %d" % probe.count("px(430)"))
        return
    if "+ 1;" not in probe:
        fail("静态锁自证失败：行内块注释把同一行的代码一起删掉了")
        return
    ok("静态锁自证：注释（含块注释续行/不缩进正文）不算违规、活代码（含行首 `*`）必被拦")

    # ★★ 2026-10-06（D1 ① → 同日 ② 用户拍板后收窄）：**「取消」不是死按钮** —— 三层静态锁。
    #   事实（改动前）：`task_shell.cpp` 里只有三处 `cancel_->setEnabled(...)`、
    #   **一处 `connect` 都没有** ⇒ 六页共用任务壳的那颗「取消」点了什么都不发生。
    #   ① 任务壳自己必须把按钮接起来（拆掉那行 ⇒ 立刻红）—— 它仍服务**保留下来的三页**；
    #   ② **保留取消的三页**必须在 worker 里轮询 `cancel_requested()`
    #      （「接线在」不等于「各页真停」—— 只有壳接线而 worker 不轮询，按钮照样是摆设）；
    #   ③ **删除取消的两页**（用户 2026-10-06 原话：「**封包和字库的取消，在这里本质是完全
    #      没有的，故直接删除即可。**」）走**反向锁**：不许再有 `cancel_requested` 轮询，
    #      且任务壳必须按 `CancelButton::No` 造 —— 谁把取消接线加回来就立刻红。
    #   每页必须**明确归类**（将来新加的页漏归类也红，不许默认放行）。
    shell_src = ROOT / "src" / "shell" / "task_shell.cpp"
    code = active_code(shell_src.read_text(encoding="utf-8", errors="replace"))
    if "connect(cancel_" not in code:
        fail("取消接线（①）：task_shell.cpp 里必须有 `connect(cancel_` —— "
             "没有它，「取消」按钮就是死的（点了不留痕、worker 也不停）")
        return
    ok("取消接线（①：task_shell.cpp 里 `connect(cancel_` 在位）")

    KEEP_CANCEL = ("convert_page.cpp", "text_page.cpp", "xml_page.cpp")
    DROP_CANCEL = ("fs_page.cpp", "fontpack_page.cpp")
    keep_offenders, drop_offenders, unclassified = [], [], []
    for src in sorted((ROOT / "src" / "shell" / "pages").rglob("*.cpp")):
        code = active_code(src.read_text(encoding="utf-8", errors="replace"))
        if "task_->run(" not in code:
            continue
        name = src.name
        if name in KEEP_CANCEL:
            if "cancel_requested()" not in code:
                keep_offenders.append(name)
        elif name in DROP_CANCEL:
            if "cancel_requested()" in code:
                drop_offenders.append(name + "（还有 cancel_requested 轮询）")
            if "CancelButton::No" not in code:
                drop_offenders.append(name + "（没有按 CancelButton::No 造壳）")
        else:
            unclassified.append(name)
    if keep_offenders:
        fail("取消轮询（①）：%s 起了任务壳却**不轮询取消位** —— 那颗按钮在它这一页仍是死的"
             % "、".join(keep_offenders))
        return
    if drop_offenders:
        fail("取消删除（②：用户拍板「封包和字库的取消…直接删除即可」）：%s —— "
             "这两页（文件系统 / 汉化包生成）不许再有取消接线，任务壳必须按 CancelButton::No 造"
             % "、".join(drop_offenders))
        return
    if unclassified:
        fail("取消归类（①/②）：%s 用了 task_->run 但没归到「保留取消」或「删除取消」那一组 —— "
             "新页必须显式归类（默认放行=又一个死按钮）" % "、".join(unclassified))
        return
    ok("取消归属明确（①：convert/text/xml 轮询取消位；②：fs/fontpack 无取消接线 + CancelButton::No）")

    # ★★ 2026-10-06（D2 批）：**"同类东西只维护一套组件"**的五条静态锁。
    #
    # 这批修的是审计里"不必要的阉割"，而其中三处的**根源**都是"各页自己又抄了一份"：
    #   · 编码转换页要用"递归遍历" ⇒ 它自己写一份 `recursive_directory_iterator` 就是第 5 份；
    #   · XML 结果表要"多列"     ⇒ 它自己写 `setSectionResizeMode`/`setStretchLastSection`
    #     就是第 2 份列策略（而且最容易漏的就是"末列吃剩余宽度"这条，漏了右半边是死区）；
    #   · 汉化包页要"从系统字体里找" ⇒ 写死 `C:\Windows\Fonts` 就丢了 `%SystemRoot%`。
    # 这五条锁各自**都能判红**（删掉共用件、把实现抄回页面、把路径写死，立刻红）。
    #
    # ① 递归遍历件**全仓库恰好那 4 份**（都是引擎里既有的：charset / overlay×2 / extract）——
    #    多出第 5 份就红。判据只认**活代码**（注释里为了讲清口径提到这个名字不算违规）。
    hits = []
    for src in sorted((ROOT / "src").rglob("*.cpp")):
        code = active_code(src.read_text(encoding="utf-8", errors="replace"))
        n = code.count("recursive_directory_iterator")
        if n:
            hits.append((str(src.relative_to(ROOT)).replace("\\", "/"), n))
    # ★★ 2026-10-08（P11b）：**显式清单**（升级，不是放宽）——
    #   本批把 XML 页"列目录"从页面（原用 Qt 的 `QDirIterator`）下沉到引擎
    #   `engine/xml/dirdiff.cpp` ⇒ 引擎侧 +1 份；**外壳侧仍是 0 份**，正是这条锁的本意
    #   （遍历只许住引擎、页面不许自己抄一份）。清单化之后：**未登记的新增照样立刻红**。
    expected_iter = {
        "src/engine/font/charset.cpp": 1,
        "src/engine/fs/overlay.cpp": 2,
        "src/engine/text/extract.cpp": 1,
        "src/engine/xml/dirdiff.cpp": 1,   # ← P11b 新登记：目录对比的遍历（下沉自 xml_page）
    }
    shell_iter = [p for p, _ in hits if p.startswith("src/shell/")]
    if dict(hits) != expected_iter or shell_iter:
        fail("递归遍历件（D2：同类东西只有一套）：实测 %s；应恰好是 %s，且 src/shell 下 0 份"
             % ("、".join("%s×%d" % (p, n) for p, n in hits) or "(无)",
                "、".join("%s×%d" % (p, n) for p, n in sorted(expected_iter.items()))))
        return
    ok("递归遍历件与**登记清单**逐项一致（%d 处、全在引擎层；外壳侧 0 份）"
       % len(expected_iter))

    # ② 编码转换页的批处理**只有一条路**：必须调引擎的 `convert_batch(`，
    #    且不许再出现"只读顶层"的旧实现（`entryList(`）——那正是"子目录被静默漏掉"的根源。
    conv = active_code((ROOT / "src" / "shell" / "pages" / "convert_page.cpp")
                       .read_text(encoding="utf-8", errors="replace"))
    need = ["convert_batch(", "batch_options_for(", "restore_backups(", "clear_backups(",
            "export_stats(", "etxt::BatchHooks"]
    missing = [token for token in need if token not in conv]
    if missing:
        fail("编码转换页批处理（D2②）：convert_page.cpp 里缺 %s —— 批处理必须走引擎那一份"
             % "、".join(missing))
        return
    if "entryList(" in conv:
        fail("编码转换页批处理（D2②）：convert_page.cpp 里还有 `entryList(` —— "
             "那是「只读顶层、子目录静默漏掉」的旧实现，必须走引擎的递归遍历")
        return
    ok("编码转换页批处理只有一条路（D2②：引擎 convert_batch + 无 entryList 旧路）")

    # ③ XML 结果表＝**多列**且列策略走共用装配件：不许再回退成单列虚拟列表，
    #    也不许自己写一份列策略（那一份只许在 pair_tree.cpp 里）。
    xml_code = active_code((ROOT / "src" / "shell" / "pages" / "xml_page.cpp")
                           .read_text(encoding="utf-8", errors="replace"))
    xml_head = active_code((ROOT / "src" / "shell" / "pages" / "xml_page.h")
                           .read_text(encoding="utf-8", errors="replace"))
    if "apply_pair_tree_spec(" not in xml_code:
        fail("XML 结果表（D2③）：xml_page.cpp 必须调**共用**的 apply_pair_tree_spec()（不是自己写列策略）")
        return
    # ★ 判据取两个形态：**类型名**（`VirtualList`）与**包含**（`virtual_list.h`）——
    #   只判类型名会漏掉"把单列列表接回来"的另一种写法（2026-10-06 实测：只判 `VirtualList`
    #   时，往 xml_page.cpp 加一句 `#include "../virtual_list.h"` 竟然没判红）。
    if "VirtualList" in xml_code or "VirtualList" in xml_head or \
            "virtual_list.h" in xml_code or "virtual_list.h" in xml_head:
        fail("XML 结果表（D2③）：xml 页又出现 VirtualList / virtual_list.h —— 结果表必须按模式声明"
             "画**多列**（单列 `\\t` 拼文本正是被改掉的那份）")
        return
    if "itemDoubleClicked" not in xml_code or "copy_id(" not in xml_code:
        fail("XML 详情栏（D2③）：xml_page.cpp 里缺 itemDoubleClicked（双击弹窗）或 copy_id（逐条复制id）")
        return
    ok("XML 结果表多列 + 详情栏/双击/复制id 接线在位（D2③）")

    # ④ 列策略**只许一份**：`setSectionResizeMode(` / `setStretchLastSection(` 在 src/shell 下
    #    只能出现在 pair_tree.cpp 里（包含它的 .h 也只许 pair_tree.h 提到 —— 注释不算）。
    col_offenders = []
    for src in sorted((ROOT / "src" / "shell").rglob("*")):
        if src.suffix not in (".cpp", ".h"):
            continue
        if src.name in ("pair_tree.cpp", "pair_tree.h"):
            continue
        code = active_code(src.read_text(encoding="utf-8", errors="replace"))
        if "setSectionResizeMode(" in code or "setStretchLastSection(" in code:
            col_offenders.append(str(src.relative_to(ROOT)).replace("\\", "/"))
    if col_offenders:
        fail("列策略（D2④）：%s 里自己写了列策略 —— 那一份只许在 src/shell/pair_tree.cpp 里"
             % "、".join(col_offenders))
        return
    ok("列策略只有 pair_tree.cpp 一份（D2④：末列吃剩余宽度那条口径不会各写各的）")

    # ⑤ 汉化包页的字体来源走 `%SystemRoot%`（**不写死 C:\Windows\Fonts**），
    #    并且"记住上次选择"真的落 user.ltx。
    # ★★ 2026-10-07（P7b）**有意改这条锁**：原来要求 `fontpack_page.cpp` 自己出现
    #    `SystemRoot`/`qEnvironmentVariable` —— 那时平台目录逻辑就写在页面里。P7b 把它抽成
    #    **引擎的唯一一份**（`engine::font::system_font_dirs()`，页面与 `api.host.system_font_dirs()`
    #    共用），页面改成调引擎 ⇒ 那两个 token 搬到了 `src/engine/font/system_dirs.cpp`。
    #    锁因此**升级成两处都判**（比原来更强：既判"页面确实用了那份实现"，也判"那份实现确实
    #    按平台取目录、且没写死 C:\Windows"），而不是放宽。
    # ★★ 2026-10-08（插件改装 P8a）：字库选择器**抽成宿主唯一一份**（`src/shell/font_picker.cpp`）。
    #   这条锁因此**升级**成三判（比原来更强，不是放宽）：
    #     ① 那份实现里确实有：引擎目录 / 候选过滤 / 默认路径 / 四态标记缓存 / Qt 字体解析 / 对话框骨架；
    #     ② **两个消费者**都在调它：内置页 + 插件门面（= 插件弹出的就是同一个界面）；
    #     ③ 对话框骨架**只许出现一次**（出现第二处 = 又写了一份界面）。
    picker_path = ROOT / "src" / "shell" / "font_picker.cpp"
    if not require_repo_file(picker_path, "字库选择器（宿主唯一一份）"):
        return
    picker_code = active_code(picker_path.read_text(encoding="utf-8", errors="replace"))
    picker_need = ["system_font_dirs(", "font_candidates(", "default_font_path(",
                   "font_mark_of(", "QFontDatabase::addApplicationFont", "cjk_state_mark(",
                   "font_picker_list", "font_picker_zoom_in", "font_picker_preview"]
    picker_missing = [token for token in picker_need if token not in picker_code]
    page_code = active_code((ROOT / "src" / "shell" / "pages" / "fontpack_page.cpp")
                            .read_text(encoding="utf-8", errors="replace"))
    facade_code = active_code((ROOT / "src" / "shell" / "widget_facade_qt.cpp")
                              .read_text(encoding="utf-8", errors="replace"))
    consumers = {"内置页": "make_font_picker(" in page_code,
                 "插件门面": "make_font_picker(" in facade_code}
    dialog_token = 'setObjectName(QStringLiteral("font_picker"))'
    owners = []
    for shell_cpp in sorted((ROOT / "src" / "shell").rglob("*.cpp")):
        if dialog_token in shell_cpp.read_text(encoding="utf-8", errors="replace"):
            owners.append(shell_cpp.name)
    dirs_code = active_code((ROOT / "src" / "engine" / "font" / "system_dirs.cpp")
                            .read_text(encoding="utf-8", errors="replace"))
    dirs_need = ["SystemRoot", "windir", "Fonts"]
    dirs_missing = [token for token in dirs_need if token not in dirs_code]
    problems = []
    if picker_missing:
        problems.append("共享件缺 %s" % "、".join(picker_missing))
    for who, ok_consumer in consumers.items():
        if not ok_consumer:
            problems.append("%s没在调共享件" % who)
    if owners != ["font_picker.cpp"]:
        problems.append("对话框骨架出现处=%s（只许 font_picker.cpp）" % (owners or "无"))
    if dirs_missing:
        problems.append("引擎实现缺 %s" % "、".join(dirs_missing))
    if problems:
        fail("系统字体选择器（D2⑤）：%s —— 字体要从系统字体目录找、四态标记判据在引擎、"
             "界面只许一份" % "；".join(problems))
        return
    ok("系统字体选择器（D2⑤：宿主唯一一份 `font_picker.cpp` —— 引擎目录（%SystemRoot%\\Fonts "
       "不写死）+ 候选过滤 + 四态标记缓存 + 预览/缩放；内置页与插件门面**共用**它，界面骨架只出现一次）")


def step_winres(release: bool) -> None:
    """Windows 版本资源（VERSIONINFO）：exe 里的元数据必须**非空**，且与 `core::version_string()` 同源。

    为什么是一等判据（用户 2026-10-04 口径）：exe "完全没有元数据、也没有签名" ⇒ 杀软误报。
    两条锁，缺一条都守不住：
      · `gen_winres.py --check` —— 生成物 `.rc` 与 `include/xrkit/core/version.h` **漂移即 FAIL**
        （仓库自带文件性质，按 require_repo_file 那一档处理，绝不 SKIP）；
      · `check_winres.py` —— 直接读 exe 里的**真实资源字节**：八个字段非空、`FileVersion` /
        `ProductVersion` 与 CLI 打印出来的 `core::version_string()` 逐字一致、
        `OriginalFilename` 必须是**本 exe 的名字**（把 CLI 的资源挂到 GUI 上也红）。
    判红办法：① 把 `.rc` 里的 FileVersion 手写成一个旧版本串 ⇒ 第二条立刻红；
              ② 改了 version.h 不重跑生成器 ⇒ 第一条立刻红。
    """
    say("[30] Windows 版本资源（VERSIONINFO）：元数据非空且与 core::version_string() 同源")
    gen = ROOT / "tools" / "gen_winres.py"
    rc_file = ROOT / "assets" / "xrkit_winres.rc"
    checker = ROOT / "tests" / "check_winres.py"
    if not require_repo_file(gen, "版本资源生成器"):
        return
    if not require_repo_file(rc_file, "版本资源生成物"):
        return
    if not require_repo_file(checker, "版本资源判据脚本"):
        return
    rc, out = run([sys.executable, str(gen), "--check"])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("GEN-WINRES"):
            say("  " + s)
    if rc != 0:
        fail("版本资源生成物与 include/xrkit/core/version.h 漂移（见上）—— 重跑 tools/gen_winres.py")
        return
    ok("assets/xrkit_winres.rc 与 version.h 逐字节一致（--check 通过）")

    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    wanted = [CLI_EXE, SHELL_EXE]
    absent = [p for p in wanted if not p.exists()]
    if absent:
        prereq("构建产物 %s" % "、".join(p.name for p in absent), "未构建（外壳步骤没跑到）", release)
        return
    # 用户实测的那两个产物在 msvc-shell 里（外加构建器）；存在就一起查 —— 三个 exe 都有资源。
    optional = [SHELL_BUILD_DIR / "tools" / "cli" / "xrkit_cli.exe",
                SHELL_BUILD_DIR / "src" / "shell" / "xrkit_build_gui.exe"]
    checked = wanted + [p for p in optional if p.exists()]
    rc, out = run([sys.executable, str(checker)] + [str(p) for p in checked], timeout=180)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("WINRES"):
            say("  " + s)
    if rc != 0:
        fail("版本资源判据不通过（见上）")
        return
    ok("版本资源：%d 个 exe 的元数据非空、且与 core::version_string() 同源" % len(checked))


def tail(text: str, n: int) -> str:
    """取最后 n 行有效输出。

    必须滤掉 MSVC 的 `/showIncludes` 噪声（"注意: 包含文件:" / "Note: including file:"）——
    实测一次构建能刷出几千行，真错误会被冲得看不见。
    """
    lines = [
        ln for ln in text.splitlines()
        if ln.strip()
        and "包含文件" not in ln
        and "Note: including file" not in ln
    ]
    # 有错误行就优先显示错误行：MSVC 的模板报错会刷出几十行 "note:"，
    # 真正的 "error Cxxxx" 往往在中间，取尾会把它冲掉。
    errors = [ln for ln in lines
              if "error C" in ln or "error:" in ln.lower() or "FAILED" in ln or "fatal error" in ln]
    return "\n".join((errors or lines)[-n:])


# ── 主流程 ──────────────────────────────────────────────────────────────
def step_golden_video_argv(release: bool) -> None:
    """视频转码 argv 对拍：2.x `plan_transcode` vs **exec 1.x `convert()` 本体实调**的基线。

    基线 `tests/golden/video_argv.json` 由 `tests/probe_video_argv.py --write` 生成（期望值现算
    自 1.x 源码，不是我手写的）。退出码：0=逐段一致，1=不一致，2=基线或 CLI 缺失。

    同一档里还有**解析**对拍（`tests/golden_video_probe.py`）：喂**真 ffprobe / 真
    `ffmpeg -encoders` 的输出字节**给我们的解析器与 1.x 的 `parse_video_info` /
    `_ensure_encoders`，逐字段比（信息面板的字段就是这么来的）。
    """
    say("[25] 视频转码 argv + 视频参数解析对拍（2.x vs 1.x 本体）")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    steps = [("argv", ROOT / "tests" / "golden_video_argv.py", "视频转码 argv 与 1.x 逐段一致"),
             ("probe", ROOT / "tests" / "golden_video_probe.py",
              "视频参数解析（ffprobe JSON / 编码器表）与 1.x 逐字段一致")]
    for name, script, ok_text in steps:
        if not require_repo_file(script, "视频%s对拍脚本" % name):
            continue
        rc, out = run([sys.executable, str(script)])
        for line in out.splitlines():
            s = line.strip()
            if s.startswith("[") or s.startswith("GOLDEN-VIDEO"):
                say("  " + s)
        if rc == 0:
            ok(ok_text)
        elif rc == 2:
            prereq("视频%s基线" % name, "缺基线或 CLI（见上）", release)
        else:
            fail("视频%s与 1.x 不一致（见上）" % name)


def step_golden_sqfs_compressors(release: bool) -> None:
    """块压缩器**读侧**判据：lzo / gzip / lz4 / zstd（⑥6.5 一般化）。

    正面＝参照 `tar2sqfs -c <压缩器>` 造的镜像能被我们正确读回；反面＝坏镜像必须**可见失败**。
    写侧（我们产出这些压缩器）本步骤不覆盖 —— 现有 writer 只有 xz。
    """
    say("[26] 块压缩器读侧：lzo / gzip / lz4 / zstd（⑥6.5）")
    script = ROOT / "tests" / "golden_sqfs_compressors.py"
    if not require_repo_file(script, "块压缩器判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("ok ") or s.startswith("✗") or s.startswith("GOLDEN-SQFS-COMPRESSORS") \
                or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("四种块压缩器的读侧与坏镜像行为都对")
    elif rc == 2:
        prereq("参照 tar2sqfs", "缺参照工具或 CLI（见上）", release)
    else:
        fail("块压缩器读侧判据不通过（见上）")


def step_golden_sqfs_metacross(release: bool) -> None:
    """元数据**跨块逻辑偏移**的回归锁（2026-09-27 真机整树跑抓到的缺陷）。

    判据：抽 `sqzy_patch` 里那个 inode 起始偏移 > 8 KiB 的文件，**与参照工具 `rdsquashfs --cat`
    的输出逐字节比**。参照工具/镜像缺失时退码 2（进"本次未覆盖"）。
    """
    say("[27] 元数据跨块偏移（真机成员 vs 参照 cat）")
    script = ROOT / "tests" / "golden_sqfs_metacross.py"
    if not require_repo_file(script, "跨块偏移判据脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("ok ") or s.startswith("✗") or s.startswith("GOLDEN-SQFS-METACROSS") \
                or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("跨块偏移成员与参照逐字节一致")
    elif rc == 2:
        prereq("参照 rdsquashfs / sqzy_patch 镜像", "缺参照或样本（见上）", release)
    else:
        fail("跨块偏移判据不通过（见上）")


def step_bench(release: bool) -> None:
    """`--bench`：三条真实 I/O 路径计时，只守"防数量级回归"的宽松上界。

    口径见 `docs/2.0.0-bench口径.md`；样本缺失退 2（进"本次未覆盖"）。
    """
    say("[28] bench：元数据列举 / 抽取吞吐 / 文本解码（只守数量级）")
    script = ROOT / "tests" / "bench.py"
    if not require_repo_file(script, "bench 脚本"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)])
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("BENCH") or s.startswith("ok ") or s.startswith("✗") or s.startswith("!!"):
            say("  " + s)
    if rc == 0:
        ok("bench 三条路径都在数量级上界内")
    elif rc == 2:
        prereq("bench 样本", "缺镜像或语料（见上）", release)
    else:
        fail("bench 超出数量级上界（见上）")


def step_px_only(release: bool) -> None:
    """像素口径（屏幕自适应，2026-09-27 用户：「我电脑是2.5K屏，所以做一个自适应」）：
    **设计稿像素一律过 px()** —— 非零裸字面量按 96 DPI 定死，在高 DPI 屏上不会跟着缩放。
    独立成脚本（tests/check_px_only.py），**不受其它步骤早退影响**（上一版我把它塞进外壳步骤，
    而外壳自检一红那一步就 return 了 ⇒ 锁根本没被执行）。
    """
    say("[px-only] 像素口径：尺寸/间距只许过 px()")
    rc, out = run([sys.executable, str(ROOT / "tests" / "check_px_only.py")], timeout=120)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("PX-ONLY"):
            say("  " + s)
    if rc == 0:
        ok("尺寸/间距一律过 px()（屏幕自适应口径）")
    else:
        fail("有尺寸/间距没过 px()（见上；白名单见 tests/check_px_only.py）")


def step_python_host(release: bool) -> None:
    """插件改装 P2（用户 2026-10-07 拍板：**Python 模块接口 + C ABI 退役**）：
    CPython 插件宿主**真的起来了**（嵌入解释器 + vendored pybind11 都通）。

    判据（三条都能红）：
      · `--python-host` 必须打出 `PYHOST enabled=1 initialized=1 version=3.…`；
      · `enabled=0`（构建期没带 Python 支持）⇒ **release 档 FAIL**（不是 SKIP —— 那会变成
        "把构建开关一关，锁就绿了"的后门；fast 档按外部前提记账）；
      · 起不来（`initialized=0`）⇒ FAIL，并把解释器给的原话（`reason=`）原样打出来（零静默）。

    独立成步（做法同 px-only / no-console）：判"宿主能不能起来"这一件事，不受其它步骤早退影响。
    """
    say("[python-host] 插件改装 P2：CPython 宿主（插件＝.py，与 C ABI 并存期）")
    if not CLI_EXE.exists():
        prereq("构建产物 %s" % CLI_EXE.name, "缺失（先跑 build 步）", release)
        return
    rc, out = run([str(CLI_EXE), "--python-host"], timeout=180)
    line = ""
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("PYHOST"):
            say("  " + s)
            if not line:
                line = s
    if not line:
        fail("--python-host 没打出 PYHOST 行（退出码 %d）" % rc)
        return
    enabled = " enabled=1" in line
    initialized = " initialized=1" in line
    if not enabled:
        prereq("CPython 插件宿主（构建期已启用）", "本次构建没带 Python 支持：" + line, release)
        return
    if rc == 0 and initialized and " version=3." in line and " gil=released" in line:
        ok("CPython 宿主起来了（" + line + "）")
    elif rc == 0 and initialized and " version=3." in line:
        # ★ GIL 没放开 ⇒ 工作线程里回调进 Python 会死等（六页都在 TaskShell 的 worker 里干活）
        fail("CPython 宿主起来了但**没放开 GIL**（`gil=held`）：工作线程回调会死等：" + line)
    else:
        fail("CPython 宿主没起来（退出码 %d）：%s" % (rc, line))


def step_python_embed(release: bool) -> None:
    """插件改装 P2c：**随包自带的 embeddable CPython**（用户口径：「发布随包自带 embeddable Python」）。

    判据（每条都能红）：
      · vendored 树在（`third_party/python-embed`：`python3XX.zip` + `python3XX.dll`）且**版本与构建期
        一致**（拿门禁自己的 `sys.version_info` 对账 —— FindPython3 找的就是它）；
      · `--python-host --python-home <embed>` 能起来（`initialized=1` 且 `version=` 对上）——
        **这一条等价于"发行到没装 Python 的机器上也能起宿主"**，因为 home 指向的是随包那份；
      · `--py-plugins <tests/python_plugins> --python-home <embed>` ⇒ `loaded=1 errors=0`
        （随包标准库里 importlib/os/tempfile 都得在，插件导入链才跑得通）；
      · **包内**判据（★ 只在包自己声明了 `python_runtime=` 时跑，口径同 `[pkg-platform]`）：
        拿最新那个声明过的包，从**产物目录**跑 `xrkit_app.exe --python-host` ⇒ `initialized=1`。
        没声明过的早期包记台账（不许无条件跑：dist 目录是运行期产物，干净检出时没有包）。
    """
    say("[python-embed] 插件改装 P2c：随包自带 embeddable CPython（标准库 + 运行期）")
    embed = ROOT / "third_party" / "python-embed"
    zips = sorted(embed.glob("python3*.zip")) if embed.is_dir() else []
    dlls = sorted(embed.glob("python3*.dll")) if embed.is_dir() else []
    if not zips or not dlls:
        fail("找不到随包用的 embeddable Python（%s：python3*.zip=%d / python3*.dll=%d）—— "
             "先跑 `python tools/fetch_third_party.py --only python-embed`"
             % (embed.relative_to(ROOT), len(zips), len(dlls)))
        return
    expect = "%d.%d.%d" % sys.version_info[:3]
    say("  vendored：%s / %s；门禁自身 Python=%s" % (zips[0].name, dlls[0].name, expect))

    def probe(argv: list[str]) -> tuple[int, str]:
        if not CLI_EXE.exists():
            return 127, ""
        return run([str(CLI_EXE), *argv], timeout=300)

    rc, out = probe(["--python-host", "--python-home", str(embed)])
    line = next((ln.strip() for ln in out.splitlines() if ln.strip().startswith("PYHOST")), "")
    if rc == 0 and " initialized=1" in line and (" version=%s " % expect) in line:
        ok("随包 embeddable CPython 能起宿主（" + line + "）")
    else:
        fail("随包 embeddable CPython 起不来（rc=%d）：%s" % (rc, line or "(无 PYHOST 行)"))

    rc2, out2 = probe(["--py-plugins", str(ROOT / "tests" / "python_plugins"),
                       "--python-home", str(embed)])
    line2 = next((ln.strip() for ln in out2.splitlines()
                  if ln.strip().startswith("PYPLUGINS files=")), "")
    if rc2 == 0 and " loaded=1 " in line2 and " errors=0" in line2:
        ok("随包标准库能跑插件导入链（" + line2 + "）")
    else:
        fail("随包标准库跑不了插件导入链（rc=%d）：%s" % (rc2, line2 or "(无 PYPLUGINS 摘要行)"))

    # 包内判据：只看**自己声明过** python_runtime= 的包（最新那个序号）
    alphas = sorted((ROOT / "dist" / "2.0.0" / "ALPHA").glob("*"),
                    key=lambda p: int(p.name) if p.name.isdigit() else -1)
    declared = None
    for path in reversed(alphas):
        version_txt = path / "version.txt"
        if not version_txt.is_file():
            continue
        text = version_txt.read_text(encoding="utf-8-sig")
        if "python_runtime=" in text:
            declared = path
            break
    if declared is None:
        # ★ 2026-10-10（P17-2，审计 04 F-05）：包内端到端事实 —— release 档不许退化为台账。
        package_fact("包内 Python 宿主判据",
                     "没有声明 `python_runtime=` 的包（早期序号没带 embeddable 标准库）", release)
        return
    exe = declared / "xrkit_app.exe"
    if not exe.is_file():
        fail("包 %s 声明了 python_runtime= 但没有 xrkit_app.exe" % declared.name)
        return
    proc_env = dict(os.environ)
    rc3, out3 = run([str(exe), "--python-host"], env=proc_env, timeout=300)
    line3 = next((ln.strip() for ln in out3.splitlines() if ln.strip().startswith("PYHOST")), "")
    # ★★ 判据必须**认准"用的是包内那份"**：只要求 `initialized=1` 是不够的 —— 本机装着
    #   Python 时，即使 home 解析漏了包内那份，解释器也会退到构建期烘焙的本机路径而"看起来正常"。
    #   `prefix=` 落在包目录里才证明"没装 Python 的机器也能起"。
    want_prefix = "prefix=" + str(declared).replace("\\", "/")
    line3_norm = line3.replace("\\", "/")
    if rc3 == 0 and " enabled=1 initialized=1" in line3 and want_prefix in line3_norm and \
            (" version=%s " % expect) in line3:
        ok("包内（%s）Python 宿主起来了，且用的确实是包内那份（%s）" % (declared.name, line3))
    else:
        fail("包内（%s）Python 宿主没起来 / 没用包内那份（要求 %s；rc=%d）：%s —— 包内 "
             "python_runtime= 声明不成立" % (declared.name, want_prefix, rc3,
                                          line3 or "(无 PYHOST 行)"))


def step_ui_facade(release: bool) -> None:
    """插件改装 P3：**控件门面（L1/L2）** —— 能力表 + 无 GUI 时的如实降级。

    判据：
      · **能力表**（`tests/check_ui_capability_table.py`）：`dir(xrkit.Api/Ui/Widget)` 与
        **预期表**逐条对账 —— **多一个＝能力泄漏、少一个＝门面被削**，都红（规格 §二「表外无门」）；
      · **无 GUI 进程**（CLI）里门面要**如实报不可用 + 理由**（零静默），不许静默返回空；
      · GUI 那条路（真控件上屏 + 回调回 Python + `get_text/set_text` + `ui.log` 进宿主日志栏）
        在 `shell-smoke` 里判 —— 那需要 Qt，纯命令行环境判不了（这一步只判"表"与"降级"）。
    """
    say("[ui-facade] 插件改装 P3：控件门面的**能力表**（表外无门）与降级口径")
    if not CLI_EXE.exists():
        prereq("构建产物 %s" % CLI_EXE.name, "缺失（先跑 build 步）", release)
        return
    rc_probe, out_probe = run([str(CLI_EXE), "--ui-facade"], timeout=300)
    for raw in out_probe.splitlines():
        s = raw.strip()
        if s.startswith("UIFACADE available=") or s.startswith("UIFACADE Api=") or \
                s.startswith("UIFACADE Ui=") or s.startswith("UIFACADE Widget="):
            say("  " + s[:150])
    rc, out = run([sys.executable, str(ROOT / "tests" / "check_ui_capability_table.py")],
                  timeout=180)
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("UICAP"):
            say("  " + s[:160])
    if rc == 0 and rc_probe == 0:
        ok("能力表逐条对上（多一个即泄漏、少一个即削门面）；无 GUI 时门面如实报不可用 + 理由")
    else:
        fail("控件门面能力表不通过（判据 rc=%d / 探针 rc=%d；见上）" % (rc, rc_probe))


def step_engine_facade(release: bool) -> None:
    """插件改装 P7a：**引擎调用门面**（用户 2026-10-07 拍板走方案 3）。

    判据（走真夹具，不是"接口存在"）：
      · `api.engine.font_size_slots()` 拿到引擎的槽位表（**个数与首尾都要对**）；
      · `api.engine.font_state_marks()` 拿到四态文案（**从引擎来**，插件不抄中文）；
      · `api.engine.font_check(<真字体>)` 拿到四态之一 —— 字体缺失时按**外部前提**记账，
        绝不当成通过（零静默：夹具里也会留一行 `check_skipped`）；
      · ★**线程纪律的另一半**：引擎调用能在**工作线程**上跑（与 `api.ui.*` 相反）。
    """
    say("[engine-facade] 插件改装 P7a：引擎调用门面（`api.engine.*`，表由样板页反向定义）")
    fixture = ROOT / "tests" / "python_plugins_engine"
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    if not fixture.is_dir():
        prereq("引擎门面夹具目录", "缺失：%s" % fixture.name, release)
        return
    marker = fixture / "engine_marker.txt"
    if marker.exists():
        marker.unlink()   # ★ 证据必须是**本轮**的（陈旧文件不算）
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    font = ""
    for candidate in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\msyh.ttf",
                      r"C:\Windows\Fonts\simsun.ttc"):
        if Path(candidate).is_file():
            font = candidate
            break
    if font:
        env["XRKIT_ENGINE_FONT"] = font
    rc, out = run([str(CLI_EXE), "--py-plugins", str(fixture)], env=env, timeout=600)
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINS") and ("error" in s or "files=" in s or "loaded=" in s):
            say("  " + s[:150])
    if not marker.is_file():
        fail("夹具没写出证据文件（`api.engine.*` 没生效？rc=%d）" % rc)
        return
    text = marker.read_text(encoding="utf-8")
    lines = [line.strip() for line in text.splitlines() if line.strip()]

    def value_of(key: str) -> str:
        for line in lines:
            if line.startswith(key + ":"):
                return line[len(key) + 1:]
        return ""

    slot_count = value_of("slot_count")
    slots = value_of("slots")
    problems = []
    if slot_count != "10":
        problems.append("槽位数=%s（应为 10）" % (slot_count or "(缺)"))
    if not slots.startswith("11,13,15"):
        problems.append("槽位表开头不对：%s" % (slots or "(缺)"))
    if not value_of("marks_ok"):
        problems.append("四态文案（ok）是空的 —— 文案必须从引擎拿")
    if not value_of("marks_missing"):
        problems.append("四态文案（missing）是空的")
    if value_of("thread_slots") != slot_count:
        problems.append("工作线程里的引擎调用没拿到同样的表（thread_slots=%s）"
                        % (value_of("thread_slots") or "(缺)"))
    if value_of("done") != "" and "done" not in lines:
        problems.append("夹具没跑到结尾")
    state = value_of("check_state")
    if font:
        if state not in ("ok", "japanese", "missing", "unknown"):
            problems.append("font_check 的状态不是四态之一：%r（字体=%s）" % (state, font))
    elif value_of("check_skipped") != "no-font":
        problems.append("没给字体时夹具应当留一行 check_skipped（零静默）")

    # ── ★★ P9a：convert 那一组（第二页迁移用的引擎面）────────────────────────
    #   判据全部走**真夹具**（真目录、真字节），不判"接口存在"。
    convert_expect = {
        "enc_manual_count": "16",                      # 源/目标共用那张表
        "enc_manual_head": "utf-8,windows-1251,windows-1252",
        "enc_target_only": "ascii",                    # 只在目标侧多出来的那一项
        "duration_3661": "1:01:01",                    # 唯一一份在引擎 `batch.cpp`
        "convert_selected": "1",
        "convert_success": "1",
        "convert_exit": "0",
        "convert_exists": "1",
        "convert_bytes_ok": "1",                       # ★逐字节 == 期望的 UTF-8（声明改写 + 无 BOM）
        "convert_messages": "1",                       # hooks 的 message 真被叫过
        "backup_exists": "1",                          # 原地转换 ⇒ `backup/<rel>.bak`
        "restore_failed": "0",
        "restore_bytes_ok": "1",                       # ★恢复要**逐字节**回到原样
        "clear_left": "0",                             # 清空之后 .bak 不在了
        "stats_ok": "1",
        "stats_bom": "1",                              # 统计 CSV 带 UTF-8 BOM（1.x 口径）
    }
    for key, want in convert_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("convert 面 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P10a：text 那一组（第三页迁移用的引擎面）──────────────────────────
    #   判据同样走**真目录真文件**：脚本改写 / .ltx 的保护性替换 / 取消谓词语义。
    text_expect = {
        "validate_bad": "1",                 # 目录名不是 gameplay/scripts ⇒ 有理由
        "validate_missing": "1",             # 目录不存在 ⇒ 有理由
        "kind_bad_ok": "0",                  # 判定必须失败
        "kind_bad_error": "1",               # 且必须给理由（零静默）
        "kind_ok": "scripts",
        "validate_good": "1",                # 好请求 ⇒ 空串（通过）
        "extract_ok": "1",
        "extract_kind": "scripts",
        "extract_files_positive": "1",
        "extract_strings_positive": "1",
        "extract_xml_name": "p10a_scripts_texts.xml",
        "extract_xml_exists": "1",
        "extract_rewrote": "1",              # 改写后出现 game.translate_string(...)
        "extract_comment_kept": "1",         # ★.ltx 注释里的命中**不许**被改写（保护性替换）
        "extract_cancel_flag": "1",          # 取消谓词一说停就停
        "extract_cancel_ok": "1",            # 但"取消"不是失败（引擎口径）
    }
    for key, want in text_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("text 面 %s=%s（应为 %s）" % (key, got or "(缺)", want))
    if not value_of("id_prefix_default"):
        problems.append("text 面 id_prefix_default 是空的（默认 ID 前缀必须来自引擎）")
    tree = value_of("extract_out_tree")
    if "scripts/a.script" not in tree or "p10a_scripts_texts.xml" not in tree:
        problems.append("text 面产物树不对：%s" % (tree or "(缺)"))

    # ── ★★ P11a：xml 那一组（第四页迁移用的引擎面）──────────────────────────
    xml_expect = {
        "xml_mode_count": "2",
        "xml_mode_keys": "stats,text",
        "xml_find_stats": "1",
        "xml_find_stats_key": "stats",
        "xml_find_unknown": "0",              # 未知键必须**不命中**（别静默退默认）
        "read_ok": "1",
        "read_has_text": "1",                 # cp1251 文件读出来是 UTF-8 且内容对
        "read_encoding": "windows-1251",
        "stats_mode": "stats",
        "stats_consistent": "0",              # 同一 id 次数不同 ⇒ 不一致
        "stats_count_diff": "1",
        "text_mode": "text",
        "text_diff_rows": "2",
        "one_side_flag": "1",
        "one_side_side": "b",
        "inline_detail_nonempty": "1",
        "full_detail_nonempty": "1",
        "full_text_detail_nonempty": "1",
        "csv_bom": "1",                       # Excel 不认无 BOM 的 UTF-8
        "csv_has_row": "1",
        "csv_write_ok": "1",
        "csv_roundtrip": "1",                 # ★落盘字节 == render_csv 的 UTF-8 字节
    }
    for key, want in xml_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("xml 面 %s=%s（应为 %s）" % (key, got or "(缺)", want))
    if not value_of("xml_find_text_columns") or value_of("xml_find_text_columns") == "0":
        problems.append("xml 面 text 模式的列数是 0（模式表没带列声明）")

    # ── ★★ P11b：xml **目录对比**（第四页真正跑起来要的那一块）───────────────
    dirs_expect = {
        "dirs_total": "5",                       # 并集：same/diff/only_a/only_b + 子目录里的同名
        "dirs_rows": "5",
        "dirs_kinds": "diff,only_a,only_b,same,same",   # 四种 kind 都要出
        "dirs_same_kind": "same",
        "dirs_nested_kind": "same",              # ★递归：子目录里的同名文件也配上了
        "dirs_diff_count": "1",
        "dirs_only_a_kind": "only_a",
        "dirs_only_b_kind": "only_b",
        "dirs_match_diff": "1",                  # 筛选命中的唯一实现（引擎）
        "dirs_match_same": "0",
        "dirs_cancel_flag": "1",                 # 取消在**第一个文件之前**生效
        "dirs_cancel_rows": "0",
    }
    for key, want in dirs_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("xml 目录对比 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P12a：video 那一组（第五页迁移用的引擎面）────────────────────────
    video_expect = {
        "video_parsed": "1",
        "video_has_video": "1",
        "video_has_audio": "1",
        "video_width": "1280",              # ffprobe JSON 解析口径
        "video_height": "720",
        "video_fps_positive": "1",
        "video_codec": "h264",
        "video_size_mb_positive": "1",
        "video_encoders": "libx264,mpeg4,libvorbis",   # `ffmpeg -encoders` 解析
        "video_argv_nonempty": "1",         # 转码 argv 由引擎构建
        "video_argv_has_input": "1",
        "video_argv_has_output": "1",
        "video_scale_applied": "1",         # 参考 640x480 ⇒ 要缩放
        "video_fps_applied": "1",           # 参考 25fps ⇒ 要调帧率
        "video_audio_fields": "1",
        "video_conflict_source": "same_as_source",
        "video_conflict_reason_nonempty": "1",
        "video_conflict_reference": "same_as_reference",
        "video_conflict_none": "none",
        "video_progress_matched": "1",
        "video_progress_percent_10": "1",
        "video_progress_capped": "1",       # 上限 99.5（永不 100）
        "video_progress_unmatched": "1",
        "video_major_version": "6",
        "video_version_rejected_8": "1",    # **只**拒 major==8（8.x theora 对 STALKER OGM 有 bug）
        "video_version_ok_6": "1",
    }
    for key, want in video_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("video 面 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P12b：宿主服务「跑进程」（第五页视频转码要用；语义唯一一份在引擎）────
    proc_expect = {
        "proc_echo_started": "1",
        "proc_echo_exit": "0",
        "proc_echo_line": "1",          # on_stdout 真的逐行回调
        "proc_echo_normal": "1",
        "proc_exit3_code": "3",         # ★退出码如实（不把"起不来"混进来）
        "proc_exit3_normal": "1",
        "proc_stderr_tail": "1",        # ★stderr 尾巴看得见
        "proc_stderr_lines": "1",
        "proc_cancel_flag": "1",        # ★取消真停
        "proc_cancel_timeout": "0",     # 而且是"真死透"，不是等超时
        "proc_cancel_fast": "1",        # 立刻回来（<3 秒）⇒ 证明是停它而不是等它
        "proc_cancel_orphan": "0",     # ★整棵树真停（取消后 tasklist 里没有 ping.exe）
        "proc_missing_started": "0",    # 起不来要如实说
        "proc_missing_error": "1",
    }
    for key, want in proc_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("跑进程 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P14-0a：fs 面第一批（纯函数那组）────────────────────────────────
    fs_expect = {
        "fs_formats_count": "8",                 # 实测：内置 8 档（含 auto / sqfs）
        "fs_has_auto": "1",
        "fs_has_sqfs": "1",
        "fs_format_count_positive": "1",
        "fs_xdb_is_db": "1",
        "fs_xdb_key": "xdb",
        "fs_xdb_name_nonempty": "1",
        "fs_auto_is_db": "0",                    # auto 不是 DB 格式
        "fs_auto_is_auto": "1",
        "fs_sqfs_is_sqfs": "1",
        "fs_is_package_db0": "1",
        "fs_is_package_txt": "0",
        "fs_pack_ext_sqfs": ".sqfs",
        "fs_pack_ext_xdb": ".xdb.db",
        "fs_preview_rows": "2",                  # 自带壳 ⇒ 原样两条（不叠第二层）
        "fs_preview_shelled_exact": "1",         # 自带壳 ⇒ 路径原样（不叠第二层）
        "fs_safe_bad_ok": "0",                   # 越界必须拒
        "fs_safe_bad_reason": "1",               # 且理由非空
        "fs_safe_ok_ok": "1",
        "fs_unpack_self_shell": "1",             # 自带壳 ⇒ 落点就是根
        "fs_plan_rows": "2",
        "fs_pack_target_ext": "1",
    }
    for key, want in fs_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("fs 面 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P14-0b：fs 面第二批（条目与合并树）────────────────────────────────
    tree_expect = {
        "loose_count": "3",                     # one.txt + sub + sub/two.txt
        "loose_error_empty": "1",
        "loose_has_sub": "1",
        "tree_sort_basename": "1",              # 按 **basename** 升序（文档口径）
        "tree_handle_positive": "1",
        "tree_sources": "a.db,b.db",            # 合并顺序 = 入参顺序
        "tree_has_files": "1",
        "tree_later_source_wins": "1",          # 6.1：后者为准
        "tree_conflict_positive": "1",          # 同路径被两个源定义
        "tree_checked_zero": "1",
        "tree_toggle_changed_positive": "1",
        "tree_toggle_raised": "1",
        "tree_snapshot_nonempty": "1",
        "tree_cleared": "1",
        "tree_roundtrip": "1",                  # 快照 → 清空 → 回灌 ⇒ 回到原样
        "tree_toggle_dir_ok": "1",
        "tree_source_state_files": "1",
        "tree_find_found": "1",
        "tree_find_missing": "1",
        "tree_freed": "1",                      # 释放后句柄失效（空表）
    }
    for key, want in tree_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("fs 树 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P14-0c：fs 面第三批（读条目/闸门/收集/解包）──────────────────────
    extraction_expect = {
        "gate_dir_nonempty": "1",
        "gate_plain_ok": "1",
        "gate_plain_same_path": "1",
        "pkg_entries_positive": "1",     # 真金样本包能列出条目
        "pkg_error_empty": "1",
        "pkg_missing_error": "1",        # 认不出/读不到要**报错**（不静默当空包）
        "pack_collect_items": "2",       # one.txt + sub/two.txt（目录不进 items）
        "pack_collect_sorted": "1",      # 产物确定性：按 rel 排序
        "extract_ok": "1",
        "extract_files_positive": "1",
        "extract_bytes_positive": "1",
        "extract_no_failures": "1",
        "extract_written_positive": "1",
        "extract_overwrite_visible": "1",   # ★ 第二次解到同一处 ⇒ **覆盖必须可见**
    }
    for key, want in extraction_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("fs 解包 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P14-0d：fs 面第四批（封包）─────────────────────────────────────────
    pack_expect = {
        "pack_sqfs_ok": "1",
        "pack_sqfs_size_positive": "1",
        "pack_sqfs_inodes": "3",               # 两个文件 **+ 根目录 inode**
        "pack_sqfs_roundtrip": "1",            # 打完再列 ⇒ 两个名字都在
        "pack_db_ok": "1",
        "pack_db_size_positive": "1",
        "pack_db_roundtrip": "1",              # 打完再列 ⇒ a.txt 在
        "pack_db_rewrites_visible": "1",       # ★ 中文名写不出 ⇒ 台账必须可见
        "pack_db_rewrite_written": "1",        # 改成了 '?' 形态
        "pack_db_bad_format_rejected": "1",    # auto 不是 DB ⇒ 拒绝
        "pack_db_bad_format_reason": "1",      # 且理由非空
    }
    for key, want in pack_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("fs 封包 %s=%s（应为 %s）" % (key, got or "(缺)", want))

    # ── ★★ P16-1a：统一产物模型（清单/安装/还原）────────────────────────────
    pkg_expect = {
        "pkg_manifest_ok": "1",          # 清单原子写 + 写后回读校验
        "pkg_install_ok": "1",
        "pkg_install_count": "2",
        "pkg_overwritten_visible": "1",  # ★ 覆盖必须可见（逐条）
        "pkg_target_new": "1",           # 真装进去了（目标是新内容）
        "pkg_restore_ok": "1",
        "pkg_restore_counts": "1",       # 还原 1 条 + 删掉 1 条（原本不存在的）
        "pkg_restored_bytes": "1",       # ★ 逐字节回旧
        "pkg_removed_new": "1",          # ★ 装了新文件要能退回去（删掉）
    }
    for key, want in pkg_expect.items():
        got = value_of(key)
        if got != want:
            problems.append("产物模型 %s=%s（应为 %s）" % (key, got or "(缺)", want))
    if not value_of("video_encoder"):
        problems.append("video 面 video_encoder 是空的（面板值必须来自 plan_transcode）")
    say("  夹具证据：%s" % text.replace("\n", " | ")[:260])
    if problems:
        fail("引擎调用门面不成立（%d 处）：%s" % (len(problems), "；".join(problems)))
        return
    if font:
        ok("引擎门面七处都通（槽位 10 档 / 四态文案 / font_check=%s / 工作线程可调 / "
           "convert 面：逐字节·备份恢复清空·CSV 带 BOM / text 面：校验·判定·真提取·取消语义 / "
           "xml 面：模式表·两种对比·单侧·引擎明细·CSV 往返一致 / "
           "**目录对比**：配对·四种 kind·递归·筛选·取消 / "
           "video 面：ffprobe 解析·编码器表·argv 与面板值·输出撞车·进度上限 99.5·版本闸门 / "
           "跑进程：逐行回调·退码如实·stderr 尾巴·取消真停且立刻回来·起不来要报 / "
           "fs 面（第一批）：格式表与谓词·包名判定·封包扩展名·落点预览与补目录·越界拒因·"
           "解包落点·封包落点）" % state)
    else:
        say("  · 未覆盖：font_check 的真字体那条（本机没有 msyh/simsun）")
        ok("引擎门面四处都通（槽位 / 四态文案 / 工作线程可调 / convert 面整条链路）；"
           "font_check 记为未覆盖")


def step_host_services(release: bool) -> None:
    """插件改装 P7b：**宿主服务门面**（`api.host.*`：偏好读写 / exe 目录 / 系统字体目录）。

    判据（走真夹具，不判"接口存在"）：
      · `pref_set` 之后再 `pref_get` 能读回**刚写的那一项**（走 `user.ltx` 的真实读写）；
      · 读一个不存在的键 ⇒ 拿到 `fallback`（不是空串、不是异常）；
      · `app_dir()` 非空且**真的是个目录**；
      · `system_font_dirs()` 非空（**与内置页同一份实现**：`engine::font::system_font_dirs`）。
    """
    say("[host-services] 插件改装 P7b：宿主服务门面（`api.host.*`，由样板页 fontpack 反向定义）")
    fixture = ROOT / "tests" / "python_plugins_host"
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    if not fixture.is_dir():
        prereq("宿主服务夹具目录", "缺失：%s" % fixture.name, release)
        return
    marker = fixture / "host_marker.txt"
    if marker.exists():
        marker.unlink()   # ★ 证据必须是**本轮**的
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    rc, out = run([str(CLI_EXE), "--py-plugins", str(fixture)], env=env, timeout=600)
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINS") and ("error" in s or "files=" in s):
            say("  " + s[:150])
    if not marker.is_file():
        fail("夹具没写出证据文件（`api.host.*` 没生效？rc=%d）" % rc)
        return
    text = marker.read_text(encoding="utf-8")
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    say("  夹具证据：%s" % text.replace("\n", " | ")[:200])

    def value_of(key: str) -> str:
        for line in lines:
            if line.startswith(key + ":"):
                return line[len(key) + 1:]
        return ""

    problems = []
    if value_of("pref_set") != "1":
        problems.append("pref_set 没成功（=%s）" % (value_of("pref_set") or "(缺)"))
    if value_of("pref_round_trip") != "hello-1234":
        problems.append("写进去的值读不回来（=%s）" % (value_of("pref_round_trip") or "(缺)"))
    if value_of("pref_fallback") != "fallback-x":
        problems.append("缺键时没返回 fallback（=%s）" % (value_of("pref_fallback") or "(缺)"))
    if value_of("app_dir_nonempty") != "1":
        problems.append("app_dir() 是空的")
    if value_of("app_dir_is_dir") != "1":
        problems.append("app_dir() 不是目录：%s" % value_of("app_dir_is_dir"))
    if not value_of("font_dir_count") or int(value_of("font_dir_count") or "0") < 1:
        problems.append("system_font_dirs() 是空的（=%s）" % (value_of("font_dir_count") or "(缺)"))
    if "done" not in lines:
        problems.append("夹具没跑到结尾")
    if problems:
        fail("宿主服务门面不成立（%d 处）：%s" % (len(problems), "；".join(problems)))
        return
    ok("宿主服务四处都通（偏好写入→读回=%s / 缺键回落 fallback / app_dir 是目录 / 系统字体目录 %s 个）"
       % (value_of("pref_round_trip"), value_of("font_dir_count")))


def step_fontpack_plugin(release: bool) -> None:
    """插件改装 P7d：**样板页的收口判据** —— 插件产出的汉化包与引擎参照**逐字节一致**。

    这条判据要的是"插件页与内置页**同源**"，所以：
      ① 参照走 `xrkit_cli --build-font-pack`（**内置 C++ 页用的就是这条引擎路**）；
      ② 插件走 `--py-plugin-call plugins <json>`（**无界面**调插件导出的 `headless(api, args)`）；
      ③ 两边都只做**一个字号**（`--only-size 11` / `only_sizes=[11]`）⇒ 秒级，能进 release 档；
      ④ 比对"文件表 + 每个文件的 sha256"，且**比较数必须 ≥1**（空集不算通过 —— 判据自证三问）。
    字体缺失按**外部前提**记账（不假装通过）。
    """
    say("[fontpack-plugin] 插件改装 P7d：样板页（汉化包生成）产物与引擎参照逐字节一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "fontpack_page.py"
    if not plugin.is_file():
        prereq("样板页插件 plugins/fontpack_page.py", "缺失", release)
        return
    font = ""
    for candidate in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\msyh.ttf",
                      r"C:\Windows\Fonts\simsun.ttc"):
        if Path(candidate).is_file():
            font = candidate
            break
    if not font:
        prereq("源字体（msyh/simsun）", "本机没有可用字体", release)
        return
    work = ROOT / "build" / "fontpack_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    xml_dir = work / "xml_in"
    xml_dir.mkdir(parents=True)
    (xml_dir / "a.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '\t<string id="a"><text>合成语料：汉字与西里尔 Привет，标点！？</text></string>\n'
        '</string_table>\n', encoding="utf-8")
    (xml_dir / "b.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
        '\t<string id="b"><text>第二条：换行与标点。？——</text></string>\n'
        '</string_table>\n', encoding="utf-8")
    ref_out = work / "ref"
    plug_out = work / "plug"
    rc_ref, out_ref = run([str(CLI_EXE), "--build-font-pack", "SoC", "chs", str(xml_dir),
                           font, str(ref_out), "--only-size", "11"], timeout=900)
    payload = json.dumps({"game": "SoC", "lang": "chs", "xml_dir": str(xml_dir),
                          "font_path": font, "out_dir": str(plug_out),
                          "only_sizes": [11]}, ensure_ascii=False)
    # ★★ P9b：**必须点名** —— `plugins/` 里不止一枚插件，探针的"第一个导出 headless 的"
    #   会随插件增删静默换对象（判据串味）；点名之后这条判据永远对着 fontpack 页。
    rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                             "--plugin", "fontpack_page.py", payload], timeout=900)
    for raw in out_plug.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL") and not s.startswith("PYPLUGINCALL result="):
            say("  " + s[:170])

    def digest(root: Path) -> dict:
        out = {}
        for path in sorted(root.rglob("*")):
            if path.is_file():
                out[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
        return out

    ref_tree = digest(ref_out)
    plug_tree = digest(plug_out)
    compared = sum(1 for key in ref_tree if key in plug_tree and ref_tree[key] == plug_tree[key])
    only_ref = sorted(set(ref_tree) - set(plug_tree))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    differing = sorted(k for k in set(ref_tree) & set(plug_tree) if ref_tree[k] != plug_tree[k])
    say("  参照 %d 个文件 / 插件 %d 个文件 / 逐字节相同 %d 个"
        % (len(ref_tree), len(plug_tree), compared))
    if only_ref or only_plug or differing:
        fail("插件产物与引擎参照不一致：只在参照 %s；只在插件 %s；内容不同 %s"
             % (only_ref[:4] or "无", only_plug[:4] or "无", differing[:4] or "无"))
        return
    if rc_ref != 0 or rc_plug != 0 or not ref_tree or compared < 1:
        fail("判据没跑成（参照 rc=%d / 插件 rc=%d / 参照文件 %d / 比较数 %d）—— 空集不算通过"
             % (rc_ref, rc_plug, len(ref_tree), compared))
        return
    ok("样板页（插件）产物与引擎参照**逐字节一致**（%d 个文件全等；同一份 `build_package`）"
       % compared)


def step_convert_plugin(release: bool) -> None:
    """插件改装 P9b：**第二页（编码转换）的收口判据**。

    判据：
      ① 参照 = `xrkit_cli --convert-batch`（内置 C++ 页用的就是这条引擎路）；
         插件 = `--py-plugin-call plugins --plugin convert_page.py`（插件页导出的 `headless`）；
      ② 两边产物**文件表 + 每文件 sha256 逐字节全等**，且**比较数 ≥1**（空集不算通过）；
      ③ 备份三件**跨实现**验一遍：CLI 造出 `backup/<rel>.bak` ⇒ **插件**的 `clear` 把它清掉
         （证明插件读得懂同一套备份布局）；恢复同理（`restore` 报告 ok ≥1）。
    源字体 / 语料都自带（本步自造夹具），字体等外部前提不涉及。
    """
    say("[convert-plugin] 插件改装 P9b：编码转换页（插件）产物与引擎参照逐字节一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "convert_page.py"
    if not plugin.is_file():
        prereq("转换页插件 plugins/convert_page.py", "缺失", release)
        return
    work = ROOT / "build" / "convert_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    src = work / "src"
    (src / "sub").mkdir(parents=True)
    body = ('<?xml version="1.0" encoding="windows-1251"?>\n<string_table>\n'
            '\t<string id="a"><text>Привет мир</text></string>\n</string_table>\n')
    (src / "a.xml").write_bytes(body.encode("cp1251"))
    (src / "sub" / "b.xml").write_bytes(body.replace('"a"', '"b"').encode("cp1251"))
    (src / "readme.txt").write_bytes(b"not an xml\n")
    ref_out = work / "ref"
    plug_out = work / "plug"

    rc_ref, out_ref = run([str(CLI_EXE), "--convert-batch", str(src), str(ref_out),
                           "windows-1251", "utf-8", "--ext", ".xml"], timeout=600)
    payload = json.dumps({"source": str(src), "output_dir": str(plug_out),
                          "extensions": [".xml"], "recursive": True, "backup": False,
                          "source_encoding": "windows-1251", "target_encoding": "utf-8"},
                         ensure_ascii=False)
    rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                             "--plugin", "convert_page.py", payload], timeout=600)
    for raw in out_plug.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL") and not s.startswith("PYPLUGINCALL result="):
            say("  " + s[:170])
    for raw in out_ref.splitlines():
        s = raw.strip()
        if s.startswith("CONVERTBATCH"):
            say("  " + s[:170])

    def digest(root: Path) -> dict:
        out = {}
        for path in sorted(root.rglob("*")):
            if path.is_file():
                out[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
        return out

    ref_tree = digest(ref_out)
    plug_tree = digest(plug_out)
    compared = sum(1 for key in ref_tree if key in plug_tree and ref_tree[key] == plug_tree[key])
    only_ref = sorted(set(ref_tree) - set(plug_tree))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    differing = sorted(k for k in set(ref_tree) & set(plug_tree) if ref_tree[k] != plug_tree[k])
    say("  参照 %d 个文件 / 插件 %d 个文件 / 逐字节相同 %d 个"
        % (len(ref_tree), len(plug_tree), compared))
    problems = []
    if only_ref or only_plug or differing:
        problems.append("产物不一致（只在参照 %s；只在插件 %s；内容不同 %s）"
                        % (only_ref[:4] or "无", only_plug[:4] or "无", differing[:4] or "无"))
    if rc_ref != 0 or rc_plug != 0 or len(ref_tree) < 2 or compared < 2:
        problems.append("判据没跑成（参照 rc=%d / 插件 rc=%d / 参照文件 %d / 比较数 %d）"
                        "—— 空集不算通过" % (rc_ref, rc_plug, len(ref_tree), compared))

    # ③ 备份三件：CLI 造备份 ⇒ **插件**清空（跨实现验同一套布局）
    rc_bak, _ = run([str(CLI_EXE), "--convert-batch", str(src), "", "windows-1251", "utf-8",
                     "--ext", ".xml", "--backup"], timeout=600)
    backup = src / "backup" / "a.xml.bak"
    had_backup = backup.is_file()
    rc_clear, out_clear = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                               "--plugin", "convert_page.py",
                               json.dumps({"action": "clear", "source_root": str(src),
                                           "extensions": [".xml"]}, ensure_ascii=False)],
                              timeout=600)
    cleared = not backup.is_file()
    say("  备份三件：CLI 造备份=%s；插件清空后 .bak 还在=%s（rc=%d）"
        % (had_backup, not cleared, rc_clear))
    if rc_bak != 0 or not had_backup:
        problems.append("CLI 原地+备份没造出 backup/<rel>.bak（rc=%d）" % rc_bak)
    if rc_clear != 0 or not cleared:
        problems.append("插件的 clear 没能清掉 CLI 造的备份（rc=%d / 还在=%s）"
                        % (rc_clear, not cleared))
    if problems:
        fail("编码转换页（插件）不成立：%s" % "；".join(problems))
        return
    ok("编码转换页（插件）与引擎参照**逐字节一致**（%d 个文件全等）；备份三件跨实现可用"
       % compared)


def step_facade_order(release: bool) -> None:
    """插件改装 P9b：**门面字段顺序 == 聚合初始化顺序**（三个门面逐项对位）。

    为什么单开一步：能力表比的是**名字集合**，顺序它管不着；而相邻字段类型相同时
    （`ask_open_file` / `ask_save_file` / `ask_directory` 都是 `std::string(const std::string&)`），
    对位错了**编译器不报错** ⇒ 参数喂给错的能力。本仓已因顺序被编译器拦下三次
    （都是类型不同才暴露），说明类型相同的那种**必须靠这一步**。
    """
    say("[facade-order] 插件改装 P9b：门面字段顺序与聚合初始化逐项对位")
    script = ROOT / "tests" / "check_facade_order.py"
    if not require_repo_file(script, "门面顺序核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("==") or s.startswith("✗") or s.startswith("✓") or s.startswith("总判"):
            say("  " + s)
    if rc == 0 and "全部对位" in out:
        ok("三个门面（Ui / Engine / Host）字段顺序与初始化**逐项对位**")
    else:
        fail("门面顺序有错位（见上）—— 类型相同的相邻字段对位错了编译器不会报错")


def step_facade_registry(release: bool) -> None:
    """插件改装 P16-0：**门面三份事实必须同名同数**（声明 / kMethodNames / has_impl）。

    为什么单开一步：能力表比的是 `dir(xrkit.Ui)` 的**名字集合**（来自 pybind `.def`），
    门面顺序比的是"结构体 vs 聚合初始化"的**顺序** —— 两者都管不着"字段声明了、
    `kMethodNames`/`has_impl` 里却没登记"这一类。实测缺口两处（Ui 的 `copy_text`、
    Host 的 `run_process` 名字不一致）：两处都让 `*_facade_has()` 恒 false，
    插件拿到的是"能力看着在、实际恒不可用"，而**没有任何一步会红**。
    独立成步（静态判据、不依赖任何构建产物）：被 `--only` 过滤时按记账进 UNCOVERED。
    退出码：0=三点闭环、1=有断言不成立（含仓库自带文件缺失 —— 那是 FAIL，不是 SKIP）。
    """
    say("[facade-registry] 插件改装 P16-0：门面三份事实（声明/kMethodNames/has_impl）三点闭环")
    script = ROOT / "tests" / "check_facade_registry.py"
    if not require_repo_file(script, "门面登记表核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("FACADE-REG") or s.startswith("== ") or s.startswith("✗ 字段有")
                or s.startswith("✗ kMethodNames") or s.startswith("✗ has_impl")
                or s.startswith("✓ 反例自证")):
            say("  " + s)
    if rc == 0 and "FACADE-REG ok" in out:
        ok("三门面三点闭环（声明 / kMethodNames / has_impl 逐名同数，反例自证 6/6）")
    else:
        fail("门面登记表有缺口（见上）—— 字段没进 kMethodNames/has_impl 的能力恒不可用")


def step_thread_contract(release: bool) -> None:
    """**线程与取消契约**的静态判据（P17-1；审计 §一·1 / 03 F-01 / 03 F-02 / 05 PLG-01 / 05 PLG-07）。

    一条判据覆盖两条契约、三条缺陷：
      ① **worker 不许碰 UI**（1.x `TaskRunner` 13.2）—— `03 F-01`（Critical，汉化包页 worker
         读下拉框 + 写 QLabel + 裸 `++refused_count_`）与五个插件页的 worker 里那些
         `ui.set_text` / `ui.log_view_append` / `ui.get_text`（被线程闸门**静默**变成 no-op）；
      ② **取消要真能读到并停**（用户红线）—— `05 PLG-01`（`ui.task_cancel_requested` 在
         worker 里被一刀切拒掉 ⇒ 四页「取消」关不掉、还谎报"已取消"）。

    为什么单开一步：修前**全仓没有一条**门禁量管这两件事（Hana 05 PLG-07 原话：「门禁全绿，
    但『取消关不掉 / worker 刷控件』这类红线级问题没有任何判据兜住」）。判据形状：
      · C++ 内置页：抽出 `task_->run(...)` 的 worker 体（剥掉"排队回主线程"那一段之后剩下的
        才是真在 worker 里跑的代码）⇒ 不许直接碰控件成员，也不许调"会碰控件的成员函数"
        （除非那条路自己带 `ui_thread_guard` 可判接缝）；
      · `.py` 插件：AST 找 `ui.task_run(..., worker)` 的 worker，worker 体内（含嵌套函数/
        lambda）只许调两个 sink 与**跨线程只读白名单**（名单从 C++ 读，不手抄）。
    独立成步（静态判据、不依赖任何构建产物）：被 `--only` 过滤时按记账进 UNCOVERED。
    退出码：0=两条契约成立、1=有断言不成立（含仓库自带文件缺失 —— 那是 FAIL，不是 SKIP）。
    """
    say("[thread-contract] P17-1：线程与取消契约（worker 不许碰 UI / 取消要读得到）")
    script = ROOT / "tests" / "check_thread_contract.py"
    if not require_repo_file(script, "线程与取消契约核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("THREAD-CONTRACT") or s.startswith("== ") or s.startswith("✗")
                or s.startswith("✓ 反例自证") or s.startswith("跨线程只读白名单")):
            say("  " + s)
    if rc == 0 and "THREAD-CONTRACT ok" in out:
        ok("worker 只碰引擎与两个 sink；取消位读得到（含反例自证）")
    else:
        fail("线程/取消契约有缺口（见上）—— worker 碰 UI 或取消读不到都算红")


def step_sqfs_dos(release: bool) -> None:
    """SquashFS 读侧**三处 DoS**的判红锁（P17-2；审计 01 F-01 / F-02 / F-03，三处都 High）。

    为什么单开一步：这三条都是「**打开一份手工构造的畸形包就挂**」——
      · `01 F-01` 目录项成环：`Image::list()` 修前无条件压栈、无 visited/深度/条目上限
        ⇒ 自环时无限展开 `a/a/a/…`（内存耗尽 / 卡死）；
      · `01 F-02` xz 字典炸弹：`decompress.cpp` 修前把 `memlimit` 置成 `UINT64_MAX`
        ⇒ 块头声明的 `dict_size`（最大约 1.5 GiB）会被真分配；
      · `01 F-03` 伪造 `block_size`：`parse_superblock` 修前不校验，而 fragment 解压把超块里的
        `bs` 直接当输出容量 ⇒ `block_size=0xFFFFFFFF` 时一次性分配约 4 GiB。
    判据形状（`tests/check_sqfs_dos.py`）：三份**现造**的最小畸形镜像各跑一次 CLI ——
    必须**在限时内明确报错**（rc≠0 + 理由非空且点名"上限"）、不留半成品；
    外加"正常镜像不受影响"与"4 字节垃圾文件的拒因不同"两条反向锁，以及反例自证。
    退出码：0=绿 / 1=有断言不成立 / 2=前提缺失（CLI 不存在等）。
    """
    say("[sqfs-dos] P17-2：SquashFS 三处 DoS（目录项成环 / xz 字典炸弹 / 伪造 block_size）")
    script = ROOT / "tests" / "check_sqfs_dos.py"
    if not require_repo_file(script, "SquashFS DoS 判据脚本"):
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("SQFS-DOS") or s.startswith("== ") or s.startswith("✗")
                or s.startswith("✓ 反例自证") or s.startswith("夹具")):
            say("  " + s)
    if rc == 0 and "SQFS-DOS ok" in out:
        ok("三处 DoS 都判得红且已修绿（畸形包明确报错 + 正常镜像不受影响）")
    elif rc == 2:
        prereq("SquashFS DoS 判据的 CLI 前提", "脚本报前提缺失（见上）", release)
    else:
        fail("SquashFS 读侧仍有 DoS 形状（见上）—— 打开畸形包必须明确报错，不是挂死/巨量分配")


def step_plugin_facade_returns(release: bool) -> None:
    """插件改装 P16-0：**插件不许丢弃「成没成」型门面调用的返回值**。

    为什么单开一步：`copy_text` 那条静默链的**第二半**在插件侧 ——
    `plugins/xml_page.py` 不看返回值、紧接着显示"已复制 ID"（谎报成功）。
    修法是看返回值走两支；本步保证"不能被写回去"。
    判据从 `widget_facade.h` 的 `UiFacade` 逐字段取**返回类型**：
    `void` / `UiHandle`（控件构造句柄）被丢弃是正常的，其余（bool 的成败位、
    int/string 的取值）被裸丢弃 ⇒ 红。**不设允许丢弃白名单**（白名单会变成橡皮章）。
    独立成步（静态判据、不依赖构建产物）。退出码：0=绿、1=有断言不成立。
    """
    say("[plugin-facade-ret] 插件改装 P16-0：插件不得丢弃「成没成」型门面返回值")
    script = ROOT / "tests" / "check_plugin_facade_returns.py"
    if not require_repo_file(script, "插件返回值核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("PLUGIN-RET") or s.startswith("✓ ")
                or s.startswith("✗ ") or s.startswith("第 ")):
            say("  " + s)
    if rc == 0 and "PLUGIN-RET ok" in out:
        ok("六个插件丢弃的只有 void/UiHandle（成败位一律看着走两支）")
    else:
        fail("有插件丢弃了「成没成」型返回值（见上）—— 失败会静默")


def step_official_pages(release: bool) -> None:
    """插件改装 P16-0b：**官方六页"是不是插件"**的三条判据 + 一条棘轮。

    为什么单开一步：`tests/` 全目录搜 `replaces` 原先是 **0 命中** —— 六枚插件确实都传了
    replaces，但**没有任何判据**锁它：去掉 replaces、或让两枚插件指向同一个内置页，
    门禁一声不响。棘轮那一条守的是"六页对等"的方向：内置 `pages/` 直连引擎头
    **逐文件只许减不许增**（今天 22 处）。
    独立成步（静态判据、不依赖构建产物）。退出码：0=绿、1=有断言不成立。
    """
    say("[official-pages] 插件改装 P16-0b：官方页 replaces 契约 + 插件隔离 + 引擎直连棘轮")
    script = ROOT / "tests" / "check_official_pages_contract.py"
    if not require_repo_file(script, "官方页契约核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("OFFICIAL-PAGES") or s.startswith("✓ 反例自证")
                or s.startswith("✗ ") or s.startswith("棘轮")):
            say("  " + s)
    if rc == 0 and "OFFICIAL-PAGES ok" in out:
        ok("六枚插件逐枚显式 replaces、六个内置页各被覆盖一次；内置页引擎直连 22 处未增")
    else:
        fail("官方页契约不成立（见上）—— 插件化「六页对等」那一条没有判据就只是口号")


def step_abi_closure(release: bool) -> None:
    """插件改装 P16-0b：`struct stk_ui` 的 16 方法 ↔ 契约表 `kUiMethods[]` **逐名同序**。

    为什么单开一步：此前全仓只有生成器自报的 `EXPECTED_UI_METHODS = 16`（管契约表**自己的**条数），
    **没有任何判据**拿契约表与 C ABI 结构体对账 ⇒ 结构体里改名/换序/少一个，
    第三方照契约写出来的插件会在运行期拿到空指针或喂错参数，而门禁一声不响。
    独立成步。退出码：0=绿、1=有断言不成立。
    """
    say("[abi-closure] 插件改装 P16-0b：struct stk_ui ↔ kUiMethods[] 闭合")
    script = ROOT / "tests" / "check_plugin_abi_closure.py"
    if not require_repo_file(script, "ABI 闭合核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("ABI-CLOSURE") or s.startswith("✓ 反例自证") or s.startswith("✗ "):
            say("  " + s)
    if rc == 0 and "ABI-CLOSURE ok" in out:
        ok("16 个契约方法与 C ABI 结构体**逐名同序**（换序/改名/少一个都会红）")
    else:
        fail("契约表与 C ABI 结构体不闭合（见上）")


def step_doc_citations(release: bool) -> None:
    """插件改装 P16-0b：判据引用的文档**必须真的存在**。

    为什么单开一步：另一个会话的只读核对发现 `check_page_host_lock.py` 把一份
    **全仓不存在**的规格文档引为判据来源 ⇒ 后来的人无从核对"这条锁在守哪句话"。
    本步扫 `tests/` `tools/` `plugins/` 里所有 `docs/*.md` 引用逐个核对存在性。
    独立成步。退出码：0=绿、1=有断言不成立。
    """
    say("[doc-cite] 插件改装 P16-0b：判据引用的 docs/*.md 必须存在")
    script = ROOT / "tests" / "check_doc_citations.py"
    if not require_repo_file(script, "文档引用核对器"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("DOC-CITE") or s.startswith("✓ 反例自证") or s.startswith("✗ "):
            say("  " + s)
    if rc == 0 and "DOC-CITE ok" in out:
        ok("判据引用的文档全部存在（引一份不存在的规格 = 判据没有出处）")
    else:
        fail("有判据引用了不存在的文档（见上）")


def step_patch_identify(release: bool) -> None:
    """插件改装 P16-1b 判定半：二进制补丁识别与 **1.x 金标准逐条对拍**。

    为什么单开一步：引擎侧的 PE 布局守卫 + 逐项判态 + 指纹状态机是从 1.x
    `engine_utf8_patch.py` 逐条搬的，"搬对了没有"只能拿 1.x 自己的输出当尺子。
    1.x 插件住在仓库外 ⇒ 由一次性引导脚本把它的结果导出成**仓库内金标准**
    （`tests/golden_patch/identify_golden.json`，含 1.x 脚本 sha256 与三份样本 sha256 ⇒ 防漂移）。
    判据：结论 key / 五个汇总计数 / `thunk_intact` / `cave_all_zero` / **逐项判态**逐条相等；
    `diff_bytes` 只打印、**不参与判定**（用户红线：v3 与 v4 该数完全相同）。
    三份样本在仓库外 ⇒ 缺件走 `prereq`。退出码：0=绿、1=判据不成立、2=样本缺件。
    """
    say("[patch-identify] P16-1b 判定半：与 1.x 金标准逐条对拍（原件 / v2 缺陷 / v4 正确）")
    script = ROOT / "tests" / "check_patch_identify.py"
    if not require_repo_file(script, "二进制补丁识别判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script), str(CLI_EXE)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("PATCH-IDENT") or s.startswith("✓ ") or s.startswith("✗ ")
                or s.startswith("⚠ ") or s.startswith("参考") or s.startswith("金标准来源")):
            say("  " + s)
    if rc == 0 and "PATCH-IDENT ok" in out:
        ok("三份样本与 1.x 金标准逐条相等（结论 / 逐项判态 / 汇总计数 / thunk / 空闲区）")
    elif rc == 2:
        prereq("二进制补丁样本", "样本缺件（外部前提；三份都在仓库外）", release)
    else:
        fail("二进制补丁识别与金标准不等（见上）")


def step_patch_apply(release: bool) -> None:
    """插件改装 P16-1b 写盘半：产物**逐字节**等于 v4 参照 ＋ 四条拒绝路径。

    判据：① 产物 sha256 == 金标准里 `v4_correct` 样本的 sha256（整文件哈希相等 ⇒ 逐字节相同）
    且字节数相等；② 跑完所有用例后**输入 sha256 未变**；③ 输出已存在且未 `--force` ⇒ `refused=exists`；
    ④ 同一条加 `--force` ⇒ 成功；⑤ 输出 == 输入 ⇒ `refused=input`（**哪怕给了 --force**）；
    ⑥ 归档 v2 缺陷样本当输入 ⇒ 被 `preserve` 守卫拦下并点名 `keep.thunk`，**且不产出任何文件**
    （零静默 + 不留半成品）；⑦ 已打过的 v4 参照当输入 ⇒ 拦下并点出"已是终态"。
    ★ 产物只写 `%TEMP%`（绝不在仓库里写产物）；`written_bytes` 只打印、不参与判定。
    退出码：0=绿、1=判据不成立、2=样本缺件。
    """
    say("[patch-apply] P16-1b 写盘半：产物逐字节 == v4 参照 + 拒绝路径 + 输入不可变")
    script = ROOT / "tests" / "check_patch_apply.py"
    if not require_repo_file(script, "二进制补丁写出判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script), str(CLI_EXE)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("PATCH-APPLY") or s.startswith("✓ ") or s.startswith("✗ ")
                or s.startswith("⚠ ")):
            say("  " + s)
    if rc == 0 and "PATCH-APPLY ok" in out:
        ok("产物逐字节等于 v4 参照；输出已存在 / 输出==输入 / 缺陷输入 / 已打过 四条路径都按预期拒绝，"
           "输入未被改动")
    elif rc == 2:
        prereq("二进制补丁样本", "样本缺件（外部前提；原件在仓库外）", release)
    else:
        fail("二进制补丁写出有判据不成立（见上）")


def step_toolchain_plugin(release: bool) -> None:
    """插件改装 P16-1c：**插件页**（汉化产线）经 `api.engine.*` 驱动引擎（两个面板）。

    为什么单开一步：P16-1b 那两步走的是 **CLI 动作**（`--patch-identify`/`--patch-apply`）；
    本步走**插件那条路**（`--py-plugin-call … --plugin toolchain_page.py <json>`）——
    证明「门面 ＋ 站点表（插件持有）＋ `py_host` 的 dict↔结构体转换 ＋ 清单写入」**整条链**都对。
    面板①（自动二进制补丁）：三样本的 verdict / 五个汇总计数 / thunk / 空闲区 / 逐项判态
    与 1.x 金标准逐条相等；产物 sha256 == 金标准 v4 参照；输出已存在 ⇒ 拒绝；缺陷输入 ⇒ 守卫拦下。
    面板②（分区字体）：与 CLI 参照 `--build-font-pack … --only-size 11` **逐字节对拍** ＋
    **档位语义**（只出所选档）＋ **清单台账**（TSV 条数 == 产物数、抽条 sha256 与实算一致）。
    外部前提：三样本（仓库外）与源字体（msyh/simsun）缺件走 `prereq`。
    退出码：0=绿、1=判据不成立、2=外部前提缺件。
    """
    say("[toolchain-plugin] P16-1c：插件页（两个面板）经 api.engine.* 驱动引擎")
    script = ROOT / "tests" / "check_toolchain_plugin.py"
    if not require_repo_file(script, "汉化产线插件页判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script), str(CLI_EXE)], timeout=1200)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("TOOLCHAIN") or s.startswith("✓ ") or s.startswith("✗ ")
                or s.startswith("⚠ ")):
            say("  " + s)
    if rc == 0 and "TOOLCHAIN ok" in out:
        ok("面板①与 1.x 金标准逐条相等＋产物逐字节；面板②与 CLI 参照逐字节＋档位语义＋清单台账")
    elif rc == 2:
        prereq("二进制补丁样本 / 源字体", "外部前提缺件", release)
    else:
        fail("汉化产线插件页有判据不成立（见上）")


def step_package_plugin_list(release: bool) -> None:
    """插件改装 P16-2a：**随包插件清单 + 包内自检口径**的静态锁（切打包口径）。

    为什么静态锁也单开一步：端到端打包要几分钟、进不了门禁，而"切打包口径"最容易
    悄悄退回去 —— 修前 `package.ps1` 只拷一枚 `hello.py` 夹具、`smoke.cpp` 的 #8 note
    **只数 `*.dll`**、判据只按 `插件：(\\d+) 个 dll` 解析 ⇒ 这段口径**根本没有判据**：
    把 `.py` 那一段删掉、或把 note 改回只报 dll，门禁一声不响。
    本步锁三件事：① 随包 `.py` 是**显式清单**且与 `plugins/*_page.py` **逐名一致**；
    ② `package.ps1` 的解析要求**新格式**（同一处同时含 `个文件`/`个 dll`/`个 py`）；
    ③ `smoke.cpp` 的 `#8 包内插件：` 那一行**同时**报出两组计数。脚本自带**反例自证**。
    """
    say("[package-plugin-list] P16-2a：随包插件清单与包内自检口径（静态锁 + 反例自证）")
    script = ROOT / "tests" / "check_package_plugin_list.py"
    if not require_repo_file(script, "随包插件清单静态锁"):
        return
    rc, out = run([sys.executable, str(script)], timeout=180)
    for line in out.splitlines():
        s = line.strip()
        if (s.startswith("PKG-PLUGINS") or s.startswith("✓ ") or s.startswith("✗ ")
                or s.startswith("--") or s.startswith("plugins/")):
            say("  " + s)
    if rc == 0 and "PKG-PLUGINS ok" in out:
        ok("随包插件清单显式（六页插件版 + 三插件同页）＋ 包内自检同时报 dll 与 py")
    else:
        fail("随包插件清单/包内自检口径有断言不成立（见上）")


def step_text_plugin(release: bool) -> None:
    """插件改装 P10b：**第三页（文本提取/改写）的收口判据**。

    判据：
      ① 参照 = `xrkit_cli --extract-text`（内置 C++ 页用的就是这条引擎路）；
         插件 = `--py-plugin-call plugins --plugin text_page.py`（插件页导出的 `headless`）；
      ② 两边产物**文件表 + 每文件 sha256 逐字节全等**，且**比较数 ≥2**（空集不算通过）；
      ③ **取消语义**：JSON 里 `"cancel": true` ⇒ 确定性地"第一次问就说停" ⇒
         结果必须 `cancelled=True` / `ok=True`（取消不是失败）/ `files=0`（还没写就停）。
    """
    say("[text-plugin] 插件改装 P10b：文本提取页（插件）产物与引擎参照逐字节一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "text_page.py"
    if not plugin.is_file():
        prereq("文本提取插件 plugins/text_page.py", "缺失", release)
        return
    work = ROOT / "build" / "text_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    src = work / "scripts"
    src.mkdir(parents=True)
    (src / "a.script").write_text('local s = "Привет, сталкер!"\n', encoding="cp1251")
    (src / "d.ltx").write_text('[wpn_akm]\ninv_name = "АКМ"\n; inv_name = Не должно попасть\n',
                               encoding="cp1251")
    ref_out = work / "ref"
    plug_out = work / "plug"

    rc_ref, out_ref = run([str(CLI_EXE), "--extract-text", str(src), str(ref_out),
                           "--prefix", "p10b", "--id-prefix", "sgtat_p10b_gp_0", "--clean"],
                          timeout=600)
    payload = json.dumps({"source_dir": str(src), "target_dir": str(plug_out), "prefix": "p10b",
                          "id_prefix": "sgtat_p10b_gp_0", "clean": True}, ensure_ascii=False)
    rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                             "--plugin", "text_page.py", payload], timeout=600)
    for raw in out_plug.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL") and not s.startswith("PYPLUGINCALL result="):
            say("  " + s[:170])
    for raw in out_ref.splitlines():
        s = raw.strip()
        if s.startswith("TEXT ok") or s.startswith("TEXT fail"):
            say("  " + s[:170])

    def digest(root: Path) -> dict:
        out = {}
        for path in sorted(root.rglob("*")):
            if path.is_file():
                out[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
        return out

    ref_tree = digest(ref_out)
    plug_tree = digest(plug_out)
    compared = sum(1 for key in ref_tree if key in plug_tree and ref_tree[key] == plug_tree[key])
    only_ref = sorted(set(ref_tree) - set(plug_tree))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    differing = sorted(k for k in set(ref_tree) & set(plug_tree) if ref_tree[k] != plug_tree[k])
    say("  参照 %d 个文件 / 插件 %d 个文件 / 逐字节相同 %d 个"
        % (len(ref_tree), len(plug_tree), compared))
    problems = []
    if only_ref or only_plug or differing:
        problems.append("产物不一致（只在参照 %s；只在插件 %s；内容不同 %s）"
                        % (only_ref[:4] or "无", only_plug[:4] or "无", differing[:4] or "无"))
    if rc_ref != 0 or rc_plug != 0 or len(ref_tree) < 2 or compared < 2:
        problems.append("判据没跑成（参照 rc=%d / 插件 rc=%d / 参照文件 %d / 比较数 %d）"
                        "—— 空集不算通过" % (rc_ref, rc_plug, len(ref_tree), compared))

    # ③ 取消语义（插件 headless；确定性谓词）
    cancel_payload = json.dumps({"source_dir": str(src), "target_dir": str(work / "cancel_out"),
                                 "prefix": "p10b", "id_prefix": "sgtat_p10b_gp_0",
                                 "clean": True, "cancel": True}, ensure_ascii=False)
    rc_cancel, out_cancel = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "text_page.py", cancel_payload], timeout=600)
    result = {}
    for raw in out_cancel.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
            try:
                result = json.loads(s.split(" result=", 1)[1])
            except ValueError:
                result = {}
    say("  取消：rc=%d cancelled=%s ok=%s files=%s"
        % (rc_cancel, result.get("cancelled"), result.get("ok"), result.get("files")))
    if result.get("cancelled") is not True or result.get("ok") is not True:
        problems.append("取消语义不对（cancelled=%s / ok=%s）—— 取消既要真停，也不能报成失败"
                        % (result.get("cancelled"), result.get("ok")))
    if result.get("files") != 0:
        problems.append("取消在**第一个文件边界**就停 ⇒ files 应为 0（实得 %s）"
                        % result.get("files"))
    if problems:
        fail("文本提取页（插件）不成立：%s" % "；".join(problems))
        return
    ok("文本提取页（插件）与引擎参照**逐字节一致**（%d 个文件全等）；取消语义（cancelled+ok+files=0）"
       "也对" % compared)


def step_video_plugin(release: bool) -> None:
    """插件改装 P12c-3：**第五页（视频转换）的收口判据**。

    判据：三块（plan / probe / encoders）都与 CLI 的内置路径**逐行一致**；
    其中 plan 用 CLI `--video-plan` 的 7 组语料（逐字转成请求 JSON，同名），
    probe/encoders 用 `tests/golden/` 里的**真 ffprobe / 真 ffmpeg -encoders** 字节。
    """
    say("[video-plugin] 插件改装 P12c-3：视频页（插件）与引擎参照逐行一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "video_page.py"
    if not plugin.is_file():
        prereq("视频插件 plugins/video_page.py", "缺失", release)
        return
    work = ROOT / "build" / "video_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)

    # ── ① plan：CLI 的 7 组语料（逐字转成请求 JSON，名字也一样）──────────────
    enc_all = ["libtheora", "libxvid", "mpeg4"]
    enc_no_xvid = ["libtheora", "mpeg4"]
    enc_only_mpeg4 = ["mpeg4"]

    def media(has_video, codec, width, height, fps, bitrate, has_audio, rate, channels, abitrate):
        return {"parsed": True, "has_video": has_video, "has_audio": has_audio,
                "video": {"codec": codec, "width": width, "height": height, "fps": fps,
                          "bitrate": bitrate},
                "audio": {"codec": "vorbis", "sample_rate": rate, "channels": channels,
                          "bitrate": abitrate},
                "duration": 60.0, "size_mb": 10.0}

    def source(width, height, fps, has_audio, parsed=True):
        info = media(True, "h264", width, height, fps, 0, has_audio, 44100, 2, 128000)
        info["parsed"] = parsed
        info["has_video"] = parsed
        return info

    cases = [
        ("theora_ref_bitrate_src_same_audio",
         media(True, "theora", 640, 480, 25.0, 800000, True, 44100, 2, 128000),
         source(640, 480, 25.0, True), enc_all),
        ("theora_ref_no_bitrate_src_diff_noaudio",
         media(True, "h264", 0, 0, 0.0, 0, False, 0, 0, 0),
         source(320, 240, 30.0, True), enc_all),
        ("mpeg4_ref_no_bitrate_src_noaudio",
         media(True, "mpeg4", 640, 480, 25.0, 0, True, 0, 0, 0),
         source(640, 480, 25.0, False), enc_all),
        ("mpeg4_ref_fallback_noxvid",
         media(True, "mpeg4", 640, 480, 25.0, 0, True, 0, 0, 0),
         source(640, 480, 25.0, True), enc_no_xvid),
        ("last_resort_only_mpeg4",
         media(True, "theora", 640, 480, 25.0, 800000, True, 44100, 2, 128000),
         source(640, 480, 25.0, True), enc_only_mpeg4),
        ("ffprobe_failed_keeps_audio",
         media(True, "theora", 640, 480, 25.0, 800000, True, 44100, 2, 128000),
         source(0, 0, 0.0, False, parsed=False), enc_all),
        ("no_reference_at_all", None, source(640, 480, 25.0, True), enc_all),
    ]

    problems = []
    for name, reference, src, encoders in cases:
        request = {"case": name, "input": "IN.avi", "output": "OUT.ogm",
                   "source": src, "available_encoders": encoders}
        if reference is not None:
            request["reference"] = reference
        request_path = work / ("plan_%s.json" % name)
        request_path.write_text(json.dumps(request, ensure_ascii=False, indent=1),
                                encoding="utf-8")
        payload = json.dumps({"mode": "plan", "request_path": str(request_path)},
                             ensure_ascii=False)
        rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "video_page.py", payload], timeout=600)
        plug_lines = []
        for raw in out_plug.splitlines():
            s = raw.strip()
            if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
                try:
                    plug_lines = json.loads(s.split(" result=", 1)[1]).get("lines", [])
                except ValueError:
                    plug_lines = []
        rc_ref, out_ref = run([str(CLI_EXE), "--video-plan"], timeout=600)
        ref_lines = [line for line in out_ref.splitlines()
                     if line.startswith("PLAN\t" + name + "\t")
                     or line.startswith("PLANSUM\t" + name + "\t")]
        if rc_plug != 0 or rc_ref != 0:
            problems.append("%s：命令没跑成（插件 rc=%d / 参照 rc=%d）" % (name, rc_plug, rc_ref))
            continue
        if not ref_lines:
            problems.append("%s：参照侧没打出这组语料（CLI 语料表变了？）" % name)
            continue
        if plug_lines != ref_lines:
            detail = ""
            for index in range(max(len(plug_lines), len(ref_lines))):
                left = plug_lines[index] if index < len(plug_lines) else "(缺)"
                right = ref_lines[index] if index < len(ref_lines) else "(缺)"
                if left != right:
                    detail = "第 %d 行：参照「%s」/ 插件「%s」" % (index, right, left)
                    break
            problems.append("%s：计划不一致 —— %s" % (name, detail))
    say("  plan：%d 组语料已对拍" % len(cases))

    # ── ② probe：四份真 ffprobe 输出 ────────────────────────────────────────
    probe_dir = ROOT / "tests" / "golden" / "video_probe"
    probe_files = sorted(probe_dir.glob("*.json")) if probe_dir.is_dir() else []
    if not probe_files:
        problems.append("probe 夹具缺失（%s 下没有 *.json）" % probe_dir)
    for probe_file in probe_files:
        payload = json.dumps({"mode": "probe", "json_path": str(probe_file)},
                             ensure_ascii=False)
        rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "video_page.py", payload], timeout=600)
        plug_lines = []
        for raw in out_plug.splitlines():
            s = raw.strip()
            if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
                try:
                    plug_lines = json.loads(s.split(" result=", 1)[1]).get("lines", [])
                except ValueError:
                    plug_lines = []
        rc_ref, out_ref = run([str(CLI_EXE), "--video-probe", str(probe_file)], timeout=600)
        ref_lines = [line for line in out_ref.splitlines() if line.startswith("PROBE\t")]
        if plug_lines != ref_lines:
            detail = ""
            for index in range(max(len(plug_lines), len(ref_lines))):
                left = plug_lines[index] if index < len(plug_lines) else "(缺)"
                right = ref_lines[index] if index < len(ref_lines) else "(缺)"
                if left != right:
                    detail = "第 %d 行：参照「%s」/ 插件「%s」" % (index, right, left)
                    break
            problems.append("%s：探测解析不一致 —— %s" % (probe_file.name, detail))
    say("  probe：%d 份真 ffprobe 输出已对拍" % len(probe_files))

    # ── ③ encoders：真 `ffmpeg -encoders` 输出 ──────────────────────────────
    # ★ 夹具实际位置：`tests/golden/video_probe/ffmpeg_encoders.txt`
    enc_file = ROOT / "tests" / "golden" / "video_probe" / "ffmpeg_encoders.txt"
    if not enc_file.is_file():
        problems.append("encoders 夹具缺失（%s）" % enc_file)
    else:
        payload = json.dumps({"mode": "encoders", "text_path": str(enc_file)},
                             ensure_ascii=False)
        rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "video_page.py", payload], timeout=600)
        plug_lines = []
        for raw in out_plug.splitlines():
            s = raw.strip()
            if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
                try:
                    plug_lines = json.loads(s.split(" result=", 1)[1]).get("lines", [])
                except ValueError:
                    plug_lines = []
        rc_ref, out_ref = run([str(CLI_EXE), "--video-encoders", str(enc_file)], timeout=600)
        ref_lines = [line for line in out_ref.splitlines()
                     if line.startswith("ENCODERS\t") or line.startswith("ENC\t")]
        if plug_lines != ref_lines:
            problems.append("编码器表不一致（参照 %d 行 / 插件 %d 行）"
                            % (len(ref_lines), len(plug_lines)))
        else:
            say("  encoders：%d 行逐行一致" % len(ref_lines))

    if problems:
        fail("视频页（插件）与引擎参照不一致：%s" % "；".join(problems[:4]))
        return
    ok("视频页（插件）三块判据全部**逐行一致**（plan %d 组语料 / probe %d 份 / encoders 1 份）"
       % (len(cases), len(probe_files)))



def step_fs_plugin(release: bool) -> None:
    """插件改装 P15-1b：**第六页（文件系统）的收口判据**。

    判据：
      ① **解包对拍**：参照 = `xrkit_cli --unpack-package <包> <outA> <格式>`（内置 C++ 页走的
         就是这条引擎路）；插件 = `--py-plugin-call plugins --plugin fs_page.py`
         （headless `{"action":"unpack"}`）⇒ **文件表 + 每文件 sha256 全等**且比较数 ≥1；
      ② **封包往返**（跨实现）：插件 headless `{"action":"pack"}` 封 SquashFS ⇒
         **CLI 解回来** ⇒ 与源目录逐字节一致；
      ③ **条目/预览**：插件 `list` 无错误且条数 ≥1；`preview` 条数 ≥ `list` 条数
         （自带壳的包，预览要补出壳那一层）。
    """
    say("[fs-plugin] 插件改装 P15-1b：文件系统页（插件）与 CLI 参照逐字节一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "fs_page.py"
    if not plugin.is_file():
        prereq("fs 插件 plugins/fs_page.py", "缺失", release)
        return
    golden = ROOT / "tests" / "golden" / "db" / "synth_xdb.db"
    if not golden.is_file():
        prereq("金样本包 tests/golden/db/synth_xdb.db", "缺失", release)
        return

    work = ROOT / "build" / "fs_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    out_ref = work / "ref"
    out_plug = work / "plug"
    out_ref.mkdir(parents=True)
    out_plug.mkdir(parents=True)

    # ── ① 解包对拍 ─────────────────────────────────────────────────────────
    rc_ref, out_ref_log = run([str(CLI_EXE), "--unpack-package", str(golden), str(out_ref), "xdb"],
                              timeout=900)
    payload = json.dumps({"action": "unpack", "packages": [str(golden)],
                          "out_root": str(out_plug), "format_key": "xdb"},
                         ensure_ascii=False)
    rc_plug, out_plug_log = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "fs_page.py", payload], timeout=900)

    def digest(root: Path) -> dict:
        out = {}
        for path in sorted(root.rglob("*")):
            if path.is_file():
                out[str(path.relative_to(root)).replace("\\", "/")] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
        return out

    ref_tree = digest(out_ref)
    plug_tree = digest(out_plug)
    compared = sum(1 for key in ref_tree if key in plug_tree and ref_tree[key] == plug_tree[key])
    only_ref = sorted(set(ref_tree) - set(plug_tree))
    only_plug = sorted(set(plug_tree) - set(ref_tree))
    differing = sorted(k for k in set(ref_tree) & set(plug_tree) if ref_tree[k] != plug_tree[k])
    say("  解包对拍：参照 %d 个文件 / 插件 %d 个文件 ⇒ 逐字节一致 %d 个"
        % (len(ref_tree), len(plug_tree), compared))
    problems = []
    if rc_ref != 0 or rc_plug != 0:
        problems.append("命令没跑成（参照 rc=%d / 插件 rc=%d）" % (rc_ref, rc_plug))
    if compared < 1:
        problems.append("一个文件都没比上（空集不算通过）")
    if only_ref:
        problems.append("参照多出的文件：%s" % ", ".join(only_ref[:3]))
    if only_plug:
        problems.append("插件多出的文件：%s" % ", ".join(only_plug[:3]))
    for key in differing[:3]:
        problems.append("内容不同：%s" % key)

    # ── ② 封包往返（插件封 ⇒ CLI 解）────────────────────────────────────────
    src = work / "src"
    (src / "sub").mkdir(parents=True)
    (src / "a.txt").write_bytes(b"hello")
    (src / "sub" / "b.bin").write_bytes(bytes(range(256)) * 4)
    items = [{"path": "a.txt", "is_dir": False, "source_path": str(src / "a.txt")},
             {"path": "sub/b.bin", "is_dir": False, "source_path": str(src / "sub" / "b.bin")}]
    packed = work / "packed.sqfs"
    pack_payload = json.dumps({"action": "pack", "out_path": str(packed), "format_key": "sqfs",
                               "items": items}, ensure_ascii=False)
    rc_pack, pack_log = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                             "--plugin", "fs_page.py", pack_payload], timeout=900)
    pack_ok = False
    for raw in pack_log.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
            try:
                pack_ok = bool(json.loads(s.split(" result=", 1)[1]).get("ok"))
            except ValueError:
                pack_ok = False
    out_back = work / "back"
    out_back.mkdir(parents=True, exist_ok=True)
    rc_back, back_log = run([str(CLI_EXE), "--unpack-package", str(packed), str(out_back)],
                            timeout=900)
    # ★ 落点带**包名壳**（不自带壳的包 ⇒ `unpack_target_dir` 会加一层 `<包名>/`）
    shell = Path(packed).stem
    back_tree = {}
    for key, value in digest(out_back).items():
        back_tree[key[len(shell) + 1:] if key.startswith(shell + "/") else key] = value
    want = {"a.txt": hashlib.sha256((src / "a.txt").read_bytes()).hexdigest(),
            "sub/b.bin": hashlib.sha256((src / "sub" / "b.bin").read_bytes()).hexdigest()}
    back_hits = sum(1 for key, value in want.items() if back_tree.get(key) == value)
    say("  封包往返：插件封包 ok=%s / CLI 解回逐字节一致 %d/%d" % (pack_ok, back_hits, len(want)))
    if not pack_ok or rc_pack != 0:
        problems.append("插件封包没成功（rc=%d）" % rc_pack)
    if rc_back != 0:
        problems.append("CLI 解回失败（rc=%d）" % rc_back)
    if back_hits != len(want):
        problems.append("封包往返内容不一致（一致 %d/%d）" % (back_hits, len(want)))

    # ── ③ 条目 / 预览 ───────────────────────────────────────────────────────
    counts = {}
    for action in ("list", "preview"):
        call = json.dumps({"action": action, "path": str(golden), "format_key": "xdb"},
                          ensure_ascii=False)
        rc, log = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                       "--plugin", "fs_page.py", call], timeout=600)
        counts[action] = None
        for raw in log.splitlines():
            s = raw.strip()
            if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
                try:
                    payload_out = json.loads(s.split(" result=", 1)[1])
                    counts[action] = int(payload_out.get("count", -1))
                    if action == "list" and payload_out.get("error"):
                        problems.append("list 报错：%s" % payload_out["error"])
                except (ValueError, TypeError):
                    pass
    say("  条目 list=%s / preview=%s" % (counts["list"], counts["preview"]))
    if not counts["list"] or counts["list"] < 1:
        problems.append("包内条目一个都没列出来（空集不算通过）")
    if counts["preview"] is None or counts["preview"] < (counts["list"] or 0):
        problems.append("预览条数少于条目数（%s < %s）—— 自带壳的包，预览要补出壳那一层"
                        % (counts["preview"], counts["list"]))

    if problems:
        fail("fs 插件判据不成立（%d 处）：%s" % (len(problems), "；".join(problems)))
    else:
        ok("文件系统页（插件）：解包逐字节一致（%d 个文件）· 封包往返 %d/%d · 条目/预览 %s/%s"
           % (compared, back_hits, len(want), counts["list"], counts["preview"]))


def step_xml_plugin(release: bool) -> None:
    """插件改装 P11b-2：**第四页（XML 对比）的收口判据**。

    判据：
      ① 参照 = `xrkit_cli --xml-compare-dirs`（内置 C++ 页用的就是这条引擎路）；
         插件 = `--py-plugin-call plugins --plugin xml_page.py`（headless 按同一行格式打）；
      ② 两边 `XMLDIRS …` 行**逐行完全一致**，且 ≥6 行（空集不算通过）；
      ③ **取消**：插件 headless 带 `"cancel": true` ⇒ `cancelled=1` 且 `rows=0`
         （取消在**第一个文件之前**生效）。
    """
    say("[xml-plugin] 插件改装 P11b-2：XML 对比页（插件）与引擎参照逐行一致")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "缺失（先跑 build 步）", release)
        return
    plugin = ROOT / "plugins" / "xml_page.py"
    if not plugin.is_file():
        prereq("XML 插件 plugins/xml_page.py", "缺失", release)
        return
    work = ROOT / "build" / "xml_plugin"
    if work.exists():
        shutil.rmtree(work, ignore_errors=True)
    dir_a = work / "a"
    dir_b = work / "b"
    (dir_a / "sub").mkdir(parents=True)
    (dir_b / "sub").mkdir(parents=True)
    same = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
            '\t<string id="a"><text>same</text></string>\n</string_table>\n')
    diff_a = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="b"><text>two</text></string>\n</string_table>\n')
    diff_b = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
              '\t<string id="a"><text>one</text></string>\n'
              '\t<string id="a"><text>one again</text></string>\n'
              '\t<string id="b"><text>two changed</text></string>\n</string_table>\n')
    for root in (dir_a, dir_b):
        (root / "same.xml").write_text(same, encoding="utf-8")
        (root / "sub" / "nested.xml").write_text(same, encoding="utf-8")
    (dir_a / "diff.xml").write_text(diff_a, encoding="utf-8")
    (dir_b / "diff.xml").write_text(diff_b, encoding="utf-8")
    (dir_a / "only_a.xml").write_text(same, encoding="utf-8")
    (dir_b / "only_b.xml").write_text(same, encoding="utf-8")

    rc_ref, out_ref = run([str(CLI_EXE), "--xml-compare-dirs", str(dir_a), str(dir_b)],
                          timeout=600)
    ref_lines = [line.strip() for line in out_ref.splitlines() if line.strip().startswith("XMLDIRS")]
    payload = json.dumps({"dir_a": str(dir_a), "dir_b": str(dir_b), "mode_key": "stats"},
                         ensure_ascii=False)
    rc_plug, out_plug = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                             "--plugin", "xml_page.py", payload], timeout=600)
    plug_lines = []
    for raw in out_plug.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
            try:
                plug_lines = json.loads(s.split(" result=", 1)[1]).get("lines", [])
            except ValueError:
                plug_lines = []
        elif s.startswith("PYPLUGINCALL") and not s.startswith("PYPLUGINCALL result="):
            say("  " + s[:170])
    say("  参照 %d 行 / 插件 %d 行" % (len(ref_lines), len(plug_lines)))
    problems = []
    if len(ref_lines) < 6 or len(plug_lines) < 6:
        problems.append("行数不足（参照 %d / 插件 %d）—— 空集不算通过"
                        % (len(ref_lines), len(plug_lines)))
    elif ref_lines != plug_lines:
        for index, (left, right) in enumerate(zip(ref_lines, plug_lines)):
            if left != right:
                problems.append("第 %d 行不同：参照「%s」/ 插件「%s」" % (index, left, right))
                break
        if not problems:
            problems.append("行数相同但内容不同（请人工看）")
    if rc_ref != 0 or rc_plug != 0:
        problems.append("命令没跑成（参照 rc=%d / 插件 rc=%d）" % (rc_ref, rc_plug))

    # ③ 取消：插件 headless 侧（确定性谓词由插件自己造）
    cancel_payload = json.dumps({"dir_a": str(dir_a), "dir_b": str(dir_b), "mode_key": "stats",
                                 "cancel": True}, ensure_ascii=False)
    rc_cancel, out_cancel = run([str(CLI_EXE), "--py-plugin-call", str(ROOT / "plugins"),
                                 "--plugin", "xml_page.py", cancel_payload], timeout=600)
    cancel_lines = []
    for raw in out_cancel.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINCALL plugin=") and " result=" in s:
            try:
                cancel_lines = json.loads(s.split(" result=", 1)[1]).get("lines", [])
            except ValueError:
                cancel_lines = []
    tail = cancel_lines[-1] if cancel_lines else ""
    say("  取消：rc=%d 收尾行=%s" % (rc_cancel, tail or "(缺)"))
    if "cancelled=1" not in tail or "rows=0" not in tail:
        problems.append("取消语义不对（收尾行「%s」应是 cancelled=1 且 rows=0）" % tail)
    if problems:
        fail("XML 对比页（插件）不成立：%s" % "；".join(problems))
        return
    ok("XML 对比页（插件）与引擎参照**逐行一致**（%d 行）；取消在第一个文件之前生效"
       % len(ref_lines))


def step_plugin_entry_lock(release: bool) -> None:
    """插件改装的**加载入口唯一性**（P2b）：两个前端都走 `load_all_plugins_from_dir`。

    为什么独立成步：这类"两边各写一条加载路径"的分叉**行为测不出来**（各自跑各自的用例都绿），
    只有"调用点在哪"能判；而且它不受其它步骤早退影响（做法同 px-only / python-host）。
    """
    say("[plugin-entry] 插件加载入口唯一性（Hub 与 CLI 走同一条路）")
    rc, out = run([sys.executable, str(ROOT / "tests" / "check_plugin_entry_lock.py")], timeout=120)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("PLUGINENTRY"):
            say("  " + s)
    if rc == 0:
        ok("两个前端共用同一个插件加载入口（DLL + .py 收在一个函数里）")
    else:
        fail("插件加载入口分叉了（见上；判据见 tests/check_plugin_entry_lock.py）")


def step_python_plugins(release: bool) -> None:
    """插件改装 P2b：**Python 插件加载链**（起宿主 → `import` → `register(api)` → 进 Registry
    → **真的消费一次**）。

    判据（每条都能红）：
      · 好夹具：`files=1 loaded=1 errors=0 registrations>=4` + `consume` 成功 + 标记文件里
        既有 `register:hello.py`（注册期跑过）又有 `click`（**回调真的回到了 Python**）；
      · 坏夹具（语法错 + 注册到一半抛异常）：**进程不崩**、两条都**点名**留痕、
        且每个坏件的 `residue … value=0`（按 owner 回滚干净）、退出码非 0；
      · 混装目录：好插件照常加载，坏的只影响它自己（不拖累）。
    """
    say("[python-plugins] 插件改装 P2b：Python 插件加载链（import → register(api) → 消费）")
    if not CLI_EXE.exists():
        prereq("构建产物 %s" % CLI_EXE.name, "缺失（先跑 build 步）", release)
        return
    rc_host, out_host = run([str(CLI_EXE), "--python-host"], timeout=180)
    host_line = next((ln.strip() for ln in out_host.splitlines()
                      if ln.strip().startswith("PYHOST")), "")
    if " enabled=0" in host_line:
        prereq("CPython 插件宿主（构建期已启用）",
               "本次构建没带 Python 支持：" + host_line, release)
        return

    good = ROOT / "tests" / "python_plugins"
    bad = ROOT / "tests" / "python_plugins_bad"
    for path in (good, bad):
        if not path.is_dir():
            fail("夹具目录不存在：%s（判据没跑成，不是通过）" % path.relative_to(ROOT))
            return

    def py_env(mark: Path) -> dict:
        env = dict(os.environ)
        env["XRKIT_PY_MARK"] = str(mark)
        return env

    def summary_line(text: str) -> str:
        return next((ln.strip() for ln in text.splitlines()
                     if ln.strip().startswith("PYPLUGINS files=")), "")

    # ① 好夹具：加载 + 消费 + 标记文件
    mark_dir = Path(tempfile.mkdtemp(prefix="xrkit_py_mark_"))
    mark = mark_dir / "mark.txt"
    rc, out = run([str(CLI_EXE), "--py-plugins", str(good)], env=py_env(mark), timeout=300)
    line = summary_line(out)
    files_ok = " files=1 " in line
    loaded_ok = " loaded=1 " in line
    errors_ok = " errors=0" in line
    regs = 0
    match = re.search(r"registrations=(\d+)", line)
    if match:
        regs = int(match.group(1))
    consumed = "PYPLUGINS consume plugin=hello.py" in out
    residue_ok = bool(re.search(r"PYPLUGINS residue plugin=hello\.py value=([1-9]\d*)", out))
    marker_text = mark.read_text(encoding="utf-8") if mark.exists() else ""
    logged = "PYPLUGINS log [hello.py]" in out
    if rc == 0 and files_ok and loaded_ok and errors_ok and regs >= 4 and consumed and \
            residue_ok and "register:hello.py" in marker_text and "click" in marker_text and logged:
        ok("Python 插件加载链全通（files=1 loaded=1 registrations=%d；`click` 标记＝回调真回到 "
           "Python；日志通道通）" % regs)
    else:
        fail("Python 插件加载链不通过（rc=%d；%s；consume=%s residue=%s 标记=%r 日志=%s）"
             % (rc, line or "(无摘要行)", consumed, residue_ok, marker_text[:60], logged))
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINS") and not s.startswith("PYPLUGINS log"):
            say("  " + s)

    # ② 坏夹具：不崩 + 两条都点名 + 回滚干净 + 退出码非 0
    rc_bad, out_bad = run([str(CLI_EXE), "--py-plugins", str(bad)], env=py_env(mark), timeout=300)
    line_bad = summary_line(out_bad)
    names = {m.group(1) for m in re.finditer(r"PYPLUGINS error plugin=(\S+)", out_bad)}
    rollback_ok = all(
        int(m.group(2)) == 0
        for m in re.finditer(r"PYPLUGINS residue plugin=(\S+) value=(\d+)", out_bad))
    residue_lines = re.findall(r"PYPLUGINS residue plugin=(\S+) value=(\d+)", out_bad)
    if rc_bad != 0 and "broken_register.py" in names and "broken_syntax.py" in names and \
            residue_lines and rollback_ok:
        ok("坏插件：不崩、两条都点名（%s）、按 owner 回滚干净（%s）"
           % ("/".join(sorted(names)),
              ", ".join("%s=%s" % (n, v) for n, v in residue_lines)))
    else:
        fail("坏插件那一侧不通过（rc=%d；%s；点名=%s；回滚=%s）"
             % (rc_bad, line_bad or "(无摘要行)", sorted(names), residue_lines))
    for raw in out_bad.splitlines():
        s = raw.strip()
        if s.startswith("PYPLUGINS") and not s.startswith("PYPLUGINS log"):
            say("  " + s)

    # ③ 混装：好插件照常加载，坏的只影响它自己
    mixed = Path(tempfile.mkdtemp(prefix="xrkit_py_plugins_mixed_"))
    shutil.copyfile(good / "hello.py", mixed / "hello.py")
    shutil.copyfile(bad / "broken_register.py", mixed / "broken_register.py")
    rc_mix, out_mix = run([str(CLI_EXE), "--py-plugins", str(mixed)], env=py_env(mark), timeout=300)
    line_mix = summary_line(out_mix)
    if " files=2 " in line_mix and " loaded=1 " in line_mix and " errors=1" in line_mix:
        ok("混装目录：好插件照常加载、坏的只影响它自己（%s）" % line_mix)
    else:
        fail("混装目录那一侧不通过（rc=%d；%s）" % (rc_mix, line_mix or "(无摘要行)"))
    shutil.rmtree(mixed, ignore_errors=True)

    # ④ ★ P7b：**插件声明所需版本** —— 声明得比宿主新 ⇒ 当场拒绝 + 留痕 + **一条注册都不留**
    requires = ROOT / "tests" / "python_plugins_requires"
    if not requires.is_dir():
        fail("版本声明夹具目录不存在：%s（判据没跑成，不是通过）"
             % requires.relative_to(ROOT))
    else:
        rc_req, out_req = run([str(CLI_EXE), "--py-plugins", str(requires)], env=py_env(mark),
                              timeout=300)
        line_req = summary_line(out_req)
        refused = "PYPLUGINS error plugin=too_new.py" in out_req
        reason_ok = "要求插件 API" in out_req
        no_residue = bool(re.search(r"PYPLUGINS residue plugin=too_new\.py value=0", out_req))
        if rc_req != 0 and " files=1 " in line_req and " loaded=0 " in line_req and \
                " errors=1" in line_req and refused and reason_ok and no_residue:
            ok("插件声明所需版本：比宿主新的**当场拒绝**并留痕（%s；一条注册都没留）" % line_req)
        else:
            fail("「插件声明所需版本」那一侧不通过（rc=%d；%s；点名=%s 理由=%s 零残留=%s）"
                 % (rc_req, line_req or "(无摘要行)", refused, reason_ok, no_residue))
        for raw in out_req.splitlines():
            s = raw.strip()
            if s.startswith("PYPLUGINS") and not s.startswith("PYPLUGINS log"):
                say("  " + s)

    shutil.rmtree(mark_dir, ignore_errors=True)


def step_page_host_lock(release: bool) -> None:
    """插件改装 P1：**Hub 不许认识具体页**（静态面）。

    为什么独立成步：改装目标"Hub ＝ 容器 + 注册表 + 机制提供者，不含业务"很容易被一次
    "顺手加回去"的 `dynamic_cast<FsPage*>` 抹掉；判据必须是静态的、且**不受别的步骤早退影响**
    （做法同 px-only）。判据本体见 `tests/check_page_host_lock.py`（含喂反例必红的自证）。
    """
    say("[page-host] 插件改装 P1：Hub 只消费页注册表，不认识具体页")
    rc, out = run([sys.executable, str(ROOT / "tests" / "check_page_host_lock.py")], timeout=120)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("PAGEHOST"):
            say("  " + s)
    if rc == 0:
        ok("Hub 只消费页注册表（静态锁：无具体页类名 / 无具体页头 / 正向锁在位）")
    else:
        fail("Hub 又认识具体页了（见上；判据见 tests/check_page_host_lock.py）")


def step_no_console(release: bool) -> None:
    """无控制台窗口（用户 2026-09-27：「不要打开cmd」）：产品代码不许用 std::system(
    （走 cmd.exe /c，GUI 里会闪黑窗），起了 QProcess 就必须置 CREATE_NO_WINDOW。
    独立成步骤，不受其它步骤早退影响（做法同 px-only）。
    ★ 2026-10-06（0-8）：本步**排在 shell-smoke 之后**（见 STEPS 里的说明）——
      它的 ③ 号判据读的是**本次构建出来的** exe 的 PE 子系统。
    """
    say("[no-console] 子进程启动：不许弹控制台窗口")
    rc, out = run([sys.executable, str(ROOT / "tests" / "check_no_console.py")], timeout=120)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("NOCONSOLE"):
            say("  " + s)
    if rc == 0:
        ok("子进程启动全部无控制台窗口")
    else:
        fail("有子进程启动会弹控制台窗口（见上）")


def step_codec_strict(release: bool) -> None:
    """严格解码/编码的**边界域**（审计 2-1 / 2-2 / H1-3 / A1-1 / A1-2）。

    为什么独立成步：这两条此前**没有任何能判红的入口** —— `--decode-text` 走六编码链
    （latin-1 兜底、永不失败）、`--codec-lossy` 走 replace 语义 ⇒ 严格路径的越界读表
    与有损分支死循环谁也判不了。本步用 `--codec-lossy ... strict` 打这两条边界。
    退出码：0=全绿、1=有判据不成立、2=构建产物缺失（外部前提）。
    """
    say("[codec-strict] 严格编解码的边界（0xFF 前导不许越界读表 / utf-32 有损不许挂死）")
    script = ROOT / "tests" / "check_codec_strict_bounds.py"
    if not require_repo_file(script, "严格编解码边界判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("CODEC-STRICT"):
            say("  " + s)
    if rc == 0:
        ok("严格路径边界全部守住（越界读表 / 有损死循环）")
    else:
        fail("严格路径边界有判据不成立（见上）")


def step_unpack_only(release: bool) -> None:
    """`only`（解包选中）语义只许一份 + 命中不了必须可见（审计 1-4 / S1-7 / S1-7 的 CLI 面）。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按假绿批的记账进 UNCOVERED。
    """
    say("[unpack-only] 解包选中的路径口径（落点路径 / 镜像内路径）+ 零静默")
    script = ROOT / "tests" / "check_unpack_only_semantics.py"
    if not require_repo_file(script, "解包选中口径判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("UNPACK-ONLY"):
            say("  " + s)
    if rc == 0:
        ok("勾选项 ↔ 包里条目按同一条规则匹配（两种写法都认）且对不上时可见")
    else:
        fail("解包选中口径有判据不成立（见上）")


def step_engine_contract(release: bool) -> None:
    """审计第 4 组余项三条（★先写锁、再修代码）+ 3-3/3-4 的静态面。

    覆盖：
      · `contract_ok()` 不校 `area` 条数 ⇒ 删掉 `AREA_BOTTOM` 也全绿（本步**注入必红**）；
      · `engine::status_line()` 的**常量自报** ⇒ `--module-subset` 收窄模块集，那一串必须跟着变；
      · `LoadResult::registrations` **死字段** ⇒ 成功路径必须报出真条数。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[engine-contract] 第 4 组余项三条（area 条数 / status_line / registrations）")
    script = ROOT / "tests" / "check_engine_contract.py"
    if not require_repo_file(script, "引擎契约判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=1800)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("[") \
                or s.startswith("ENGINE-CONTRACT"):
            say("  " + s)
    if rc == 0:
        ok("area 条数/status_line/registrations 三条都判得红且已修绿")
    elif rc == 2:
        prereq("引擎契约判据的外部前提", "缺构建产物/工具链", release)
    else:
        fail("引擎契约判据有不成立的断言（见上）")


def step_page_status_label(release: bool) -> None:
    """审计补充 F3 第 1 条：**页面级消息标签是"孤儿顶层控件"**（效应等同用户可见缺陷）。

    事实（修前）：`xml_page.cpp:166` / `text_page.cpp:111` / `convert_page.cpp:241` 都写
    `status_ = new QLabel(task_->status())` —— `TaskShell::status()` 返回的是 **QString**
    （控件访问器叫 `status_label()`）⇒ 造出一个**无父**标签（顶层窗口、从不进布局）
    ⇒ 全页消息（空目录 / 导出成功失败 / 复制 id / 还没选源）用户完全看不见，且无人拥有。
    行为面（真的画出来、真的可见）在 `src/shell/smoke.cpp` 里，随 shell 自检跑。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[page-status] 页面级消息标签必须有父、进布局（F3 孤儿顶层控件）")
    script = ROOT / "tests" / "check_page_status_labels.py"
    if not require_repo_file(script, "页面级消息标签判据"):
        return
    rc, out = run([sys.executable, str(script)], timeout=120)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("PAGE-STATUS"):
            say("  " + s)
    if rc == 0:
        ok("三页 + 视频页的页面级消息标签都有父、进布局")
    else:
        fail("有页面级消息标签是孤儿顶层控件（见上）")


def step_orphan_inc(release: bool) -> None:
    """审计补充 H1-1（S1）的**判据原文**：遍历 `src/**/*.inc`，要求每个都能被某生成器声明产出。

    事实（修前）：`src/engine/fs/cp1251_tables.inc` 是**手写孤儿**（生成器只写 text/、
    门禁只查 text/），而 `src/engine/fs/db.cpp` 用的是**引号 include** ⇒ 先命中同目录的
    fs 版；两份同名副本内容还不一样（0x98：fs 版 `0xFFFD` = replace 语义；text 版
    `0xFFFF` = 严格解码必须失败）。删掉/改名 fs 版 ⇒ 静默改用另一份且编译不报错。
    现在两份都由 `tools/gen_encoding_tables.py` 产出、`--check` 逐字节校验两份。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[orphan-inc] src/**/*.inc 不许有「没人声明产出」的孤儿（H1-1）")
    script = ROOT / "tests" / "check_orphan_inc.py"
    if not require_repo_file(script, "生成物孤儿判据"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("ORPHAN-INC"):
            say("  " + s)
    if rc == 0:
        ok("每份 .inc 都被生成器声明产出 + 两份 cp1251 只差 0x98 一处")
    else:
        fail("有 .inc 是孤儿 / 两份 cp1251 超出有意差异（见上）")


def step_font_read_failure(release: bool) -> None:
    """审计补充 A1-4（数据丢失/零静默）：读文件失败被当成"空文件"。

    事实（修前）：`package.cpp` / `charset.cpp` 各有一份"打不开就 `return {}`"的读文件助手，
    而 `decode_xml_bytes({})` **必然成功** ⇒ 出包侧写出空 XML 并记成功、字符集侧整份语料的
    字符被静默丢弃（`files_scanned++` 而 `files_undecodable` 不变）。
    判据＝开不了的文件必须**明确报错并计入失败**（夹具用 `%TEMP%` + 独占锁）。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[font-read-fail] 读文件失败不许被当成空文件（A1-4）")
    script = ROOT / "tests" / "check_font_read_failure.py"
    if not require_repo_file(script, "读文件失败判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("FONT-READ-FAIL"):
            say("  " + s)
    if rc == 0:
        ok("打不开的 XML 会被明确报错并计入失败（不再当空文件）")
    elif rc == 2:
        prereq("读文件失败判据的外部前提", "缺构建产物", release)
    else:
        fail("读文件失败仍被静默当成空文件（见上）")


def step_video_overwrite(release: bool) -> None:
    """审计补充 F1-1/F1-2/F1-3（S1）：`-y` 恒在 argv 里 ⇒ 覆盖是静默的。

    事实（修前）：`video_page.cpp` 起跑前**零存在性检查**，唯一的"防自毁"只作用于默认名、
    只跟参考比 ⇒ 源本身是 `.ogm` 时默认输出＝源文件，叠上 `-y` 就是**截断输入**；
    另外取消超时支把 `process_` 置空却不 `disconnect()`（同形三处）。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[video-overwrite] 视频输出目标守卫（-y 不许静默覆盖 / 输出不许等于输入）")
    script = ROOT / "tests" / "check_video_overwrite_guard.py"
    if not require_repo_file(script, "视频输出目标守卫判据"):
        return
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("VIDEO-OVERWRITE"):
            say("  " + s)
    if rc == 0:
        ok("输出=源/参考必须拒、已存在必须先问；进程对象置空前都先 disconnect")
    elif rc == 2:
        prereq("视频输出目标守卫的外部前提", "缺构建产物", release)
    else:
        fail("视频输出目标守卫有判据不成立（见上）")


def step_package_platform(release: bool) -> None:
    """用户 2026-10-07 裁决：部署舞台必须**同时**带 `qwindows.dll` 与 `qoffscreen.dll`。

    事实：`windeployqt` 按 app 的实际调用挑，只铺 qwindows；而 `--smoke` / `--layout-report`
    走离屏。修前 `package.ps1` 的处置是"跑判据前临时拷、跑完删掉（保持包精简）" ⇒
    **产物本身不可自检**：包内 exe 跑 `--smoke` 直接 `0xC0000409`（BEX64 / Qt6Core / P7=0x267a8），
    实测 ALPHA.57/58/59 同一签名。裁决改为随包携带（109 KB）。
    另一条同族假判据：结论行原只 grep `SHELL-SMOKE ok`，而收尾行**恒**以 ok 开头
    （`SHELL-SMOKE ok checks=N failed=1` 也是 "ok"）⇒ 现在要求 `failed=0`。

    判据分两层：
      · 静态（任何环境都能判）：`package.ps1` 必须拷 qoffscreen、不许再删它、必须断言产物里
        两份都在、结论行必须要求 `failed=0`、必须把契约声明 `platform_plugins=` 写进 version.txt；
      · 产物（有包时）：最新序号里 platforms\\ 两份都在 + **从产物目录真跑一次** `--smoke`
        （子进程 `QT_QPA_PLATFORM=offscreen`）⇒ 退出码 0 且 `failed=0`。
        序号没有契约声明（修前打的 ALPHA.57/58/59）⇒ 记**台账**（可见、release 不阻断）；
        声明了就必须成立。这样"门禁早于打包"这个顺序不会因为旧序号而假红。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED。
    """
    say("[pkg-platform] 包内平台插件（qwindows + qoffscreen）+ 从产物里真跑自检")
    script = ROOT / "tests" / "check_package_platform.py"
    if not require_repo_file(script, "包内平台插件判据"):
        return
    rc, out = run([sys.executable, str(script)], timeout=900)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("PKG-PLATFORM") \
                or s.startswith("原始行") or s.startswith("产物"):
            say("  " + s)
    if rc == 0:
        ok("包内同时带 qwindows + qoffscreen，且已从产物里真跑一次自检（failed=0）")
    elif rc == 2:
        # ★ 2026-10-10（P17-2，审计 04 F-05）：release 档按前提缺失（FAIL），其余档记台账。
        package_fact("包内平台插件/从产物里真跑自检",
                     "本次没有带契约声明的新序号包（同一判据写在 package.ps1 里，每次打包都会真跑）",
                     release)
    else:
        fail("包内平台插件判据不成立（见上）")


def step_version_identity(release: bool) -> None:
    """审计补充 H1-2（S1，用户 2026-10-07 拍板"候选 A"）：版本号只许**一份**真来源。

    事实（修前）：`version.json` 与 `include/xrkit/core/version.h` 是两个"唯一事实来源"且
    **无任何比对** —— exe 的资源与 `core::version_string()` 编自 version.h 的常量
    （恒 `ALPHA.0`），包目录名/`version.txt` 来自 version.json；两条既有锁都在各自源内自比。
    后果（对外可见，本批实测原始行）：
      · `xrkit_cli --exe-version dist\\2.0.0\\ALPHA\\60\\xrkit_app.exe`
        ⇒ `EXEVERSION product=2.0.0 ALPHA.0 file=2.0.0 numeric=2.0.0.0`
      · 而同一个包 `version.txt` ⇒ `version=2.0.0 ALPHA.60`

    口径（用户拍板）：真来源＝`version.json` 的 tree（最后一次成功发包）；`version.h` 是
    派生（`kPrerelease="<标记>.<序号>"`、`kBuildCount=<序号>`），写回点在 `--build-package`
    写 version.json 之后、`build.ps1` 之前（原子写 + 失败回滚 ⇒ 序号不被吃掉）；
    判据只对 **包内 exe** + `version.h`/`.rc` 断言，**不**对 build 目录的 exe 断言
    （开发构建用缺省值合法）；期望值从 tree 现算、**不读 `current.index`**。

    独立成步（不塞进 selftest）：被 `--only` 过滤时按既有记账进 UNCOVERED；
    没有"带身份声明的新包"时 ③ 记**台账**（旧序号是修前打的）。
    """
    say("[version-identity] 版本号一份真来源（tree → version.h → .rc → 包内 exe）")
    script = ROOT / "tests" / "check_version_identity.py"
    if not require_repo_file(script, "版本身份判据"):
        return
    rc, out = run([sys.executable, str(script)], timeout=300)
    for line in out.splitlines():
        s = line.strip()
        if s.startswith("\u2713") or s.startswith("\u2717") or s.startswith("VERSION-IDENTITY") \
                or s.startswith("原始读数") or s.startswith("真来源"):
            say("  " + s)
    if rc == 0:
        ok("version.h / .rc / 包内 exe 的版本身份三者与 version.json 的 tree 一致")
    elif rc == 2:
        # ★ 2026-10-10（P17-2，审计 04 F-05）：同上 —— 包内 exe 版本身份那一条。
        package_fact("包内 exe 的版本身份",
                     "本次没有带 `exe_identity=` 声明的新序号包（同一判据写在打包链里："
                     "`--build-package` 写回 + package.ps1 逐字比包内 exe）", release)
    else:
        fail("版本身份有判据不成立（见上）")


STEPS = [
    ("build", step_build),
    ("no-qt", step_no_qt_lock),
    ("no-qt-injection", step_no_qt_injection),
    ("px-only", step_px_only),
    # ★★ 2026-10-07（插件改装 P1）：Hub 不许认识具体页（静态锁）。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED（不塞进 shell-smoke，免得被早退带走）。
    ("page-host", step_page_host_lock),
    # ★★ 2026-10-07（插件改装 P2）：CPython 插件宿主能不能起来（嵌入解释器 + vendored pybind11）。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED；构建期没带 Python ⇒ release FAIL（不是 SKIP）。
    ("python-host", step_python_host),
    # ★★ 2026-10-07（插件改装 P2b）：Python 插件加载链（import → register(api) → 消费 → 回滚）。
    ("python-plugins", step_python_plugins),
    # ★★ 2026-10-07（插件改装 P2b）：加载入口唯一性（静态锁；两边各写一条路的分叉测不出来）。
    ("plugin-entry", step_plugin_entry_lock),
    # ★★ 2026-10-07（插件改装 P2c）：随包自带 embeddable CPython（标准库 + 运行期）+
    #   包内 `--python-host` 判据（只在包自己声明 `python_runtime=` 时跑，口径同 pkg-platform）。
    ("python-embed", step_python_embed),
    # ★★ 2026-10-07（插件改装 P3）：控件门面能力表（表外无门）＋ 无 GUI 时的如实降级。
    #   GUI 那条路（真控件 + 回调回 Python）在 shell-smoke 里判（需要 Qt）。
    ("ui-facade", step_ui_facade),
    # ★★ 2026-10-07（插件改装 P7a）：**引擎调用门面**（方案 3）—— 走真夹具验四处。
    ("engine-facade", step_engine_facade),
    # ★★ 2026-10-07（插件改装 P7b）：**宿主服务门面**（偏好读写 / exe 目录 / 系统字体目录）。
    ("host-services", step_host_services),
    # ★★ 2026-10-08（插件改装 P7d）：**样板页收口** —— 插件产物与引擎参照逐字节一致。
    ("fontpack-plugin", step_fontpack_plugin),
    # ★★ 2026-10-08（插件改装 P9b）：**第二页收口** —— 编码转换插件页。
    ("convert-plugin", step_convert_plugin),
    # ★★ 2026-10-08（插件改装 P10b）：**第三页收口** —— 文本提取插件页。
    ("text-plugin", step_text_plugin),
    # ★★ 2026-10-08（插件改装 P11b-2）：**第四页收口** —— XML 对比插件页。
    ("xml-plugin", step_xml_plugin),
    ("fs-plugin", step_fs_plugin),
    # ★★ 2026-10-08（插件改装 P12c-3）：**第五页收口** —— 视频转换插件页。
    ("video-plugin", step_video_plugin),
    # ★★ 2026-10-08（P9b）：门面顺序核对（类型相同的相邻字段错了编译器不报）
    ("facade-order", step_facade_order),
    # ★★ P17-1（2026-10-10）：**线程与取消契约**的静态锁（worker 不许碰 UI / 取消要读得到）。
    #   放在 facade-* 旁边：它同样要读门面的"跨线程只读白名单"（唯一一份口径在 C++ 侧）。
    ("thread-contract", step_thread_contract),
    # ★★ 2026-10-08（P16-0）：门面登记表三点闭环（字段 / kMethodNames / has_impl 逐名同数）
    ("facade-registry", step_facade_registry),
    # ★★ 2026-10-08（P16-0）：插件侧不许丢弃「成没成」型门面返回值（静默链的第二半）
    ("plugin-facade-ret", step_plugin_facade_returns),
    # ★★ 2026-10-09（P16-0b）：官方页 replaces 契约 + 插件隔离 + 内置页引擎直连棘轮
    ("official-pages", step_official_pages),
    # ★★ 2026-10-09（P16-0b）：struct stk_ui ↔ kUiMethods[] 逐名同序
    ("abi-closure", step_abi_closure),
    # ★★ 2026-10-09（P16-0b）：判据引用的文档必须存在
    ("doc-cite", step_doc_citations),
    # ★★ 2026-10-09（P16-1b 判定半）：二进制补丁识别与 1.x 金标准逐条对拍
    ("patch-identify", step_patch_identify),
    # ★★ 2026-10-09（P16-1b 写盘半）：产物逐字节 == v4 参照 + 四条拒绝路径
    ("patch-apply", step_patch_apply),
    # ★★ 2026-10-09（P16-1c）：插件页（汉化产线）经 api.engine.* 驱动引擎（两面板）
    ("toolchain-plugin", step_toolchain_plugin),
    # ★★ 2026-10-09（P16-2a）：随包插件清单 + 包内自检口径（切打包口径的静态锁）
    ("package-plugin-list", step_package_plugin_list),
    ("selftest", step_selftest),
    ("cli", step_cli_contract),
    ("corpus", step_corpus),
    ("golden-sqfs", step_golden_sqfs),
    ("golden-extract", step_golden_extract),
    ("golden-db", step_golden_db),
    ("db-11xx-dirlock", step_db_11xx_dirlock),
    ("db-name1251-lock", step_db_name_cp1251_lock),
    ("unpack-rules", step_unpack_rules),
    ("fs-hardening", step_fs_hardening),
    # ★★ P17-2（2026-10-10）：SquashFS 读侧三处 DoS（01 F-01/F-02/F-03，三处都 High）。
    #   与 fs-hardening 相邻（同族：畸形包不许把进程带走），独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("sqfs-dos", step_sqfs_dos),
    ("sqfs-lreg", step_sqfs_lreg_layout),
    ("golden-sqfs-pack", step_golden_sqfs_pack),
    ("golden-sqfs-pack-real", step_golden_sqfs_pack_real),
    # ★★ A 批（2026-10-06）：判据**先验选择**这件事本身也要有锁（先验结论 / 等价判据真跑 /
    #   三态退码 / `判据：` 行在任何封包输出之前）。独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("sqfs-pack-criterion", step_sqfs_pack_criterion),
    ("text-tables", step_text_tables),
    ("font-tables", step_font_tables),
    # ★★ 2026-10-07（本批·H1-1）：`.inc` 孤儿检测 + 两份 cp1251 表的"只许一处有意差异"。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("orphan-inc", step_orphan_inc),
    ("golden-text-decode", step_golden_text_decode),
    ("golden-text-extract", step_golden_text_extract),
    ("ltx-replace", step_ltx_replace_lock),
    # ★ S1 批两条（2026-10-06）：文本提取守卫在真跑那条路上生效 / 插件 init 失败回滚注册。
    #   各自独立成步（不塞进 selftest）：被 `--only` 过滤时会按假绿批的记账进 UNCOVERED。
    ("extract-guard", step_extract_guard),
    ("plugin-init-rollback", step_plugin_init_rollback),
    # ★★ 2026-10-06（S2 批）：2-1/2-2 的边界域、1-4 的解包选中口径。
    #   各自独立成步（不塞进 selftest）：被 `--only` 过滤时会按假绿批的记账进 UNCOVERED。
    ("codec-strict", step_codec_strict),
    ("unpack-only", step_unpack_only),
    # ★★ 2026-10-07（本批·A1-4）：读文件失败不许当空文件（%TEMP% 夹具 + 独占锁）。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("font-read-fail", step_font_read_failure),
    ("golden-text-sniff", step_golden_text_sniff),
    ("golden-font-raster", step_golden_font_raster),
    ("golden-font-metrics", step_golden_font_metrics),
    ("golden-font-render", step_golden_font_render),
    ("golden-font-resample", step_golden_font_resample),
    ("golden-font-charset", step_golden_font_charset),
    ("golden-font-package", step_golden_font_package),
    ("golden-xml-stats", step_golden_xml_stats),
    ("golden-video-argv", step_golden_video_argv),
    # ★★ 2026-10-07（本批·F1-1/F1-2/F1-3）：视频输出目标守卫（-y 不许静默覆盖）。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("video-overwrite", step_video_overwrite),
    ("golden-sqfs-compressors", step_golden_sqfs_compressors),
    ("golden-sqfs-metacross", step_golden_sqfs_metacross),
    ("bench", step_bench),
    ("shell-smoke", step_shell_smoke),
    # ★★ 2026-10-06（审计处理批·0-8 后半）：这一步**必须排在 shell 构建之后**。
    #   修前它在第 5 步、外壳构建在第 36 步，而它的 ③ 号判据读的是**磁盘上现存的**
    #   `xrkit_app.exe` / `xrkit_build_gui.exe` 的 PE 子系统 ⇒
    #     ① 干净 `build/`（从未构建过）上首次运行必然假红（"一个产品 exe 都没找到"）；
    #     ② 重复运行判的是**上一轮**留下的旧 exe —— 这一轮把 Console 子系统改回去也照样绿。
    #   现在挪到 `shell-smoke` 之后：这一轮的 exe 刚编出来，判的是**本次**产物。
    ("no-console", step_no_console),
    # ★★ 2026-10-06（本批）：审计第 4 组余项三条（area 条数 / status_line 常量自报 /
    #   registrations 死字段）。排在 shell-smoke 之后：那一步才把 CLI 与外壳都编出来。
    ("engine-contract", step_engine_contract),
    # ★★ 2026-10-07（本批·F3）：页面级消息标签不许是"孤儿顶层控件"（静态面；行为面在 smoke）。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("page-status", step_page_status_label),
    # 排在 shell-smoke **之后**：那一步才把 xrkit_app.exe 编出来（这一步要读它的真实资源字节）
    ("winres", step_winres),
    # ★★ 2026-10-07（用户裁决）：包内平台插件（qwindows + qoffscreen）+ 从产物里真跑自检。
    #   独立成步：被 `--only` 过滤时进 UNCOVERED；没有带契约声明的新包时记台账。
    ("pkg-platform", step_package_platform),
    # ★★ 2026-10-07（H1-2）：版本号一份真来源（tree → version.h → .rc → 包内 exe）。
    ("version-identity", step_version_identity),
]


def main() -> int:
    global FAILURES

    parser = argparse.ArgumentParser(description="2.x 门禁")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--fast", action="store_true", help="公开 CI：跳过外部前提步骤并记账")
    group.add_argument("--release", action="store_true", help="发版：任何 SKIP / 未覆盖即 FAIL")
    parser.add_argument("--only", default="", help="只跑名字含该子串的步骤")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    release = args.release
    mode = "release" if release else ("fast" if args.fast else "local")
    say("=" * 66)
    say("STALKER 工具集 2.x 门禁   档位=%s   仓库=%s" % (mode, ROOT))
    say("=" * 66)

    # ★★ 2026-10-06（审计处理批·0-1/0-2）：`--only` 是**局部调试**通道，与 release 档
    #   "本次覆盖：全部步骤均已执行"这个结论互斥。此前这里直接 `continue`，跳过的步骤
    #   **既不记账也不进未覆盖清单** ⇒ `--release --only winres` 只要那一步绿就打印
    #   `RESULT: PASS` 并谎称"全部步骤均已执行"（任何人都能一键把门禁变绿）。
    #   现在：`--only` 过滤掉的步骤**逐条记账**（记成"未执行"而不是"未覆盖所需外部前提"，
    #   两者性质不同），于是收尾那条 `release and UNCOVERED → FAIL` 有了活的写入者。
    #   注意 `--fast` 档不判红：`--fast --only` 仍可局部调试，但结论行会如实说"本次未覆盖"。
    ran: set[str] = set()
    # ★★ 2026-10-10（P17-53，来自 P17-51/52 的实测发现）：**"真机样本/打包产物"这一族会飘**。
    #   证据：`golden-sqfs-pack-real` 在 P17-51 那轮全量 --fast 判红（`FAIL 真机样本封包产物不一致`），
    #   同一族代码在 P17-52 那轮全量 --fast **PASS**；此前 `pkg-platform` 也有过"首跑红、连跑三次绿"。
    #   机制推断：这类步骤要读真机样本目录 / 跑包内产物，对**机器负载与样本目录状态**敏感，
    #   ⇒ **单次红不等于回归**。处置（本批落地）：这一族**首次判红就自动复跑一次**，
    #   复跑绿 ⇒ 按「飘」登记（撤销首次那一次失败计数 + 打一行可见提示），
    #   复跑仍红 ⇒ 照旧判红。★ 只对这**两个**步骤生效，且**必须留下可见痕迹**（零静默）。
    FLAKY_RETRY = {"golden-sqfs-pack-real", "pkg-platform"}
    flaky_seen: list[str] = []
    for name, fn in STEPS:
        if args.only and args.only not in name:
            continue
        before = FAILURES
        fn(release)
        if FAILURES > before and name in FLAKY_RETRY:
            say("  ↻ %s 首次判红 —— 按「飘」口径**复跑一次**再定性（P17-51/52 批报告）" % name)
            retry_before = FAILURES
            fn(release)
            if FAILURES == retry_before:
                flaky_seen.append(name)
                FAILURES = before  # 复跑绿：撤销首次那一次失败计数
                say("  ≈ %s：首次红、复跑绿 ⇒ 登记为「飘」（不改代码、不判红）" % name)
            else:
                say("  ✗ %s：复跑仍红 ⇒ 按真失败处置" % name)
        ran.add(name)
        say("")

    # 覆盖度只认一件事：**整套步骤里哪些真的执行了**。不按 `--only` 的过滤结果推断，
    # 也不因为"没红"就假定全覆盖（修前正是这么谎报的）。
    missing = [name for name, _ in STEPS if name not in ran]
    if missing:
        if len(missing) == len(STEPS):
            UNCOVERED.append("全部 %d 个步骤均未执行（`--only %r` 一个都没匹配上）"
                             % (len(STEPS), args.only))
        else:
            UNCOVERED.append("步骤 %s（共 %d/%d 个未执行%s）"
                             % ("、".join(missing), len(missing), len(STEPS),
                                "，本次由 `--only %r` 过滤掉" % args.only if args.only else ""))

    say("-" * 66)
    if UNCOVERED:
        say("本次未覆盖（%d 项）：" % len(UNCOVERED))
        for item in UNCOVERED:
            say("  · " + item)
    else:
        say("本次覆盖：全部步骤均已执行")
    if LEDGERS:
        # ★ T-H1：差异台账**单独一段** —— 它是"已知且被冻结的能力缺口"，不是未覆盖
        #   （所以 release 档不因它 FAIL），但**必须看得见**（静默就是原来那条假绿）。
        say("本次差异台账（已知分歧/能力缺口，%d 项；release 不阻断，但脚本侧第三态退码非零）："
            % len(LEDGERS))
        for item in LEDGERS:
            say("  · " + item)
    say("-" * 66)

    if release and UNCOVERED:
        say("release 档：未覆盖清单非空 → 整体 FAIL")
        FAILURES += 1

    if FAILURES:
        say("RESULT: FAIL（%d 处失败）" % FAILURES)
        return 1
    say("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
