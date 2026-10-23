# `smoke.cpp` 批十六（第 7351-7850 行）—— ★★★ P13-2a-2 的采集竞态（已在 HEAD 修掉的那一条）

---

## 1. P6 夹具（树/标签页/行尾动作/右键/列表激活）一次造齐（第 7364-7457 行）

夹具里有 **三套"独立写文件"的回调**（`on_row_action` / `on_ctx_choose` / `on_list_activate`），
注释点明原因（第 7438 行）：
> ★ 纯 Python 写文件：**不**再进 C++ 的 `mark()`（**上一版崩点就在这条再入链**）

⇒ 这是"**回调里再回到宿主记录器**"导致崩溃的实测修正（与 `font_picker` 那条定时器链同族）。

## 2. P15-1a 标签页四条（第 7489-7539 行）
`tabs->count()==2` / 两个标签文字 / **塞进第 2 页的控件真是那一页的后代**
（`tabs->widget(1)->isAncestorOf(button)`）/ 初始 `2,0` / 点「切到第二页」后
**Qt 侧 `currentIndex()==1` 且 Python 侧 `2,1`**。

## 3. P15-0 三条（第 7541-7609 行）
- **多选按树序**：`selected_multi:A,B`；
- **点行身体**：`invokeMethod(itemClicked)` ⇒ 插件收到 `B`；
- **输入变化**：`probe->setText("abc")` ⇒ 回调拿到 `abc`。

★ 第 7545 行的完成条件："等句柄能被 `qobject_cast` 成 `FacadeTree`（≤1000ms）"。

## 4. P13-1a 行尾动作的四条（第 7612-7698 行）
- **完成条件**：等"带 `ra1` 的门面树"出现（≤2000ms），超时记 note（tree 数 + childCount）；
- ★ **手搓一格**（第 7650-7651 行）："不依赖真实布局：树没显示时 `visualItemRect` 是空矩形 ⇒ 命不中"；
- 四条断言：`eaten`（吃掉事件）/ `fired == "ra1"` / **方块外不触发** / **没有动作的行不触发**。

## 5. P13-2a 右键菜单（第 7700-7742 行）
`QTimer::singleShot(0, …)` 在**嵌套事件循环**里抓 `activePopupWidget()`，
收标签 → 触发「甲」→ `popup->close()` 退出 `exec()`；
断言 `seen` 含甲/乙、且插件收到 `甲`。

## 6. ★★★ P13-2a-2：采集竞态 —— **已在 HEAD 修掉的那一条**（第 7754-7839 行）

两段注释合起来就是完整病历：

### （a）第一版：`wait_until` 轮询 ⇒ **挂死**
> ★★ 2026-10-22：**必须用 QTimer 在模态循环里采**。上一版用 `emit` 之后 `wait_until` 轮询
> ⇒ `QMenu::exec()` 开的**嵌套事件循环**把调用方挡住、popup 永远没人关
> ⇒ 出包自检**挂死**（实测 00:53 起的 `xrkit_app` 一直不退出）。

### （b）第二版：**无条件 `stop()`** ⇒ **采集窗口被提前关掉**
```cpp
emit only_tree->customContextMenuRequested(QPoint(9, 9));
// ★★ 2026-10-23（用户口径）：**不许无条件 stop** ——
//   走 QMenu::exec() 时 emit 会阻塞、timer 在嵌套循环里能跑（seen 非空）；
//   走 popup() 非模态时 emit 立刻返回，这里 stop 会赶在第一个 10ms tick 之前
//   把采集窗口关掉 ⇒ seen_only 永远为空（这正是 P13-2a-2 的红）。
const bool took_in_nested_loop = !seen_only.isEmpty();
if (took_in_nested_loop) menu_timer.stop();
else { wait_until([&]{ return !seen_only.isEmpty(); }, 1000); menu_timer.stop(); }
```

★★★ 这段**就是我在早前那轮亲手修过的那条**（"P13-2a-2 `menu_timer.stop()` 无条件 ⇒
popup 路径一条都没采到"，修法正是"分支 + `wait_until`"；实测 P13 由 ×3 降到 ×1）。
⇒ 说明**那次修复已在 HEAD 里**，所以 P13-2a-2 现在若还偶发红，原因**不是**这个采集窗口了。

### （c）还留着的两处可疑
1. **`:7841-7845` 又用 `current_page_id()` 拼 `context_menu` 的键** —— 与 P16-3c 同一个脆弱点；
2. **`:7834-7839`**：采不到时把 `menu_timer_fired` 改写成 `actions=-1 popup=0` ——
   这个"补记"会**覆盖**掉先前真采到的那行 trace（同一个 event 名），诊断时容易误读。

## 7. ★ P13-2a-2 的"正向承诺"（第 7744-7747 行）
> 注册表只认内置六页那几个 host（`Registry::bad_slot` → `known_host`）⇒
> 无 `replaces` 的插件页**本就不该**有别的插件的 context 命令；
> 而"无注册即无痕迹"（1.x）意味着**连分隔符都不许加**、菜单里只有插件自己的标签。

⇒ 所以这条判据的期望是**恰好两项（甲/乙）**，不是"至少两项"。

## 8. 待继续
`smoke.cpp` 7851-8850。
