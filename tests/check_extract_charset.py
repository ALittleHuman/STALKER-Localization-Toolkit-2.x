# -*- coding: utf-8 -*-
"""文本提取的**字符集口径**判据（用户 2026-10-28：「符合文本的格式就全部提取出来」）。

事实：1.x 的启发式是"含**西里尔**或 ASCII 字母才算正文" ⇒ 纯中文正文（`你好，旅行者`）
一条都进不来；而且"含 ASCII 字母"会把 `actor_bandit` 这类标识符也抽出来。
本判据钉住新的口径：

  ① 中文正文（XML `<text>` 与脚本字符串字面量）**都要抽出来**；
  ② 标识符式的 ASCII 单 token（`actor_bandit`）**不许**被抽（否则 id 全进表）；
  ③ 西里尔正文照旧要抽（1.x 回归）；
  ④ 改写后的脚本里，被抽的字符串换成 `game.translate_string("<id>")`，id 在 XML 里能找到。

夹具现造在 `%TEMP%`，不碰真机语料。
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

XML_CN = ('<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
          '\t<string id="a1"><text>你好，旅行者</text></string>\n'
          '</string_table>\n')
SCRIPT = ('local t = "你好世界"\n'
          'local id = "actor_bandit"\n'
          'local ru = "Привет мир"\n')

PROBLEMS: list[str] = []


def ok(msg: str) -> None:
    print("  \u2713 " + msg)


def fail(msg: str) -> None:
    print("  \u2717 " + msg)
    PROBLEMS.append(msg)


def run(argv: list[str], timeout: int = 300) -> tuple[int, str]:
    proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=timeout)
    return proc.returncode, proc.stdout.decode("utf-8", errors="replace")


def texts_of(xml: Path) -> list[str]:
    body = xml.read_text(encoding="utf-8", errors="replace")
    return re.findall(r"<text>(.*?)</text>", body, re.S)


def main() -> int:
    print("== 文本提取：字符集口径（中文正文要抽、标识符不许抽）==")
    if not CLI.is_file():
        print("!! 缺 %s" % CLI)
        return 2
    tmp = Path(tempfile.mkdtemp(prefix="xrkit_extract_charset_"))
    try:
        gp = tmp / "src" / "gameplay"
        sc = tmp / "src" / "scripts"
        gp.mkdir(parents=True)
        sc.mkdir(parents=True)
        (gp / "a.xml").write_text(XML_CN, encoding="utf-8")
        # ★ 2026-11-10（用户实测）：声明 windows-1251 的 gameplay XML 必须按声明解，
        #   不许被 GB18030 误判成 ќпытный / Ћпытный 之类残渣。
        cp1251_text = "Опытный сталкер. Детальная информация отсутствует."
        (gp / "cp1251.xml").write_bytes(
            ('<?xml version="1.0" encoding="windows-1251"?>\n<string_table>'
             '<string id="c1"><text>%s</text></string></string_table>' % cp1251_text
             ).encode("cp1251"))
        # ★ 2026-11-10（用户）：没有文本的文件**不许复制**到输出目录
        (gp / "no_text.xml").write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n<string_table>\n'
            '<string id="x"><text></text></string>\n</string_table>\n', encoding="utf-8")
        (sc / "a.script").write_text(SCRIPT, encoding="utf-8")
        (sc / "no_text.ltx").write_text(
            "[logic]\nactive = ph_idle@open\ntips = actor_inventory_box_use\n", encoding="utf-8")

        # ① gameplay：中文 XML 正文要抽
        rc, out = run([str(CLI), "--extract-text", str(gp), str(tmp / "out_gp")])
        xml_gp = tmp / "out_gp" / "gs_gameplay_texts.xml"
        if rc == 0 and xml_gp.is_file():
            got = texts_of(xml_gp)
            if any("你好" in t for t in got):
                ok("gameplay：中文正文被抽出（%d 条）" % len(got))
            else:
                fail("gameplay：中文正文没被抽出（实得 %r）" % got)
        else:
            fail("gameplay 提取失败：rc=%d\n%s" % (rc, out[-400:]))
        if not (tmp / "out_gp" / "no_text.xml").exists():
            ok("gameplay：没有文本的 XML 没被复制到输出")
        else:
            fail("gameplay：没有文本的 no_text.xml 仍被复制了")
        if "notext=1" in out:
            ok("gameplay：no-text 计数=1")
        else:
            fail("gameplay：没有 notext=1 台账（%r）" % out.splitlines()[-1:])
        # ★ cp1251 声明必须被尊重（否则会出现 ќпытный 这类 Mac/GB 残渣）
        if xml_gp.is_file():
            gp_text = xml_gp.read_text(encoding="utf-8", errors="replace")
            if cp1251_text in gp_text and "ќ" not in gp_text:
                ok("gameplay：windows-1251 声明按声明解码（无 ќ 残渣）")
            else:
                fail("gameplay：cp1251 解码残渣（含 ќ=%s / 命中正确文本=%s）"
                     % ("ќ" in gp_text, cp1251_text in gp_text))

        # ② scripts：中文/西里尔都抽，标识符不抽
        rc, out = run([str(CLI), "--extract-text", str(sc), str(tmp / "out_sc")])
        xml_sc = tmp / "out_sc" / "gs_scripts_texts.xml"
        if rc == 0 and xml_sc.is_file():
            got = texts_of(xml_sc)
            joined = "\n".join(got)
            if "你好世界" in joined:
                ok("scripts：中文字面量被抽出")
            else:
                fail("scripts：中文字面量没被抽出（实得 %r）" % got)
            if "Привет мир" in joined:
                ok("scripts：西里尔字面量照旧被抽出（1.x 回归）")
            else:
                fail("scripts：西里尔字面量丢了（实得 %r）" % got)
            if "actor_bandit" not in joined:
                ok("scripts：标识符 `actor_bandit` 没被当成正文")
            else:
                fail("scripts：标识符被抽出来了（会把 id 全灌进文本表）")
        else:
            fail("scripts 提取失败：rc=%d\n%s" % (rc, out[-400:]))
        if not (tmp / "out_sc" / "scripts" / "no_text.ltx").exists():
            ok("scripts：没有文本的 .ltx 没被复制到输出")
        else:
            fail("scripts：没有文本的 no_text.ltx 仍被复制了")
        if "notext=1" in out:
            ok("scripts：no-text 计数=1")
        else:
            fail("scripts：没有 notext=1 台账（%r）" % out.splitlines()[-1:])

        # ③ 改写后的脚本：字面量换成 translate_string(<id>) 且 id 在 XML 里
        rewritten = tmp / "out_sc" / "scripts" / "a.script"
        if rewritten.is_file() and xml_sc.is_file():
            body = rewritten.read_text(encoding="utf-8", errors="replace")
            ids = re.findall(r'game\.translate_string\("([^"]+)"\)', body)
            have = set(re.findall(r'<string id="([^"]+)"', xml_sc.read_text(
                encoding="utf-8", errors="replace")))
            if ids and set(ids) <= have:
                ok("改写脚本：%d 个 translate_string 的 id 都能在 XML 里找到" % len(ids))
            else:
                fail("改写脚本的 id 与 XML 对不上（ids=%r / xml 里 %d 个）" % (ids, len(have)))
        else:
            fail("缺改写后的脚本或 XML（脚本改写链没跑）")
        # ④ 2026-10-28（用户）：「一段文本没有换行、超过上限 ⇒ 弹警告 + 写日志」。
        #   长文本源：800 字无换行 ⇒ 必须有 `TEXT warn long=1` 且落 `_xrkit_long_texts.log`；
        #   短文本源 ⇒ `long=0` 且不生成日志。
        long_src = tmp / "src_long" / "scripts"
        long_src.mkdir(parents=True, exist_ok=True)
        # 2000 字（>1024）⇒ 必须告警
        (long_src / "long.script").write_text(
            'local t = "%s"\n' % ("Текст для проверки. " * 100).strip(), encoding="utf-8")
        out_long = tmp / "out_long"
        rc, out = run([str(CLI), "--extract-text", str(long_src), str(out_long)])
        log_file = out_long / "_xrkit_long_texts.log"
        if rc == 0 and "TEXT warn long=1" in out and log_file.is_file():
            body = log_file.read_text(encoding="utf-8", errors="replace")
            if "long.script|sgtat_" in body:
                ok("超长无换行文本：告警进 stdout 且日志点名 文件|id|长度")
            else:
                fail("日志里没有「文件|id|长度」三段（实得 %r）" % body[:160])
        else:
            fail("超长文本没告警/没日志：rc=%d\n%s" % (rc, out[-400:]))

        # ②-b 用户 2026-10-28：「>512 不需要告警」⇒ 800 字无换行必须 **long=0**。
        mid_src = tmp / "src_mid" / "scripts"
        mid_src.mkdir(parents=True, exist_ok=True)
        (mid_src / "mid.script").write_text(
            'local t = "%s"\n' % ("Текст для проверки. " * 40).strip(), encoding="utf-8")
        out_mid = tmp / "out_mid"
        rc, out = run([str(CLI), "--extract-text", str(mid_src), str(out_mid)])
        if rc == 0 and "long=0" in out and not (out_mid / "_xrkit_long_texts.log").exists():
            ok("800 字无换行：不告警（上限 1024，>512 不算超限）")
        else:
            fail("800 字却告警了（上限应已抬到 1024）：rc=%d\n%s" % (rc, out[-300:]))

        short_src = tmp / "src_short" / "scripts"
        short_src.mkdir(parents=True, exist_ok=True)
        (short_src / "short.script").write_text('local t = "短文本"\n', encoding="utf-8")
        out_short = tmp / "out_short"
        rc, out = run([str(CLI), "--extract-text", str(short_src), str(out_short)])
        if rc == 0 and "long=0" in out and not (out_short / "_xrkit_long_texts.log").exists():
            ok("短文本：不告警、不写日志（判据不是恒红）")
        else:
            fail("短文本却告警/写日志：rc=%d\n%s" % (rc, out[-300:]))

        # ⑤ 多源合并：根 scripts + configs/scripts 的文本表追加到同一份 XML
        ap1 = tmp / "ap1" / "scripts"
        ap2 = tmp / "ap2" / "scripts"
        ap1.mkdir(parents=True)
        ap2.mkdir(parents=True)
        (ap1 / "a.script").write_text('local a = "Hello world"\n', encoding="utf-8")
        (ap2 / "b.script").write_text('local b = "Second text"\n', encoding="utf-8")
        ap_out = tmp / "ap_out"
        rc1, _ = run([str(CLI), "--extract-text", str(ap1), str(ap_out)])
        rc2, _ = run([str(CLI), "--extract-text", str(ap2), str(ap_out),
                      "--append-texts", "--id-prefix", "sgtat_cfg_sc_0"])
        ap_xml = ap_out / "gs_scripts_texts.xml"
        if rc1 == 0 and rc2 == 0 and ap_xml.is_file():
            body = ap_xml.read_text(encoding="utf-8", errors="replace")
            ids = re.findall(r'<string id="([^"]+)"', body)
            if len(ids) == 2 and len(set(ids)) == 2 and "Hello world" in body and "Second text" in body:
                ok("多源追加：两条文本合并到同一份 XML，id 不冲突")
            else:
                fail("多源追加结果不对：ids=%r" % ids)
        else:
            fail("多源追加失败：rc=%d/%d" % (rc1, rc2))

        # ★ 2026-11-10：页面必须同时扫根 `scripts/` 与 `configs/scripts/`，
        #   否则真机样例根 scripts 的 334 条文本会被整批漏掉。
        xml_src = (ROOT / "src" / "shell" / "pages" / "xml_page.cpp").read_text(
            encoding="utf-8")
        if "root_scripts" in xml_src and "append_texts" in xml_src:
            ok("页面：根 scripts + configs/scripts 都扫，文本表追加合并")
        else:
            fail("页面：没有覆盖根 scripts / 追加文本表")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if PROBLEMS:
        print("EXTRACT-CHARSET: FAIL（%d 处）" % len(PROBLEMS))
        return 1
    print("EXTRACT-CHARSET: PASS")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    raise SystemExit(main())
