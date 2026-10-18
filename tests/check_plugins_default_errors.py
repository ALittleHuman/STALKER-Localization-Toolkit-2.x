# -*- coding: utf-8 -*-
r"""判据 C-01：**默认插件目录的加载错误不许被吞**（第三方只读审计 C-01）。

修前事实（审计原话 + 我的实测）
------------------------------
`tools/cli/main.cpp` 里只有 `if (plugins_given)` 才把 `PLUGINS dir=… errors=N` 与逐条
`PLUGINS warn …` 打到 **stdout**；**默认路径**（`exe 旁边的 plugins\`，也就是用户真会用的那条）
加载失败**只在 `core::log` 里留一行**，而日志可以关、也会被刷走 ⇒ 用户看到的是
"插件好像没生效"，**没有任何计数可查**（零静默失守）。

修法
----
默认路径下把同一笔账打到 **stderr**，前缀 `PLUGINS-DEFAULT`：
  `PLUGINS-DEFAULT dir=… errors=N` ＋ 逐条 `PLUGINS-DEFAULT warn plugin=… reason=…`
★ 为什么走 stderr：修前那条"默认路径 stdout 与修前逐字节一致"的口径要**保住**
（既有判据/契约比较的是 stdout）；而"加载失败"这件事必须看得见 ⇒ 换流，不换语义。

断言（黑盒：把 CLI 拷出来跑，**不污染构建树**）
----------------------------------------------
A 默认路径 + 一枚坏 `plugins\\bogus.dll` ⇒ **stderr** 出现 `PLUGINS-DEFAULT` 且 `errors=1`；
B 同一次运行 **stdout 里没有 `PLUGINS`**（逐字节一致那条口径还在）；
A2 stderr 逐条带 `plugin=bogus.dll`（点名，不只报数）；
C 默认目录**干净**时**不**打 `PLUGINS-DEFAULT`（不制造噪声）；
D 显式 `--plugins` ⇒ stdout 照旧 `PLUGINS dir=… errors=1`（旧路一字不改）；
E 静态锁：源码里那个 `else if (!plugin_errors.empty())` 分支在、且写的是 `std::cerr`。

退出码：0=绿 / 1=红 / 2=缺输入（CLI 产物或源码）。
"""

from __future__ import annotations

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
# CLI 除了 exe 还要这两枚（实测：少了就 0xC0000135 = STATUS_DLL_NOT_FOUND）
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
        # 跑不起来就**明说**（别把崩溃伪装成"没有输出"）
        return None, "", "RUN-ERROR: %s" % exc
    out = (proc.stdout or b"").decode("utf-8", "replace")
    err = (proc.stderr or b"").decode("utf-8", "replace")
    return proc.returncode, out, err


def main() -> int:
    print("== C-01：默认插件目录的加载错误不许被吞 ==")
    if not CLI.is_file():
        print("PLUGINS-DEFAULT skip 缺 CLI 构建产物")
        return 2
    if not SRC.is_file():
        print("PLUGINS-DEFAULT skip 缺 tools/cli/main.cpp")
        return 2

    # ★ 门禁里这个临时目录常被别的步骤按着句柄（拷进去的 python3XX.dll / exe 还没被回收）
    #   ⇒ 清理时 rmtree 会抛异常，把**判据自己**拖成 rc=1（实测：全量门禁里 A–D 全绿、
    #   E1/E2 一行都没打出来就 FAIL）。`ignore_cleanup_errors` 只影响清理，不影响判据。
    with tempfile.TemporaryDirectory(prefix="c01_", ignore_cleanup_errors=True) as tmp:
        box = Path(tmp)
        # ★ 拷不出一份能跑的副本（例如门禁里别的步骤还按着 python3XX.dll）⇒ **记 skip**，
        #   不许把"判据自己没跑成"伪装成"判据失败"（实绿/实红之外只允许第三种：未覆盖）。
        try:
            for name in ["xrkit_cli.exe"] + RUNTIME:
                src = CLIDIR / name
                if not src.is_file():
                    print("PLUGINS-DEFAULT skip 缺 %s（拷不出一份能跑的 CLI）" % name)
                    return 2
                shutil.copy2(src, box / name)
        except OSError as exc:
            print("PLUGINS-DEFAULT skip 拷贝 CLI 副本失败（%s）" % exc)
            return 2
        target = box / "nope.db"

        # A：默认目录里放一枚坏 dll
        plugged = box / "plugins"
        plugged.mkdir()
        (plugged / "bogus.dll").write_bytes(b"not a pe")
        rc, out, err = run(box, ["--dump", str(target)])
        want("PLUGINS-DEFAULT" in err and "errors=1" in err,
             "A 默认路径下加载失败 ⇒ **stderr** 有 `PLUGINS-DEFAULT … errors=1`",
             [l.strip() for l in err.splitlines() if "PLUGINS-DEFAULT" in l][:1])
        want("PLUGINS" not in out,
             "B 同一次运行的 **stdout 里没有 `PLUGINS`**（默认路径 stdout 逐字节一致的口径还在）",
             [l.strip() for l in out.splitlines() if "PLUGINS" in l][:1] or "干净")
        want("plugin=bogus.dll" in err, "A2 逐条**点名**（`plugin=bogus.dll`），不只报个总数")

        # C：干净目录（存在但空）⇒ 不许有噪声
        shutil.rmtree(plugged)
        plugged.mkdir()
        rc, out, err = run(box, ["--dump", str(target)])
        want("PLUGINS-DEFAULT" not in err and "PLUGINS" not in out,
             "C 默认目录存在但为空 ⇒ **不打** `PLUGINS-DEFAULT`（不制造噪声）")

        # C2（本批被门禁抓到的那条）：**默认目录不存在** ⇒ 也不算错、也不打（口径见 README）
        shutil.rmtree(plugged)
        rc, out, err = run(box, ["--dump", str(target)])
        want("PLUGINS-DEFAULT" not in err and "PLUGINS" not in out,
             "C2 默认目录**不存在** ⇒ 同样静默（「plugins 目录不存在也不算错」是本仓既有口径；"
             "第一版把它当错误报出来，门禁 `golden-db` 步当场红）",
             [l.strip() for l in err.splitlines() if "PLUGINS" in l][:1] or "干净")

        # C3：显式指向一个**不存在的目录** ⇒ 仍然要报账（用户明确指了地方，找不到就得说）
        rc, out, err = run(box, ["--plugins", str(box / "nope_plugins"), "--dump", str(target)])
        want("PLUGINS " in out and "errors=" in out,
             "C3 显式 `--plugins <不存在的目录>` ⇒ 照旧报账（这一档不因 C2 被吞掉）",
             [l.strip() for l in out.splitlines() if l.startswith("PLUGINS ")][:1])

        # D：显式 --plugins 的旧路一字不改
        plugged.mkdir()
        (plugged / "bogus.dll").write_bytes(b"not a pe")
        rc, out, err = run(box, ["--plugins", str(plugged), "--dump", str(target)])
        want("PLUGINS dir=" in out and "errors=1" in out,
             "D 显式 `--plugins` ⇒ stdout 照旧 `PLUGINS dir=… errors=1`",
             [l.strip() for l in out.splitlines() if l.startswith("PLUGINS ")][:1])

    src = SRC.read_text(encoding="utf-8", errors="replace")
    want("} else if (!plugin_errors.empty()) {" in src,
         "E1 源码里那个 `else if (!plugin_errors.empty())` 分支在（默认路径也报账）")
    tail = src[src.find("} else if (!plugin_errors.empty()) {"):][:1200]
    want("std::cerr << \"PLUGINS-DEFAULT dir=\"" in tail,
         "E2 走的是 **stderr**（`std::cerr`）而不是 stdout —— 保住逐字节一致那条口径")

    print("-" * 66)
    if PROBLEMS:
        print("PLUGINS-DEFAULT fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("PLUGINS-DEFAULT ok %d 项（默认路径的加载失败打到 stderr 且逐条点名；stdout 一致；"
          "干净目录不噪声；显式路径一字不改）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
