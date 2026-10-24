# -*- coding: utf-8 -*-
"""打一个包（指定标记与序号）。从 `build/logs/_pkg.py` 转正 —— 那份在 `build/`（gitignore）里，
`clean.ps1` 会清掉，所以把带标记参数的那一份放到 `tools/`。

用法：
  python tools/pack_one.py 258 BETA      # 2.0.0 BETA.258
  python tools/pack_one.py 259 ALPHA

约定：
  · 序号必须**大于** `version.json` 里已出现过的最大序号（否则 `--build-package` 会拒）；
  · 打包脚本会在失败时自动回滚版本三件套（`version.json` / `version.h` / `.rc`）——
    失败后先看 `dist\\FAIL\\build_<序号>_*.log`（若已被归档，看 `build\\logs\\dist_fail_archive_*`）；
  · ★ 打包前必须没有残留 `xrkit_app.exe`（`package.ps1` 会因此拒打，否则 `vcruntime140.dll` 会 PermissionDenied）。
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _utf8_stdio() -> None:
    """★ 2026-10-24：控制台可能是 GBK ⇒ 直接 print 带 `⇒`/`✓` 的 CLI 输出会
    UnicodeEncodeError（BETA.2 打包时实测：包已经打好，却被"打印"这一步弄成 exit 1）。
    与 `run_ci.py` 同一条纪律：脚本**自己**保证能打印。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass


def main() -> int:
    _utf8_stdio()
    idx = sys.argv[1] if len(sys.argv) > 1 else ""
    marker = sys.argv[2] if len(sys.argv) > 2 else "ALPHA"
    if not idx:
        print("用法：python tools/pack_one.py <序号> [标记]")
        return 2
    cli = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
    if not cli.is_file():
        print("找不到 CLI：%s（先跑 tests/run_ci.py --fast 的 [1] 构建步骤）" % cli)
        return 2
    env = dict(os.environ)
    env["PATH"] = r"C:\Users\Administrator01\AppData\Local\Python\bin;" + env.get("PATH", "")
    # ★★ 2026-10-23：把**本启动器自己的解释器**告诉打包链（`--build-package` 要跑
    #   `tools/gen_winres.py`）。PATH 上的 `python` 在本机是 WindowsApps 的占位别名
    #   （返回 9009）⇒ 光改 PATH 不可靠；显式给 `XRKIT_PYTHON` 才是单一真值。
    env["XRKIT_PYTHON"] = sys.executable
    proc = subprocess.run([str(cli), "--build-package", str(ROOT / "version.json"),
                           "2.0.0", marker, idx],
                          cwd=str(ROOT), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=3000)
    print(proc.stdout.decode("utf-8", errors="replace"))
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
