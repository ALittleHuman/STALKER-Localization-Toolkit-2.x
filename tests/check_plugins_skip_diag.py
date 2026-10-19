# -*- coding: utf-8 -*-
r"""判据 C-03：**诊断动作不该加载插件**（第三方只读审计 C-03）。

修前事实（审计原话）
--------------------
`tools/cli/main.cpp` 在动作分派前**无条件** `load_all_plugins_from_dir`
⇒ 每条命令都加载插件（`.py` 插件还会把 **CPython 起起来**）——诊断动作因此
变慢、有副作用，**输出会被坏插件污染**。

修法
----
**白名单**，只收"完全不碰注册表"的动作：`--exe-version`（只读 exe 的 VERSIONINFO）。
★ `--help`/`--version`/无动作 本来就在参数解析阶段返回，走不到加载那一段。
⚠ **绝不能进白名单**：`--dump`/`--list`（要解密插件）、`--unpack-*`、`--pack-*`、
  `--convert-*`、`--selftest`（它自己就在验插件加载）。
★ 用户**显式**给 `--plugins` 就照旧加载（明确要求的事不做"贴心的忽略"）。

断言（黑盒：把 CLI ＋ 两枚 Python DLL 拷到临时目录跑，**不污染构建树**）
----------------------------------------------------------------------
A 默认目录里有坏 `plugins\bogus.dll` 时：
  · `--exe-version` ⇒ stderr **没有** `PLUGINS-DEFAULT`（＝没去加载插件）；
A2 同一次运行 stdout 里也没有 `PLUGINS`（逐字节一致口径不变）；
B 对照：`--dump <不存在的文件>` ⇒ **仍然加载**（stderr 有 `PLUGINS-DEFAULT`）——
  证明白名单没把"需要插件的动作"一起关掉；
C 静态锁：源码里 `skip_plugins` 的白名单**只有** `--exe-version`（多收一个就要重新论证）。

退出码：0=绿 / 1=红 / 2=缺输入（CLI 产物或源码）。
"""

from __future__ import annotations

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
CLIDIR = ROOT / "build" / "msvc-release" / "tools" / "cli"
CLI = CLIDIR / "xrkit_cli.exe"
SRC = ROOT / "tools" / "cli" / "main.cpp"
RUNTIME = ["python3.dll", "python314.dll"]

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


def run(box: Path, args: list, timeout: int = 300) -> tuple:
    try:
        proc = subprocess.run([str(box / "xrkit_cli.exe")] + args, capture_output=True,
                              timeout=timeout, cwd=str(box))
    except (OSError, subprocess.SubprocessError) as exc:
        return None, "", "RUN-ERROR: %s" % exc
    return (proc.returncode,
            (proc.stdout or b"").decode("utf-8", "replace"),
            (proc.stderr or b"").decode("utf-8", "replace"))


def main() -> int:
    print("== C-03：诊断动作不该加载插件（白名单：--exe-version）==")
    if not CLI.is_file():
        print("PLUGINS-SKIP-DIAG skip 缺 CLI 构建产物")
        return 2
    if not SRC.is_file():
        print("PLUGINS-SKIP-DIAG skip 缺 tools/cli/main.cpp")
        return 2

    # ── 静态锁先判（它不该被黑盒的失败带走）──────────────────────────
    src = SRC.read_text(encoding="utf-8", errors="replace")
    m = re.search(r"const bool skip_plugins = ([^;]+);", src)
    want(m is not None, "C0 源码里有 `skip_plugins` 的判定式")
    if m:
        expr = m.group(1)
        want('action == "--exe-version"' in expr,
             "C1 白名单里**有** `--exe-version`（诊断动作跳过加载）", expr.strip()[:90])
        strays = [a for a in re.findall(r'action == "(--[a-z0-9\-]+)"', expr)
                  if a != "--exe-version"]
        want(not strays, "C2 白名单里**只有** `--exe-version`（每多收一个都要重新论证它不碰注册表）",
             ("多收：" + ",".join(strays)) if strays else "")
        want("plugins_given" in expr, "C3 显式 `--plugins` 时照旧加载（不替用户做主）")
    want("!skip_plugins && (plugins_given || dir_exists)" in src,
         "C4 加载那一支真的用上了 `skip_plugins`（不是只声明）")

    tmp = tempfile.mkdtemp(prefix="c03_")
    try:
        box = Path(tmp)
        try:
            for name in ["xrkit_cli.exe"] + RUNTIME:
                shutil.copy2(CLIDIR / name, box / name)
        except OSError as exc:
            print("PLUGINS-SKIP-DIAG skip 拷贝 CLI 副本失败（%s）" % exc)
            return 2
        plugged = box / "plugins"
        plugged.mkdir()
        (plugged / "bogus.dll").write_bytes(b"not a pe")

        rc, out, err = run(box, ["--exe-version", str(box / "xrkit_cli.exe")])
        want("PLUGINS-DEFAULT" not in err,
             "A `--exe-version` ⇒ stderr **没有** `PLUGINS-DEFAULT`（＝没去加载插件）",
             [l.strip() for l in err.splitlines() if "PLUGINS" in l][:1] or "干净")
        want("PLUGINS" not in out, "A2 同一次运行 stdout 也没有 `PLUGINS`（口径不变）")
        want(rc == 0, "A3 `--exe-version` 仍然正常退出（没把功能弄坏）", "rc=%s" % rc)

        rc2, out2, err2 = run(box, ["--dump", str(box / "nope.db")])
        want("PLUGINS-DEFAULT" in err2,
             "B 对照：`--dump`（要插件的那条路）**仍然加载**插件",
             [l.strip() for l in err2.splitlines() if "PLUGINS-DEFAULT" in l][:1] or "没有 —— 白名单关过头了")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("PLUGINS-SKIP-DIAG fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PLUGINS-SKIP-DIAG ok %d 项（`--exe-version` 不加载插件；`--dump` 照旧加载；"
          "显式 `--plugins` 照旧）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
