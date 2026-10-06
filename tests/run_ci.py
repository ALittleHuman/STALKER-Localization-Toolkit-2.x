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

    `--bench` 的**能力**在测试侧（[28] bench.py，口径见 docs/2.0.0-bench口径.md）；
    CLI 里**不再保留**那个只会 SKIP 的占位动作（它会把 release 档的"未覆盖"清单顶住 ⇒ 发版永远过不了）。
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
    if "apply_load_order()" not in fs_code or fs_code.count("apply_load_order()") < 3:
        fail("加载序（3-4）：fs_page.cpp 的 `apply_load_order()` 必须被**每条加源路径**调用"
             "（`push_source` / `scan_dir` 都要；实测出现 %d 次）"
             % fs_code.count("apply_load_order()"))
        return
    if "sort_sources_by_basename" not in fs_code:
        fail("加载序（3-4）：排序必须交给引擎 `sort_sources_by_basename`（页面不许自己排）")
        return
    ok("加载序只此一份且每条加源路径都走（3-4：`apply_load_order`）")

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
    total_iter = sum(n for _, n in hits)
    shell_iter = [p for p, _ in hits if p.startswith("src/shell/")]
    if total_iter != 4 or shell_iter:
        fail("递归遍历件（D2：同类东西只有一套）：src/ 下 `recursive_directory_iterator` 必须恰好 4 份"
             "（引擎里既有的那些）、src/shell 下 0 份；实测 %d 份：%s"
             % (total_iter, "、".join("%s×%d" % (p, n) for p, n in hits)))
        return
    ok("递归遍历件只有 4 份、外壳侧 0 份（D2①：没有第 5 份遍历）")

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
    font_code = active_code((ROOT / "src" / "shell" / "pages" / "fontpack_page.cpp")
                            .read_text(encoding="utf-8", errors="replace"))
    font_need = ["SystemRoot", "qEnvironmentVariable", "font_candidates(", "default_font_path(",
                 "make_font_picker(", "save_user_value(", "QFontDatabase::addApplicationFont"]
    font_missing = [token for token in font_need if token not in font_code]
    if font_missing:
        fail("系统字体选择器（D2⑤）：fontpack_page.cpp 里缺 %s —— 字体要从系统字体目录找、"
             "并把上次的选择写进 user.ltx" % "、".join(font_missing))
        return
    ok("系统字体选择器（D2⑤：%SystemRoot%\\Fonts + 候选过滤 + user.ltx 记住上次选择）")


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


STEPS = [
    ("build", step_build),
    ("no-qt", step_no_qt_lock),
    ("no-qt-injection", step_no_qt_injection),
    ("px-only", step_px_only),
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
    ("sqfs-lreg", step_sqfs_lreg_layout),
    ("golden-sqfs-pack", step_golden_sqfs_pack),
    ("golden-sqfs-pack-real", step_golden_sqfs_pack_real),
    # ★★ A 批（2026-10-06）：判据**先验选择**这件事本身也要有锁（先验结论 / 等价判据真跑 /
    #   三态退码 / `判据：` 行在任何封包输出之前）。独立成步：被 `--only` 过滤时进 UNCOVERED。
    ("sqfs-pack-criterion", step_sqfs_pack_criterion),
    ("text-tables", step_text_tables),
    ("font-tables", step_font_tables),
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
    ("golden-text-sniff", step_golden_text_sniff),
    ("golden-font-raster", step_golden_font_raster),
    ("golden-font-metrics", step_golden_font_metrics),
    ("golden-font-render", step_golden_font_render),
    ("golden-font-resample", step_golden_font_resample),
    ("golden-font-charset", step_golden_font_charset),
    ("golden-font-package", step_golden_font_package),
    ("golden-xml-stats", step_golden_xml_stats),
    ("golden-video-argv", step_golden_video_argv),
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
    # 排在 shell-smoke **之后**：那一步才把 xrkit_app.exe 编出来（这一步要读它的真实资源字节）
    ("winres", step_winres),
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
    for name, fn in STEPS:
        if args.only and args.only not in name:
            continue
        fn(release)
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
