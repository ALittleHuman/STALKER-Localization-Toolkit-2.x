# `include/xrkit/engine/plugin/widget_facade.h`（392 行，全文读完）

> 这是**整个插件体系的核心契约**，P5 / P13 / P16 / P17-1 / P12c-2 全部落在它的某一行上。
> 下面每条都标了行号，可直接对照。

## 1. 文件为什么叫 `widget_facade.h` 而不是 `ui_facade.h`（第 4-7 行）

Qt 的 **AUTOUIC** 会把 `#include "ui_*.h"` 当成 `.ui` 生成物，导致外壳 `AutoUic error`。
⇒ **文件名是躲 AUTOUIC 的**，类名/函数名仍是 `UiFacade` / `set_ui_facade`。
**推论**：任何"整理命名"的冲动都不能把它改回 `ui_facade.h`，否则 CMake 直接红。

## 2. 三条口径（第 14-21 行）

1. **门面由外壳注入**（`set_ui_facade`），引擎（Qt-free）只持指针转发；没门面时每个方法返回 null/0
   且**逐条记账**（零静默），但**注册面照常可用**（无 GUI 进程也能驱动注册）。
2. **表外无门**：Python 能看到的只有本文件的字段；判据是
   `tests/check_ui_capability_table.py` 把 `dir(xrkit.Ui)` 与预期表对账，**多一个就红**。
3. **`parent==nullptr` ⇒ 落到"当前面板根"**（第 20-21 行）——这就是 `UiPanelScope` 存在的理由，
   也是 P5 那条"root 生命周期"的总根。

## 3. 控件下限三条（与 P16-3a/3b/3c 一一对应）

| 行 | 方法 | 口径 |
|---|---|---|
| 136 | `list(parent,row_height)` | 虚拟化单列列表 |
| 142 | `list_self_scrolling(parent,min_body_height)` | 容器+ScrollViewport+VirtualList 当 body，**句柄＝容器** |
| 147-148 | `button_on_menu(button, items, on_choice)` | **左键**动作菜单，回调给**下标** |

★ 第 143-146 行明确写着：`button_on_menu`（左键点按钮）**不是** `context_menu`（右键，第 123-127 行）。
两个是**不同的路**：前者 `setMenu`，后者 `exec`。⇒ 我们这轮加的 `menu_build/ready/show` 三事件
必须**两条路都发**，否则 P13 与 P16 必有一个等不到（当时的判断是对的）。

## 4. 树（第 245-318 行）

- **懒建**：`has_children=true` 只画箭头，用户真展开才 `on_expand(key)`，插件再 `tree_set_children`（246-249）。
- **三态勾选**：向下传播、向上重算祖先（249-250）。
- **过滤**：宿主按"任一列包含子串"匹配，命中行连祖先一起显示（251-252）。
- `TreeNodeSpec{key,cells,has_children,checkable,checked,has_action,tooltip,matched}`（258-273）。
- 行尾动作 `tree_on_action` 回调给 **key**（不是行号），只有 `has_action` 的行会画（283-288）。

## 5. TaskPanel / `task_run`（第 189-207 行）——★ P17-1② 的契约在这里

- worker 在**工作线程**跑，只拿到两个 sink：`progress(0~100)` / `message(text)`；
  **工作线程里只许调这两个**，其余门面方法一律回主线程（192 行）。
- `task_cancel_requested(panel)` 是**取消谓词**（201 行）。
- `task_on_message`（206-207）是**回主线程通道**：worker 里 `message(...)` 之后排队回主线程再调，
  **回调里可以直接碰控件** —— 这是 P12c-2「回调要改控件」的**唯一合法路径**。

## 6. ★★ 第 334-351 行：`task_cancel_requested` 的跨线程白名单

这段注释就是 P17-1② 的历史：

> 修前事实（PLG-01）：闸门一刀切 ⇒ worker 里读取消位**恒为 false** ⇒ 用户在四页点「取消」关不掉，
> 任务照跑到结束，状态栏还谎报"已取消"。

判据：**只读、不碰控件**的方法才准进白名单；实现侧必须线程安全
（`task_cancel_requested` 只读 `TaskShell` 的原子取消位；`on_ui_thread` 只比线程 id）。
名单在 `src/engine/plugin/widget_facade.cpp`（与 `kMethodNames[]` 同一处，便于对账）。

⇒ **对 P17-1② 的直接推论**：`task_cancel_requested` 已经在白名单里，所以"worker 里读到"这条路
**是通的**；我们看到的"没读到"要么是**读的时机**（取消位还没置），要么是**夹具读的不是这个方法**。
下一轮该查夹具里到底调的是 `task_cancel_requested` 还是别的。

## 7. 当前面板根与页面身份（第 353-390 行）

```cpp
void set_current_panel_root(UiHandle root, const char* reason = "other");   // 356
void set_current_page_context(const std::string& page_id, Registry* registry); // 359
UiHandle current_panel_root();                                             // 371
std::string current_page_id();                                             // 369

struct UiPanelScope {                                                      // 374
    UiPanelScope(UiHandle root);                                           // 375  ← 只设 root
    UiPanelScope(UiHandle root, const std::string& page_id, Registry*);     // 377  ← 连页面身份
    ~UiPanelScope();
    UiHandle previous_ = nullptr;          // 383
    UiHandle scope_root_ = nullptr;        // 385（dtor 用来判 lifo）
    bool restores_page_context_ = false;   // 387（只有 3 参构造置位）
    std::string previous_page_id_;         // 388
    Registry* previous_registry_ = nullptr;// 389
};
```

★ **关键结构性事实**：
- 只有 **3 参构造**才恢复 page context；**1 参构造（`py_host.cpp` 的 Python 包装用的就是它）不碰 page_id**。
- 也就是说：**Python builder 里看到的 `current_page_id()` 是 Hub 那层 3 参 scope 设的**，
  而不是 py 包装设的 ⇒ py 层的 dtor 只恢复 root，不动 page。
- `scope_root_`（385）是我 2026-10-22 加来判 `lifo` 的；判据实测**全部 lifo=1**，乱序假设已排除。

## 8. trace 与 flush（第 361-368 行）

- `trace_event(kind, detail)`：引擎侧唯一入口，无 Qt，写 `%TEMP%/page_trace.txt`
  （**实际已改成 `page_trace_engine.txt`**，注释还没跟着改 —— 一处待修的小不一致）。
- `flush_page_trace()`：必须在 `xrkit_app` 退出前调；引擎自己注册了 atexit。

## 9. 由这份文件直接得出的三条判断

1. **P13 与 P16 的菜单是两条路**（`exec` vs `setMenu`），判据必须分别等，不能共用"popup 出现"这一个信号。
2. **P17-1② 的"取消关不掉"是历史 bug（PLG-01），已在契约层修好**（白名单）；
   现在的偶发红**不是**契约问题，要去查夹具/时机。
3. **P12c-2 的回调必须走 `task_on_message`**（206-207）才能"合法改控件"；
   若夹具直接在 worker 里 `set_text`，闸门会拒（这正好能解释 `worker_thread_ui:True` 与
   `worker_ui_thread:0` 的组合读数）。

## 10. 待核对（下一轮）

- 第 361-363 行注释里的文件名与实际写盘文件不一致（`page_trace.txt` vs `page_trace_engine.txt`）。
- 第 336-347 行说名单在 `widget_facade.cpp`，实际我已在其中加了 `py_builder_enter` 等；
  需核对 `kMethodNames[]` 与本文件的字段**逐名一致**（`check_ui_capability_table.py` 依赖它）。
