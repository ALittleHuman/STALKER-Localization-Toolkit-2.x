# -*- coding: utf-8 -*-
r"""判据 A-1：**编码转换必须认得宽 BOM**（第三方审计 T-01；先补锁，再改代码）。

事实（本仓代码核，不是推演）：
  · `sniff.cpp:396-410` 的 `decode_bytes()` 有正确分支：认出宽 BOM ⇒ **把整段原始字节交给对应 codec**
    （由 codec 自己吃掉 BOM），并把 `used_encoding` 置成 `utf-16`/`utf-32`；
  · `convert.cpp:184-195` 的 `convert_bytes()` **只剥 2 字节就丢掉了"它曾是 UTF-16"这个事实**，
    随后对剩下的字节跑 `sniff_encoding()` ⇒ UTF-16LE 的西里尔高字节是 0x04、没有 ≥0x80，
    判定链落到「默认 windows-1251 低置信」⇒ 用有损解码解出**夹 NUL 的乱码**，
    再按目标编码写回、统计报成功。
  ⇒ 两条路径**能力不对称**，而 `convert_page.cpp` 与 `--convert-batch` 都直调后者。

本判据用 **CLI `--convert-batch`** 走真实产品路径，四份同内容夹具：
  ① UTF-8 无 BOM ② UTF-8 有 BOM ③ UTF-16LE 有 BOM ④ UTF-16BE 有 BOM
断言三条：
  A 四份转换到 `utf-8` 后**产物字节完全相同**（都不带 BOM）—— 现在③④会红；
  B ③④ 的产物里**不含 NUL**（有损解码的特征）；
  C ③④ 的日志里 `step` 必须是 BOM 判出的宽编码，不是「默认(windows-1251)」。

★★ 状态（2026-10-16）：**WIP，尚未挂进门禁**。两条如实记：
  ① 本判据**已经证出 T-01**：UTF-16LE/BE 两份的判定行是
     `step=默认(windows-1251) encoding=windows-1251 … 证据 0 字节` —— 与第三方审计描述一致。
  ② 但**产物断言现在红得不是地方**：`--convert-batch` 在 `auto` 下按设计要过「用户确认编码」闸
     （P17-122），CLI 没有放行开关 ⇒ 四份都 `rc=3`、无产物。
     ⇒ 一个"因为调用方式不对而红"的判据**不是锁**（本仓纪律：红证要红在该红的地方）。
     待办二选一：(a) 修好 BOM 后高置信不再需确认，产物断言自然可用；
     (b) 另加一条只断言 `step` 的锁（那条现在就能红在该红的地方）。
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


def write_variants(base: Path) -> dict:
    out = {}
    b = TEXT.encode("utf-8")
    out["utf8_nobom"] = b
    out["utf8_bom"] = b"\xef\xbb\xbf" + b
    out["utf16le"] = b"\xff\xfe" + TEXT.encode("utf-16-le")
    out["utf16be"] = b"\xfe\xff" + TEXT.encode("utf-16-be")
    return out


def main() -> int:
    print("== 编码转换的宽 BOM（T-01）：四份同内容夹具必须转到同一份字节 ==")
    if not CLI.is_file():
        print("BOM-CONVERT skip 缺 CLI 构建产物")
        return 2
    with tempfile.TemporaryDirectory(prefix="bomconv_") as tmp:
        base = Path(tmp)
        variants = write_variants(base)
        results = {}
        for name, data in variants.items():
            src = base / name / "a.xml"
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_bytes(data)
            dst = base / ("out_" + name)
            dst.mkdir()
            p = subprocess.run([str(CLI), "--convert-batch", str(src.parent), str(dst), "auto",
                                "utf-8", "--ext", ".xml", "--no-recursive"],
                               capture_output=True, timeout=600)
            log = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
            got = dst / "a.xml"
            results[name] = (p.returncode, got.read_bytes() if got.is_file() else None, log)
            print("   [%s] rc=%s 产物=%s 字节" % (name, p.returncode,
                                                len(results[name][1]) if results[name][1] is not None else "无"))
            for line in log.splitlines():
                if "step" in line or "encoding" in line:
                    print("       " + line.strip()[:130])
                    break

        ref = results["utf8_nobom"][1]
        want(ref is not None, "① UTF-8 无 BOM 有产物")
        for name in ("utf8_bom", "utf16le", "utf16be"):
            got = results[name][1]
            want(got is not None, "%s 有产物" % name)
            if got is not None and ref is not None:
                want(got == ref, "%s 产物与 ① **逐字节相同**" % name,
                     "不同（%d vs %d 字节）" % (len(got), len(ref)))
            if got is not None:
                want(b"\x00" not in got, "%s 产物里**不含 NUL**" % name,
                     "含 %d 个 NUL" % got.count(b"\x00"))
        for name in ("utf16le", "utf16be"):
            log = results[name][2]
            want("windows-1251" not in log,
                 "%s 的判定**不是**「默认(windows-1251)」" % name,
                 "日志里出现了 windows-1251")

    print("-" * 66)
    if PROBLEMS:
        print("BOM-CONVERT fail %d 项不成立（**这正是 T-01 的现场**）：" % len(PROBLEMS))
        for item in PROBLEMS:
            print("  · " + item)
        return 1
    print("BOM-CONVERT ok %d 项（四份夹具归一到同一份字节、无 NUL、判定走 BOM）" % CASES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
