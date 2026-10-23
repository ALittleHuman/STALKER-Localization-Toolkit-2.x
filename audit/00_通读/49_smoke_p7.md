# `smoke.cpp` 批七（第 3351-3850 行）—— 落点规则的四条锁 / 解包封包端到端

---

## 1. ★★ 同一条缺陷的"**真实症状锁**"（第 3354-3385 行）

前一条（3344-3352）是 `is_package_file_name` 的**单元锁**；这一条紧跟着**再走一遍用户真入口**：
```
造一个只含 patch_00.xdb 的目录 → FsPage::add_paths({dir}) → 断言 source_count()==1 && loaded==1
```
注释写明："旧规则下 `.xdb` 被判为非包 ⇒ **静默忽略（源 0）** ⇒ 改回旧规则立刻红"。

⇒ **这是本仓判据的最高形态**：同一条事实配两把锁 ——
① **单元级**（函数返回值），② **真实入口级**（用户会走的那条路）。
我这几轮的教训（"判据考的要不是在用的那个函数"）正是缺了②。

## 2. ★★ 落点规则的四条锁（第 3387-3444 行）

架构红线原话：「**GUI 的功能，CLI 肯定要全部拥有啊。哪有 GUI 的细节功能比 CLI 还多的。**」
⇒ 落点规则搬进引擎（`package_folder_name` / `unpack_target_dir` / `plan_unpack_targets`），
shell 与 CLI `--unpack-package` **共用同一份**。

四条锁：
| 锁 | 内容 |
|---|---|
| ① | `package_folder_name`：`gamedata.db0→gamedata`、`gamedata.sq_base→gamedata`、`a.b.db0→a.b`、`patch_00.xdb→patch_00` |
| ② | `unpack_target_dir`：**自带壳**（条目以 `configs/` 开头）⇒ 不再叠一层 |
| ②b | ★ **裸目录项 `configs` 也算自带壳** —— "否则真机会叠出 `configs/configs`（2026-10-04 实测抓到的缺陷）" |
| ③ | ★★ **判红锁①：量词"全部" ⇒ "过半"** —— 真机 9 个 VESSEL 包的形状是"裸壳项 `ai` + **一条根级 `readme.md`** + 一堆 `ai/…`"；旧量词被 `readme.md` 一条掀翻 ⇒ 解出 `ai/ai/alife/…` |

## 3. ★ 落点预览与"条目数一致性"（第 3446-3560 行）

- `plan_unpack_targets` 的**每个落点**都要能被 `unpack_target_dir` 复现（同源）；
- ★ `preview_layout` 与 `unpack_target_dir` **必须给出同一个落点**（"预览与实际落点不许分叉"）；
- 解包后**逐条核对产物**（文件存在 / 大小对 / 空文件也建）。

## 4. ★★ 解包/封包端到端（第 3562-3850 行）

形态固定：**造真包 → 走页面真入口 → 读回校验**。

### （a）解包三态
- 成功：`files_written` 与条目数一致 + 每个产物存在 + **内容与源一致**；
- **覆盖必须可见**：`overwritten` 逐条（"目标已存在"时）；
- 失败/跳过：`failures` 逐条（"rel：原因"）。

### （b）封包（XDB / SquashFS 各一遍）
- `run_pack(&summary)` ⇒ 产物读回 `package_entries()` **条目数对**；
- ★ **封包输出路径要回填到输入框**（"1.x 的 H1"）：
  `pack_out_edit()->text().contains("packed.xdb.db")`。

### （c）★ 一条"路径回填"细节
`xdb_out = pk_root + "/packed.xdb.db"` —— 因为 `resolve_pack_target` 会给"无扩展名"补扩展，
而这里故意给一个**带扩展名的名字**，用来钉"不再二次补扩展"。

## 5. 这一段的判据形态总结（可复用模板）
```
① 造最小真实夹具（真包、真目录、真页面）
② 走**用户会走的那条入口**（add_paths / run_pack / unpack_*）
③ 断言可观测结果（计数 / 产物 / 落点 / 回填）
④ 断言"负向"（该拒的拒、该留痕的留痕）
⑤ 注释里写"改回旧实现 ⇒ 立刻红"（判红办法）
```

---

## 6. 待继续（3851 起）

封包余下、其余五页、P4/P5/P13/P16/P17、⑬ 版本资源。
