#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""IX-Ray 生成器的**启动器**（产品内唯一入口）—— 把 vendored fontTools 放进 sys.path 再跑助手。

为什么要有这一层（而不是直接 `python ixrgen.py`）：
  · 产品里不能改 `python314._pth`（那是运行时全局路径，会影响插件与宿主）；
  · 也不靠环境变量 `PYTHONPATH`（进程封装不传环境 ⇒ 靠它就等于靠"外面恰好设了"）；
  ⇒ 用一个两行的启动器把路径插进去，**唯一的依赖事实**就是"`fontlib/fontTools` 在启动器旁边"。

目录约定（打包后）：
  <app>/tools/ixr_run.py       ← 本文件
  <app>/tools/ixrgen.py        ← 助手（plugins_ixr/_lib/ixrgen.py 的副本）
  <app>/tools/fontlib/fontTools/  ← vendored fontTools

用法（与助手完全一致，参数原样透传）：
  python tools/ixr_run.py --pack <整包> --font <CJK> --out <目录> [...]
  python tools/ixr_run.py --selftest     ← 自检：能 import fontTools 就打印 ok

退出码：与助手一致（0/1/2）；启动器自身找不到助手/字体库 ⇒ 1 并说出缺什么。
"""
from __future__ import annotations

import os
import runpy
import sys


def _here() -> str:
    return os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    here = _here()
    fontlib = os.path.join(here, "fontlib")
    for p in (os.path.join(fontlib, ""), fontlib, here):
        # 顺序：fontlib 在前（vendored 优先），工具目录在后（助手自己）
        if p not in sys.path:
            sys.path.insert(0, p)

    if "--selftest" in sys.argv[1:]:
        try:
            import fontTools  # noqa: F401
        except Exception as exc:  # noqa: BLE001
            print("IXR-RUN selftest fail reason=%r" % (exc,))
            return 1
        print("IXR-RUN selftest ok fonttools=%s" % fontTools.version)
        return 0

    helper = os.path.join(here, "ixrgen.py")
    if not os.path.isfile(helper):
        print("IXR-RUN error reason=缺 ixrgen.py（应与启动器同目录）：%s" % helper)
        return 1
    try:
        import fontTools  # noqa: F401
    except Exception as exc:  # noqa: BLE001
        print("IXR-RUN error reason=缺 vendored fontTools（tools/fontlib）：%r" % (exc,))
        return 1

    sys.argv = [helper] + sys.argv[1:]
    runpy.run_path(helper, run_name="__main__")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            pass
    raise SystemExit(main())
