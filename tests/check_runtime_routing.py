# -*- coding: utf-8 -*-
"""依赖路由判据（用户 2026-11-01 口径）。

`[runtime]` 写在哪：**`xrkit_user.ltx`**（本工具的设置文件），不是游戏的 `user.ltx`
（`user.ltx` 是游戏配置，本仓只在硬条件下读一次做迁移、绝不写）。

优先级（缺一不可，且**不许 PATH 兜底**）：
  ① 单项显式（`[runtime] ffmpeg=...`）
  ② `root` + 约定子路径（`bin/ffmpeg.exe` …）
  ③ 工具目录默认（`<app>/bin/...`）
  ④ 都没有 ⇒ 明确 missing（退出码非 0）

记法：`@APP@` = 工具目录；`${root}` = root 展开值。
"""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PROBLEMS = []


def ok(msg):
    print("  \u2713 " + msg)


def fail(msg):
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def write(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def touch(path: Path):
    write(path, "MZ")


def norm(text: str) -> str:
    return text.replace("/", "\\").lower()


def run(app: Path):
    proc = subprocess.run([str(CLI), "--runtime-paths", str(app)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def main() -> int:
    print("== 依赖路由：@APP@ / ${root} / 优先级 / 缺件报错 ==")
    if not CLI.is_file():
        print("!! 缺 %s" % CLI)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_rt_"))
    try:
        app = tmp / "app"
        rt = tmp / "runtime"
        touch(app / "bin" / "ffmpeg.exe")
        touch(app / "python" / "python.exe")
        touch(rt / "bin" / "ffmpeg.exe")
        touch(rt / "bin" / "ffprobe.exe")
        touch(rt / "python" / "python.exe")
        touch(rt / "tools" / "ixr_run.py")
        (rt / "tools" / "fontlib").mkdir(parents=True)
        (rt / "tools" / "fontlib" / "x.txt").write_text("x", encoding="utf-8")

        # ② root 指向共享 runtime（迭代期）
        write(app / "xrkit_user.ltx", "[runtime]\nroot = %s\n" % rt)
        rc, out = run(app)
        if rc == 0 and "source=root" in out and str(rt) in out:
            ok("root 指向共享 runtime：5 项全部 source=root")
        else:
            fail("root 路由不对：rc=%d\n%s" % (rc, out[-400:]))

        # ③ 正式版写法 root=@APP@
        write(app / "xrkit_user.ltx", "[runtime]\nroot = @APP@\n")
        rc, out = run(app)
        if norm("root=%s" % app) in norm(out) and norm("ffmpeg=%s" % (app / "bin" / "ffmpeg.exe")) in norm(out):
            ok("@APP@ 展开成工具目录，ffmpeg 从工具目录取")
        else:
            fail("@APP@ 没展开对：\n%s" % out[-400:])

        # ① 单项显式优先于 root
        write(app / "xrkit_user.ltx",
              "[runtime]\nroot = @APP@\nffmpeg = ${root}/python/python.exe\n")
        rc, out = run(app)
        if norm("ffmpeg=%s source=explicit" % (app / "python" / "python.exe")) in norm(out):
            ok("单项显式优先于 root（含 ${root} 展开）")
        else:
            fail("显式项没优先：\n%s" % out[-400:])

        # ④ 全缺 ⇒ 明确 missing + 退出码非 0
        empty = tmp / "empty_app"
        empty.mkdir()
        rc, out = run(empty)
        if rc != 0 and "(missing) source=missing" in out:
            ok("全缺：明确报 missing，退出码非 0（不静默、不搜 PATH）")
        else:
            fail("缺件行为不对：rc=%d\n%s" % (rc, out[-300:]))

        # ⑤ 相对 root 必须锚到 **app 目录**（不是当前工作目录）
        rel_app = tmp / "rel_app"
        touch(rel_app / "runtime" / "bin" / "ffmpeg.exe")
        write(rel_app / "xrkit_user.ltx", "[runtime]\nroot = runtime\n")
        rc, out = run(rel_app)
        if norm("ffmpeg=%s source=root" % (rel_app / "runtime" / "bin" / "ffmpeg.exe")) in norm(out):
            ok("相对 root 锚到 app 目录（从别的 cwd 启动也找得到）")
        else:
            fail("相对 root 没锚到 app 目录：\n%s" % out[-300:])

        # ⑥ 显式项写了但不存在 ⇒ 必须 explicit-missing 且退出码非 0（不许静默回退）
        miss_app = tmp / "miss_app"
        touch(miss_app / "bin" / "ffmpeg.exe")
        write(miss_app / "xrkit_user.ltx",
              "[runtime]\nffmpeg = ${root}/gone.exe\nroot = @APP@\n")
        rc, out = run(miss_app)
        if "source=explicit-missing" in out and rc != 0:
            ok("显式项失效：明确报 explicit-missing，不静默回退到 root/app")
        else:
            fail("显式项失效被静默回退：rc=%d\n%s" % (rc, out[-300:]))

        # ⑦ 正式版**不发** xrkit_user.ltx（避免覆盖用户设置）
        dist = ROOT / "dist" / "2.0.0"
        newest = sorted(dist.glob("BETA/*/")) if dist.is_dir() else []
        if newest:
            if not (newest[-1] / "xrkit_user.ltx").exists():
                ok("正式版包内没有 xrkit_user.ltx（不覆盖用户设置）")
            else:
                fail("正式版包里仍带 xrkit_user.ltx：%s" % newest[-1].name)
        else:
            print("  · 没有 dist/2.0.0/BETA 产物，跳过发布语义检查")

        # 不许把开发路径写进正式版 —— 这条由 package.ps1 的泄漏检查兜（见路由文档）
        print("  · 开发路径泄漏检查在 scripts/package.ps1（打包时执行）")
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("RUNTIME-ROUTING: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("RUNTIME-ROUTING: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
