# `src/shell/widget_facade_qt.cpp`（1364 行，全文读完）—— **P5 的根在这里**

> 这是控件门面的 **Qt 实现**（`kQtFacade`，约 100 个 lambda）。它与 `python → PyUi → UiFacade`
> 那一端拼起来，整条配置链就完整了。

---

## 1. ★★★ `resolve_parent()`（第 286-307 行）与 `place()`（第 326-366 行）

```cpp
QWidget* resolve_parent(plug::UiHandle parent) {
    QWidget* widget = unwrap(parent);
    QWidget* resolved = widget;
    if (resolved == nullptr) resolved = unwrap(plug::current_panel_root());
    ... trace "resolve|parent_arg=… resolved=… current_root=… same=…" ...
    return resolved;
}

void place(QWidget* parent, QWidget* child) {
    if (parent == nullptr || child == nullptr) return;   // ★ 静默返回，**不记 trace**
    ...
    layout->addWidget(child);
    trace "place|parent=… layout=… before=… child=… after=…"
}
```

### ★ 这解释了"空页且无痕迹"的完整路径

1. `resolve_parent` 返回 **nullptr**（parent 为空 **且** `current_panel_root()` 也空/已死）；
2. `table()->label(nullptr, wrap(nullptr), …)` ⇒ `plugin_ui.cpp` 造出一个**顶级控件**（不在任何容器里）；
3. `place(nullptr, child)` ⇒ **第 327 行直接 return** ⇒ **没有 `place` trace**；
4. 于是：**控件造出来了、容器里什么都没有（`anything=0`）、没有异常、没有 WARN、没有 trace**。

⇒ 与 `PyUi::available()` 的两类 WARN 合起来看，判别表就是：

| 现象 | 含义 |
|---|---|
| 有「控件门面不可用」WARN | 门面没装/缺方法 |
| 有「只许在宿主 UI 线程调用」WARN | 线程闸门拒了 |
| 有 `resolve|… resolved=0` 且**没有**对应 `place` | ★ **parent 为空**（本文件第 327 行静默返回） |
| 有 `resolve|… resolved=X` 且 `place|parent=X` | 控件真落进了容器 ⇒ 页不该空 |

**⇒ P5/P12c-2 的"空页"几乎可以断定落在第 3 行那个形态**（`resolved=0`）。
下一批唯一要确认的是：**为什么那一次 `current_panel_root()` 是空的**（Hub 传进来的 parent 又是空？）
—— 而这条链只剩 `hub.cpp` 的 `sync_pages()` 没读。

---

## 2. ★★ S-01：句柄活性注册表（第 222-268 行）

修前 `alive()` = `unwrap(widget) != nullptr`，而 `unwrap` 就是 `reinterpret_cast` ⇒
**QWidget 析构后指针不变 null ⇒ alive() 永远 true** ⇒ 之后解引用就是悬垂访问。

修法：每个发出去的句柄登记一个 `QPointer<QWidget>`：
- `handle_of()` 是**唯一发句柄入口** ⇒ 在这里登记；
- `unwrap()` 是所有 L1/L2 取控件的**唯一入口** ⇒ 已登记但死了 ⇒ 返回 nullptr；
- `alive()` 已登记看 QPointer、**没登记按旧口径（非空）**（宿主自有句柄不能替它编结论）；
- **表不删死项**（删了会退回"没登记⇒true"，正好抵消本修法）。

⇒ 与 `LinkProbe::StatFailed`/`LooseInfo.truncated` 同族：**"不知道"要单独一档**。

---

## 3. ★★ `on_ui_thread()`（第 1006-1023 行）——**第二道加固是死代码**

```cpp
[]() {
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) return true;                       // 判不了 ≠ 拒绝
    const bool same_qthread = QThread::currentThread() == app->thread();
    const bool same_std = (ui_thread_id() == std::thread::id())
                              ? same_qthread
                              : (std::this_thread::get_id() == ui_thread_id());
    return same_qthread && same_std;
}
```

注释说"`ui_thread_id()` 只在主线程装配时登记"，但**全仓搜索没有任何一处给它赋值**
（只有 `widget_facade_qt.cpp:1019/1021` 在读它）⇒ `ui_thread_id()` **恒为默认构造值**
⇒ `same_std` 恒等于 `same_qthread` ⇒ **第二道检查是死代码**。

⇒ 结论：
- 注释与实际不符（"只在主线程装配时登记"——没有登记代码）；
- **实际生效的判据只有 `QThread::currentThread() == app->thread()`**；
- 而注释里说的"实测出现过工作线程里调 ui.label 没有被拒"，正是这条判据的已知弱点。

★ 这是本仓"注释比实现强"的又一例，而且是**安全相关**的（线程闸门）。
  修法很轻：在 `install_qt_ui_facade` 的主线程分支里加 `ui_thread_id() = std::this_thread::get_id();`
  **但我没有改**（本轮只读）。

---

## 4. `kQtFacade` 的三种 lambda 形态

1. **L1（造控件）**：`resolve_parent` → `table()->xxx(nullptr, wrap(host), …)` →
   `place(host, unwrap(created))` → `handle_of(created)`。
   ★ 注意它把 `host` 既当 parent 传给 `plugin_ui`，又自己 `place` 一次 ——
   `plugin_ui` 那一层用 `pack` 语义、这里再 `place` 进纵向布局。
2. **L2（改控件）**：`qobject_cast<XxxWidget*>(unwrap(widget))` → 直接改（null 就静默返回）。
3. **新能力（list/tree/task/viewport/split/tabs/drop_zone）**：直接造宿主自己的组件
   （注释第 690-693 行解释了为什么这几件可以直接碰 Qt）。

## 5. 三处"必须记下来"的实现细节

**（a）`context_menu` 的回调接在 `QAction::triggered`（第 650-657 行）**
> `exec()` 只在"用户点菜单项"那条路径上返回被选项，程序里 `trigger()` 或 `close()` 都会让它
> 返回 nullptr ⇒ 用返回值接回调会**静默丢事件**（判据现场就是这样空的）。

**（b）`split_set_collapsed` 用 `hide()` 而不是把 sizes 压 0（第 1120-1123 行）**
> 别用"把 sizes 压到 0"：那会被 `minimumSizeHint` 顶回去（本批自检第一次就是这么红的：
> `setSizes` 之后第 3 格仍是 239 px）。

**（c）`place()` 的"字段行"分组（第 321-353 行，P16-2c）**
上一件是 `QLabel`、这一件是 `QLineEdit` ⇒ 两件成组进 `field_row`。
注释写明：**"正确的修法是宿主把行容器真的给插件页，不是改判据"**
—— 因为判据「⑭ 标签背后不许有深灰底板」扫 `field_row`，候选集为 0 就红（自带"空集即红"自证）。

## 6. `install_qt_ui_facade` 的 sink 归属（第 1332-1362 行）

- **只有主线程的装配才设置 `log_sink`**（修前每个 Hub 都覆盖 ⇒ 后装配的抢走日志、前一个析构后悬垂）；
- `g_log_sink_owner` 记归属；`clear_qt_ui_facade_log_sink(owner)` 只有 owner 一致才清。

⇒ 这条直接关系到"日志要记 app 内操作（LogBox）"那条判据：
  若装配发生在**非主线程的 Hub**（自检里就有），`log_append` **不会被设置** ⇒
  `ui.log()` 静默无效 ⇒ LogBox 里没有那行。**这又是一个"判据读数依赖装配线程"的坑。**

## 7. 一处代码卫生问题（我造成的）

第 666-681 行（`context_menu` 里的 menu_build/menu_ready/menu_show 三条 trace）缩进错乱
（我 2026-10-23 打补丁时没对齐）。功能正确，但下次改这段要顺手格式化。

---

## 8. 这批的结论（三条）

1. **"空页且无痕迹"的唯一形态**：`resolve` 的 `resolved=0` **且没有对应 `place`**（`place` 第 327 行静默返回）。
   ⇒ 判据/诊断应当**在 `place(parent==nullptr)` 时记一条 trace**（现在完全没有）。
2. **`on_ui_thread` 的第二道检查是死代码**（`ui_thread_id()` 从未赋值）——注释与实现不符。
3. **`log_append` 只在主线程装配时生效** ⇒ 非主线程构造的 Hub 里 `ui.log` 是静默无效的。

## 9. 待继续

`src/shell/hub.cpp`（818）—— **`sync_pages()` 是 P5 的最后一环**（谁给 builder 传了 nullptr parent）。
