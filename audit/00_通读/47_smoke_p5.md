# `smoke.cpp` 批五（第 2351-2850 行）—— 取消 / 孤儿标签 / 虚拟列表 / 六页框架 / fs 页形态

---

## 1. ★★ ① 取消（第 2347-2434 行）—— 六条判据 + 一条反向锁

夹具：worker 循环 400 次、每次 5ms、**每轮读 `cancel_requested()`**（"取消：在循环边界停下"）。

| 判据 | 内容 |
|---|---|
| 起得来 | 13.1 闸门不变 |
| 按钮可点 | 跑起来时 `isEnabled()` |
| 点按钮**真的置位** | `cancel_requested() && cancel_requests()==1` |
| **真停** | `!running() && worker_stopped && stop_ms < 2000` |
| 留痕 | 事件流里有 `'c'` |
| **理由优先收尾** | `status() == cancel_reason()` 且含"已取消" |
| 按钮复位 | `disabled` + 文案回「取消」 |
| ★ **反向锁** | **没点取消**的正常任务 ⇒ 取消位恒假、`cancel_requests()==0` |

★★ 夹具里的**声明顺序**注释极重要（第 2351-2352 行）：
> worker 线程按引用捕获下面这几个原子 ⇒ 它们必须**先于** TaskShell 声明
> （TaskShell 的析构会 join 线程；后声明的先销毁 ⇒ 反了就是 **use-after-free**）。

⇒ 这正是"**判据自己的夹具会踩内存生命周期**"的实例，也是我这几轮反复看到的一类。

## 2. ★★ ②「封包/字库的取消整块删除」（第 2437-2463 行）

用户原话：「**封包和字库的取消，在这里本质是完全没有的，故直接删除即可。**」
口径：这两页**连按钮对象都不许存在**（"藏起来/置灰/点不动"仍是假的取消）；
判据形态是**双向**的：`fs.task()->cancel_button() == nullptr` ＋
`convert/text/xml` 三页**仍保留**。

## 3. ★★ F3「孤儿顶层控件」（第 2465-2508 行）

修前：`status_ = new QLabel(task_->status());` —— `TaskShell::status()` 返回 **QString**
（控件访问器叫 `status_label()`），于是 `QLabel(const QString&, QWidget* = nullptr)`
造出一个**无父标签** ⇒ 它是**顶层窗口**、从不进布局（全仓 `addWidget(status_)` 零命中）
⇒ 全页消息用户**一个字都看不见**，而且没人拥有它（泄漏）。

判据四连：`parentWidget() != nullptr` ＋ `page->isAncestorOf(label)` ＋ `!isWindow()` ＋
**页显示后真的可见且宽高 > 0**。

⇒ 又一次"**类型选错了但编译器不报错**"（`QString` 能隐式转成 `QLabel(text, parent=nullptr)`）。
  这与 `LinkProbe` 那种"二态编码"是同族：**API 形状允许静默降级**。

## 4. ⑩ 虚拟化列表（第 2510-2594 行）

- **10.2 规则逐组对拍**：`{top,view_h,h,total} → {first,last}` 5 组（含"夹到 total"与"末尾"两种边界）；
- 真列表：`visible_range()==(0,11)`、`built_count()==11`、**`provider_calls()==11`**（10.3 同族：只对视口内行取文本）；
- 滚动后范围变 (9,20) 而**池子不涨**（仍 11）；
- **池子与 total 无关**（100 行与 100 万行都是 11）；
- 换选一行的成本：`big < 30ms` 且 `big/small < 5x`（"与条数无关"）。

## 5. ★★ ⑤ 六页框架（第 2596-2728 行）—— **P5 判据的同源祖先**

- `page_min_h()` 必须是**函数**：`{1.0→430, 1.5→645, 2.0→860}`（"写成常量这三组全返回 430，当场露馅"）；
- ★ **5.3 的反面证据**：`fresh_viewport.min_body_height()==0 && page_min_h()!=0`
  ⇒ "**没设过 minsize 的视口默认就是 0** ⇒ '忘了设'必然与 `page_min_h()` 不等"
  ⇒ **所以下面那条判据是有牙的，不是自证**。
- 逐页断言：`minsize_ok()` ＋ `viewport()->fit()==Content` ＋
  **`body()->parentWidget()==viewport()`** ＋ **操作栏不在视口内** ＋
  探针按钮在 `action_layout` 里且**不在视口内**；
- ★★ 规则 2 的实测：**裸 Toplevel** 压到 `page_min_h()` 时操作栏必须还在
  —— "夹具必须是裸 Toplevel：Hub 式布局永远压不矮，挂在 Hub 上量这一条等于量不到（M2-2 踩过同一个坑）"；
- 5.1 另一面：**标签条与页栈不许被包进任何滚动视口**（"否则一滚主导航就滚走了"）。

⇒ **这就是 P5 那条判据的来路**：`page->minsize_ok()` 与 `fit()==Content` 是同一族的框架断言。
  而 P5 的"页不是 PageBase"分支（`dynamic_cast` 失败 ⇒ 走 else）在这里**没有对应判据** ——
  因为这里逐页跑的是 `hub.page_at(i)`，**只对内置页**（插件页若为占位页会 `page==nullptr` ⇒ `frame_bad++`）。

★★ 这给了一条**现成的修复方向**：把 `frame_bad`/`order_bad` 的计数式断言用在**插件页**上，
  而不是每页单独写一套 `wait_page_ready` + 特例判断。

## 6. ⑥ fs 页界面形态（第 2730-2850 行）

- 6.13：下拉**条数 == 格式表条数**；★ `auto` 必须**第一项**；
  `auto` ＝不给格式（`format_key_to_db("auto")` 无值；`format_key_is_auto` 为真）；
  **封包下拉不含 auto**（= formats.size()-1 条）；
- ★ 判据"下拉取值是内部键、显示名解析不出来"：
  ```cpp
  const bool key_ok = is_sqfs(key) || to_db(key).has_value() || is_auto(key);
  const bool label_resolves = is_sqfs(label) || to_db(label).has_value();   // 必须为假
  ```
  ⇒ 这是 1.x "存显示名 ⇒ `ALL_FMTS.get(fmt)` 必然取不到"那条事故的判据；
- 6.4：**页里没有 `drop_zone` 控件**、拖放直接挂在包列表（`acceptDrops()`）；
- ★★ 用户第 6 条"包列表和文件列表用同一个小组件"：可判红形式 = **同一套表头行为**
  （恰好一列吃剩余宽 + 其余 Interactive + `minimumSectionSize` 相同），
  并明确 **`acceptDrops` 不算对齐项**（实例级特性）；
- ★★「？」的硬判据：**列的总宽必须铺满视口**，否则右半边是死区
  （"改回去立刻红：真机 318 < 717"）。

## 7. 待继续（2851 起）

fs 页余下（勾选/解包/封包/上下文菜单）、其余五页、P4/P5/P13/P16/P17、⑬ 版本资源。
