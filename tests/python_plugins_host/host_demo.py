# -*- coding: utf-8 -*-
"""2.x Python 插件夹具（**宿主服务门面**，P7b：偏好读写 / exe 目录 / 系统字体目录）。

样板页 fontpack 真的要用这三件，所以它们是"反向定义"出来的、不是猜的：
  · `api.host.pref_set` / `pref_get` —— 记住用户上次选的东西（`user.ltx` 的 `[section] key`）；
  · `api.host.app_dir()` —— exe 所在目录（发行版放 ffmpeg/ffprobe、也是 `user.ltx` 的落点）；
  · `api.host.system_font_dirs()` —— 系统字体目录（**与内置页同一份实现**）。

标记落点：`<plugin_dir>/host_marker.txt`（门禁按行断言）。
"""

import os

PLUGIN_INFO = {"name": "宿主服务夹具", "version": "1.0"}

SECTION = "xrkit_fixture"
VALUE = "hello-1234"


def register(api):
    def mark(text):
        with open(os.path.join(api.plugin_dir, "host_marker.txt"), "a", encoding="utf-8") as handle:
            handle.write(text + "\n")

    host = api.host
    wrote = host.pref_set(SECTION, "round_trip", VALUE)
    mark("pref_set:%d" % int(wrote))
    mark("pref_round_trip:%s" % host.pref_get(SECTION, "round_trip", "(缺)"))
    mark("pref_fallback:%s" % host.pref_get(SECTION, "missing_key", "fallback-x"))

    app = host.app_dir()
    mark("app_dir_nonempty:%d" % int(bool(app)))
    mark("app_dir_is_dir:%d" % int(os.path.isdir(app)))

    dirs = host.system_font_dirs()
    mark("font_dir_count:%d" % len(dirs))
    mark("font_dir_first:%s" % (dirs[0] if dirs else "(空)"))
    mark("done")
