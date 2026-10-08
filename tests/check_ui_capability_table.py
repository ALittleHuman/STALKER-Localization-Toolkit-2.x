#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""插件改装的**能力表**判据（P3，规格 §二「表外无门，静态判红」）。

口径：插件能看到的**只有**下面这几张表。判据拿 `xrkit_cli --ui-facade` 打出来的
`dir(xrkit.Api/Ui/Widget)` 与预期表**逐条对账**：
  · **多一个名字 ⇒ 红**（能力泄漏必须是有意为之：先改本文件、写清它为什么存在，再进代码）；
  · **少一个名字 ⇒ 红**（门面被削 = 用户下限「控件的操控必须至少支持现有功能与观感」的降级）。

顺带判：**无 GUI 进程里门面必须如实报"不可用 + 理由"**（零静默），而不是静默返回空。

输出：`UICAP ok` / `UICAP fail <详情>`；退出码 0/1。
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "build" / "msvc-release" / "tools" / "cli" / "xrkit_cli.exe"

# ★★ 预期能力表（**唯一事实来源**；每条写清"它为什么存在"）────────────────────
# Api：插件身份 + 注册面 + 控件门面入口
EXPECT_API = {
    "api_version": "契约版本（规格 §五「API 表版本化」）",
    "plugin_file": "插件文件名（owner；也是命令 id 前缀）",
    "plugin_dir": "插件所在目录（夹具据此落标记文件）",
    "ui": "★控件门面（L1/L2 的入口；表外无门）",
    "engine": "★引擎调用门面（方案 3：表由样板页反向定义；表外无门）",
    "host": "★宿主服务门面（偏好/exe 目录/系统字体目录；表外无门）",
    "log": "写宿主日志（引擎日志 + 诊断缓冲）",
    "register_command": "注册命令（可被挂载点引用）",
    "register_menu_item": "把命令挂到 (host, location) 槽位",
    "register_option_entry": "往组件**已有**下拉里加条目",
    "register_option": "**新建**一个属于插件的下拉",
    "register_panel": "贡献一个面板（控件门面的落点之一）",
    "register_tool": "贡献一个 Hub 页（控件门面的落点之一）",
}
# Ui：L1 装配（10）+ L2 操作/服务（8）+ ★P4 重型 L2 第一批（19）
EXPECT_UI = {
    "label": "L1 文本", "button": "L1 按钮（回调）", "row": "L1 行容器",
    "checkbox": "L1 勾选框（回调）", "entry": "L1 单行输入", "text": "L1 多行输入",
    "entry_on_change": "★P15-0 输入变化回调（fs 搜索框边打边筛）",
    "combo": "L1 下拉（回调）", "drop_box": "L1 拖入目标", "pack": "L1 显式摆放",
    "text_view": "L1 只读多行视图",
    "progress": "进度条（video 页转码进度；0–100）",
    "set_progress": "改进度条（按 Qt 口径夹到 0–100）",
    "progress_value": "读进度条当前值（判据用）",
    "set_text": "L2 改文本", "get_text": "L2 读文本", "set_enabled": "L2 启用/禁用",
    "combo_value": "L2 读下拉当前项（样板页要读「选的是哪一档」）",
    "checkbox_state": "L2 读勾选框勾选态（样板页要读「勾没勾」）",
    "set_checked": "L2 写勾选态（convert 页：输出目录非空 ⇒ 取消勾选「备份」）",
    "combo_set_items": "L2 重建下拉项（xml 页切模式换筛选下拉）",
    "alive": "L2 句柄是否还有效", "log": "L2 宿主日志栏（LogView）",
    "ask_yes_no": "L2 宿主对话框", "ask_open_file": "L2 宿主文件选择",
    "ask_save_file": "L2 宿主保存对话框",
    "ask_directory": "L2 宿主目录选择（样板页的「浏览目录」按钮；C ABI 没有这一项）",
    "pick_font": "L2 字库选择器（宿主唯一一份：系统字体+四态标记+预览+缩放；异步回调）",
    "open_path": "L2 在文件管理器里打开路径（convert 页的「打开输出」）",
    "context_menu": "宿主渲染右键菜单（插件只给标签 + 收回调；标签原文回传）",
    "copy_text": "L2 写系统剪贴板（xml 页「复制 ID」）",
    # ★★ P4：重型 L2 第一批。加这批名字是**有意为之**（规格 §三 的"表内方法"）——
    #    判据里"多一个即泄漏"就是拦"顺手加的"；每条写清它替六页里的哪件事。
    "list": "L2.1 虚拟化行列表（宿主建/渲染/复用；本批是单列文本形态）",
    "list_set_row_count": "L2.1 行数（宿主据此决定建几行控件）",
    "list_set_row_text": "★L2.5 行文本 provider：**只在可见行**上被调（10.3 同族）",
    "list_row_text": "L2.1 读回某行文本（判据用；不动池子与选中）",
    "list_select_row": "L2.1 选中某行", "list_selected_row": "L2.1 当前选中行",
    "list_on_activate": "L2.1 列表激活（双击/回车；回调给行号）",
    "list_content_height": "L2.1 内容总高（宿主页/视口据此撑开滚动范围）",
    "log_view": "L2.2 行式日志视图（插件**自己**的日志区）",
    "log_view_append": "L2.2 追加一行（不重写全文 ⇒ 不 O(n²)）",
    "log_view_clear": "L2.2 清空", "log_view_text": "L2.2 读回全文（判据用）",
    "drop_zone": "L2.3 拖入区域（宿主判定「本地文件才接」）",
    "drop_zone_on_drop": "★L2.5 落下回调（收到本机路径列表）",
    "task_panel": "L2.4 宿主任务壳（进度/消息/失败/取消/状态栏）",
    "task_run": "★L2.4 起任务：worker 在**工作线程**里跑，收 progress/message 两个 sink",
    "task_status": "L2.4 当前状态文案", "task_cancel": "L2.4 请求取消",
    "task_cancel_requested": "L2.4 worker 轮询取消位（协作式取消）",
    "task_on_message": "任务壳回主线程消息回调（**回调里可以碰控件**）",
    "on_ui_thread": "★线程纪律：门面方法只许在宿主 UI 线程调用（其余一律拒绝并记账）",
    # ★★ P5：重型 L2 第二批 —— 滚动视口 + 可分栏（六页每一页都要的那两件"框架件"）
    "viewport": "L2.5 滚动视口：宿主造「容器+视口+画布」，**句柄＝画布**（条子只许宿主造，1.6）",
    "viewport_fit_content": "L2.5 fit=Content/Viewport（静态内容 vs 自带条子的弹性内容，3.2）",
    "viewport_set_min_body_height": "L2.5 内容高**不许**用请求高度顶（3.3），由调用方给",
    "viewport_sync": "L2.5 每次显式整组重排（1.8）",
    "viewport_can_scroll": "L2.5 现在能不能滚（1.2/1.7：装得下 ⇒ 条子不占位）",
    "viewport_scroll_top": "L2.5 当前竖向偏移（判据/联动用）",
    "split": "L2.6 可分栏容器（横向/纵向）",
    "split_add_pane": "L2.6 加一格，**返回该格容器**（插件往它里面 pack）",
    "split_set_max_size": "L2.6 每格上限（六页里日志/预览格都靠它别吃满窗口）",
    "split_set_collapsed": "L2.6 完全收起（4.2/4.3：收起后改窗口尺寸不弹回）",
    "split_is_collapsed": "L2.6 是否收起", "split_pane_count": "L2.6 格数",
    "split_sash_pos": "L2.6 第 index 条分隔条位置（-1=没有）",
    # ★★ P15-1a：标签页（fs 页「解包 / 封包」两标签）
    "tabs": "标签页容器（每页一个容器句柄）",
    "tabs_add_page": "加一页 ⇒ 该页容器（标题是标签上的字）",
    "tabs_page_count": "页数",
    "tabs_set_current": "切到第 index 页（越界不动）",
    "tabs_current_index": "当前页下标（-1=没有）",
    # ★★ P6：重型 L2 第三批 —— 树（fs 页那棵的用户可见语义）
    "tree": "L2.7 树（多列；句柄＝树自身）",
    "tree_set_columns": "L2 换列声明（xml 页切模式换表头/列宽）",
    "tree_set_root": "L2.7 声明根节点（清空重建）",
    "tree_set_children": "L2.7 填某行的子（**懒建**的落点）",
    "tree_on_action": "行尾动作被点（fs 页的 ✕；回调给行 key；只有 has_action 的行）",
    "tree_clear": "L2.7 清空",
    "tree_row_count": "L2.7 当前**可见**行数（祖先都展开 + 未被过滤）",
    "tree_keys": "L2.7 当前可见行的 key（树序；判据用）",
    "tree_set_expanded": "L2.7 展开/收起某行（**展开态保持**）",
    "tree_expanded_keys": "L2.7 已展开的 key",
    "tree_set_checked": "L2.7 改勾选（**向下传播 + 向上重算**三态）",
    "tree_checked": "L2.7 某行是否勾上", "tree_checked_keys": "L2.7 全勾的行",
    "tree_partial_keys": "L2.7 半态的行（三态）",
    "tree_set_filter": "L2.7 过滤（宿主匹配任一列；命中行连祖先一起显示）",
    "tree_filter_hits": "L2.7 命中数（与界面上的计数**同源**）",
    "tree_select": "L2.7 选中某行", "tree_selected_key": "L2.7 当前选中行",
    "tree_selected_keys": "★P15-0 选中的一组行（按树序）",
    "tree_on_expand": "★L2.5 展开回调（懒建：这时才要子节点）",
    "tree_on_check": "★L2.5 勾选回调（用户改了哪一行）",
    "tree_on_activate": "★L2.5 激活回调（双击叶子）",
    "tree_on_click": "★P15-0 点行身体（单击；语义在插件侧）",
}
# Widget：**故意为空** —— 不透明句柄，插件不能对它做任何事（禁 L4 裸 Qt）
EXPECT_WIDGET: dict[str, str] = {}
# ★★ P7a：**引擎调用门面**（`api.engine.*`；用户 2026-10-07 拍板走方案 3）
#   本批只开样板页 fontpack **真正用到**的那几件（反向定义，不是猜）；
#   下批随样板页迁移补齐（偏好读写/系统字体目录那些属**宿主服务**，进 UI 门面，不进这里）。
EXPECT_ENGINE = {
    "font_check": "字体四态检查（缺字形/日语字体/正常/未知）＋ 缺字清单",
    "font_pack": "出整棵 gamedata 汉化包（与内置页同一个引擎实现）",
    "font_state_marks": "四态文案（**从引擎拿**，插件不许自己抄一份中文）",
    "font_size_slots": "字号槽位表（文件名与 ltx 用的是槽位名）",
    # ★★ P9a：第二页迁移（convert 页）用到的引擎面 —— 每加一项都要在门禁里有判据
    "text_encodings": "源/目标两张编码表（唯一一份在 engine/text/encodings.cpp）",
    "text_convert_batch": "批量转换（扫描台账 / 备份 / 零静默计数）",
    "text_restore_backups": "把 backup/<rel>.bak 恢复回原位（逐条报账）",
    "text_clear_backups": "清空备份（删了哪些、失败哪些，逐条报账）",
    "text_write_stats_csv": "导出统计 CSV（空串=成功；否则是失败原因）",
    "text_format_duration": "秒数 → 1.x 那种 H:MM:SS（唯一一份在引擎 batch.cpp）",
    # ★★ P10a：第三页迁移（text 页）用到的引擎面
    "text_default_id_prefix": "ID 前缀默认值（唯一一份在引擎 extract.cpp）",
    "text_validate_extract": "提取请求校验（给用户看的那句话来自引擎）",
    "text_extract_kind": "源目录判定（必须是 gameplay / scripts 本身）",
    "text_extract_dir": "真提取（含文件边界的取消谓词 / 零静默台账）",
    # ★★ P11a：第四页迁移（xml 页）用到的引擎面
    "xml_modes": "XML 模式表（下拉与内部键的唯一来源）",
    "xml_find_mode": "按内部键/显示名找模式（找不到要能看出来）",
    "text_read_bytes": "按编码读文本（六步判定链；auto=不手动指定）",
    "xml_compare_stats": "按行数/ID 统计对比",
    "xml_compare_text": "按 ID 文本对比",
    "xml_only_one_side": "只有一侧有该文件",
    "xml_inline_detail": "表格里那一格（**引擎渲染**）",
    "xml_full_detail": "完整明细（**引擎渲染**）",
    "xml_full_text_detail": "文本模式完整明细（**引擎渲染**）",
    "xml_render_csv": "CSV 文本（BOM + \\r\\n，引擎唯一实现）",
    "xml_write_csv": "渲染 + 原子写盘（空串=成功）",
    # ★★ P11b：目录对比（第四页真正跑起来要的那一块）
    "xml_compare_dirs": "目录对比（遍历/配对/逐文件比/给 kind；取消在文件边界）",
    "xml_row_matches": "筛选命中（row_kinds 空=全过；differ_types 要同时命中）",
    # ★★ P12a：第五页迁移（video 页）用到的引擎面
    "video_find_ffmpeg": "找 ffmpeg/ffprobe（只认两处：user.ltx + 程序目录；不搜 PATH）",
    "video_major_version": "ffmpeg -version 的主版本号",
    "video_version_rejected": "版本闸门（太老/太新都拒）",
    "video_plan_transcode": "构建转码 argv 与面板要显示的每个值",
    "video_check_output_conflict": "输出撞车（源/参考/已存在）+ 引擎给的理由",
    "video_parse_ffprobe": "ffprobe JSON → 媒体信息",
    "video_parse_encoders": "`ffmpeg -encoders` → 编码器集合",
    "video_progress_from_line": "进度行 → 百分比（上限 99.5）",
    # ★★ P14-0a：fs 面第一批（第六页迁移用的纯函数那组）
    "fs_formats": "格式表（下拉取值 + 可见名 + is_sqfs/is_auto）",
    "fs_format_to_db": "这个键是不是 DB 格式；是就给键与可见名",
    "fs_format_is_sqfs": "这个键是不是 SquashFS",
    "fs_format_is_auto": "这个键是不是 auto（自洽判定）",
    "fs_is_package_name": "这个文件名像不像包",
    "fs_pack_ext_for": "封包产物扩展名（sqfs ⇒ .sqfs；其余 ⇒ .<键>.db）",
    "fs_package_folder_name": "包名壳（解包落点那一层）",
    "fs_resolve_output_root": "输出根归一（前源目录 → 当前 → 工具目录）",
    "fs_unpack_target_dir": "解包落点（自带壳 ⇒ 根；否则 根/壳）",
    "fs_resolve_pack_target": "封包落点（要不要前端弹「保存到」）",
    "fs_safe_out_path": "越界拒因（ok=false 时 reason 非空）",
    "fs_plan_unpack_targets": "多源落点清单",
    "fs_preview_layout": "右栏解包产物预览的条目表",
    # ★★ P14-0b：fs 面第二批（合并树走**句柄**）
    "fs_loose_entries": "散装目录的条目表（error 非空=没读成）",
    "fs_sort_sources_by_basename": "按文件名排序后的下标（加载顺序）",
    "fs_tree_build": "建合并树 ⇒ 句柄（用完 fs_tree_free）",
    "fs_tree_free": "释放句柄",
    "fs_tree_sources": "合并顺序（=加载顺序，后者赢）",
    "fs_tree_flatten": "打平成一列（前序遍历，目录在前）",
    "fs_tree_toggle_all": "全选/全不选（只改 checked）",
    "fs_tree_toggle_dir": "某目录整棵打勾/取消",
    "fs_tree_toggle_source": "整包勾选（返回改动的文件数）",
    "fs_tree_source_state": "某源（文件数, 已勾数）",
    "fs_tree_checked_paths": "勾选态路径快照",
    "fs_tree_apply_checked_paths": "按路径回灌勾选态",
    "fs_tree_conflict_count": "同路径多源定义次数",
    "fs_tree_checked_file_count": "全树已勾文件数",
    "fs_tree_find": "按路径找节点",
    # ★★ P14-0c：fs 面第三批（读条目/闸门/收集/解包）
    "fs_gate_path": "解密闸门：该读哪个路径（引擎读入口也跑它）",
    "fs_decrypt_output_dir": "解密产物落点（两边同源）",
    "fs_package_entries": "包内条目表（超块判 sqfs / 否则 DB 自动+回退键）",
    "fs_collect_pack_files": "封包要收哪些文件（排包文件/去重排序/越界计数）",
    "fs_extract_package": "统一从包导出（sqfs/DB 内部分派；计数+覆盖可见）",
    # ★★ P14-0d：fs 面第四批（封包）
    "fs_pack_squashfs": "封 SquashFS（引擎落盘；返回 inode/块/去重计数）",
    "fs_pack_db": "封 X-Ray DB（读文件→编码→原子写盘；**名字改写台账**）",
    # ★★ P16-1a：统一产物模型（清单/安装/还原）
    "pkg_write_manifest": "清单原子写 + 写后回读校验",
    "pkg_install": "装产物：先备份被覆盖的原件 + 逐条台账（覆盖必须可见）",
    "pkg_restore": "还原：有备份逐字节回；原本不存在 ⇒ 删掉（可回退）",
}
# ★★ P7b：**宿主服务门面**（`api.host.*`）—— 宿主替插件办的杂事（样板页 fontpack 真的要用）
EXPECT_HOST = {
    "pref_get": "读用户偏好（`user.ltx` 的 `[section] key`，缺失返回 fallback）",
    "pref_set": "写用户偏好（**只写点名那一项**，不整块覆盖别的键）",
    "app_dir": "exe 所在目录（发行版放 ffmpeg/ffprobe、`user.ltx` 的落点）",
    "system_font_dirs": "系统字体目录（唯一一份：内置页与插件共用）",
    "run_process": "跑外部进程（唯一一份语义：不弹控制台/取消真停/退码如实/stderr 尾巴）",
}


def run_probe() -> tuple[int, str]:
    proc = subprocess.run([str(CLI), "--ui-facade"], cwd=str(ROOT), capture_output=True,
                          timeout=300)
    return proc.returncode, (proc.stdout or b"").decode("utf-8", errors="replace")


def field(out: str, name: str) -> str:
    """取 `name=值`（值到空白为止）。★ 不能用"按第一个 `=` 切一行"的朴素解析：
    第一行是**多个**键值对（`available=0 methods=… reason=…`），朴素解析会把
    `0 methods=…` 整个当成 available 的值（自证第一版就死在这里）。"""
    match = re.search(r"(?:^|\s)" + re.escape(name) + r"=(\S*)", out)
    return match.group(1) if match else ""


def reason_of(out: str) -> str:
    match = re.search(r"reason=(.*)", out)
    return match.group(1).strip() if match else ""


def table_of(out: str, cls: str) -> str:
    match = re.search(r"UIFACADE " + re.escape(cls) + r"=(.*)", out)
    if match is None:
        return ""
    return match.group(1).strip()


def scan(out: str) -> list[str]:
    offenders: list[str] = []
    for cls, expected in (("Api", EXPECT_API), ("Ui", EXPECT_UI), ("Widget", EXPECT_WIDGET),
                          ("Engine", EXPECT_ENGINE), ("Host", EXPECT_HOST)):
        if re.search(r"UIFACADE " + re.escape(cls) + r"=", out) is None:
            offenders.append("探针没打出 `UIFACADE %s=` 那一行（判据没跑成，不是通过）" % cls)
            continue
        offenders += compare(cls, table_of(out, cls), expected)
    # ★ 无 GUI 进程（CLI）：门面必须如实报不可用 + 给理由（零静默）
    available = field(out, "available")
    reason = reason_of(out)
    if available == "0":
        if not reason:
            offenders.append("门面不可用却没给理由（零静默）")
        elif not re.search(r"没注入|缺方法", reason):
            offenders.append("门面不可用的理由不像理由：「%s」" % reason)
    elif available == "1":
        if not field(out, "methods"):
            offenders.append("available=1 但 methods= 是空的（装了个空门面？）")
    elif not available:
        offenders.append("探针没给出 available=（判据没跑成，不是通过）")
    return offenders


def compare(name: str, got_raw: str, expected: dict[str, str]) -> list[str]:
    got = [x for x in got_raw.split(",") if x]
    offenders: list[str] = []
    for extra in sorted(set(got) - set(expected)):
        offenders.append("%s 多了一个名字「%s」—— 能力泄漏（要么删掉它，要么先改本判据并写清理由）"
                         % (name, extra))
    for missing in sorted(set(expected) - set(got)):
        offenders.append("%s 少了「%s」（%s）—— 门面被削＝用户下限降级"
                         % (name, missing, expected[missing]))
    return offenders


_GREEN = "\n".join([
    "UIFACADE available=0 methods= reason=宿主没注入控件门面（CLI/无 GUI 进程）",
    "UIFACADE Api=" + ",".join(sorted(EXPECT_API)),
    "UIFACADE Ui=" + ",".join(sorted(EXPECT_UI)),
    "UIFACADE Widget=",
    "UIFACADE Engine=" + ",".join(sorted(EXPECT_ENGINE)),
    "UIFACADE Host=" + ",".join(sorted(EXPECT_HOST)),
])
_RED = {
    "多一个能力（泄漏）": _GREEN.replace("UIFACADE Api=", "UIFACADE Api=backdoor,"),
    "少一个能力（削门面）": _GREEN.replace(",get_text", ""),
    "不可用却没理由": _GREEN.replace("reason=宿主没注入控件门面（CLI/无 GUI 进程）", "reason="),
    "缺一整行": "\n".join(_GREEN.splitlines()[:2]),
}


def selfcheck() -> list[str]:
    broken: list[str] = []
    if scan(_GREEN):
        broken.append("干净样本被误报：%s" % scan(_GREEN))
    for name, out in _RED.items():
        if not scan(out):
            broken.append("反例「%s」没被判红" % name)
    return broken


def main() -> int:
    broken = selfcheck()
    if broken:
        print("UICAP fail 判据自证不过（%d 处）：" % len(broken))
        for item in broken:
            print("           UICAP " + item)
        return 1
    if not CLI.is_file():
        print("UICAP fail 找不到 %s（判据没跑成，不是通过）" % CLI.relative_to(ROOT))
        return 1
    rc, out = run_probe()
    for raw in out.splitlines():
        s = raw.strip()
        if s.startswith("UIFACADE"):
            print("           " + s[:160])
    if rc != 0:
        print("UICAP fail 探针退出码 %d（要求 0）" % rc)
        return 1
    offenders = scan(out)
    if offenders:
        print("UICAP fail %d 处：能力表不对" % len(offenders))
        for item in offenders[:10]:
            print("           UICAP " + item)
        return 1
    print("UICAP ok 能力表 %d+%d 个名字逐条对上（判据自证 %d 红 + 1 绿；Widget 故意为空＝不透明句柄）"
          % (len(EXPECT_API), len(EXPECT_UI), len(_RED)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
