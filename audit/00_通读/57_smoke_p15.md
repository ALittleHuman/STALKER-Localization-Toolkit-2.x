# `smoke.cpp` 批十五（第 6851-7350 行）—— ★★★ P5 判据本体 ＋ 一个**必然误报**的诊断

---

## 1. P17-1 取消探针：三条"取证的纪律"（第 6851-7070 行）
- 页没注册 ⇒ **不等**，先把 `plugin_errors` / 夹具大小 / 前 80 字节 / 可见页集合全部记进 notes；
- ★ 用户 2026-10-20 口径（第 6901-6904 行）：
  > 「先判 `probe_page >= 0`；为负时**不要等**。只有 `>=0` 时，再 `wait_until(start_button != nullptr, 2000ms)`。
  >  **等待后重新取** `page_widget` 与按钮指针，不要沿用等待前的（避免 `deleteLater` 后的悬垂）。」
- ★ ② 的同步改用 **`TaskShell::set_worker_finished_hook`**（事件优先），
  事件到了**再有界等 marker 落盘** —— "只信事件不确认 marker 就会读到'没读到'"；
- ★ ③ 那条**以前是同步读一次**（第 7017-7021 行）：
  > worker 的 `message('loops:N')` 要经过 `task_on_message` 排到主线程才落到标签上 ⇒
  > 负载高时"回调还没到"就被判成"worker 改到了控件"（实测报错文案正是 `标签实得 未被改写`）。
  > 口径：「修判据的**完成条件**，不加固定等待，也不重试」。
- ★ 红的时候把**标记全文**打进 notes（"临时目录会被清理，不能依赖它"）。

## 2. ★★★ P5 块（第 7073-7350 行）

### （a）静态钩子必须撤（第 7130-7140 行）
```cpp
Hub::set_page_ready_hook([&page_ready_seen](const QString& id){ page_ready_seen << id; });
Hub hub(nullptr, dir);
struct PageReadyHookReset { ~PageReadyHookReset() { Hub::set_page_ready_hook(nullptr); } } reset;
```
注释："静态钩子必须在本块结束前撤掉：它捕获的是**栈上的** `page_ready_seen`，
留着会被后面别的 Hub 构造调到 ⇒ **悬垂引用（实测 smoke 在 l5_demo 之后直接崩掉）**"。

### （b）★★ 必须 `show()` ＋ `switch_to_page()`（第 7155-7160 行）
> 不 show 的话 `QSplitter::sizes()` 恒为 `[0,0,0]`，那样判的是"**布局没跑**"而不是"分栏不对"。
> ★ 还要**切到这一页**：`QStackedWidget` 只给"当前页"派发布局 ⇒ 不切的话几何拿不到。

### （c）★★ 切页之后**重新取**页实例（第 7166-7169 行，2026-10-23）
> 原来 `page_widget` 是在 `hub.show()/switch_to_page` **之前**取的，
> 若切页导致页被重建，旧指针的宽度/layout 都不对 ⇒ **`wait_page_ready` 必红**。

### （d）★★ `wait_page_ready` 的**顺序**曾经反了（第 7202-7206 行，2026-10-22）
> `wait_page_ready` 要求页有宽度，而它原来在 `hub.show()` / `switch_to_page` **之前**
> ⇒ 宽度自然是 0，于是**偶发**"内容已建（`childCount=238`）却判未就绪"。

### （e）★★★★ **一个必然误报的诊断**（第 7213-7238 行）

```cpp
// 「有界等待"插件页内容就绪"，超时明确报未就绪」
while (…) {
    if (page_widget != nullptr && !page_widget->findChildren<FacadeTree*>().isEmpty()) { ready = true; break; }
    …
}
if (!ready) notes.append("P5 诊断：插件页内容未就绪（等 2000ms；objectName=%1 childCount=%2）");
```

**而 P5 的夹具 `l5_demo.py`（第 7085-7114 行）压根没有 `ui.tree(...)`**
—— 它只造 `viewport / label / list / split`。**没有 `FacadeTree` ⇒ `ready` 永远为 false**
⇒ **每一轮 P5 都会白等 2 秒，并且无条件追加一条「插件页内容未就绪」的 note**（哪怕一切正常）。

★★ 这是从 P4 块（那一块确实有 `ui.tree`）**照搬过来没改的判据**。
  危害：它是 `result.notes` 里的**假证据** —— 我们在红的时候若读 notes，
  会看到"内容未就绪"而误判成"页没建出来"，实际它只是**在数一个夹具永远不造的类型**。

### （f）P5 的**主判据**（第 7264-7273 行）
```cpp
auto* page_base = dynamic_cast<PageBase*>(page_widget);
if (page_base != nullptr) {
    check(page_base->viewport() && page_base->viewport()->fit() == Fit::Content && page_base->minsize_ok(),
          "P5 插件页的宿主外壳不对（自带视口 fit/minsize 没接上）");
} else {
    check(dynamic_cast<ScrollViewport*>(page_widget) != nullptr,
          "P5 插件页没被套上宿主视口（长内容会滚不动）");
}
```
★ 注释给了口径演进史：修前宿主套的是**裸 `ScrollViewport`**、页控件本人就是视口 ⇒
判据写成"页控件必须 `dynamic_cast` 成 `ScrollViewport`"；P16-2c 之后页控件是 `PageBase`。
"判据随之量**它的自带视口**（更严：还量 `minsize_ok()`）。**没有放宽。**"

★★ **对我们的意义**：这条红 ⇔ `page_widget` **不是 `PageBase` 也不是 `ScrollViewport`**
⇔ 它是**占位页**（`make_placeholder` 返回裸 `QWidget`）或裸容器。
这与我们 `anything=0 ⇒ 占位页` 的推断**完全一致**。

### （g）★ 一条判据哲学的实例（第 7343-7346 行）
> 为什么不在插件侧判"位置 > 0"：`build()` 跑在**布局之前**，那时读到的必然是 0 ——
> 拿它判红就是"**判据没有能失败的输入**"的反面（这次是"**判据的输入天生为 0**"）。

## 3. 待继续
`smoke.cpp` 7351-8350。
