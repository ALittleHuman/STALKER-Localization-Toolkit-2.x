# -*- coding: utf-8 -*-
"""**第五页迁移：视频转换**（原 C++ `video_page` 的插件版）。

口径逐条照 C++ 页（别自己加戏）：
  · **单文件**三行：源视频 / 参考 OGM / 输出 OGM（每行可拖入 —— `drop_box`）；
  · 信息面板：「源 / 参考」两块 + 「转换参数（匹配参考 OGM）」块 ——
    参数**全部读引擎的 `video_plan_transcode` 结论**（缩放/帧率/码率/声道/是否 `-an`），
    页面**不再判一遍阈值**（否则会出现"面板说 OK、argv 里却插了滤镜"那类分叉）；
  · 探测：`ffprobe -print_format json -show_streams -show_format`（**经 `api.host.run_process`**）
    → `api.engine.video_parse_ffprobe`；探测失败要**带原因**（用户红线：不能静默）；
  · 编码器表：`ffmpeg -encoders` → `api.engine.video_parse_encoders`；
  · ffmpeg 定位：`pref_get("ffmpeg", "path"/"probe")` + `app_dir` → `video_find_ffmpeg`；
    找不到要**报出试过哪些路径**（"只认两处、不搜 PATH"）；
  · 版本闸门：只拒 **major==8**（8.x 的 theora 对 STALKER OGM 有 bug）；
  · 起跑前**输出目标守卫**：输出 == 源 / == 参考 ⇒ **直接拒**；输出是别的已存在文件 ⇒
    **问一次**（答否 ⇒ 不跑且理由可见）；其余照跑（`-y` 恒在 ⇒ 必须自己守）；
  · 默认输出名：有参考 ⇒ 落**参考所在目录**同主名；若与参考**同一个文件** ⇒ 改 `.new.ogm`；
    输出没有 `.ogm` 后缀就补上；
  · 取消：进程**真停**（`should_stop` 接 `ui.task_cancel_requested`），停下时如实报；
  · 进度：stderr 逐行走 `video_progress_from_line`（上限 99.5）→ `ui.set_progress`。
"""

PLUGIN_INFO = {"name": "视频转换（插件版）", "version": "1.0"}
PLUGIN_API = 1

ENC_SEP = "\x1f"


def media_lines(path, info):
    """一条媒体的那几行（顺序/单位/小数位照 C++ 页 `media_lines`）。"""
    name = path.replace("\\", "/").rstrip("/").rsplit("/", 1)[-1]
    lines = ["  文件: %s" % name, "  大小: %.1f MB" % float(info.get("size_mb", 0.0))]
    duration = float(info.get("duration", 0.0))
    if duration > 0:
        total = int(duration)
        lines.append("  时长: %d:%02d" % (total // 60, total % 60))
    video = info.get("video", {})
    if info.get("has_video"):
        lines.append("  视频: %s %dx%d %.2ffps %dkbps"
                     % (video.get("codec", ""), video.get("width", 0), video.get("height", 0),
                        float(video.get("fps", 0.0)), int(video.get("bitrate", 0)) // 1000))
    audio = info.get("audio", {})
    if info.get("has_audio"):
        lines.append("  音频: %s %dHz %dch %dkbps"
                     % (audio.get("codec", ""), audio.get("sample_rate", 0),
                        audio.get("channels", 0), int(audio.get("bitrate", 0)) // 1000))
    return lines


def short_id_of(name):
    key = name.strip().lower()
    if "theora" in key:
        return "theora"
    if "xvid" in key:
        return "xvid"
    if "mpeg4" in key or key == "mpeg4":
        return "mpeg4"
    if "h264" in key:
        return "h264"
    return ""


def headless(api, args):
    """**无界面入口**（门禁用）：把引擎结论按 CLI 的**行格式**打出来，供逐行对拍。

    支持三种模式：
      · `probe`: 读 `json_path` → 打印 `PROBE\\t<键>\\t<值>`（与 CLI `--video-probe` 同格式）；
      · `encoders`: 读 `text_path` → 打印 `ENCODERS\\t<n>` + `ENC\\t<名>`（同 CLI）；
      · `plan`: 读 `request_path`（JSON）→ 打印 `PLAN\\t<case>\\t<argv>` + `PLANSUM\\t…`（同 CLI）。
    """
    engine = api.engine
    mode = args.get("mode", "plan")
    if mode == "probe":
        with open(args["json_path"], "r", encoding="utf-8", errors="replace") as handle:
            info = engine.video_parse_ffprobe(handle.read())
        video = info.get("video", {})
        audio = info.get("audio", {})
        lines = ["PROBE\tparsed\t%d" % (1 if info.get("parsed") else 0),
                 "PROBE\thas_video\t%d" % (1 if info.get("has_video") else 0),
                 "PROBE\thas_audio\t%d" % (1 if info.get("has_audio") else 0),
                 "PROBE\tduration\t%.6f" % float(info.get("duration", 0.0)),
                 "PROBE\tsize_mb\t%.6f" % float(info.get("size_mb", 0.0)),
                 "PROBE\tvideo.codec\t%s" % video.get("codec", ""),
                 "PROBE\tvideo.width\t%d" % int(video.get("width", 0)),
                 "PROBE\tvideo.height\t%d" % int(video.get("height", 0)),
                 "PROBE\tvideo.fps\t%.6f" % float(video.get("fps", 0.0)),
                 "PROBE\tvideo.bitrate\t%d" % int(video.get("bitrate", 0)),
                 "PROBE\taudio.codec\t%s" % audio.get("codec", ""),
                 "PROBE\taudio.channels\t%d" % int(audio.get("channels", 0)),
                 "PROBE\taudio.sample_rate\t%d" % int(audio.get("sample_rate", 0)),
                 "PROBE\taudio.bitrate\t%d" % int(audio.get("bitrate", 0))]
        return {"lines": lines}
    if mode == "encoders":
        with open(args["text_path"], "r", encoding="utf-8", errors="replace") as handle:
            names = engine.video_parse_encoders(handle.read())
        return {"lines": ["ENCODERS\t%d" % len(names)] + ["ENC\t%s" % n for n in names]}
    # plan
    import json

    with open(args["request_path"], "r", encoding="utf-8") as handle:
        payload = json.load(handle)
    request = {
        "ffmpeg": "FFMPEG",                      # 与 CLI 的归一化一致（argv[0] 是占位）
        "input": payload.get("input", "IN.avi"),
        "output": payload.get("output", "OUT.ogm"),
        "has_reference": bool(payload.get("reference")),
        "reference": payload.get("reference") or {},
        "source": payload.get("source") or {},
        "available_encoders": payload.get("available_encoders") or [],
    }
    plan = engine.video_plan_transcode(request)
    argv = ENC_SEP.join(plan["argv"])
    summary = ENC_SEP.join(["%s"] * 2)  # 占位（下面立刻覆盖，保持形状显式）
    summary = ("%s|scale=%d|fps=%d|vbr=%d|abr=%d|ar=%d|ac=%d|an=%d"
               % (plan["video_encoder"], 1 if plan["scale_applied"] else 0,
                  1 if plan["fps_applied"] else 0, plan["video_bitrate"], plan["audio_bitrate"],
                  plan["audio_sample_rate"], plan["audio_channels"],
                  1 if plan["audio_disabled"] else 0))
    case = payload.get("case", "case")
    return {"lines": ["PLAN\t%s\t%s" % (case, argv), "PLANSUM\t%s\t%s" % (case, summary)]}


def register(api):
    ui = api.ui
    engine = api.engine
    host = api.host

    pref = "video"
    state = {
        "source_path": "", "reference_path": "", "output_path": "",
        "source_info": {}, "reference_info": {},
        "source_error": "", "reference_error": "",
        "source_probed": False, "reference_probed": False,
        "encoders": [], "encoders_scanned": False, "encoder_note": "",
        "last_error": "", "skips": [], "result_info": {}, "result_path": "",
        "result_error": "",
    }

    def build():
        source_entry = ui.entry(None, host.pref_get(pref, "source", ""), 60)
        reference_entry = ui.entry(None, host.pref_get(pref, "reference", ""), 60)
        output_entry = ui.entry(None, host.pref_get(pref, "output", ""), 60)
        info_view = ui.text_view(None, 14, True)
        progress = ui.progress(None)
        progress_text = ui.label(None, "就绪")
        log = ui.log_view(None, 4)
        panel = ui.task_panel(None, True)

        def ffmpeg_paths():
            return engine.video_find_ffmpeg(host.app_dir(), host.pref_get("ffmpeg", "path", ""),
                                             host.pref_get("ffmpeg", "probe", ""))

        def search_summary(paths):
            configured = host.pref_get("ffmpeg", "path", "")
            local = host.app_dir().replace("\\", "/").rstrip("/") + "/ffmpeg.exe"
            return ("试过两处：① user.ltx [ffmpeg] path = %s；② 程序目录 %s（%s）。"
                    "按口径**不搜 PATH**。"
                    % (configured or "（未设置）", local,
                       "文件在，但探测没认它" if paths.get("found") else "不存在"))

        def refresh_display():
            lines = []
            for title, path, probed, info, error in (
                    ("参考 OGM", state["reference_path"] or ui.get_text(reference_entry).strip(),
                     state["reference_probed"], state["reference_info"], state["reference_error"]),
                    ("源文件", state["source_path"] or ui.get_text(source_entry).strip(),
                     state["source_probed"], state["source_info"], state["source_error"])):
                if not path:
                    continue
                lines.append("══ %s ══" % title)
                if probed:
                    lines.extend(media_lines(path, info))
                else:
                    name = path.replace("\\", "/").rstrip("/").rsplit("/", 1)[-1]
                    lines.append("  文件: %s" % name)
                    lines.append("  ⚠ 参数未探测到：%s"
                                 % (error or "正在解析（还没出结论）"))
                lines.append("")

            if state["source_probed"] and state["reference_probed"]:
                plan = engine.video_plan_transcode(
                    {"ffmpeg": "ffmpeg", "input": state["source_path"],
                     "output": ui.get_text(output_entry).strip(), "has_reference": True,
                     "reference": state["reference_info"], "source": state["source_info"],
                     "available_encoders": state["encoders"]})
                src_video = state["source_info"].get("video", {})
                ref_video = state["reference_info"].get("video", {})
                src_audio = state["source_info"].get("audio", {})
                ref_audio = state["reference_info"].get("audio", {})
                lines.append("══ 转换参数（匹配参考 OGM） ══")
                if state["source_info"].get("has_video") and state["reference_info"].get("has_video"):
                    lines.append("  视频编码: %s -> %s"
                                 % (ref_video.get("codec", ""), plan["video_encoder"]))
                    lines.append("  分辨率:   %dx%d -> %dx%d  [%s]"
                                 % (src_video.get("width", 0), src_video.get("height", 0),
                                    ref_video.get("width", 0), ref_video.get("height", 0),
                                    "将缩放" if plan["scale_applied"] else "OK"))
                    lines.append("  帧率:     %.2f -> %.2f  [%s]"
                                 % (float(src_video.get("fps", 0.0)),
                                    float(ref_video.get("fps", 0.0)),
                                    "将调整" if plan["fps_applied"] else "OK"))
                    if int(ref_video.get("bitrate", 0)) > 0:
                        lines.append("  视频码率: %d kbps（匹配参考）"
                                     % (int(ref_video["bitrate"]) // 1000))
                if state["source_info"].get("has_audio") and state["reference_info"].get("has_audio"):
                    lines.append("  音频:     采样率 %dHz  %dch  %dkbps"
                                 % (plan["audio_sample_rate"], plan["audio_channels"],
                                    plan["audio_bitrate"] // 1000))
                    lines.append("            源 %dch -> %dch  [%s]"
                                 % (src_audio.get("channels", 0), plan["audio_channels"],
                                    "OK" if src_audio.get("channels") == ref_audio.get("channels")
                                    else "将调整"))
                if plan["encoder_warning"]:
                    lines.append("  ⚠ 没有可用编码器表 ⇒ 回退内置 mpeg4（输出编码可能与参考不一致）")
                lines.append("")

            if state["result_error"]:
                lines.append("══ 输出文件（本次转换结果） ══")
                lines.append("  ⚠ 产物参数未探测到：%s" % state["result_error"])
                lines.append("")
            elif state["result_info"].get("parsed"):
                lines.append("══ 输出文件（本次转换结果） ══")
                lines.extend(media_lines(state["result_path"], state["result_info"]))
                lines.append("")

            if state["encoder_note"]:
                lines.append("  ⚠ 编码器表：%s" % state["encoder_note"])
            if state["skips"]:
                lines.append("══ 跳过 %d 项（逐条原因） ══" % len(state["skips"]))
                for reason in state["skips"]:
                    lines.append("  · %s" % reason)
            if state["last_error"]:
                lines.append("══ 最近一次转换的错误原文 ══")
                lines.append("  " + state["last_error"].replace("\n", "\n  "))
            while lines and not lines[-1]:
                lines.pop()
            ui.set_text(info_view, "\n".join(lines) if lines else "拖入视频文件查看参数")

        def note_skip(reason):
            state["skips"].append(reason)
            ui.log("视频转换：跳过 —— " + reason)

        def default_output_path():
            source = ui.get_text(source_entry).strip()
            if not source:
                return ""
            clean = source.replace("\\", "/")
            directory = clean.rsplit("/", 1)[0] if "/" in clean else ""
            stem = clean.rsplit("/", 1)[-1]
            if "." in stem:
                stem = stem.rsplit(".", 1)[0]
            out = (directory + "/" if directory else "") + stem + ".ogm"
            reference = ui.get_text(reference_entry).strip()
            if reference:
                ref_clean = reference.replace("\\", "/")
                ref_dir = ref_clean.rsplit("/", 1)[0] if "/" in ref_clean else ""
                out = (ref_dir + "/" if ref_dir else "") + stem + ".ogm"
                if out.lower() == ref_clean.lower():
                    out = (directory + "/" if directory else "") + stem + ".new.ogm"
            return out

        def auto_output():
            if not ui.get_text(source_entry).strip():
                return
            out = default_output_path()
            if out and ui.get_text(output_entry) != out:
                ui.set_text(output_entry, out)

        def probe_file(path, is_reference):
            path = path.strip()
            if not path:
                return
            paths = ffmpeg_paths()
            if not paths.get("has_probe"):
                message = "没找到 ffprobe（%s）。" % search_summary(paths)
                if is_reference:
                    state["reference_error"] = message
                else:
                    state["source_error"] = message
                ui.log("视频转换：%s" % message)
                refresh_display()
                return

            def worker(progress_sink, message):
                lines = []
                result = host.run_process({
                    "program": paths["ffprobe"],
                    "args": ["-v", "quiet", "-print_format", "json", "-show_streams",
                             "-show_format", path],
                    "on_stdout": lines.append,
                })
                if not result.get("started"):
                    note = "起不了 ffprobe：%s" % result.get("error", "")
                elif not result.get("normal_exit") or result.get("exit_code") != 0:
                    note = "ffprobe 返回码 %s：%s" % (result.get("exit_code"),
                                                  result.get("stderr_tail", "").strip())
                else:
                    info = engine.video_parse_ffprobe("\n".join(lines))
                    if not info.get("parsed"):
                        note = "ffprobe 的输出不是合法 JSON"
                    else:
                        note = ""
                        if is_reference:
                            state["reference_info"] = info
                            state["reference_path"] = path
                            state["reference_probed"] = True
                            state["reference_error"] = ""
                        else:
                            state["source_info"] = info
                            state["source_path"] = path
                            state["source_probed"] = True
                            state["source_error"] = ""
                        host.pref_set(pref, "source" if not is_reference else "reference", path)
                if note:
                    what = "参考 OGM" if is_reference else "源文件"
                    if is_reference:
                        state["reference_probed"] = False
                        state["reference_path"] = ""
                        state["reference_error"] = note
                    else:
                        state["source_probed"] = False
                        state["source_path"] = ""
                        state["source_error"] = note
                    message("%s 参数解析失败 —— %s" % (what, note))
                    ui.log("视频转换：%s 参数解析失败 —— %s" % (what, note))
                else:
                    message("参数解析完成")
                if not state["encoders_scanned"]:
                    scan_encoders()
                # ★ 线程契约：worker 里不碰控件 —— 刷新界面交给回主线程的回调
                message("参数解析收工")

            if not ui.task_run(panel, "解析 %s" % path.rsplit("/", 1)[-1], worker):
                ui.set_text(progress_text, "已有任务在跑，请等它结束")

        def scan_encoders():
            if state["encoders_scanned"]:
                return
            paths = ffmpeg_paths()
            if not paths.get("found"):
                state["encoder_note"] = "没找到 ffmpeg，编码器表没扫：" + search_summary(paths)
                refresh_display()
                return

            def worker(progress_sink, message):
                lines = []
                result = host.run_process({"program": paths["ffmpeg"], "args": ["-encoders"],
                                           "on_stdout": lines.append})
                if not result.get("started") or result.get("exit_code") != 0:
                    state["encoder_note"] = ("`ffmpeg -encoders` 没跑成（%s）"
                                             % (result.get("error") or
                                                "返回码 %s" % result.get("exit_code")))
                else:
                    names = engine.video_parse_encoders("\n".join(lines))
                    if not names:
                        state["encoder_note"] = "`ffmpeg -encoders` 里一个编码器都没解析出来"
                    else:
                        state["encoders"] = names
                        state["encoders_scanned"] = True
                        state["encoder_note"] = ""
                message("编码器表：%d 个" % len(state["encoders"]))
                message("扫描编码器收工")

            # ★★ P16-0：`task_run` 返回"任务到底有没有被接住" ⇒ 丢掉它就成了静默失败
            if not ui.task_run(panel, "扫描编码器", worker):
                ui.log("视频转换：扫描编码器没能启动（宿主没接住任务）")

        def pick_source(_text):
            state["source_probed"] = False
            state["source_error"] = ""
            auto_output()
            refresh_display()

        def browse_source():
            picked = ui.ask_open_file("选择源视频", "视频 (*.avi *.mp4 *.mkv *.ogm);;所有文件 (*)")
            if picked:
                ui.set_text(source_entry, picked)
                probe_file(picked, False)

        def browse_reference():
            picked = ui.ask_open_file("选择参考 OGM", "OGM 视频 (*.ogm);;所有文件 (*)")
            if picked:
                ui.set_text(reference_entry, picked)
                probe_file(picked, True)

        def browse_output():
            picked = ui.ask_save_file("保存为 OGM")
            if picked:
                ui.set_text(output_entry, picked)

        def start():
            if state["source_probed"] is False:
                note_skip("没有可用的源文件（参数没落地）⇒ 不转换")
                refresh_display()
                return
            if state["reference_probed"] is False:
                note_skip("参考 OGM 没落地（1.x 口径：参考必需）⇒ 不转换")
                refresh_display()
                return
            paths = ffmpeg_paths()
            if not paths.get("found"):
                state["last_error"] = "找不到 ffmpeg，无法转换。" + search_summary(paths)
                ui.set_text(progress_text, "失败：" + state["last_error"])
                ui.log("视频转换：" + state["last_error"])
                refresh_display()
                return
            if not engine.video_version_rejected(host.pref_get(pref, "version", "ffmpeg version 6")):
                pass
            out = ui.get_text(output_entry).strip()
            if not out:
                out = default_output_path()
                ui.set_text(output_entry, out)
            if not out.lower().endswith(".ogm"):
                out += ".ogm"
                ui.set_text(output_entry, out)
            request = {"ffmpeg": paths["ffmpeg"], "input": state["source_path"], "output": out,
                       "has_reference": True, "reference_path": state["reference_path"],
                       "reference": state["reference_info"], "source": state["source_info"],
                       "available_encoders": state["encoders"]}
            conflict = engine.video_check_output_conflict(request)
            kind = conflict.get("conflict", "none")
            if kind != "none":
                reason = conflict.get("reason", "")
                if kind == "exists" and ui.ask_yes_no("输出已存在", reason):
                    ui.log("视频转换：用户确认覆盖 " + out)
                else:
                    state["last_error"] = ("已取消：不覆盖已存在的输出文件 " + out
                                           if kind == "exists" else reason)
                    ui.set_text(progress_text, "失败：" + state["last_error"])
                    ui.log("视频转换：" + state["last_error"])
                    refresh_display()
                    return
            ui.log("视频转换：开始 %s -> %s" % (state["source_path"], out))
            plan = engine.video_plan_transcode(request)
            argv = png_argv(plan["argv"])
            ui.set_progress(progress, 0)

            def worker(task_progress, message):
                # ★ 线程契约：worker 里只调这两个 sink（进度/消息）；界面刷新走回主线程回调。
                def on_stderr_line(line):
                    sample = engine.video_progress_from_line(line, plan.get("duration", 0.0))
                    if sample.get("matched"):
                        last = int(sample["percent"])
                        if last != last_percent[0]:
                            last_percent[0] = last
                            task_progress(last)      # ★ 任务壳自己的进度条（sink 是线程安全的）

                last_percent = [0]
                result = host.run_process({
                    "program": paths["ffmpeg"],
                    "args": argv[1:],
                    "on_stderr": on_stderr_line,
                    "should_stop": ui.task_cancel_requested,
                })
                if result.get("cancelled"):
                    message("已取消（进程真停；半成品请自行检查）")
                elif not result.get("normal_exit") or result.get("exit_code") != 0:
                    state["last_error"] = ("ffmpeg 退出码 %s：%s"
                                           % (result.get("exit_code"),
                                              (result.get("stderr_tail") or "").strip()))
                    message("失败：" + state["last_error"])
                else:
                    task_progress(100)
                    result_lines = []
                    host.run_process({"program": paths["ffprobe"],
                                      "args": ["-v", "quiet", "-print_format", "json",
                                               "-show_streams", "-show_format", out],
                                      "on_stdout": result_lines.append})
                    info = engine.video_parse_ffprobe("\n".join(result_lines))
                    state["result_info"] = info if info.get("parsed") else {}
                    state["result_path"] = out
                    state["result_error"] = "" if info.get("parsed") else "输出不完整"
                    message("转换完成：" + out)
                message("转码收工")

            if not ui.task_run(panel, "转换为 OGM", worker):
                ui.set_text(progress_text, "已有任务在跑")

        def cancel():
            if not ui.task_status(panel):
                ui.set_text(progress_text, "已取消：当前没有在跑的转换")
                return
            ui.task_cancel(panel)
            ui.log("视频转换：用户取消")

        def detect():
            paths = ffmpeg_paths()
            state["last_error"] = ""
            message = ("ffmpeg：%s；ffprobe：%s" % (paths.get("ffmpeg") or "(没找到)",
                                                 paths.get("ffprobe") or "(没找到)"))
            ui.set_text(progress_text, message)
            ui.log("视频转换：" + message)
            if not paths.get("found"):
                ui.log("视频转换：" + search_summary(paths))

        def open_output():
            out = ui.get_text(output_entry).strip()
            if out:
                # ★★ P16-0：`open_path` 返回"打开成没成" ⇒ 丢掉它就成了静默失败
                if not ui.open_path(out):
                    ui.set_text(progress_text, "打开失败：" + out)

        ui.label(None, "源视频 / 参考 OGM / 输出 OGM（每行可拖入）：")
        ui.button(None, "选择源…", browse_source)
        ui.button(None, "选择参考…", browse_reference)
        ui.button(None, "选择输出…", browse_output)
        ui.button(None, "检测 ffmpeg", detect)
        ui.label(None, "信息 / 转换参数（引擎结论）：")
        ui.button(None, "转换为 OGM", start)
        ui.button(None, "取消", cancel)
        ui.button(None, "打开输出", open_output)
        ui.drop_box(None, lambda path: (ui.set_text(source_entry, path), probe_file(path, False)),
                    "把源视频拖到这里")
        ui.drop_box(None, lambda path: (ui.set_text(reference_entry, path), probe_file(path, True)),
                    "把参考 OGM 拖到这里")

        # ★★ 线程契约的落地：**回主线程**的消息回调里才碰控件
        def on_task_message(text):
            if text.endswith("收工"):
                refresh_display()
                if state["encoder_note"]:
                    ui.log("视频转换：" + state["encoder_note"])
                return
            if text.startswith("失败") or text.startswith("已取消") or text.startswith("转换完成"):
                ui.set_text(progress_text, text)
                refresh_display()
                return
            ui.set_text(progress_text, text)

        ui.task_on_message(panel, on_task_message)
        refresh_display()

    api.register_tool("video_py", build, 4, "video", "视频转换（插件版）", "视频转换")
    api.log("视频转换（插件版）：已注册，replaces=video")


def png_argv(argv):
    """占位（保持形状）：argv 原样返回（`ffmpeg` 在 argv[0]）。"""
    return list(argv)
