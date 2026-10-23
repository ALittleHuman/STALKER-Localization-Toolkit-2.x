# `smoke.cpp` 批十四（第 6351-6850 行）—— ★ P16-3c 的判据本体与它的脆弱点

---

## 1. 共享前置：`wait_page_ready` 之后"超时不重试"（第 6359-6366 行）
```cpp
QString why_ready;
if (!wait_page_ready(page_widget, 2000, &why_ready)) result.notes.append("页就绪诊断：" + why_ready);
```
⇒ **只记 note，断言照旧判红**（与我 `45_smoke_p3.md` 记的 `wait_page_ready` 契约一致）。

## 2. P4 的四条"宿主组件真的被用上"（第 6378-6437 行）
- **虚拟列表**：`provider_calls <= 8`（"应当只为可见行取，远小于 200"）＋
  **`row:199` 不许出现在 provider 阶段**（"把没显示的行也取了"）＋ 按需读第 199 行**真的能取到**；
- **日志视图**：`viewtext:第一行|第二行`、`after_clear:[]`；
- **拖入区域**：造**真 `QDropEvent`** 派发给 `FileDropFilter::dispatch`（离屏没有平台拖放）；
- **任务壳**：`run:1`、`worker_done`、`progress(100)` 到宿主进度条。

★ P16-3a 多选三连：`single_selected_rows == [7]`（不回归）/
`multi == [1, 5, 9]`（设 `[5,1,9,1]` ⇒ **升序去重**）/ `single_after_off == [1] && selected_after_off == 1`。

★ P17-82：插件造的 list/tree **自动**装了宿主拖放过滤器（`is_attached_to`），
且 `dispatch` 之后 Python 回调收到**全部**路径（`C:/tmp/b.txt,C:/tmp/c.txt`）。

★ P16-3b-2：`set_self_scrolling_body(true)` 的视口 —— 拖条子 ⇒ `list->scroll_top() == target`；
画布几何 `x==y==0 && height <= 视口高 && height < content_height()`。

## 3. ★★★ P16-3c（第 6565-6623 行）—— 判据本体，与一个**脆弱点**

### （a）完成条件：等**宿主回调**（不是轮询控件）
```cpp
const bool ok = wait_until([&]() {
    const QString page = QString::fromStdString(xrkit::plugin::current_page_id());   // ★ 关键
    const QString key  = page + "|button_on_menu";
    const int n = menu_ready_actions.contains(key) ? menu_ready_actions.value(key)
                 : menu_ready_actions.value("button_on_menu", -1);   // 裸键：仅诊断
    return n == 2;
}, 2000);
if (!ok) notes.append("P16-3c 诊断：等两项菜单 2000ms 未就绪（actions=%1）");
```
- 注释："只等第一项会漏掉'**第二项没装进去**'那一类"；
- 超时**照旧判红**，note 带 `actions`。

### （b）三条断言
```cpp
check(marker_value("menu_ok") == "1" && two_mode && two_mode->menu() && actions == 2, …);
if (菜单两项) { two_mode->menu()->actions().at(1)->trigger(); … check(pick == "1"); }
```
★ 注释（第 6613 行）："触发后必须**重读** mark 文件（上面的 `text` 是**早先快照** ⇒ 恒空）"。

### （c）★★ 脆弱点：键里的 `page` 取自 **`current_page_id()`（当前页）**

而 `hub.cpp:829-831` 里 `render_plugin_slots()` **每注册一个 tool 就 `set_current_page(0)`**
⇒ 若 `notify_menu_ready` 在**页面已经切走之后**才被调用（例如 builder 被延后执行、
或菜单构建发生在某个 queued 回调里），键就会变成 `fs|button_on_menu` ⇒ **查不到** ⇒
落回裸键 `-1` ⇒ 等 2000ms 超时 ⇒ note 记 `actions=-1`（或 0），随后那条 `check` 也可能因为
`two_mode->menu()==nullptr` 而红。

⇒ **这正是"合法场景下偶发红"的形状**，也是"**用当前页当身份**"的经典坑（与 `hub.cpp` 的
  `notify_menu_ready(id, actions, page)` 那次修复同源 —— 那次把 page 改成**菜单构建点捕获**，
  而**判据侧仍在读 `current_page_id()`**，两边口径不齐）。

★★ 这给了一条具体、可落地的修法：**判据侧也用"构建点捕获的 page"** ——
  即 `menu_ready_actions` 里存**两套键**（`page|id` 与 `id`），判据只用裸键 + 断言"只有一个 page 命中"；
  或者让 `notify_menu_ready` **回传 page** 到文件/标记里。

## 4. ★★ C-13：日志落盘 + 一条"不要删正在被打开的文件"（第 6640-6668 行）

修前伤：`Hub::log_line` 只进 LogBox ⇒ 用户运行出来的 `logs/*.txt` 是 **0 字节**。

★★ 而**真因**（用户 2026-10-20）：
> 修前这里 `QFile::remove(hub.log_file_path())` —— 那是**正在被 `core::log` 的 `g_file` 打开着**的文件。
> Windows 上要么删不掉，要么目录项没了而句柄还指向旧文件 ⇒ 之后写进去的标记按路径读回**看不到**，
> 看起来像"sink 丢写"。改成**切一份新文件**再写标记、再读（产品路径不动）。

判据三条：`MARKER-C13-UI` 在文件里、`MARKER-C13-ENGINE` 在文件里、**文件不是 0 字节**。

## 5. ★★ P17-1/PLG-01：插件页「取消」的四条判据（第 6727-6850 行）

修前事实：`PyUi::available()` 把"非 UI 线程调 `ui.*`"**一刀切拒掉**，而**取消谓词是插件 worker
在自己的工作线程里同步轮询的** ⇒ `ui.task_cancel_requested` **恒 False** ⇒ 四页的「取消」
关不掉、任务照跑、状态栏还谎报"已取消"。

四条（缺一不算过）：
1. worker 里**读得到**取消位，且没点取消时是 `False`；
2. 点宿主那颗**真「取消」按钮**之后，同一个读变成 `True`、循环当场退出；
3. worker 里碰**会改控件**的 `ui.set_text` **仍然被拒**（白名单没把纪律冲掉）；
4. 取消**真的让任务停下来**（`loops` 远小于 2000 —— 不是"跑满刚好退出"）。

★★ 夹具里一条自己的血账（第 6767-6770 行）：
> ★ 2026-10-21：**env 必须单独一行** —— 挂在值行后面会把 `cancel_read:0` 变成
> `cancel_read:0 pid:... tid:...`，而判据做的是 `marker_value(...) == '0'` 的**精确比较**
> ⇒ **我自己把判据弄红了**（实测：取消位读到 1，检查仍报"没读到"）。

★ 以及"写完做一次**读回校验**再构造 Hub"（第 6828-6838 行）：
"写成功 ≠ 读得到（截断/缓存/杀软扫描都会让'看起来写了'）"。

★ 页没注册时**先让 `plugin_errors` 说话**（第 6846-6850 行）：
"页没注册有两种可能（加载竞态 / 夹具没写成），必须留下可读证据，**不能靠等 2 秒去糊**"。

## 6. 待继续
`smoke.cpp` 6851-7850（P5 / 13.x / ⑬ 版本资源等）。
