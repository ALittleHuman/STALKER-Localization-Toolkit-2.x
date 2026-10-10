#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本**判定链**（B 链，六步）黄金语料：2.x vs 1.x `toolkit_textio.sniff_encoding` + `decode_bytes`。

四层判据（很关键 —— 不许放宽，只许**显式改写**并写明为什么变）：

  ① **硬判据**：`(used_encoding, forced, 码位数, 正文 sha256)` 必须与 1.x 一致；
  ② **命名台账** `naming_divergences`：`(step, 胜出编码)` 允许与 1.x 不同，但只允许出现在
     台账里登记过的那些样本上（正文一致，只是步号/编码名换了写法）；
  ③ **能力缺口台账** `capability_gaps`：连正文都不一致的样本（GBK/Big5/Shift-JIS 这类要靠
     chardet 的家族，以及"声明了但我们没有解码器"的），逐条冻结我们的行为；
  ④ ★ **口径差异台账** `intent_divergences`（2026-10-06 新增）：**有意与 1.x 不同**的那一批 ——
     用户拍板的单字节家族置信度系统让"谎报 windows-1251 的西欧文件"不再被 1251 吃掉。
     台账同样冻结我们的行为：哪天差异消失（1.x 追上来 / 我们退回去）也会红。

另有一条**独立于 1.x 的硬判据**（`check_spa_corpus`）：用户那份 109 个西语 xml 的真语料，
断言"不再按 windows-1251 解"且"输出里不出现西里尔乱码"。

零静默自证（每次跑都验，不需要基线）：
  · `rc == 2` ⟺ `lowconf == 1`（低置信的退出码语义）；
  · `lowconf == 1` ⟹ `step == 6`（只有"纯猜"才是低置信）；
  · `overdecl == 1` ⟹ `step == 5` 且 `enc` 与声明不同（统计否决声明必须可见）；
  · `(enc != used)` ⟺ `mism == 1`（判定结论 ≠ 实际使用编码必须报出来）。

    python tests/golden_text_sniff.py --write
    python tests/golden_text_sniff.py [--id 子串] [--cli <xrkit_cli.exe>]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
BASELINE = HERE / "golden" / "text_sniff.json"
WORK = ROOT / "build" / "golden_text_sniff"
TOOLKIT_1X = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit")

REAL_ROOTS = [
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\gameplay"),
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\config\scripts"),
    Path(r"E:\Software\Games\STALKER\Localization\Gamedata\A.R.E.A\Original\configs\gameplay"),
    Path(r"E:\Software\Games\STALKER\Localization\Gamedata\A.R.E.A\Original\configs\scripts"),
    Path(r"E:\Software\Games\STALKER\Original\SoC_Source\S.T.A.L.K.E.R. Shadow of Chernobyl_RUS"),
    Path(r"E:\Software\Games\STALKER\Games\Golden Sphere\gamedata\scripts"),
]
PER_ROOT = 20

# ★ 用户那份西语真语料（109 个 xml，真值 cp1252，其中 65 个**谎报** windows-1251）。
#   它不是"1.x 对拍"样本（1.x 在这批上就是错的），所以走独立硬判据。
SPA_ROOT = Path(r"E:\Software\Games\STALKER\Games\STALKER Wind of Time KamteK Edition"
                r"\KamteK\KamteK Addonpack\configs\text\spa - 副本")

RUS = "Привет, сталкер!"
CJK = "你好，潜行者"

DECL = re.compile(rb'<\?xml[^>]*encoding\s*=\s*["\']([^"\']+)["\']', re.I | re.S)

SINGLE_BYTE_FAMILY = ("windows-1251", "windows-1252", "latin-1", "iso-8859-1", "ascii")

# ★ 合成样本里"**真编码**是多字节家族"的那些：它们的正文差异永远是**能力缺口**
#   （我们没移植 chardet / 没有那张解码器表），**不是**本次口径造成的 ——
#   哪怕最终落点是 windows-1252（那只是"统计在一个它管不着的家族上顺手给了个答案"）。
#   判据必须按**真值**分，不能按"谁最后写了这个字节"分。
SYNTH_TRUE_MULTIBYTE = {
    "bom_utf16le", "bom_utf16be", "bom_utf32le", "utf16le_nobom", "utf16be_nobom",
    "utf16le_odd", "utf8_cjk", "gbk_cjk", "gb18030_4byte", "sjis", "big5",
    "decl_utf8", "decl_lies_utf8", "decl_gb2312", "ovr_1251_on_utf8",
}


def synth_samples() -> dict[str, tuple[bytes, str]]:
    """→ {名字: (字节, 手动指定编码)}（手动指定为空表示 auto）"""
    s: dict[str, tuple[bytes, str]] = {}
    s["bom_utf8"] = (b"\xef\xbb\xbf" + RUS.encode("utf-8"), "")
    s["bom_utf16le"] = (RUS.encode("utf-16"), "")
    s["bom_utf16be"] = (b"\xfe\xff" + RUS.encode("utf-16-be"), "")
    s["bom_utf32le"] = (RUS.encode("utf-32"), "")
    s["utf16le_nobom"] = (RUS.encode("utf-16-le"), "")
    s["utf16be_nobom"] = (RUS.encode("utf-16-be"), "")
    s["utf16le_odd"] = (RUS.encode("utf-16-le")[:-1], "")
    s["utf8_cjk"] = ((RUS + CJK).encode("utf-8"), "")
    s["ascii"] = (b"<config>hello</config>", "")
    s["ascii_tab_crlf"] = (b"a\tb\r\nc\n", "")
    s["ascii_nul"] = (b"abc\x00def", "")
    s["ascii_ctrl"] = (b"abc\x01\x1bdef", "")
    s["ascii_del"] = (b"abc\x7fdef", "")
    s["ascii_formfeed"] = (b"abc\x0cdef", "")
    s["empty"] = (b"", "")
    s["cp1251"] = (RUS.encode("cp1251"), "")
    s["cp1251_short"] = ("Я".encode("cp1251"), "")
    s["cp1252"] = ("Café — naïve".encode("cp1252"), "")
    s["koi8r"] = (RUS.encode("koi8-r"), "")
    s["gbk_cjk"] = (CJK.encode("gbk"), "")
    s["gb18030_4byte"] = ("\U0001F600".encode("gb18030"), "")
    s["sjis"] = ("こんにちは世界".encode("shift_jis"), "")
    s["big5"] = ("你好潛行者".encode("big5"), "")
    s["latin1_bytes"] = (b"abc\x98\x81def", "")
    s["decl_1251"] = (('<?xml version="1.0" encoding="windows-1251"?><c>' + RUS + "</c>")
                      .encode("cp1251"), "")
    s["decl_utf8"] = (('<?xml version="1.0" encoding="utf-8"?><c>' + RUS + "</c>")
                      .encode("utf-8"), "")
    s["decl_lies_utf8"] = (('<?xml version="1.0" encoding="utf-8"?><c>' + RUS + "</c>")
                           .encode("cp1251"), "")
    s["decl_case_ws"] = (b'<?xml version="1.0" ENCODING = \'windows-1251\' ?><c>'
                         + RUS.encode("cp1251") + b"</c>", "")
    s["decl_gb2312"] = (('<?xml version="1.0" encoding="gb2312"?><c>' + CJK + "</c>")
                        .encode("gb2312"), "")
    s["decl_then_body_nl"] = (b'<?xml version="1.0" encoding="windows-1251"?>\r\n<c>'
                              + RUS.encode("cp1251") + b"</c>\r\n", "")
    # ── ★ 2026-10-06 用户口径的边界夹具（单字节家族置信度系统）────────────────
    # 谎报 windows-1251 的西语（真值 cp1252）：新链必须**否决声明**并报出来
    s["decl_lies_1252"] = (b'<?xml version="1.0" encoding="windows-1251" ?><c>'
                           + "Lanzagranadas bajo el cañón ¡Quita el silenciador!".encode("cp1252")
                           + b"</c>", "")
    # 诚实声明 windows-1251 + 真西里尔字节（**反向锁**：不许被统计带偏）
    s["decl_honest_1251"] = (b'<?xml version="1.0" encoding="windows-1251" ?><c>' + RUS.encode("cp1251")
                             + "Опасность рядом, будь осторожен, сталкер.".encode("cp1251") + b"</c>", "")
    # 第 3 步（手动指定）—— 锁
    s["ovr_1251_on_utf8"] = (RUS.encode("utf-8"), "windows-1251")
    s["ovr_utf8_on_1251"] = (RUS.encode("cp1251"), "utf-8")
    s["ovr_auto_on_cp1251"] = (RUS.encode("cp1251"), "auto")
    return s


def chain_step_1x(raw: bytes, override: str = "auto") -> tuple[int, str]:
    """用 1.x 的原语按**它的顺序**算胜出步（它的函数本身不返回步号）。

    ★ 顺序别搞错：第 2 步（多字节检查）排在第 3 步（手动指定）之前 ——
      给 UTF-8 字节手动指定 windows-1251，胜出的仍然是"多字节检查"。
    """
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    import toolkit_textio as T  # noqa: PLC0415

    bom = T.bom_encoding(raw)
    if bom:
        return 1, bom
    if T.is_multibyte_utf8(raw):
        return 2, "utf-8"
    o = (override or "").strip().lower()
    if o and o != "auto":
        return 3, T.ENCODING_ALIASES.get(o, o)
    m = DECL.search(raw[:500])
    if m:
        decl = m.group(1).decode("ascii", "replace").strip().lower()
        cand = T.ENCODING_ALIASES.get(decl, decl) if decl else None
        if T.bytes_fit_encoding(raw, cand):
            return 4, cand
    guess, conf = T._chardet_guess(raw)
    if guess and guess not in T._CYRILLIC_ALTS:
        if conf >= T._CHARDET_MIN_CONF or not T._is_single_byte_codec(guess):
            return 5, guess
    return 6, "windows-1251"


MULTIBYTE_FAMILIES = ("gbk", "gb18030", "gb2312", "big5", "cp932", "shift", "sjis",
                      "utf-16", "utf-32", "utf-7", "euc", "iso-2022")


def classify_divergence(ref: dict) -> str:
    """给"正文都不一致"的差异定性（写进台账，供门禁与文档引用）。"""
    enc = (ref["enc"] or "").lower()
    if ref["step"] == 4:
        # 声明被字节证实、但我们没有那个解码器（如 gb2312）
        return "unsupported-declaration"
    if any(enc.startswith(p) or p in enc for p in MULTIBYTE_FAMILIES):
        # 1.x 用 chardet 认出多字节家族，我们没移植 chardet
        return "chardet-family"
    # 其余都是 1.x 的单字节误判（实测：把 cp1251 俄文判成 cp1250）
    return "misdetect-single-byte"


def is_intent_divergence(ref: dict, ours: dict) -> bool:
    """这条差异是不是**本次口径**（2026-10-06 单字节家族置信度系统）有意造成的。

    判据只看**参照侧**的落点：1.x 落在单字节家族里（1251/1252/latin-1/ascii）而我们的
    统计给出另一个单字节落点 ⇒ 口径差异。1.x 落在别的家族（cp1250 的 chardet 误判、
    gbk/big5/utf-16 的多字节家族）⇒ 那属于既有的两类台账，不是本次口径。
    """
    renc = (ref["enc"] or "").lower()
    if renc not in SINGLE_BYTE_FAMILY:
        return False
    if ours.get("overdecl") == 1:
        return True
    return ours.get("step") == 5 and ours.get("statn", 0) >= 2


def ref_side(path: Path, override: str) -> dict:
    if str(TOOLKIT_1X) not in sys.path:
        sys.path.insert(0, str(TOOLKIT_1X))
    import toolkit_textio as T  # noqa: PLC0415

    raw = path.read_bytes()
    step, enc = chain_step_1x(raw, override)
    text, used, forced = T.decode_bytes(raw, override or None)
    data = text.encode("utf-8")
    return {"step": step, "enc": enc, "used": used, "forced": bool(forced),
            "chars": len(text), "text_sha256": hashlib.sha256(data).hexdigest()}


def our_side(cli: Path, path: Path, override: str, out_file: Path) -> dict:
    # ★ P17-31（04 F-06）：本步在**同一 out_file** 上反复写（各用例复用）⇒ 必须显式 `--force`
    #   （守卫口径：输出已存在即拒，除非 --force；这里语义本来就是"要覆盖"）。
    argv = [str(cli), "--sniff", str(path), str(out_file), "--force"]
    if override and override != "auto":
        argv += ["--encoding", override]
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = proc.stdout.decode("utf-8", errors="replace").strip()
    # ★ rc=2 = "判定完成但**低置信**"（零静默的退出码语义），不再等于失败。
    if proc.returncode not in (0, 2):
        raise RuntimeError("rc=%d %s" % (proc.returncode, text[:200]))
    line = [ln for ln in text.splitlines() if ln.startswith("SNIFF ok")]
    if not line:
        raise RuntimeError("找不到 SNIFF ok 收尾行：%s" % text[:200])
    f = dict(kv.split("=", 1) for kv in line[-1].split()[2:])
    data = out_file.read_bytes()
    return {"step": int(f["step"]), "enc": f["enc"], "used": f["used"],
            "forced": f["forced"] == "1", "chars": int(f["chars"]),
            "text_sha256": hashlib.sha256(data).hexdigest(),
            "rc": proc.returncode, "lowconf": int(f.get("lowconf", 0)),
            "overdecl": int(f.get("overdecl", 0)), "statn": int(f.get("statn", 0)),
            "mism": int(f.get("mism", 0)), "conf": f.get("conf", "")}


def collect() -> list[tuple[str, Path, str]]:
    out: list[tuple[str, Path, str]] = []
    synth = WORK / "synth"
    if synth.exists():
        shutil.rmtree(synth)
    synth.mkdir(parents=True)
    for name, (data, override) in synth_samples().items():
        p = synth / name
        p.write_bytes(data)
        out.append(("synth/%s" % name, p, override))
    for root in REAL_ROOTS:
        if not root.is_dir():
            continue
        tag = root.parent.name + "-" + root.name
        for i, p in enumerate(sorted(x for x in root.rglob("*") if x.is_file())[:PER_ROOT]):
            out.append(("real/%s/%03d" % (tag, i), p, ""))
    return out


def zero_silence_checks(sample_id: str, ours: dict) -> list[str]:
    """每次跑都验的"零静默"自证（不需要基线）。"""
    bad: list[str] = []
    if (ours["rc"] == 2) != (ours["lowconf"] == 1):
        bad.append("rc=%d 与 lowconf=%d 不自洽（2 ⟺ 低置信）" % (ours["rc"], ours["lowconf"]))
    if ours["lowconf"] == 1 and ours["step"] != 6:
        bad.append("lowconf=1 但 step=%d（只有第 6 步'纯猜'才允许低置信）" % ours["step"])
    if ours["overdecl"] == 1 and ours["step"] != 5:
        bad.append("overdecl=1 但 step=%d（否决声明必须落在第 5 步）" % ours["step"])
    if (ours["enc"] != ours["used"]) != (ours["mism"] == 1):
        bad.append("mism=%d 与 enc=%s/used=%s 不自洽" % (ours["mism"], ours["enc"], ours["used"]))
    if ours["forced"] == 1 and ours["used"] != "latin-1":
        bad.append("forced=1 但 used=%s（forced 只表示落到 latin-1 保底）" % ours["used"])
    return bad


def check_spa_corpus(cli: Path, out_file: Path, failures: list[str]) -> int:
    """★ 独立硬判据：用户那份 109 个西语 xml —— 不再按 windows-1251 解、不出西里尔乱码。

    返回"按 windows-1251 解"的文件数（-1 表示夹具不可达）。
    """
    if not SPA_ROOT.is_dir():
        return -1
    files = sorted(SPA_ROOT.glob("*.xml"))
    if not files:
        return -1
    as_1251: list[tuple[str, int, str]] = []
    cyr = 0
    for p in files:
        raw = p.read_bytes()
        n_hi = sum(1 for b in raw if b >= 0x80)
        ours = our_side(cli, p, "", out_file)
        text = out_file.read_bytes().decode("utf-8", "replace")
        if any("\u0400" <= c <= "\u04FF" for c in text):
            cyr += 1
        if ours["used"] == "windows-1251" and n_hi > 0:
            m = DECL.search(raw[:500])
            as_1251.append((p.name, n_hi, m.group(1).decode("ascii", "replace") if m else ""))
    print("  西语真语料 %d 个：按 windows-1251 解 %d 个；输出含西里尔 %d 个"
          % (len(files), len(as_1251), cyr))
    for name, n_hi, decl in as_1251:
        print("     仍按 1251：%s（高位字节 %d，声明 %s）" % (name, n_hi, decl))
    # ① 只有**证据不足（<2 个高位字节）**的文件才允许按声明退回 windows-1251
    for name, n_hi, decl in as_1251:
        if n_hi >= 2:
            failures.append("spa/%s：高位字节 %d 却仍按 windows-1251 解（统计该有决定性）"
                            % (name, n_hi))
        if decl.strip().lower() not in ("windows-1251", "windows-1252", "latin-1", "iso-8859-1"):
            failures.append("spa/%s：退回 windows-1251 但声明是 %s（理由不成立）" % (name, decl))
    # ② 含高位字节、且被 1251 吃掉的文件数必须 <= 1（实测就是 1 个单字节证据的 gunsl_names.xml）
    if len(as_1251) > 1:
        failures.append("spa：按 windows-1251 解的文件有 %d 个（口径要求只有'证据不足'的极少数）"
                        % len(as_1251))
    # ③ 输出里不许出现西里尔乱码（唯一例外是上一条那个单字节文件）
    if cyr != len(as_1251):
        failures.append("spa：输出含西里尔的文件 %d 个，与'按 1251 解'的 %d 个不一致"
                        % (cyr, len(as_1251)))
    # ④ 诚实声明 windows-1252 的必须仍然正确
    honest = 0
    for p in files:
        raw = p.read_bytes()
        m = DECL.search(raw[:500])
        decl = m.group(1).decode("ascii", "replace").strip().lower() if m else ""
        if decl != "windows-1252":
            continue
        ours = our_side(cli, p, "", out_file)
        honest += 1
        if ours["enc"] != "windows-1252" or ours["lowconf"] == 1:
            failures.append("spa/%s：诚实声明 windows-1252 却没走第 4 步（实得 %s step=%d）"
                            % (p.name, ours["enc"], ours["step"]))
    print("     诚实声明 windows-1252 的 %d 个：仍全部走 windows-1252" % honest)
    # ⑤ 用户点名的那个文件：`cañon` 的 ñ 必须是 U+00F1，且不许出现西里尔
    key = SPA_ROOT / "gunsl_keys.xml"
    if key.is_file():
        ours = our_side(cli, key, "", out_file)
        text = out_file.read_bytes().decode("utf-8", "replace")
        ok_n = "ca\xf1on" in text
        ok_c = not any("\u0400" <= c <= "\u04FF" for c in text)
        if not ok_n:
            failures.append("spa/gunsl_keys.xml：正文里找不到 `cañon`（ñ 必须是 U+00F1）")
        if not ok_c:
            failures.append("spa/gunsl_keys.xml：输出里有西里尔乱码")
        if ours["enc"] != "windows-1252" or ours["overdecl"] != 1:
            failures.append("spa/gunsl_keys.xml：必须由统计**否决** windows-1251 声明"
                            "（实得 enc=%s overdecl=%d）" % (ours["enc"], ours["overdecl"]))
        print("     gunsl_keys.xml：enc=%s overdecl=%d `cañon`=%s 无西里尔=%s"
              % (ours["enc"], ours["overdecl"], ok_n, ok_c))
    return len(as_1251)


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    parser = argparse.ArgumentParser(description="文本判定链逐字节/逐字段对拍")
    parser.add_argument("--write", action="store_true", help="用 1.x 生成基线（并登记台账）")
    parser.add_argument("--id", default="", help="只跑 id 含该子串的样本")
    parser.add_argument("--cli", default="", help="指定 xrkit_cli.exe（默认 release 构建）")
    parser.add_argument("--no-spa", action="store_true", help="跳过西语真语料判据（调试用）")
    args = parser.parse_args()

    cli = Path(args.cli).resolve() if args.cli else CLI
    if not cli.exists():
        print("!! 找不到 %s（先跑 python tests/run_ci.py --only build）" % cli)
        print("GOLDEN-TEXT-SNIFF: SKIP (cli-missing)")
        return 2

    WORK.mkdir(parents=True, exist_ok=True)
    items = [(i, p, o) for i, p, o in collect() if not args.id or args.id in i]
    if not items:
        print("!! 没有样本")
        print("GOLDEN-TEXT-SNIFF: SKIP (no-samples)")
        return 2

    out_file = WORK / "decoded.txt"
    if args.write and args.id:
        # ★ `--write` 是**整体重写**基线（不含"合并"语义）：带 `--id` 跑会把基线截成一条，
        #   其余样本的台账全丢（2026-09-24 实测踩到，靠 git 恢复）。要么全量写，要么只跑不写。
        print("!! 禁止 `--write --id` 组合：`--write` 会整体重写基线，带 --id 会把其余样本抹掉。")
        print("GOLDEN-TEXT-SNIFF: FAIL (write-with-id)")
        return 1
    if args.write:
        entries = []
        naming: dict[str, dict] = {}
        gaps: dict[str, dict] = {}
        intent: dict[str, dict] = {}
        for sample_id, path, override in items:
            ref = ref_side(path, override)
            ours = our_side(cli, path, override, out_file)
            entry = {"id": sample_id, "path": str(path), "override": override,
                     "size": path.stat().st_size,
                     "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "ref": ref, "ours": ours}
            entries.append(entry)
            same_text = (ref["text_sha256"] == ours["text_sha256"]
                         and ref["used"] == ours["used"]
                         and ref["forced"] == ours["forced"]
                         and ref["chars"] == ours["chars"])
            frozen = [ours["step"], ours["enc"], ours["used"], ours["overdecl"], ours["lowconf"]]
            if not same_text:
                short = sample_id.split("/", 1)[1] if sample_id.startswith("synth/") else ""
                if short in SYNTH_TRUE_MULTIBYTE:
                    gaps[sample_id] = {
                        "kind": "chardet-family",
                        "note": ("真编码是多字节家族（见 SYNTH_TRUE_MULTIBYTE）——我们没移植 chardet；"
                                 "2026-10-06 的口径只改单字节家族，这条仍如实冻结我们的落点"),
                        "ref": [ref["step"], ref["enc"], ref["used"]],
                        "ours": frozen}
                elif is_intent_divergence(ref, ours):
                    intent[sample_id] = {
                        "kind": "single-byte-statistic",
                        "note": ("★ 本次口径（2026-10-06）**有意与 1.x 不同**：声明与'鲁棒'一起进"
                                 "置信度系统 ⇒ 谎报 windows-1251 的西欧文件被统计否决。"
                                 "用户原话见 docs\\2.0.0-行为口径对照表.md ⑨9.8/9.9。"),
                        "ref": [ref["step"], ref["enc"], ref["used"]],
                        "ours": frozen}
                else:
                    kind = classify_divergence(ref)
                    note = {
                        "chardet-family": "1.x 用 chardet 认出多字节家族；我们没移植 chardet（真能力缺口）",
                        "unsupported-declaration": "声明被字节证实，但我们没有该编码的解码器（真能力缺口）",
                        "misdetect-single-byte": ("1.x 的单字节误判（实测：把 cp1251 俄文判成 cp1250；"
                                                  "文件自己写着 coding: windows-1251-dos）——"
                                                  "我们**故意不复刻**"),
                    }[kind]
                    gaps[sample_id] = {"kind": kind, "note": note,
                                       "ref": [ref["step"], ref["enc"], ref["used"]],
                                       "ours": frozen}
            elif (ref["step"], ref["enc"]) != (ours["step"], ours["enc"]):
                naming[sample_id] = {"ref": [ref["step"], ref["enc"]],
                                     "ours": [ours["step"], ours["enc"]]}
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_bytes((json.dumps({
            "note": "基线由 1.x toolkit_textio（sniff_encoding + decode_bytes）生成；"
                    "naming_divergences = 只差(步号,编码名)、正文一致；"
                    "capability_gaps = 正文也不同的已知能力缺口（冻结我们的行为）；"
                    "intent_divergences = ★2026-10-06 口径**有意**造成的正文差异（同样冻结）",
            "entries": entries,
            "naming_divergences": naming,
            "capability_gaps": gaps,
            "intent_divergences": intent,
        }, indent=1, ensure_ascii=False) + "\n").encode("utf-8"))
        print("已写基线 %d 条 → %s" % (len(entries), BASELINE.relative_to(ROOT)))
        print("  正文一致但(步号,编码名)不同：%d 个 → %s"
              % (len(naming), ", ".join(sorted(naming)[:6])))
        kinds: dict[str, list[str]] = {}
        for sid, item in gaps.items():
            kinds.setdefault(item["kind"], []).append(sid)
        print("  正文也不同（能力缺口台账）：%d 个" % len(gaps))
        for kind, ids in sorted(kinds.items()):
            print("    %-24s %3d 个 → %s" % (kind, len(ids), ", ".join(sorted(ids)[:5])))
        print("  正文也不同（★口径差异台账，有意与 1.x 不同）：%d 个 → %s"
              % (len(intent), ", ".join(sorted(intent)[:8])))
        print("GOLDEN-TEXT-SNIFF: PASS (write)")
        return 0

    if not BASELINE.exists():
        print("!! 缺基线（先跑 --write）")
        print("GOLDEN-TEXT-SNIFF: FAIL (no-baseline)")
        return 1

    blob = json.loads(BASELINE.read_text(encoding="utf-8"))
    base = {e["id"]: e for e in blob["entries"]}
    naming = blob.get("naming_divergences", {})
    gaps = blob.get("capability_gaps", {})
    intent = blob.get("intent_divergences", {})
    failures: list[str] = []
    stale: list[str] = []
    compared = 0
    for sample_id, path, override in items:
        exp = base.get(sample_id)
        if exp is None:
            print("  !! 基线里没有 %s（夹具变了？重跑 --write）" % sample_id)
            failures.append(sample_id)
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != exp["sha256"]:
            stale.append(sample_id)
            continue
        try:
            ours = our_side(cli, path, override, out_file)
        except Exception as exc:  # noqa: BLE001
            print("  ✗ %s：%s" % (sample_id, exc))
            failures.append(sample_id)
            continue
        compared += 1
        for problem in zero_silence_checks(sample_id, ours):
            print("  ✗ %s：零静默自证失败 —— %s" % (sample_id, problem))
            failures.append(sample_id)
        ref = exp["ref"]
        in_gap = sample_id in gaps
        in_intent = sample_id in intent
        same_text = (ref["text_sha256"] == ours["text_sha256"]
                     and ref["used"] == ours["used"]
                     and ref["forced"] == ours["forced"]
                     and ref["chars"] == ours["chars"])
        same_name = (ref["step"], ref["enc"]) == (ours["step"], ours["enc"])
        frozen = [ours["step"], ours["enc"], ours["used"], ours["overdecl"], ours["lowconf"]]

        # ④ 口径差异台账：**有意**不同 ⇒ 必须仍不同，且我们的行为与台账一致
        if in_intent:
            want = intent[sample_id]["ours"]
            if same_text:
                failures.append(sample_id)
                print("  !! %s：这条★口径差异已消失（正文与 1.x 一致了）——"
                      "请更新 docs ⑨9.8/9.9 与本台账" % sample_id)
            elif frozen != want:
                failures.append(sample_id)
                print("  ✗ %s：口径差异行为变了（台账 %s；实得 %s）" % (sample_id, want, frozen))
            continue
        # ③ 能力缺口台账：登记过的缺口必须**仍然**表现为缺口，且我们的行为与台账一致
        if in_gap:
            want = gaps[sample_id]["ours"]
            if same_text:
                failures.append(sample_id)
                print("  !! %s：这条缺口似乎已被补齐（正文已与基线一致）——"
                      "请更新规格文档 §5 与本台账" % sample_id)
                continue
            if frozen != want:
                failures.append(sample_id)
                print("  ✗ %s：缺口行为变了（台账 %s；实得 %s）" % (sample_id, want, frozen))
            continue
        # ① 硬判据：不在任何台账里 ⇒ 解码结果必须一致
        if not same_text:
            failures.append(sample_id)
            print("  ✗ %s：解码结果与基线不一致（基线 used=%s chars=%d text=%s；"
                  "候选 used=%s chars=%d text=%s）"
                  % (sample_id, ref["used"], ref["chars"], ref["text_sha256"][:12],
                     ours["used"], ours["chars"], ours["text_sha256"][:12]))
            continue
        # ② 命名台账：不在这两个台账里 ⇒ (step, enc) 也必须一致
        if sample_id in naming:
            want = naming[sample_id]["ours"]
            if same_name:
                failures.append(sample_id)
                print("  !! %s：命名差异已消失（%s）——请更新台账" % (sample_id, want))
            elif [ours["step"], ours["enc"]] != want:
                failures.append(sample_id)
                print("  ✗ %s：命名差异变了（台账 %s；实得 %s）"
                      % (sample_id, want, [ours["step"], ours["enc"]]))
        elif not same_name:
            failures.append(sample_id)
            print("  ✗ %s：(step,enc) 与基线不一致且**未登记**：基线 (%d,%s) / 候选 (%d,%s)"
                  % (sample_id, ref["step"], ref["enc"], ours["step"], ours["enc"]))

    # ★ 独立于 1.x 的硬判据（用户那份西语真语料）
    n_1251 = -1
    if not args.no_spa:
        n_1251 = check_spa_corpus(cli, out_file, failures)
        if n_1251 < 0:
            print("  !! 西语真语料不可达：%s" % SPA_ROOT)

    print("")
    if stale:
        print("  样本内容变了 %d 个：%s" % (len(stale), ", ".join(stale[:5])))
    print("  台账：命名差异 %d 条 / 能力缺口 %d 条 / ★口径差异 %d 条"
          % (len(naming), len(gaps), len(intent)))
    verdict = "FAIL" if failures else ("PARTIAL" if stale else "PASS")
    print("GOLDEN-TEXT-SNIFF: %s (compared=%d failed=%d stale=%d spa_as_1251=%d)"
          % (verdict, compared, len(failures), len(stale), n_1251))
    if failures:
        return 1
    return 2 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
