#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本**解码层**黄金语料：六编码链（2.x C++ vs 1.x `read_text_file`）逐字节对拍。

判据：同一份输入，我们的产物（解码后的 UTF-8 正文）与 1.x 的 `read_text_file`
返回的正文**逐字节相同**，且报出的编码名一致。

为什么单独锁这一层：整条文本链的落点全靠它（乱码的历史血账都在这里——1.x 曾因
"手工单字节名单漏了 cp865"整篇乱码）。它也是**确定性**的：1.x 的 `read_text_file`
是六编码链，不含统计探测，所以能被逐字节复刻。

    python tests/golden_text_decode.py --write    # 用 1.x 生成基线
    python tests/golden_text_decode.py            # 对拍（跑 2.x 的 --decode-text）
    python tests/golden_text_decode.py --id real  # 只跑真语料 / synth 合成样本

语料两半：
  · 真语料：6 棵真实 STALKER 树各取前 N 个文件（cp1251 俄文 / UTF-8 / ASCII 混合）
  · 合成边界：BOM 五种、纯 ASCII、cp1251、cp1252 未定义字节、gb18030 双/四字节、
    cp1251+cp1252 都解不开（落 latin-1）、空文件、截断 UTF-8
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BASELINE = HERE / "golden" / "text_decode.json"
WORK = ROOT / "build" / "golden_text_decode"

TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")

# 真语料：6 棵真树各取前 PER_ROOT 个文件（路径不存在就跳过并记账）
REAL_ROOTS = [
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\gameplay"),
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\scripts"),
    Path(r"E:\Software\Games\STALKER\Localization\Gamedata\A.R.E.A\Original\configs\gameplay"),
    Path(r"E:\Software\Games\STALKER\Localization\Gamedata\A.R.E.A\Original\configs\scripts"),
    Path(r"E:\Software\Games\STALKER\SoC_Source\S.T.A.L.K.E.R. Shadow of Chernobyl_RUS"),
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\scripts"),
]
PER_ROOT = 30

RUS = "Привет, сталкер! Ты должен идти на Янтарь."          # 俄文
CJK = "你好，潜行者。这是中文测试。"                          # 中文


def synth_samples() -> dict[str, bytes]:
    """合成边界样本（字节固定，可复现）。"""
    out: dict[str, bytes] = {}
    out["utf8_sig.bin"] = b"\xef\xbb\xbf" + RUS.encode("utf-8")
    out["utf8_plain.bin"] = RUS.encode("utf-8") + b"\n" + CJK.encode("utf-8")
    out["ascii.bin"] = b"<config>\n  <text>Hello</text>\n</config>\n"
    out["empty.bin"] = b""
    out["cp1251.bin"] = RUS.encode("cp1251")
    out["cp1251_crlf.bin"] = (RUS.encode("cp1251") + b"\r\n" + RUS.encode("cp1251") + b"\r\n")
    # 0x98：cp1251 未定义、cp1252 定义为 U+02DC ⇒ 落 windows-1252
    out["cp1251_undefined_then_1252.bin"] = b"abc\x98def"
    # 0x81：cp1252 未定义、cp1251 定义为 U+0403 ⇒ 落 windows-1251
    out["cp1252_undefined_then_1251.bin"] = b"abc\x81def"
    # 0x98 与 0x81 同时在：cp1251 挂、cp1252 也挂 ⇒ 落 latin-1
    out["latin1_fallback.bin"] = b"abc\x98\x81def"
    out["gb18030_two_byte.bin"] = CJK.encode("gb18030")
    out["gb18030_four_byte.bin"] = "\U0001F600\U00010000".encode("gb18030")  # SMP ⇒ 四字节
    out["gb18030_bmp_gap.bin"] = "\u0452".encode("gb18030")                  # BMP 空洞里的码位
    out["gb18030_lone_80.bin"] = b"abc\x80def"      # 0x80 在 gb18030 非法（Python 实测）
    out["utf8_truncated.bin"] = b"\xe4\xb8"          # 不是合法 UTF-8 ⇒ 会落到 gb18030
    out["cp1252_western.bin"] = "Café — naïve".encode("cp1252")
    return out


def real_files() -> list[Path]:
    out: list[Path] = []
    for root in REAL_ROOTS:
        if not root.is_dir():
            continue
        files = sorted(p for p in root.rglob("*") if p.is_file())[:PER_ROOT]
        out.extend(files)
    return out


def baseline_1x(path: Path) -> tuple[str, int, str]:
    """1.x `read_text_file` → (编码名, 码位数, 正文 UTF-8 的 sha256)。"""
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    from toolkit_textio import read_text_file  # noqa: PLC0415

    text, enc = read_text_file(str(path))
    data = text.encode("utf-8")
    return enc, len(text), hashlib.sha256(data).hexdigest()


def candidate_2x(path: Path, out_file: Path) -> tuple[str, int, str, str]:
    proc = subprocess.run([str(CLI), "--decode-text", str(path), str(out_file)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    if proc.returncode != 0:
        raise RuntimeError(text[:200])
    line = [ln for ln in text.splitlines() if ln.startswith("DECODE ok")]
    if not line:
        raise RuntimeError("找不到 DECODE ok 收尾行：%s" % text[:200])
    fields = dict(kv.split("=", 1) for kv in line[-1].split()[2:])
    data = out_file.read_bytes()
    return fields["enc"], int(fields["chars"]), hashlib.sha256(data).hexdigest(), line[-1]


def collect() -> list[tuple[str, Path]]:
    """→ [(id, 文件路径)]：真语料用 "real/<序号>_<名字>"，合成样本用 "synth/<名字>"。"""
    items: list[tuple[str, Path]] = []
    synth_dir = WORK / "synth"
    if synth_dir.exists():
        shutil.rmtree(synth_dir)
    synth_dir.mkdir(parents=True)
    for name, data in synth_samples().items():
        p = synth_dir / name
        p.write_bytes(data)
        items.append(("synth/%s" % name, p))
    for i, p in enumerate(real_files()):
        items.append(("real/%03d_%s" % (i, p.name), p))
    return items


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="文本解码层逐字节对拍")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线")
    parser.add_argument("--id", default="", help="只跑 id 含该子串的样本")
    args = parser.parse_args()

    if not CLI.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % CLI)
        print("GOLDEN-TEXT-DECODE: SKIP (cli-missing)")
        return 2

    WORK.mkdir(parents=True, exist_ok=True)
    items = [(i, p) for i, p in collect() if not args.id or args.id in i]
    if not items:
        print("!! 没有样本（真语料目录都不可达？）")
        print("GOLDEN-TEXT-DECODE: SKIP (no-samples)")
        return 2

    if args.write:
        records = []
        for sample_id, path in items:
            enc, chars, digest = baseline_1x(path)
            records.append({"id": sample_id, "path": str(path),
                            "size": path.stat().st_size,
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                            "enc": enc, "chars": chars, "text_sha256": digest})
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_bytes((json.dumps(
            {"note": "基线由 1.x toolkit_textio.read_text_file 生成", "entries": records},
            indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
        print("已写基线 %d 条 → %s" % (len(records), BASELINE.relative_to(ROOT)))
        print("GOLDEN-TEXT-DECODE: PASS (write)")
        return 0

    if not BASELINE.exists():
        print("!! 缺基线（先跑 --write）")
        print("GOLDEN-TEXT-DECODE: FAIL (no-baseline)")
        return 1

    base = {e["id"]: e for e in json.loads(BASELINE.read_text(encoding="utf-8"))["entries"]}
    failures: list[str] = []
    stale: list[str] = []
    compared = 0
    for sample_id, path in items:
        exp = base.get(sample_id)
        if exp is None:
            print("  !! 基线里没有 %s（夹具变了？重跑 --write）" % sample_id)
            failures.append(sample_id)
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() != exp["sha256"]:
            stale.append(sample_id)
            continue
        out_file = WORK / "decoded.txt"
        try:
            enc, chars, digest, line = candidate_2x(path, out_file)
        except Exception as exc:  # noqa: BLE001
            print("  ✗ %s：%s" % (sample_id, exc))
            failures.append(sample_id)
            continue
        if enc != exp["enc"] or chars != exp["chars"] or digest != exp["text_sha256"]:
            failures.append(sample_id)
            print("  ✗ %s" % sample_id)
            print("      基线 enc=%s chars=%d text=%s" %
                  (exp["enc"], exp["chars"], exp["text_sha256"][:16]))
            print("      候选 enc=%s chars=%d text=%s" %
                  (enc, chars, digest[:16]))
            print("      %s" % line)
        compared += 1

    print("")
    if stale:
        print("  样本内容变了（与基线记录不符）%d 个：%s%s"
              % (len(stale), ", ".join(stale[:5]), " …" if len(stale) > 5 else ""))
    verdict = "FAIL" if failures else ("PARTIAL" if stale else "PASS")
    print("GOLDEN-TEXT-DECODE: %s (compared=%d failed=%d stale=%d)"
          % (verdict, compared, len(failures), len(stale)))
    if failures:
        return 1
    return 2 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
