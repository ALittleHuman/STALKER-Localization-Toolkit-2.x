# `smoke.cpp` 批四（第 1851-2350 行）—— 滚动余下 / 分栏 / 日志栏 / 任务壳

---

## 1. ★★ ⑦-g：原生滚动区 vs 外层视口（第 1938-2004 行）—— 用户真机缺陷的能红断言

用户原话：
> 「把日志调出来，**光标放在小滚动条的窗口内滚轮就不是控制小滚动条而是控制大滚动条**，
>  把日志收起就正常。日志内也是正常。」

机制：树/文本框是 `QAbstractScrollArea` 而**不是** `ScrollViewport` ⇒ 旧实现第一圈
"找第一个能滚的视口"会把它们**整圈跳过** ⇒ 直接命中**祖先页视口**并把它滚了；
"日志一收起就正常"正是因为那时外层视口装得下（`range==0`），才轮到内层自己滚。

判据用**公开决策入口** `decide_wheel`（无副作用）：
- 光标在树里 ⇒ `decision.bar == tree_bar && decision.owner == inner_tree`；
- 光标在视口空白处 ⇒ 外层 `vbar()`；
- 光标在**装得下**的普通控件上 ⇒ 外层（1.1 冒泡）；
- 再真发一次：内层动、外层**一步都不许动**。

判红办法：把 `decide_wheel` 改回"先只找 ScrollViewport、找不到再看原生滚动区"。

★ 手法值得抄：**把决策抽成无副作用的纯函数**，判据考它 + 再考一次真实效果。

## 2. ⑦-d/e/f：fit 规则、显式重排、嵌套路由（第 1856-1936 行）
- `fit=Viewport ⇒ 内容高 = max(minsize, 视口高)`；`fit=Content ⇒ 不小于 viewport`；
- 显式重排：竖条贴右（`geometry().right()==width()-1`）、横条贴底、画布吃剩余宽；
- 嵌套：内层能滚 ⇒ 内层动、外层不动；内层改成装得下 ⇒ **让给外层**。

## 3. ⑧ 分栏（第 2007-2099 行）
- 命中区 `max(6, px(5))`；命中/区外各一条；
- **2.1 拖动期一步几何都不改**（`pane_size` 与控件 `height()` 都不变），只有预览位置变；
- 松手一次重排：`pane_size(0) == size_top+30`、`clamp_count() == before+1`；
- 4.2 收起后**改窗口尺寸不弹回**（`resize` + `request_clamp` + 等 180ms）；
- **2.5 去抖**：连续 5 次 `request_clamp()` 期间 `clamp_count` 不变，到点只 +1；
- **2.7 成本口径必须含重绘**（`repaint()`）—— "只跑 `update_idletasks` 会低估约一半"；
  上限 30ms（PySide6 试点 12~15ms）。

## 4. ★★ ⑨ 日志栏（第 2101-2184 行）

### 4.4 LogBox 三个计数
`append_batched(500)` ⇒ `pending()==500 && toPlainText().isEmpty()`（80ms 内不动文档）；
**用 `wait_until(flush_count>0, 2000)` 代替 sleep**（我 2026-10-22 改的）；
`blockCount()==500`；`clear_log()` ⇒ 1 行 + pending 0；6000 行 `flush()` ⇒ `trim_count()==1 && ≤3000`。

### 4.1 日志不归窗口那根管
`hub.log_box()->verticalScrollBar() != nullptr`（自带条子）；
`qobject_cast<ScrollViewport*>(hub.centralWidget()) == nullptr`（中央区**不是**窗口级视口）。
★ **日志面板默认折叠**，判据用 `isHidden()` 而不是 `isVisible()`
—— "离屏下 Hub 没 show，`isVisible()` 恒 false（父窗隐藏）"。

### ③ Hub 启动即挂落盘 + 界面行进文件
- 先把 `core::set_log_file("")` **清空全局状态**再建 Hub ⇒ 判据"测的是 Hub 自己"
  （"拆掉 ctor 里那行 ⇒ 红"），而不是被上一个 Hub 顺手挂上的状态兜着；
- 先把目录清空 ⇒ 保证"连着两次导出落在同一秒的同一个基名"这条路径可测；
- 判 `log_file_path()` 非空 + `log_file_active()` + 文件存在；
- ★★ 决策 5.5：**「导出日志」按钮已删除** ⇒ 判据**反过来**判"按钮不许回来"
  （`findChild<QPushButton*>("log_export") == nullptr`）；
- `log_line(marker)` + `flush()` ⇒ 文件里必须含 marker，且**不许 0 字节**。

## 5. ★★ 13.x 任务壳（第 2220-2345 行）

### （a）13.1 重入那条**原来有两个洞**（第 2228-2239 行）
修前：`check(!task.run("第二次", nullptr) || true, …)`
- `|| true` ⇒ **整句恒真**（无论结果如何都绿）；
- 传 `nullptr` worker，而 `task_shell.cpp` 在 worker 为空时**直接 return false**、
  **根本没走到"重入拒绝"那条分支** ⇒ 名字声称的"重入被拒"**一点都没被测到**。

现在：喂**真的 worker**，把"第二次必须被拒"钉在返回值上，并用 `rejected_count()==1` 作牙齿。

### （b）13.2/13.3 事件序列
`TaskShell::Event{kind, value, on_main_thread}`：`p`=进度 `m`=消息 `e`=错误 `f`=结束。
判据：`all_main`（所有 UI 更新都在主线程）+ `order` 里 `e` 在 `f` 之前 + 越界值夹到 100。

### （c）★★ P17-4：线程槽位不许只增不减（第 2287-2310 行）
顺序跑 5 轮、每轮等收工 ⇒ `worker_slots() <= 1`（修前＝5 ⇒ 红）。
★ 一条踩坑记录：**变量名避开 Qt 的 `slots` 宏** —— 上一版叫 `slots` 直接 `C2059/C4091`
（"Qt 把 `slots` 定义成空宏 ⇒ 声明被吃掉"）。

### （d）★★ P17-5：`report_*` 从 worker 调必须安全（第 2312-2345 行）
两条判据：
1. worker 里调完**当场不许改控件**（说明它排队了，不是直落）；
2. 泵一次事件之后控件必须被改，**且那条事件记的线程位是"在主线程"**。
判红办法：把 `report_message` 换回直调 `apply_message` ⇒ ①立刻红。

## 6. 待继续（2351 起）

取消（①）余下、⑩~⑬ 各页、P4/P5/P13/P16/P17、⑬ 版本资源。
