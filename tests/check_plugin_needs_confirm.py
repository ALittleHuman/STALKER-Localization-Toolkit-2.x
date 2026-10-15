# -*- coding: utf-8 -*-
"""P17-122 判据：注入/不注入两种情形，引擎的行为必须分开。"""
import json, subprocess, sys, tempfile
from pathlib import Path
ROOT = Path(r"E:\Software\Games\STALKER\Localization\Tools\STALKER_Toolkit_2.x")
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"
PLUGINS = ROOT / "tests" / "python_plugins_confirm"
bad = 0

def call(action, src, out):
    args = json.dumps({"action": action, "source": str(src), "out_dir": str(out)})
    p = subprocess.run([str(CLI), "--py-plugin-call", str(PLUGINS), "confirm_probe.py", args],
                       capture_output=True, timeout=300)
    txt = ((p.stdout or b"") + (p.stderr or b"")).decode("utf-8", "replace")
    for line in txt.splitlines():
        if " result=" in line:
            return json.loads(line.split(" result=", 1)[1]), txt
    return None, txt

with tempfile.TemporaryDirectory(prefix="cf_") as tmp:
    src = Path(tmp) / "src"; src.mkdir()
    # 会触发编码确认的输入（声明与实际字节冲突）
    (src / "a.xml").write_bytes('<?xml version="1.0" encoding="windows-1251"?>\n<a>\u041f</a>\n'.encode("utf-8"))
    for action, want_asked, want_nc in (("without_confirm", 0, True), ("with_confirm", 1, False)):
        got, txt = call(action, src, Path(tmp) / ("out_" + action))
        if got is None:
            print("  \u2717 %-16s \u62ff\u4e0d\u5230 result\uff1a%s" % (action, txt.strip()[:120])); bad += 1; continue
        ok = (got["asked"] == want_asked) and (got["needs_confirm"] == want_nc)
        bad += 0 if ok else 1
        print("  %s %-16s asked=%d needs_confirm=%s exit=%s registered=%s"
              % ("ok " if ok else "\u2717  ", action, got["asked"], got["needs_confirm"],
                 got["exit_code"], got["registered"]))
        if got["asked"]:
            print("       \u95ee\u53e5\u9996\u884c\uff1a%s" % got["asked_question"].splitlines()[0][:90])
print("RESULT:", "PASS" if bad == 0 else "FAIL(%d)" % bad)
sys.exit(1 if bad else 0)
