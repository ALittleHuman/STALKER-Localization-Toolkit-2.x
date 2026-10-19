# -*- coding: utf-8 -*-
"""判据 H-02：设置文件与游戏的 `user.ltx` **分开**，旧文件只在硬条件下迁移。

修前事实（审计原话）：`core/theme.cpp::user_ltx_path()` 返回 `app_dir()/user.ltx`，
与**游戏自己的** `user.ltx` 同名；`save_user_values()` 会把整份文件重渲染一遍
（键还在，注释与空行全丢）⇒ 对游戏配置就是破坏。

修法（用户裁决，方案 1）：设置写 `xrkit_user.ltx`；旧 `user.ltx` **只在**「新文件不存在 ＋
旧文件存在 ＋ 旧文件里有我们写过的键（`[theme] mode|scale` / `[ffmpeg] path|probe`）」时
原子迁移一次，**旧文件不删不改不再写**；其余情况**一个字节都不碰**。

断言（黑盒 `--ui-scale-probe` ＋ 静态锁）：
A 旧文件有我们的键 ⇒ 报告 `path=` 指向 `xrkit_user.ltx`、读到 `scale=1.25`、新文件生成，
  且**旧文件 sha256 前后不变**；
B 旧文件只有游戏键 ⇒ **不迁移**（不生成新文件），旧文件也不变；
C 没有旧文件 ⇒ 不新建；
D 静态锁：路径是 `xrkit_user.ltx`、`migrate_legacy_user_config` 有声明与实现、
  实现里有 `has_our_keys` 硬条件、走 `write_file_atomic`、迁移时明说旧文件未改动。

退出码：0=绿 / 1=红 / 2=缺输入。
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

ROOT = Path(__file__).resolve().parent.parent
CLIDIR = ROOT / "build" / "msvc-release" / "tools" / "cli"
CLI = CLIDIR / "xrkit_cli.exe"
CPP = ROOT / "src" / "core" / "theme.cpp"
HDR = ROOT / "include" / "xrkit" / "core" / "theme.h"
RUNTIME = ["python3.dll", "python314.dll"]

OURS = """// 游戏也会写这个文件；这一份里**同时**有我们写过的 [theme]
[general]
some_game_key = 1

[theme]
; 用户选过的主题
mode = dark
scale = 1.25

[ffmpeg]
path = D:\\tools\\ffmpeg.exe
"""

GAME_ONLY = """[general]
some_game_key = 1

[graphics]
renderer = renderer_r2.5
"""

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


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16] if path.is_file() else "<none>"


def probe(box: Path) -> str:
    try:
        p = subprocess.run([str(box / "xrkit_cli.exe"), "--ui-scale-probe"],
                           capture_output=True, timeout=300, cwd=str(box))
    except (OSError, subprocess.SubprocessError) as exc:
        return "RUN-ERROR: %s" % exc
    return (p.stdout or b"").decode("utf-8", "replace")


def main() -> int:
    print("== H-02：设置文件与游戏的 user.ltx 分开 ＋ 有条件迁移 ==")
    if not CLI.is_file():
        print("USER-LTX skip 缺 CLI 构建产物")
        return 2
    if not CPP.is_file() or not HDR.is_file():
        print("USER-LTX skip 缺 theme.cpp/theme.h")
        return 2

    cpp = CPP.read_text(encoding="utf-8", errors="replace")
    hdr = HDR.read_text(encoding="utf-8", errors="replace")
    i = cpp.find("std::string user_ltx_path()")
    body = cpp[i:cpp.find("\n}", i) + 2] if i >= 0 else ""
    want("xrkit_user.ltx" in body, "D1 `user_ltx_path()` 指向 `xrkit_user.ltx`",
         [l.strip() for l in body.splitlines() if "xrkit_user" in l][:1])
    want('L"user.ltx"' not in body, "D2 那个函数里不再出现 `user.ltx`")
    want("int migrate_legacy_user_config();" in hdr, "D3 `migrate_legacy_user_config` 有声明")
    j = cpp.find("int migrate_legacy_user_config() {")
    impl = cpp[j:cpp.find("\n}\n", j) + 3] if j >= 0 else ""
    want("has_our_keys" in impl, "D4 实现里用了「有没有我们写过的键」这条硬条件")
    want("write_file_atomic" in impl, "D5 迁移走原子写")
    want("未改动" in impl, "D6 迁移时明说旧文件没被改（零静默）")

    tmp = tempfile.mkdtemp(prefix="h02_")
    try:
        box = Path(tmp)
        try:
            for name in ["xrkit_cli.exe"] + RUNTIME:
                shutil.copy2(CLIDIR / name, box / name)
        except OSError as exc:
            print("USER-LTX skip 拷贝 CLI 副本失败（%s）" % exc)
            return 2
        legacy, new_file = box / "user.ltx", box / "xrkit_user.ltx"

        legacy.write_text(OURS, encoding="utf-8")
        before = sha(legacy)
        out = probe(box)
        want("xrkit_user.ltx" in out, "A1 报告 `path=` 指向 `xrkit_user.ltx`",
             [l.strip() for l in out.splitlines() if "path=" in l][:1])
        want("scale=1.25" in out, "A2 迁移后读到旧设置（scale=1.25）")
        want(new_file.is_file(), "A3 `xrkit_user.ltx` 已生成")
        want(sha(legacy) == before, "A4 **旧 `user.ltx` 逐字节未变**",
             "%s -> %s" % (before, sha(legacy)))

        new_file.unlink(missing_ok=True)
        legacy.write_text(GAME_ONLY, encoding="utf-8")
        before = sha(legacy)
        probe(box)
        want(not new_file.is_file(), "B1 只有游戏键 ⇒ **不迁移**")
        want(sha(legacy) == before, "B2 旧文件同样逐字节未变")

        legacy.unlink(missing_ok=True)
        probe(box)
        want(not new_file.is_file(), "C1 没有旧文件 ⇒ 不新建（走默认值）")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("-" * 66)
    if PROBLEMS:
        print("USER-LTX fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("USER-LTX ok %d 项（设置写 xrkit_user.ltx；有我们的键才迁移；旧文件逐字节不变）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
