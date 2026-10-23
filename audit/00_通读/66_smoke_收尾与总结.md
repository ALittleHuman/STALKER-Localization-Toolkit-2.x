# `smoke.cpp` 批二十四（第 11601-11989 行）—— **全文 11989 行读完**

---

## 1. 3-1 第二半：勾**一个包**（3 项）后再加包 ⇒ 仍 3（第 11603-11610 行）
第一半是"勾 3 项（`check_all` 真实入口）→ 加包 ⇒ 仍 3"；
第二半换成"勾**源行**（包行）→ 加包 ⇒ 仍 3"。

## 2. ★★ 3-2：三条判据 + 两条反向锁（第 11614-11664 行）
① 搜不存在的名字 ⇒ **树上 0 行**（修前：整棵树照显示）；
② **计数也是 0**（修前计数读的是"全量"）；
③ **反向锁①**：搜 `c.txt` **仍能看见命中项**（"别把过滤做成了'永远 0 行'"）；
④ **反向锁②**：清空查询 ⇒ 回到"不过滤"。

⇒ 这是"**正向 + 反向**成对写"的样板：只写正向就会把"永远 0 行"当成修好了。

## 3. ★★ 3-3：前提与真判据分开（第 11666-11712 行）
- 夹具用**认不出的文件**（`not-a-real-package`）⇒ 断言 `source_count() == 0`，
  并**明说这是前提**："这里的判据是「判重先于加载」这条路不许放两份进来"；
- 真判据①：`add_source`（跳过格式判定）用两种斜杠写法各加一次 ⇒
  **左树源节点数 == 源数**（修前同层同名覆盖 ⇒ 节点数会少）；
- 真判据②：直接问引擎 `core::path_equals(native, forwardSlash)` 必须为真。

★ 注释里如实交代了取舍："用 SquashFS 最小镜像夹具太贵 ⇒ 这里只锁'同一份路径的两种写法必须判成同一份'
这条纯判据（直接问引擎的判重实现）"。

## 4. ★★ 3-4：basename 升序 ⇒ **赢家是后者**（第 11714-11777 行）
夹具：`gamedata.sqzy_patch` 先加（大小 20）、`gamedata.sq_base` 后加（大小 10），
两者壳都是 `gamedata` ⇒ 同名文件真的合并；**basename 升序 ⇒ `sqzy_patch` 赢**。
判据两条：`item->text(2)`（来源列）含 `sqzy_patch`；`item->text(1)`（大小列）含 20。

★ 展开方式是**四轮"收集所有目录节点再逐个 expand"**（懒建：逐级展开）；
★ 诊断串里带 `all_paths` 与 `kid_counts`（"懒建占位"与"真子项"的区别就在 `kids` 这个数上"）。

## 5. ★★ 3-6：`~TaskShell` 只 join 不取消 ⇒ **暂停中关窗永不退出**（第 11779-11807 行）

```cpp
auto* shell = new TaskShell(nullptr);
shell->run("3-6 取消锁", [shell](progress, message) {
    while (!shell->cancel_requested()) sleep(2ms);   // 没有取消位就永远停不下来（修前的形状）
});
… 等 200ms 让它真的进循环 …
delete shell;                       // ★ 判据点
check(elapsed < 5000, "暂停中析构必须有界返回");
```
⇒ 这条判据的形状值得记：**用"析构必须在有界时间内返回"来量"进程能不能退出"**。

## 6. ★★ 3-5：worker 里调 `compare_dirs`（第 11814-11969 行）

判据①（跨线程不许碰控件）：
```cpp
page.set_mode("stats");  // 清空
const int before = tree->topLevelItemCount();
std::thread worker([&]{ worker_rows = page.compare_dirs(…); }); worker.join();
check(worker_rows == 2 && tree->topLevelItemCount() == before,   // ★ 当场不许变
      "worker 里调 compare_dirs ⇒ 树上当场不变（排队回主线程）");
```

判据②（真实入口：点「开始比较」）：
- **主线程同步判**控件被禁用（`compare_controls_enabled() == false`）；
- ★★ P17-6：**「导出 CSV」与「清除」也要一起禁** ——
  "导出是真竞态：`export_csv` 遍历 `results_`/`visible_` + `count_kinds()`，
  而 worker 正在 `results_.clear()+push_back()` ⇒ 同一个 `std::vector` 的主/worker 并发读写（UB/崩）"；
- ★ **第二道闸**：绕过按钮直调 `export_csv_auto` 也必须被拒**且不留文件**；
- 收尾后控件恢复；再泵事件 ⇒ 排队的那次刷新落地（树 == 2）。

判据③（P17-10 遍历错误可见）：
先断言正常比较**不该**有"结论不可信"，再用**不存在的目录**比一次 ⇒
统计行必须含"目录遍历出错"与"结论不可信"。

## 7. 收尾（第 11976-11989 行）
```cpp
static bool flush_registered = false;
if (!flush_registered) { flush_registered = true; std::atexit(&flush_shell_page_trace); }
flush_shell_page_trace();
flush_engine_page_trace();
return result;
```
★ 注释："**子进程内**落盘（外壳/引擎各一份），atexit 兜底（幂等）；
引擎侧自己在 `widget_facade.cpp` 里注册了 atexit flush（不依赖本文件的 include）"。

---

## 8. `smoke.cpp` 全文读完后：**它能证什么、不能证什么**

### 能证
- 每一族缺陷都有"**正向 + 反向**"或"**一正一反**"成对判据；
- 每个红都写了"**判红办法**"（把修法换回修前形态 ⇒ 立刻红）；
- 判据本身有**自证样本**（B1-5/B1-9 那一类"候选集空 / 断言不是控制流"的修法）。

### 不能证
- **没有覆盖"插件页为空"这一族的具体形态**：P5 的判据是
  "`page_base` 或 `ScrollViewport`"二选一，占位页两样都不是 ⇒ 报"没被套上宿主视口"；
  但它**不区分**"builder 抛了"与"builder 造了但没挂上"（`hub.cpp:632-677` 那一层才知道）。
- **⑭ 的候选集为空是"红"而不是"未覆盖"**（B1-5 的选择），于是"页没切对"会表现成"底板有涂"。
- P16-3c / P13-2a-2 的 `menu_ready` 键都依赖 `current_page_id()`（当前页），与 `hub.cpp` 的
  "构建点捕获 page"口径不齐。

⇒ 这三条正是我们**当前三个红**（P5 / ⑭ / P16-3c）的**判据侧成因**。
