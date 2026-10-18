# -*- coding: utf-8 -*-
r"""判据 A-1：**编码转换必须认得宽 BOM**（第三方审计 T-01）。

事实（本仓代码核 ＋ 行为实测）：
  · `sniff.cpp:396-410` 的 `decode_bytes()` 一直是对的：认出宽 BOM ⇒ **把整段原始字节交给对应 codec**
    （由 codec 自己吃掉 BOM 并按它定 LE/BE），`used_encoding` 置成 `utf-16`/`utf-32`；
  · `convert.cpp:184-195` 的 `convert_bytes()` 修前**只剥 2 字节就把"它曾是 UTF-16"丢掉** ⇒
    剩余字节没有 ≥0x80（UTF-16LE 的西里尔高字节是 0x04）⇒ 判定链落到「默认 windows-1251 低置信」
    ⇒ 有损解出一串夹 NUL 的乱码、按目标编码写回、统计报成功。
  · 实测（修前）：`step=默认(windows-1251) encoding=windows-1251 … 证据 0 字节`。

本判据走**真实产品路径**（CLI `--convert-batch`），四份同内容夹具：
  ① UTF-8 无 BOM ② UTF-8 有 BOM ③ UTF-16LE 有 BOM ④ UTF-16BE 有 BOM
断言：
  A 四份都必须打出判定行（认不出就没得判 ⇒ 红）；
  B ③④ 的 `step` **必须是 BOM 判出的宽编码**（含 `BOM`/`utf-16`），**不许**是 `windows-1251`；
  C ① 必须判成 utf-8（反向格：别为了过 ③④ 把 UTF-8 也判成 BOM）；
  D **若**产生产物：四份产物逐字节相同、且不含 NUL。

★ 为什么 D 是"若"：`--convert-batch` 在 `auto` 下按设计要过「用户确认编码」闸（P17-122），
  CLI 没有放行开关 ⇒ 现在四份都会 `rc=3`、无产物。**本判据不假装它验了产物** ——
  产物那条要等"BOM 高置信是否免确认"这个产品口径定下来才能变成硬断言（另立待办）。
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

TEXT = '<string id="a"><text>\u041f\u0440\u0438\u0432\u0435\u0442</text></string>\n'
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


def variants() -> dict:
    b = TEXT.encode("utf-8")
    return {
        "utf8_nobom": b,
        "utf8_bom": b"\xef\xbb\xbf" + b,
        "utf16le": b"\xff\xfe" + TEXT.encode("utf-16-le"),
        "utf16be": b"\xfe\xff" + TEXT.encode("utf-16-be"),
    }


def main() -> int:
    print("== 编码转换的宽 BOM（T-01）：判定必须是 BOM 步骤，不是默认 windows-1251 ==")
    if not CLI.is_file():
        print("BOM-CONVERT skip 缺 CLI 构建产物")
        return 2
    with tempfile.TemporaryDirectory(prefix="bomconv_") as tmp:
        base = Path(tmp)
        res = {}
        for name, data in variants().items():
            src = base / name / "a.xml"
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_bytes(data)
            dst = base / ("out_" + name)
            dst.mkdir()
            p = subprocess.run([str(CLI), "--convert-batch", str(src.parent), str(dst),
                                "auto", "utf-8", "--ext", ".xml", "--no-recursive"],
                               capture_output=True, timeout=600)
            log = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
            diag = ""
            for line in log.splitlines():
                if "diagnostics step=" in line:
                    diag = line.strip()
                    break
            got = dst / "a.xml"
            res[name] = {"rc": p.returncode, "diag": diag,
                         "data": got.read_bytes() if got.is_file() else None, "log": log}
            print("   [%-10s] rc=%s  diag=%s" % (name, p.returncode, diag[:96] or "（无）"))

        for name in variants():
            want(bool(res[name]["diag"]), "%s 打出了判定行" % name)

        for name in ("utf16le", "utf16be"):
            d = res[name]["diag"]
            want("windows-1251" not in d, "%s **不**被判成 windows-1251" % name,
                 "实际：" + d[:80] if "windows-1251" in d else "")
            want("BOM" in d and "utf-16" in d, "%s 判定走 **BOM(utf-16)**" % name, d[:80])

        want("utf-8" in res["utf8_nobom"]["diag"] and "BOM" not in res["utf8_nobom"]["diag"],
             "① UTF-8 无 BOM 仍判 utf-8（反向格：不许把 UTF-8 也判成 BOM）",
             res["utf8_nobom"]["diag"][:70])

        # D：产物只在真有产物时才断言（并**明说**它可能因为确认闸缺席）
        produced = {n: r["data"] for n, r in res.items() if r["data"] is not None}
        if produced:
            ref = produced.get("utf8_nobom")
            for name, data in produced.items():
                if ref is not None:
                    want(data == ref, "%s 产物与 ① 逐字节相同" % name,
                         "不同（%d vs %d 字节）" % (len(data), len(ref)))
                want(b"\x00" not in data, "%s 产物不含 NUL" % name,
                     "含 %d 个 NUL" % data.count(b"\x00"))
        else:
            stopped = all("stopped" in r["log"] and "确认" in r["log"] for r in res.values())
            want(stopped, "四份都因「需要确认编码」停下（**产物未生成，本判据未验产物**）",
                 "有文件既没产物也没说为什么 ⇒ 静默，红")

    print("-" * 66)
    if PROBLEMS:
        print("BOM-CONVERT fail %d 项不成立：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BOM-CONVERT ok %d 项（宽 BOM 走 BOM 步骤、UTF-8 不被误判；产物那条见文件头待办）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
