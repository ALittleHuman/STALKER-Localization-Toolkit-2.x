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
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build" / "msvc-release"
CLI_EXE = BUILD_DIR / "tools" / "cli" / "xrkit_cli.exe"
ENV_CACHE = ROOT / "build" / "msvc_env.txt"

VS_ROOT = Path(os.environ.get("XRKIT_VS", r"E:\Software\Tools\Visual Studio\2022\BuildTools"))
QT_ROOT = Path(os.environ.get("XRKIT_QT", r"E:\Software\Tools\Qt\6.8.3\msvc2022_64"))
VCVARS = VS_ROOT / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
CMAKE = VS_ROOT / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
NINJA_DIR = VS_ROOT / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja"

UNCOVERED: list[str] = []
FAILURES = 0
_MSVC_ENV: dict[str, str] | None = None


# ── 基础设施 ────────────────────────────────────────────────────────────
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
            env[key] = value
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


def require_repo_file(path: Path, what: str) -> bool:
    """仓库自带的门禁文件缺件 = FAIL，绝不 SKIP。"""
    if path.exists():
        return True
    fail("仓库自带文件缺失（不许静默通过）：%s（%s）" % (path.relative_to(ROOT), what))
    return False


# ── 步骤 ────────────────────────────────────────────────────────────────
def step_build(release: bool) -> None:
    say("[1] 构建 core/engine/cli")
    env = load_msvc_env()
    if env is None:
        prereq("MSVC 工具链", "找不到 vcvars64.bat：%s" % VCVARS, release)
        return
    if not require_repo_file(CMAKE, "VS 自带 CMake"):
        return

    rc, out = run([str(CMAKE), "-S", str(ROOT), "-B", str(BUILD_DIR),
                   "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], env=env)
    if rc != 0:
        fail("configure 失败（退出码 %d）\n%s" % (rc, tail(out, 20)))
        return

    rc, out = run([str(CMAKE), "--build", str(BUILD_DIR)], env=env)
    if rc != 0:
        fail("build 失败（退出码 %d）\n%s" % (rc, tail(out, 30)))
        return
    ok("configure + build 通过")


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
    try:
        probe.write_text("#include <QtCore/QString>\n", encoding="utf-8")
        rc_inject, out_inject = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)], env=env)
    finally:
        if probe.exists():
            probe.unlink()
    rc_after, _ = run([str(CMAKE), "-DXRKIT_ROOT=%s" % ROOT, "-P", str(script)], env=env)

    if rc_before != 0:
        fail("注入前基线就不通过 —— 这条锁现在无论红绿都不可信")
        return
    if rc_inject == 0:
        fail("往 core 里塞了 Qt 头却没变红 —— 这是「永远为真的锁」")
        return
    if rc_after != 0:
        fail("删掉注入文件后没恢复绿灯 —— 锁有残留状态")
        return
    ok("基线绿 → 注入红 → 还原绿（J5 确实在测东西）")


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

    M0 阶段 --bench/--themecheck/--dump 尚未实现，**必须**返回 2 并记入未覆盖，
    绝不允许把"没实现"混成 0。
    """
    say("[5] CLI 退出码契约")
    if not CLI_EXE.exists():
        prereq("构建产物 xrkit_cli.exe", "未构建", release)
        return

    expectations = [
        (["--version"], 0, "", "--version"),
        (["--selftest"], 0, "SELFTEST", "--selftest"),
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
        (["--bench"], "BENCH skip", "--bench"),
        (["--themecheck"], "THEMECHECK skip", "--themecheck"),
    ):
        rc, out = run([str(CLI_EXE)] + argv)
        if rc != 2 or marker not in out:
            fail("%s 期望 exit=2 且输出含 %r，实际 exit=%d 输出=%r" % (label, marker, rc, out.strip()))
        else:
            UNCOVERED.append("%s（引擎/外壳尚未实现，CLI 显式 SKIP）" % label)
            ok("%s → exit=2（已记入未覆盖）" % label)


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
    耗时口径：local/fast 抽 200 个文件（≈90 s），release 跑整棵树（≈3.5 min）。
    """
    say("[11] 真机样本：SquashFS 封包逐字节对拍")
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
                or s.startswith("ok 逐字节一致") or s.startswith("输入树"):
            say("  " + s)
    if rc == 0:
        ok("真机样本封包产物逐字节一致")
    elif rc == 2:
        prereq("真机语料样本 / rdsquashfs", "样本或参照工具缺失（见上）", release)
    else:
        fail("真机样本封包产物不一致（见上）")


def step_text_tables(release: bool) -> None:
    """编码表锁：磁盘上的 .inc 必须与 Python 同名 codec 一致。

    这是"仓库自带门禁文件"性质：表被手改、或换了 Python 版本导致 codec 变了 → FAIL。
    表错了的后果就是整篇乱码，而且是**逐字节判据**才能发现的那种错。
    """
    say("[12] 编码表与 Python codec 一致（cp1251 / cp1252 / gb18030 / gbk）")
    script = ROOT / "tools" / "gen_encoding_tables.py"
    if not require_repo_file(script, "编码表生成器"):
        return
    for inc in ("cp1251_tables.inc", "cp1252_tables.inc", "gb18030_tables.inc",
                "gbk_tables.inc"):
        if not require_repo_file(ROOT / "src" / "engine" / "text" / inc, "编码表生成物"):
            return
    rc, out = run([sys.executable, str(script), "--check"])
    if rc != 0:
        fail("编码表与 codec 不一致：\n%s" % tail(out, 10))
        return
    ok("四张表与 codec 完全一致")


def step_golden_text_decode(release: bool) -> None:
    """黄金语料差分：文本**解码层**（六编码链）2.x vs 1.x 的 read_text_file。

    退出码：0=全部逐字节一致，1=有不一致，2=有样本变动/不可达（按外部前提处理）。
    """
    say("[13] 黄金语料差分：文本解码（2.x vs 1.x 的 read_text_file）")
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
    say("[14] 黄金语料差分：文本提取（2.x vs 1.x）")
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

    判据分三层（第 5 步统计探测未移植 chardet，必须如实记账，不许放宽）：
      ① 硬判据 = (used, forced, 码位数, 正文 sha256) 一致；
      ② 命名台账 = 允许 (step, 编码名) 不同，但只允许台账里登记过的那些；
      ③ 差异台账 = 连正文都不同的样本（含 1.x 的 cp1250 误判）逐条冻结。
    退出码：0=一致，1=不一致，2=样本缺失/变动。
    """
    say("[15] 黄金语料：文本判定链（2.x vs 1.x）")
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
                or s.startswith("台账") or s.startswith("样本内容变了"):
            say("  " + s)
    if rc == 0:
        ok("判定链解码结果与 1.x 一致（差异台账已冻结）")
    elif rc == 2:
        prereq("真语料文本样本", "样本缺失或内容变动（见上）", release)
    else:
        fail("判定链与 1.x 不一致（见上）")


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
STEPS = [
    ("build", step_build),
    ("no-qt", step_no_qt_lock),
    ("no-qt-injection", step_no_qt_injection),
    ("selftest", step_selftest),
    ("cli", step_cli_contract),
    ("corpus", step_corpus),
    ("golden-sqfs", step_golden_sqfs),
    ("golden-extract", step_golden_extract),
    ("golden-db", step_golden_db),
    ("golden-sqfs-pack", step_golden_sqfs_pack),
    ("golden-sqfs-pack-real", step_golden_sqfs_pack_real),
    ("text-tables", step_text_tables),
    ("golden-text-decode", step_golden_text_decode),
    ("golden-text-extract", step_golden_text_extract),
    ("golden-text-sniff", step_golden_text_sniff),
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

    for name, fn in STEPS:
        if args.only and args.only not in name:
            continue
        fn(release)
        say("")

    say("-" * 66)
    if UNCOVERED:
        say("本次未覆盖（%d 项）：" % len(UNCOVERED))
        for item in UNCOVERED:
            say("  · " + item)
    else:
        say("本次覆盖：全部步骤均已执行")
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
