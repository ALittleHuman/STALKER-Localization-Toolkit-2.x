# src/core 批三：`build_version.cpp`（577 行，全文读完）—— src/core 收尾

---

## 1. 手写极简 JSON（第 22-227 行）

只覆盖 `version.json` 的形状（对象/数组/字符串/整数，**不支持小数**），不引第三方。
理由："这个文件是我们自己写的，schema 也由我们定；用一个够用的子集解析器比拉一个大依赖更可控，
也满足 core 不许 include Qt/重依赖。"

### ★★ P17-15（第 135-152 行）：**递归深度上限 64**
修前 `value()` 对 `{`/`[` **递归**下降，深度只受输入长度限制 —— 一份 `[[[[…` 的 **100 万层嵌套**
（几十 KB 文本）就能把 1 MB 线程栈打穿 ⇒ **崩而不是报错**。
现在进一层 +1，超上限**带理由返回 false**；真实形状只有 3–4 层，64 足够宽。
判据：`xrkit_app --smoke` 里"8000 层嵌套必须报错而不是崩"那条。

⇒ **"不可信输入 + 递归"必须有深度上限**，而且**要有判据**（否则上限删了也没人知道）。

### ★★ 审计 2-5（第 212-223 行）：`std::stoll` 抛 `out_of_range` ⇒ `std::terminate`
`version.json` 里一个超出 `long long` 的整数（`99999999999999999999`）会抛
`std::out_of_range`，而这条链的调用方**都没有 try**（`load_version_store` → CLI `run_cli` 无顶层
catch → `main`）⇒ 异常穿透 ⇒ **用户看到的是"程序直接没了"**。
现在解析不了就**当解析失败**（走既有的"解析失败一个字节都不写"保护），不抛。

⇒ **"解析用户文件"这条路上的每个 `stoi/stoll/stod` 都是潜在 terminate**。
  我们的判据脚本读日志/JSON 时也要遵守（Python 会抛异常，但至少有 traceback）。

### ★ 第 322-330 行：**BOM 吃掉**（实测踩到）
PowerShell `Set-Content -Encoding UTF8` / 记事本写出来的 JSON **带 UTF-8 BOM**，
解析器在位置 0 就撞上 `0xEF` ⇒ 报"无法识别的记号"，而调用方会把它当成"文件不存在"
⇒ **把用户的历史构建记录覆盖掉**。
现在先吃掉 BOM（语义上它就是"文件是 UTF-8"的标记，不是内容）。

⇒ **判据/工具读外部文件时必须容忍 BOM** —— 这是我们自己踩过的（`docs/` 与 `version.json`）。

---

## 2. 写盘一律原子 + 写后读回（第 264-271、501-542 行）

- `write_text` 从裸 `ofstream(trunc)` 改成 `core::write_file_atomic`（2-5 后半）；
- `save_version_header`（写回 `version.h` 的两行）四步：
  ① 先校验值（序号 ≥0；标记串不许带引号/分号/换行）；
  ② 读原文 → **只改两行**（`kPrereleaseLine` / `kBuildCountLine`），其余字节原样；
  ③ **原子写**（半写的 `version.h` 会编出坏版本串）；
  ④ **写后读回校验**（不是"我以为我写了"），不一致就报出"期望 X / 实测 Y"。

★ 还有一个细节（第 521-524 行）：`if (text == before) { error = "写回没有改变任何字节（形态对不上？）"; }`
—— **"没改到任何东西"本身就是一个错误**。

⇒ 这条对我们特别有用：**"操作成功但什么也没发生"必须报错**。
  我们的"修了判据但读数没变"正是这种情形 —— 该有一个"变了没有"的断言。

---

## 3. `last_release`（第 544-568 行）：**不用 `current_index`**

> 遍历 tree 找**最大序号**（不是 `current_index`：那是"下一个号"，
> 门禁的构建 GUI 一动就会把它推一格 —— **实测 59 → 60**）。

⇒ **"同一个概念的两个字段"必须分清哪个是权威**。这与 `version.h:16/23`（派生自 tree）一致。

---

## 4. `esc()` / `render` 的形态（第 229-250、378-408 行）

- `esc` 把 `"` `\` `\n` `\r` `\t` 与 `<0x20` 转义（后者用 `\uXXXX`）；
- 输出是**人可读的多行 JSON**（不是紧凑串）—— 便于 git diff。

⇒ 产物可 diff 是**运维友好**的一部分：我们改 `version.json` 时能看出改了什么。

---

## 5. src/core 六个文件读完后的总结

| 文件 | 一句话 |
|---|---|
| `log.cpp` | 两条通道（`log` 带 stderr，`log_block` 只写文件）；prune 的两处路径比较教训 |
| `path.cpp` | UTF-8 ↔ 宽串；**`path_equals` 只折 ASCII**（与 Python `normcase` 有分歧） |
| `atomic_write.cpp` | FNV-1a 流式校验 + 落点大小 + remove 重试；探针三入口互不混用 |
| `check.cpp` | 逐条 catch；ASCII 收尾行 `SELFTEST pass= fail=` |
| `version.cpp` | 手写 semver 校验 |
| `theme.cpp` | 2.x 配色覆盖；`app_dir` 翻车两次；`1e400`=+∞ 的 UB；`user.ltx` 改名与迁移 |
| `build_version.cpp` | 手写 JSON + 深度上限 + BOM + 原子写 + 写后读回 + "没改到东西就是错" |

★ **`core` 层最值得抄的三条**：
1. **"没改到任何东西"要报错**（`save_version_header` 的 `text == before`）；
2. **解析不可信输入要防递归/防溢出/容 BOM**；
3. **写盘一律原子 + 写后读回**（不是"我以为我写了"）。

## 6. 待核对

- `src/core` 完成（9 个文件里 `CMakeLists.txt` 与 `theme_tables.inc` 是生成物/构建脚本，
  `theme_tables.inc` 已随 `theme.cpp` 读过其用法）。
- 下一步进 `src/engine/plugin/`（3790 行的 `py_host.cpp` 是重头，还有 `widget_facade.cpp` 396 行
  ——我们加过大量探针的那个文件）。
