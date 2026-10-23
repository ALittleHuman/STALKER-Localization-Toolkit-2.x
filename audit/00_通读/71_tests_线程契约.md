# `tests/check_thread_contract.py`（658 行，全文读完）—— 方案 C 的落点，已存在

> 结论：**方案 C（生命周期/线程契约静态锁）不是"要新写"，而是"要扩写"** ——
> 这个脚本已经有 658 行、且形态完全正确（读权威口径 + 反例自证 + fail-closed）。

---

## 1. 它用**静态分析**兜住两条契约（文件头）

① **worker 不许碰 UI**；② **取消必须真能读到并停**。

修前三处事实（都在 `smoke.cpp` 与页面里被复核过）：
- `fontpack_page.cpp`：worker 直接调 `build()` ⇒ 跨线程碰 QWidget + 数据竞争 + 析构期访问违例（03 F-01 **Critical**）；
- `plugins/*.py`：worker 里 `ui.set_text`/`get_text` 被门面**静默变成 no-op** ⇒ 页内状态永远空白；
- `ui.task_cancel_requested` 被闸门一刀切 ⇒ **取消关不掉还谎报"已取消"**（05 PLG-01 High）。

## 2. ★★ A 侧（C++）：三个"必须剥掉"的合法通道

判据把 worker 体抽出来后，**先剥掉 `QMetaObject::invokeMethod(...)` 整段**（换等长空白）：
> ★ 为什么必须有这一步：`invokeMethod(this, lambda, Qt::QueuedConnection)` 里的 lambda
> **在主线程跑** —— 这是本仓"worker 不碰控件"的**合法通道**。
> 不剥掉它，判据会把"排队回主线程"误判成"worker 碰控件"（**假红**）；
> **假红比不判更坏 —— 它会把正确写法逼着改坏。**

同理 `cpp_member_functions()` 也剥（`XmlPage::compare_dirs` 就是"自己把 UI 工作排队回去"的那种）。

★ 还有一处极细的判别（`touches_widgets`）：
> 成员名后面紧跟 `(` 的**当函数调用**处理，不算"控件成员" —— 否则 `refresh_rows()`
> （一个会碰树的方法）会被当成控件成员名匹配。

## 3. ★★ "可判接缝"：接缝必须写在**第一次碰控件之前**
```python
_SEAM_RE = r"ui_thread_guard\s*\(|ui_thread_dispatch\s*\(|QCoreApplication\s*::\s*instance\s*\(\s*\)|on_ui_thread\s*\("
def seam_before_widgets(body): …  return seam.start() < at   # at = 第一个碰控件的位置
```
> 只认"接缝在前"：接缝写在末尾、控件已经被改过 ⇒ 不算。

⇒ 这就是"**注释里说的纪律**"变成"**可判的约束**"的样板。

## 4. ★★ B 侧（Python）：白名单**读 C++，不抄 C++**
```python
def worker_safe_methods():   # 从 src/engine/plugin/widget_facade.cpp 的 kWorkerSafeMethods[] 读
```
> 权威在 `widget_facade.cpp` 的 `kWorkerSafeMethods[]`，本脚本**读它而不是抄它**
> —— 免得两处口径分叉。

★ 并且**fail-closed**：读不到白名单 ⇒ 直接判 fail（不是"跳过"）；
★ 还查**悬空名**：白名单里的名字必须在门面头文件里存在（`dangling` 检查）。

## 5. ★★ `worker_functions()` 堵了两个真坑（注释里写明了）
① 插件里几乎都是 `ui.task_run(panel, "t", worker)` —— 传的是**名字**，
   只往回走实参节点不够（名字节点里没有函数体）⇒ 先建"函数名 → 定义"表再解析；
② 同一个文件里往往有**多个同名 `worker`**（`convert_page.py` 三个任务各一个）
   ⇒ 按"**行号最靠近调用点**的那个定义"解析，否则一处违规会被重复报 N 次。

## 6. ★★ 反例自证 10 条（`selfcheck`，第 568-597 行）——这就是"判据自己先要可信"
A 侧 7 条：直接碰控件 / 调碰控件的成员函数 / 干净样本 / **带守卫的碰控件函数（应绿）** /
**排队回主线程（应绿）** / **排队之外仍直接碰（应红）** / **自己判线程 + 排队（应绿）**。
B 侧 3 条：`set_text` / lambda 里 `log_view_append` / 干净样本。

⇒ 每条判据都配了"**该绿的绿**"与"**该红的红**"，这是本仓最完整的自证集。

---

## 7. 对本轮方案的修正：**C 的执行方式变了**

原方案 C 我写的是"新写一个 py 脚本 ≈200 行"。**实际应该**：

| 原计划 | 修正为 |
|---|---|
| 新写生命周期静态锁 | **扩写本脚本**：加三个文件级检查（见下） |
| 扫 `singleShot(` 是否带 context | 加 `_SINGLE_SHOT_RE` + `extract_balanced`（已有函数可复用）判第二实参 |
| 扫静态 `set_*_hook(` 是否有配对复位 | 加"同一别名文件里 `set_X_hook(` 与 `set_X_hook(nullptr)` 必须成对出现" |
| 扫 `deleteLater()` 前是否 `disconnect` | 加"`deleteLater()` 所在函数体内必须有 `disconnect`"（`video_page.cpp:104` 的正确形态当模板） |

**成本重估**：不是 200 行新脚本，而是 **≈60 行增量**（复用 `strip_comments` /
`extract_balanced` / `cpp_member_functions` 全部现成）。

⇒ 这让"只做三件事"里的 C 从"半天"降到"**1~2 小时**"。

---

## 8. 本轮结论

- **线程/取消这一类契约，本项目已经有正确的静态判据**，且带 10 条反例自证；
- 缺的只有**生命周期那三条**（`singleShot` context / 静态钩子复位 / `QProcess` disconnect），
  而它们的**正确形态在代码里都已有先例**（`font_picker.cpp` 修后、`smoke.cpp:7138` 的 RAII、`video_page.cpp:104`）；
- 所以方案 C 的**真实工作量被低估了**（是扩写，不是新写），方案 A/B 的估算不变。

⇒ 「只做三件事」的总成本从 **1.5 人日** 修正为 **≈1 人日**（A 半天 + B 一天 + C 1~2 小时，
其中 A 与 C 可并行）。
