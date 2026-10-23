# `pages/fs_page.cpp` 中段（第 421-900 行）

---

## 1. 两个工具行的 `setSizePolicy(Preferred, Fixed)`（第 452-455、481 行）

修前事实（注释原文）：
> 工具行是普通 `QWidget`、垂直策略默认 `Preferred`；一旦同面板里的列表被隐藏/没有最小高，
> 布局会把剩余空间全塞给它 ⇒ 里面的按钮**被垂直居中** ⇒ 看起来就是"**飘在空白中间**"。

⇒ 钉成 `Fixed` 后，无论列表在不在，工具行都只在顶部一条。
★ 这是"**布局策略是外观的一部分**"的实例：改了控件层级就要重看尺寸策略。

## 2. 分隔条方向被用户改过一次（第 483-488 行）
- 2026-09-30：要"上面包列表、下面文件列表"之间的**可拖拽分隔条** ⇒ 竖向 `QSplitter`；
- 2026-10-01：「这个不要做成上下，**做成左右**」⇒ 改**水平**：包列表在左、条目在右。
两段注释都保留着 —— "谁改的、为什么"可追溯。

## 3. ★ 按钮语义的三条用户裁定（第 430-444 行）
| 按钮 | 口径 |
|---|---|
| 「全选」 | 原「全选包」；行为＝**选中而非勾选**（`selected` ≠ `checked`，与 `overlay.h:13-14` 一致） |
| 「加载选中」 | `load_selected()` |
| 「取消加载」 | `unload_selected()` |
| 「清空」 | `clear_button_` |

★ 而「扫描目录」「移除」被**删掉**（第 419-420 行注释）：
"加包多了'选文件夹…'模式、功能重合"、"移除改到每行的行尾 ✕"。

## 4. 「加包」是**左键二选一菜单**（第 520-560 行）—— **P16-3c 的参照实现**

```cpp
QMenu* menu = new QMenu(add_button_);
QAction* pick_files = menu->addAction("选择包文件…");
QAction* pick_dir   = menu->addAction("选择文件夹…");
add_button_->setMenu(menu);
```
★ 这正是 `widget_facade.h:143-146` 注释里点名的"内置 FS 页「加包」＝包／文件夹二选一"
—— **插件的 `button_on_menu` 就是照它做的**。所以 P16-3c 的语义参照在这里。

## 5. 拖入的处理分三支（第 780-900 行）

| 拖进来的是什么 | 行为 |
|---|---|
| **单个包文件** | `gate_source()` → `add_source()` → 立即 `load_one_source()` |
| **目录** | `scan_sources()`（家族通配）→ 每个包 `add_source()` |
| 目录里的包 | ★ 用户 2026-10-03 改口径：**全部自动加载**（撤销"扫出来的只入列表"），理由是"用户实测零点几秒，DB 读盘已是 seek 式（每包只读块头＋尾部条目表）" |

★ 自动加载后 `rebuild_tree()`（"新加载的内容要进右栏＝解包产物预览"）。

## 6. ★ `gate_source()` 的日志必须说清"这一份是解出来的"（第 201-251 行）
`plugin::gate_db_path` 的 `used_plugin` ⇒ 路径换成解密产物 + `log_message("…由插件解密后读取")`。
★ 而 `add_loose_dir()` 用 **`LooseInfo.truncated` 结构化标志**：
**截断＝加进去＋警告**，真错＝拒绝加载 —— 修前"任何非空 `error` 都当硬失败 ⇒ 超深目录整份拒载"。

## 7. 搜索是**边打边筛**（第 470-474 行）
`search_edit_` 的 `placeholderText` 写着"按路径过滤（边打边筛）"；
实现走 `apply_filter_to` / `FacadeTree::set_filter`（树侧唯一实现）。

---

## 8. 这一段的检查项

| 检查 | 为什么 |
|---|---|
| 工具行尺寸策略是否 Fixed | 否则"按钮飘在空白中间"（列表隐藏时） |
| `selected` 与 `checked` 是否分开 | 用户 2026-09-29 第 1/2 条明确要求 |
| `button_on_menu` 的参照实现 | 内置 FS 页「加包」就是 **QMenu + 两个 QAction** |
| 拖入目录是否自动加载 | 用户 2026-10-03 改口径 |
| 截断是否用结构化标志 | `LooseInfo.truncated`，不是 error 文案 |

## 9. 待继续（901-2446）

`load_one_source` / `build_all_descendants` / 勾选三态回灌 / 筛选一次建树 / 解包主流程 /
封包主流程 / `scan_sources` 的计数与 `loaded_*` / `plugin_context_menu` 接入（P13）等。
