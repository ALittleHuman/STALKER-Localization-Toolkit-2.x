# `smoke.cpp` 批十三（第 5851-6350 行）—— ★★★ P5/P12c-2 空页的**两条真机制**在这里

---

## 1. P2b：Python 插件在 GUI 上的四件事（第 5840-5891 行）
夹具现写进临时目录，四件判据：① 命令出现在 fs 页工具栏（`objectName` 前缀 `plugin_smoke_hello`）；
② **点按钮 ⇒ 插件目录里出现 `clicked`**；③ 无插件问题；④（`write_fixture_safely` 落盘成功）。

★ 注意第 5877 行的细节：**标记文件读完就关** —— "下面要删整个目录（Windows 上开着删不掉）"。

## 2. ★★ P3：控件门面的五项（第 5893-6171 行）
① 页进页表；② L1 控件都按宿主排版落了位；③ 点按钮回 Python 且 `get_text` 读到真值；
④ `set_text` 改得动控件；⑤ `ui.log()` 进宿主日志栏。

★★ P17-90/91 两条**契约参数**判据（很值得抄）：
- `ui.entry(None, textvariable='契约写法初值', width=20, show=False)` ——
  修前绑定叫 `initial`/`password` ⇒ **契约写法直接 TypeError**（"假缺口真存在"）；
  判据同时看**控件上真有那个初值** ＋ **标记里那行原文对**（失败时把 Python 抛的原文贴进消息）；
- `ui.drop_box(None, None, '契约宽度框', 137)` —— 修前门面**硬写 0** ⇒ 宽度**静默丢掉**；
  判据：`maximumWidth() == core::px(137)`，"把门面那第三个实参改回 `0` ⇒ 立刻红（能报出实得值）"。

## 3. ★★★ P12c-2 诊断行（第 6103-6146 行）—— 我认得的那个 `labels_count` 就在这里

```cpp
QStringList labels_cb;  QWidget* labels_root = page_widget;
result.notes.append("P12c-2 诊断：page=%1 child=%2 placeholder=%3 page_widget=%4 "
                    "labels_root=%5 labels_count=%6 stack_widget=%7 same_stack=%8 …");
```
★ 两处迭代痕迹：
- 第 6115 行："★ 2026-10-23：**递归**找占位文案（上一版只看直接子件 ⇒ **误报 placeholder=0**）"；
- `placeholder` 三态：`0` / `1`（含"业务内容按 M2-6"或"逐片接入"）/ `no-text`（一个非空标签都没有）。

★ 还写了一条 `read|<t>|<tid>|<pid>` 进 `l2_trace.txt`：
"把'判据开始读标记'的时刻也写进 l2_trace.txt（与夹具的 build 事件同一格式），
这样能判'**是 builder 没跑完，还是判据读太早**'"。

## 4. ★★★ P4 夹具里那条**真机制**（第 6207-6220 行）

> 修前**每个 provider 行都 `open(..., 'a')` 一次**（200 行 = 200 次 open/close）。
> 只要一次被杀软/文件锁挡一下，`mark` 抛异常 ⇒ **provider 抛** ⇒ **builder 半路中断**
> ⇒ Hub 的 S-02 护栏把整页换成**占位页**（现场正好是"**标签是占位页文案、列表为空**"）。
> 现在：整个 build **只开一次句柄**、写后立即 flush，而且**绝不抛异常**（诊断 I/O 不许弄死被测对象）。

★★★ **这就是"空页 + 占位文案 + 列表为空"的一条确切成因**，且它**天然偶发**
（取决于杀软/文件锁/时序）—— 与我们观测到的"同一份代码 13~25/30 漂移"完全对得上。

而成因的**性质**是：**诊断代码（mark）把被测对象弄死了**。
⇒ 与 `theme.cpp` 那条"每次 apply_theme 都重造样式对象"、以及我们自己的"探针改变时序"同族。

## 5. ★★ `_trace` 与 `_guard` 的分工（第 6222-6265 行）
- `_trace(event)`：**独立** `l2_trace.txt`，一行 `event|monotonic|thread_id|pid`，
  只记 `build_start / built / error` —— "既判断'builder 没跑完'还是'判据读太早'，
  **又不会破坏 `marker_value` 的精确比较**（值行保持原样）"；
- `_guard(fn)`：包住 `build`，异常时把 `traceback.format_exc()` 写 `build_error.txt`、
  记 `_trace('error')`、**再抛**（让 S-02 也看得见）。

⇒ 这是"**诊断通道与判据通道分开**"的规范做法。

## 6. ★★★ 一条"白烧一轮编译"的夹具教训（第 6289-6296 行）

> ★★ 注意（本批实测踩到、白烧一轮编译）：这一段是**夹具的 Python 源码**
> （C++ 字符串字面量逐条拼接），所以**注释也必须写在字符串里面** ——
> 在字面量之间插一行 C++ 的 `//` 注释会把拼接链**打断**（`source` 只剩前半段），
> 生成的 `l2_demo.py` 于是**语法错、插件加载失败**，而 **C++ 这边照样编得过**。

⇒ 这给了一条**排查捷径**：空页 + 插件加载失败时，先去看生成的 `.py` 是不是被截断/语法错。
  （对照 `17_py_host_*.md` 记的 `host()` 能定位行号 —— 但 `--smoke` 的夹具是现写的，行号对不上仓库文件。）

## 7. ★ P17-82/83 的两条"有意不在 smoke 里测"
- `list_on_drop` / `tree_on_drop`：**在** smoke 里测（宿主造列表/树时自动装过滤器）；
- `ask_*` 的 `filetypes/initialdir/initialfile`：**不在** smoke 里测
  —— "真发对话框会在无人环境里阻塞、且观察不到" ⇒ 归**静态判据**
  `tests/check_ask_dialog_params.py`（逐项核对 pybind 收得下三个参数 ＋ 宿主真接上），并**自带反例自证**。

⇒ "**哪种判据该在哪一层**"的判断写在了代码里，不是随口一句"没验过"。

## 8. `ui.button_on_menu` 的真实判据（第 6313-6317 行）—— P16-3c 的实现侧
```python
mbtn = ui.button(None, '两模式', None)
mark('menu_ok:%d' % int(ui.button_on_menu(mbtn, ['选包文件…','选文件夹…'], on_pick)))
```
⇒ 门面返回 bool 被记进标记；smoke 后面据此判。**若这条 `mark` 没写出**（比如 provider 抛异常那条链），
  `menu_ok` 就读不到 ⇒ 后续"找按钮 / 点菜单"的判据全空转 —— 这正是我们 P16-3c 的观测形态。

## 9. 待继续
`smoke.cpp` 6351-7350（P4 余下 + P5 起点）。
