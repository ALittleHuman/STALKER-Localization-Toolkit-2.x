#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 xrkit::engine::text 的**单字节家族统计剖面**（sniff_profiles.inc）。

用途：判定链（B 链）第 4/5 步的"鲁棒"证据 —— 用户 2026-10-06 口径：
    「鲁棒和声明实际上地位差不多，实际上更加准确的是把声明读出来的也纳入置信度的系统。」
    「字节判断和手动设置是死板的……后面的两个才是不确定因素，也就是一般意义上的置信度系统。」
    「声明的优先级，仍然是比鲁棒稍微高一点的，因为大多数声明是正确的，鲁棒通常来说更加不稳定。」

模型：两条单字节假设
    H_ru = **俄语文本 / windows-1251**
    H_we = **西欧文本 / windows-1252**
对每个高位字节 b（>=0x80）算条件概率 Q_H(b) = 该假设下"高位字节等于 b"的概率，
再压成一维权重  w[b] = log10( Q_ru(b) / Q_we(b) )。
运行期只需把 w[b] 累加再除以高位字节数 ⇒ 归一化对数似然比：
    正 = 像俄语（windows-1251）  负 = 像西欧（windows-1252/latin-1）

Q 从**已发表的字频/标点频**经 Python 的 cp1251 / cp1252 codec 映射而来
（不是从任何被测语料里学出来的 —— 语料只用于验收，见 tests/golden_text_sniff.py）。

用法：
    python tools/gen_sniff_profiles.py            # 生成/刷新
    python tools/gen_sniff_profiles.py --check     # 只校验盘上的表与数据一致（门禁用）
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "engine" / "text" / "sniff_profiles.inc"

# ── 已发表字频（每 100 个字母）──────────────────────────────────────────
# 俄语字频表（о е а и н т с р в л … 起头，标准的 33 字母表）
RU_LETTERS = {
    "о": 10.97, "е": 8.45, "а": 8.01, "и": 7.35, "н": 6.70, "т": 6.26, "с": 5.47,
    "р": 4.73, "в": 4.54, "л": 4.40, "к": 3.49, "м": 3.21, "д": 2.98, "п": 2.81,
    "у": 2.62, "я": 2.01, "ы": 1.90, "ь": 1.74, "г": 1.70, "з": 1.65, "б": 1.59,
    "ч": 1.44, "й": 1.21, "х": 0.97, "ж": 0.94, "ш": 0.73, "ю": 0.64, "ц": 0.48,
    "щ": 0.36, "э": 0.32, "ф": 0.26, "ъ": 0.04, "ё": 0.04,
}
# 俄语排版里会进"高位字节"的标点（« » № — – … NBSP „ “ ” •）
RU_PUNCT = {"«": 0.10, "»": 0.10, "№": 0.02, "—": 0.05, "–": 0.02, "…": 0.02,
            "\u00a0": 0.05, "„": 0.03, "“": 0.03, "”": 0.03, "•": 0.01}
# cp1251 里存在、但俄语里极罕的字母（乌克兰/塞尔维亚/马其顿）——给一个地板值，
# 否则它们会拿到 Q_ru=0 而让"乌克兰语文件"被硬推给 cp1252。
RU_RARE = "ЂЃЄЅІЇЈЉЊЌЋЏђѓєѕіїјљњќћџЎў"
RU_RARE_FREQ = 0.0005
RU_CAPS_SHARE = 0.04  # 大写约占同名小写的 4%

# 西欧多语合成（英/西/法/德/意/葡，六语等权）——只列 cp1252 的**高位**字母
WE_ACCENT = {
    "à": 0.20, "á": 0.11, "â": 0.025, "ã": 0.117, "ä": 0.09, "å": 0.005, "æ": 0.002,
    "ç": 0.098, "è": 0.122, "é": 0.705, "ê": 0.10, "ë": 0.003, "ì": 0.017, "í": 0.153,
    "î": 0.008, "ï": 0.003, "ñ": 0.053, "ò": 0.025, "ó": 0.285, "ô": 0.022, "õ": 0.033,
    "ö": 0.052, "ø": 0.005, "ù": 0.052, "ú": 0.057, "û": 0.003, "ü": 0.117, "ý": 0.002,
    "þ": 0.001, "ÿ": 0.002, "ß": 0.052, "œ": 0.003, "ð": 0.001,
    "À": 0.010, "Á": 0.006, "Â": 0.002, "Ã": 0.006, "Ä": 0.005, "Å": 0.001, "Æ": 0.001,
    "Ç": 0.005, "È": 0.006, "É": 0.035, "Ê": 0.005, "Ë": 0.001, "Ì": 0.001, "Í": 0.008,
    "Î": 0.001, "Ï": 0.001, "Ñ": 0.003, "Ò": 0.001, "Ó": 0.014, "Ô": 0.001,
    "Õ": 0.002, "Ö": 0.003, "Ø": 0.001, "Ù": 0.003, "Ú": 0.003, "Û": 0.001,
    "Ü": 0.006, "Ý": 0.001, "Þ": 0.001, "Œ": 0.001, "Š": 0.002, "š": 0.002,
    "Ž": 0.002, "ž": 0.002, "Ÿ": 0.001, "ƒ": 0.001,
}
# 西欧排版里的高位标点
WE_PUNCT = {"«": 0.02, "»": 0.02, "¡": 0.005, "¿": 0.005, "–": 0.05, "—": 0.03,
            "“": 0.05, "”": 0.05, "‘": 0.02, "’": 0.02, "…": 0.01, "•": 0.005,
            "€": 0.01, "™": 0.003, "\u00a0": 0.02, "°": 0.005, "©": 0.003, "®": 0.002,
            "§": 0.005, "¶": 0.002, "·": 0.005, "´": 0.002, "¨": 0.002, "µ": 0.005,
            "²": 0.003, "³": 0.002, "¹": 0.002, "¼": 0.002, "½": 0.004, "¾": 0.001,
            "×": 0.003, "÷": 0.002, "¢": 0.002, "£": 0.005, "¥": 0.002, "¤": 0.001,
            "¦": 0.001, "¯": 0.001, "¸": 0.001, "ª": 0.01, "º": 0.01, "ˆ": 0.002,
            "˜": 0.001, "‰": 0.001, "‹": 0.001, "›": 0.001, "†": 0.002, "‡": 0.001}

# "该编码里有这个字符、但我们的字频表没量到它" 时的**条件概率地板**。
# ★ 为什么必须是条件概率（归一化之后）而不是字频：
#   俄语表的字频总量约 102（高位字节**就是**正文），西欧表约 2.7（高位字节只占百分之几）
#   —— 同一个"字频地板"落到两张表上会差 40 倍，于是"西欧标点"被算得比"俄语罕用字母"
#   常见得多，一份**损坏的 UTF-8 俄文**就会被推去 windows-1252（实测踩到过）。
#   地板取 1e-4：低于两张表里最小的已量项（俄语 ъ=3.9e-4、西欧 ¸=3.7e-4）。
FLOOR_Q = 1e-4


def ru_freq() -> dict[str, float]:
    f = dict(RU_LETTERS)
    for ch, v in RU_LETTERS.items():
        f[ch.upper()] = v * RU_CAPS_SHARE
    f.update(RU_PUNCT)
    for ch in RU_RARE:
        f.setdefault(ch, RU_RARE_FREQ)
    return f


def we_freq() -> dict[str, float]:
    f = dict(WE_ACCENT)
    f.update(WE_PUNCT)
    return f


def hi_profile(freq: dict[str, float], codec: str) -> dict[int, float]:
    """字符频率 → **高位字节**（>=0x80）条件分布 Q(b)（只用已量到的字符）。"""
    out: dict[int, float] = {}
    for ch, v in freq.items():
        try:
            b = ch.encode(codec)
        except Exception:  # noqa: BLE001
            continue
        if len(b) != 1 or b[0] < 0x80:
            continue
        out[b[0]] = out.get(b[0], 0.0) + v
    total = sum(out.values())
    if total <= 0:
        raise SystemExit("!! 剖面为空：%s" % codec)
    return {k: v / total for k, v in out.items()}


def defined_bytes(codec: str) -> set[int]:
    """该编码里 0x80..0xFF 有定义的字节（cp1251 缺 0x98；cp1252 缺 0x81/8D/8F/90/9D）。"""
    out = set()
    for b in range(0x80, 0x100):
        try:
            bytes([b]).decode(codec)
        except Exception:  # noqa: BLE001
            continue
        out.add(b)
    return out


DEF_RU = defined_bytes("cp1251")
DEF_WE = defined_bytes("cp1252")


def weights() -> list[float]:
    import math
    q_ru = hi_profile(ru_freq(), "cp1251")
    q_we = hi_profile(we_freq(), "cp1252")
    out = [0.0] * 256
    for b in range(0x80, 0x100):
        # ★ 两条假设里**任一边没有这个字节的定义** ⇒ 该字节的"语言证据"记 0（中性）。
        #   反例教训（2026-10-06 实测）：cp1252 的 0x81/0x8D/0x8F/0x90/0x9D 是未定义字节，
        #   而 cp1251 把它们映射成 Ѓ/Ќ/Џ/ђ/ћ。若给 Q_we 用 1e-6 地板，这 5 个字节会各自
        #   贡献 +0.68 的**强俄语证据** —— 一份"西语文本 + 几个解不出的字节"就会被推成俄语。
        #   正确的读法是："cp1252 解不出这个字节"说明**它不是干净的 cp1252**，并不说明它是俄语
        #   （真正的落点是 latin-1，那由调用方按"家族内宽松写法"处理）。
        if b not in DEF_RU or b not in DEF_WE:
            continue
        a = max(q_ru.get(b, 0.0), FLOOR_Q)
        c = max(q_we.get(b, 0.0), FLOOR_Q)
        out[b] = math.log10(a / c)
    return out


def render(w: list[float]) -> str:
    lines = [
        "// 本文件由 tools/gen_sniff_profiles.py 生成，请勿手改。\n",
        "// kSniffByteWeight[b]（仅 b >= 0x80 非零）= log10( Q_ru(b) / Q_we(b) )：\n",
        "//   Q_ru = 俄语文本按 windows-1251 编码时、高位字节等于 b 的概率\n",
        "//   Q_we = 西欧文本按 windows-1252 编码时、高位字节等于 b 的概率\n",
        "// 正 = 像俄语（windows-1251），负 = 像西欧（windows-1252）。**不做缩放**，\n",
        "// 所以：每个高位字节的平均值就是可解释的对数似然比（见 sniff.cpp 的门槛）。\n",
        "// ★ 任一边**没有该字节定义**的位置记 0（中性）—— 见生成器里的反例说明。\n",
        "// 数据来源 = 已发表的字频/排版标点频（见生成器），**不是**从被测语料学的。\n\n",
        "static const float kSniffByteWeight[256] = {\n",
    ]
    for i in range(0, 256, 8):
        row = ", ".join("%+.6ff" % w[j] for j in range(i, i + 8))
        lines.append("    %s,%s\n" % (row, "  // 0x%02X" % i))
    lines.append("};\n")
    return "".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验盘上的表与数据一致")
    args = ap.parse_args()
    text = render(weights())
    if args.check:
        if not OUT.is_file():
            print("!! 缺生成物 %s" % OUT.relative_to(ROOT))
            print("GEN-SNIFF-PROFILES: FAIL")
            return 1
        # ★★ 2026-10-07（审计补充 H1-4 余条）：原来用 `OUT.read_text(encoding="utf-8")` ——
        #   `read_text` 走 **universal newlines**，会把盘上的 `\r\n` 读成 `\n`
        #   ⇒ **CRLF 化的 .inc 照样 PASS**（其余 6 个生成器的 `--check` 都是 `read_bytes`
        #   逐字节比）。判据必须逐字节：行尾被转换、BOM 被加/删、多一个字节都必须红。
        #   门禁侧对应一条**注入必红**：把本文件行尾改成 CRLF ⇒ `--check` 必须退非零。
        cur = OUT.read_bytes()
        want = text.encode("utf-8")
        if cur != want:
            print("!! %s 与生成器不一致（逐字节：盘上 %d 字节 / 期望 %d 字节，盘上 CRLF %d 个）"
                  "—— 重跑 python tools/gen_sniff_profiles.py"
                  % (OUT.relative_to(ROOT), len(cur), len(want), cur.count(b"\r\n")))
            print("GEN-SNIFF-PROFILES: FAIL")
            return 1
        print("GEN-SNIFF-PROFILES: PASS")
        return 0
    # ★ 与其他生成器同姿势（`gen_encoding_tables.py` 用 write_bytes）：**显式 LF**。
    #   `write_text` 在 Windows 上会把 \n 翻成 \r\n，与仓库里其余 .inc 的换行不一致。
    OUT.write_bytes(text.encode("utf-8"))
    w = weights()
    nz = [b for b in range(0x80, 0x100) if abs(w[b]) > 1e-9]
    print("已写 %s（非零字节 %d 个）" % (OUT.relative_to(ROOT), len(nz)))
    top = sorted(nz, key=lambda b: -w[b])[:6]
    bot = sorted(nz, key=lambda b: w[b])[:6]
    print("  最像俄语：" + ", ".join("0x%02X=%+.3f" % (b, w[b]) for b in top))
    print("  最像西欧：" + ", ".join("0x%02X=%+.3f" % (b, w[b]) for b in bot))
    return 0


if __name__ == "__main__":
    sys.exit(main())
