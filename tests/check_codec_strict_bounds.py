#!/usr/bin/env python
# -*- coding: utf-8 -*-
r"""判红锁：严格解码/编码的**边界域**（审计 2-1 / 2-2 / H1-3 / A1-1 / A1-2）。

为什么需要它：严格路径此前**没有任何黑盒入口**能打这两条边界 ——
  · `--decode-text` 走六编码链（latin-1 兜底 ⇒ 永不失败）；
  · `--codec-lossy` 走 replace 语义。
⇒ `gb18030`/`gbk`/`gb2312` 严格解码对首字节 `0xFF` 的**越界读表**、`utf32_encode_impl`
有损分支的**死循环**，谁都判不了红（门禁的 `golden_lossy_codec` 用 `bytes(range(0x80,0x100))`，
`0xFF` 落在末尾先被"截断"挡掉，正好覆盖不到）。

判据（每条都断言）：
  ① `0xFF`/`0xFF 0x40`/`0xFF 0xFE` 的**严格**解码对 gb18030/gbk/gb2312 **必须失败**
     （Python 同名 codec 一律 `UnicodeDecodeError`；修前实测 gb18030 `FF 40` → `ef a6 a1`、
     gbk → `00`，都是越界读到的垃圾被当成合法解码结果）；
  ② 反向：合法序列（中文/ASCII/空）与合法对 + 非法尾字节的**既有语义**不许被误拒；
  ③ `utf-32` 有损编码非法 UTF-8 **必须返回**（修前死循环 + 无限增长），
     且产物形态与 utf-16 兄弟分支同口径（有效前缀 + 一个 `?`，然后收尾）。

输出：`✓`/`✗` 行 + 收尾行 `CODEC-STRICT ok …` / `CODEC-STRICT fail …`；退出码 0/1/2。
夹具一律 `%TEMP%`，每次调用带**硬超时**（挂死就是判据本身）。
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

PROBLEMS = []
CHECKS = 0


def want(cond, label, detail=""):
    global CHECKS
    CHECKS += 1
    if cond:
        print("  \u2713 " + label)
    else:
        print("  \u2717 " + label + ("\u2014\u2014" + detail if detail else ""))
        PROBLEMS.append(label)


def call(args, timeout):
    """返回 (rc, out)；超时给 rc=124（**不挂死测试进程**）。"""
    try:
        p = subprocess.run([str(CLI)] + args, cwd=str(ROOT), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        return 124, "[超时 %ss —— 判据就是「它必须返回」]" % timeout


def main() -> int:
    if not CLI.exists():
        print("CODEC-STRICT fail reason=构建产物缺失（先跑门禁的 build 步骤）：%s" % CLI)
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="xrkit_codec_strict_"))
    try:
        out_file = work / "out.bin"

        # ── ① 0xFF 前导：三个编码的**严格**解码必须失败 ──────────────────────
        bad = {
            "ff40": bytes([0xFF, 0x40]),
            "fffe": bytes([0xFF, 0xFE]),
            "ff": bytes([0xFF]),
            "ffff": bytes([0xFF, 0xFF]),
        }
        for codec in ("gb18030", "gbk", "gb2312"):
            for name, data in bad.items():
                src = work / ("%s_%s.bin" % (codec, name))
                src.write_bytes(data)
                rc, out = call(["--codec-lossy", "decode", codec, str(src), "strict",
                                str(out_file), "--force"], timeout=60)
                want(rc != 0 and "CODEC error" in out,
                     "\u2460 strictly %s %s 必须失败（Python 同名 codec 必抛）" % (codec, name),
                     "exit=%d out=%s" % (rc, out.strip()[:160]))

        # ── ② 反向：合法用法不许被误拒 ───────────────────────────────────────
        good = {
            "ascii": b"abc",
            "empty": b"",
            "zh": "\u4e2d".encode("gb18030"),
        }
        for codec in ("gb18030", "gbk", "gb2312"):
            for name, data in good.items():
                src = work / ("good_%s_%s.bin" % (codec, name))
                src.write_bytes(data)
                rc, out = call(["--codec-lossy", "decode", codec, str(src), "strict",
                                str(out_file), "--force"], timeout=60)
                want(rc == 0 and "CODEC ok" in out,
                     "\u2461 strictly %s 的合法输入 %s 必须成功" % (codec, name),
                     "exit=%d out=%s" % (rc, out.strip()[:160]))

        # ── ③ utf-32 有损编码非法 UTF-8：必须返回（修前死循环）───────────────
        bad_utf8 = work / "bad_utf8.txt"
        bad_utf8.write_bytes(b"ok\xe4\xb8bad")
        for codec in ("utf-32", "utf-32-le", "utf-32-be"):
            rc, out = call(["--codec-lossy", "encode", codec, str(bad_utf8), str(out_file), "--force"],
                           timeout=20)
            want(rc == 0 and "CODEC ok" in out,
                 "\u2462 utf-32 家族有损编码非法 UTF-8 必须返回（修前：死循环 + 无限增长）"
                 " [%s]" % codec,
                 "exit=%d out=%s" % (rc, out.strip()[:200]))
        # 产物形态与 utf-16 兄弟分支同口径：有效前缀 "ok" + 一个 `?`，然后收尾
        rc, _ = call(["--codec-lossy", "encode", "utf-16-le", str(bad_utf8), str(out_file), "--force"],
                     timeout=20)
        if rc == 0:
            want(out_file.read_bytes() == b"o\x00k\x00?\x00",
                 "\u2462 utf-16-le 有损分支的**既有**口径：有效前缀 + 一个 ?",
                 out_file.read_bytes().hex(" "))
        else:
            want(False, "\u2462 utf-16-le 有损编码应当成功", "exit=%d" % rc)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    verdict = "ok" if not PROBLEMS else "fail"
    print("CODEC-STRICT %s (checks=%d failed=%d)" % (verdict, CHECKS, len(PROBLEMS)))
    return 0 if not PROBLEMS else 1


if __name__ == "__main__":
    # 与 golden_lossy_codec 同一条零静默纪律：任何路径都要有收尾行（含 reason）
    import time as _t
    _t0 = _t.time()
    try:
        _rc = main()
    except BaseException as exc:  # noqa: BLE001
        print("CODEC-STRICT fail (reason=%s: %s; elapsed=%.1fs)"
              % (type(exc).__name__, exc, _t.time() - _t0), flush=True)
        raise SystemExit(1)
    print("CODEC-STRICT: elapsed=%.1fs" % (_t.time() - _t0), flush=True)
    sys.exit(_rc)
