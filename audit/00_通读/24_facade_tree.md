# `src/shell/facade_tree.cpp`（372 行，全文读完）

---

## 1. 构造口径（第 15-73 行）—— "宿主规范，不是 Qt 默认"

```
setUniformRowHeights(true)                 // 等行高
setRootIsDecorated(true)
setSelectionMode(ExtendedSelection)        // 多选（控件下限 #2）
setExpandsOnDoubleClick(true)
setAlternatingRowColors(false)             // 不许交替行色（主题面）
setMinimumHeight(px(120))
```

四条信号接线：
| 信号 | 行为 |
|---|---|
| `itemExpanded` | **懒建**：只回调 `on_expand_(key)`，插件自己决定填不填子 |
| `itemChanged` | **向下传播**（勾父带子孙）+ **向上重算**（三态）+ 回调（`updating_` 守卫防递归） |
| `itemClicked` | 只报 key（`on_click_`），**替插件决定语义** |
| `itemDoubleClicked` | **只在叶子上**触发 `on_activate_`（有子的行双击＝展开，不抢） |

★ `itemChanged` 里那条"勾父带子孙"是自检抓出来的：
> 修前只做了向上重算 ⇒ 用户勾父时子仍不动，**判据当场红**。

## 2. `set_columns`：列策略**不在这里写**（第 75-87 行）
门禁 D2④ 锁着"列策略只许在 `pair_tree.cpp` 里"（fs 两棵树与 XML 结果表共用）。
本函数只把门面列声明翻成 `PairTreeSpec` 再调 `apply_pair_tree_spec`。

## 3. `add_node`：三个自定义角色 + 一条"不塞占位子项"（第 89-120 行）

| 位置 | 内容 |
|---|---|
| `Qt::UserRole` | `key`（字符串） |
| `Qt::UserRole + 1` | `has_action`（行尾动作开关） |
| `Qt::UserRole + 2` | `matched`（是否命中筛选；**怎么画由呈现层决定**） |

★ `has_children` ⇒ **只设 `ShowIndicator`，不塞占位子项**：
> 塞了就会被算进行数/可见行，判据会跟着错。

⇒ 这是"**不要把占位数据塞进被测结构**"的一条实例。

## 4. 可见行与选中（第 169-239 行）

- `item_visible()`：自己不隐藏 **且所有祖先都展开**；
- `visible_keys()`：**只在 expanded 时递归子** ⇒ 得到"展开可见行"的顺序；
- `selected_keys()`：走 `QTreeWidgetItemIterator`（**树序**），注释明确"`selectedItems()` 的顺序不作保证"；
- `selected_key()`（单数）：`selectedItems().first()` —— **无序**，只用于"随便取一个"的场合。

⇒ 判据要点：**要"树序"必须用 `selected_keys()`，不能用 `selectedItems()`**。

## 5. 三态勾选（第 247-287 行）
`refresh_ancestors`：自下而上，按子态算父态（全勾⇒勾、全不勾⇒不勾、否则半勾）；
`set_checked(key, on)`：先向下全设，再**从父开始** refresh。

## 6. 过滤（第 306-337 行）
`apply_filter_to`：命中本行 ⇒ `++hits_`；**命中子孙也显示本行**（否则找不到路径）；
可见性 = `self_match || child_match || needle 为空`。
`set_filter("")` ⇒ `hits_ = 0`（"空串＝不过滤"，与界面"无搜索"一致）。

## 7. 行尾动作（第 340-370 行）—— P13
`set_on_action` / `set_action_mark`（默认 `✕`）都会 `install_action_delegate()`；
**没有回调就不画委托**（"免得出现看得见点不着的 ✕"）；
委托只装在**末列**（吃剩余宽度那一列 ⇒ ✕ 贴行尾）；
`has_action` 标志从 `UserRole+1` 读；点击后回调给 **key**。

---

## 8. 对 P13/P16 的结论

1. **树的右键菜单不在本文件里**：它由门面的 `context_menu` 给**目标控件**设
   `CustomContextMenu` 策略 + 接 `customContextMenuRequested`。
   ⇒ 判据 emit `only_tree->customContextMenuRequested(...)` 是**绕过 Qt 的鼠标路由**直接发信号，
   只要该控件的策略被门面设过就会触发。**若插件没调 `ui.context_menu`，这个信号没有任何接收者**
   ⇒ 菜单永不出现 ⇒ 判据拿到 `[]`（**这正是 P13-2a-2 偶发红的另一种可能**）。
2. **`visible_row_count` = 展开可见行**，不是总行数；判"40 行列表"那类断言要按展开态算。
3. **`selectedKeys` 与 `selectedItems` 语义不同**（前者树序、后者无序）。
