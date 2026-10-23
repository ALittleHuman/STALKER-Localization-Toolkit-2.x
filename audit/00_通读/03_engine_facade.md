# `include/xrkit/engine/plugin/engine_facade.h`（785 行，全文读完）

> 这张表是**插件能看到的全部引擎能力**（= 六个内置页迁移后的能力面）。
> 它的注释里藏着本仓最集中一族的**历史 bug 与判据哲学**，比代码本身更值得抄下来。

---

## 1. 为什么是"声明式表"而不是直绑（第 2-22 行）

用户问"哪个方案插件能力最强"，结论选**方案 3**：
- 方案 1（直绑引擎函数）与方案 3 **运行时完全一样**，差别在**治理**：直绑会让"顺手多暴露一个内部函数"
  变成免费 ⇒ 必然长成影子 API、且没法版本化；
- 方案 3＝**声明式表**（本文件）+ **能力表判据**（多一个＝泄漏、少一个＝削能力）
  ⇒ 加能力是"有意为之的一次提交"；
- 方案 2（子进程 `xrkit_cli.exe`）达不到同样能力：富结构过不去、编排要重写，
  且**支持不了"插件给引擎加扩展点"** ⇒ 只留作诊断/兜底。

三条口径（14-17 行）：门面由外壳注入 / **表外无门** / **值类型 Python 友好**（不漏 C++ 结构体与指针）。

---

## 2. ★★ P17-122（第 136-144 行）：**"需要用户确认"必须是具名状态，不能从 `exit_code==3` 推**

修前事实：引擎正确返回了 `needs_confirm` + `exit_code=3` + 问句，但**门面这一层没透**
⇒ 插件只能看 `exit_code` ⇒ 命中"`exit_code != 0` ⇒ 完成但有失败"分支 ⇒ 界面显示
**「完成，但有失败：成功 0 / 跳过 0 / 失败 0」**（自相矛盾）。

★ 作者原话判据：**"拿数字/字符串当语义通道，必烂"** ⇒ 语义走具名字段；`exit_code==3` 只作兼容。

---

## 3. ★★ P17-134（第 427-436 行）：**信号在门面这一层"丢在中间"**

修前：引擎把"到上限了"放在 `fs::LooseInfo.truncated`（P17-120 / P2-5），
而门面的 `fs_loose_entries` **没有参数接它** ⇒ 信号**丢在门面**；`py_host` 的 dict 里也没有
⇒ 插件只拿到 entries+error ⇒ 「截断」从"误报失败"退化成**完全静默**（比原来更糟）。

⇒ **这是我们这轮最该记住的一条**：`page_ready` / `menu_ready` / `worker_finished` 这些信号，
只要**任何一层没把它透出去**，就会表现成"偶发红"或"完全静默"。
**"信号在中间层丢掉"是本仓反复出现的一族**（P17-122 的 confirm、P17-134 的 truncated、
P16-0 的字段名不一致，都是同一族）。

---

## 4. ★ P17-85（第 438-448 行）：`FsPackageInfo` 的诚实口径

`used_seek == true` ⇒ `whole_file_reason` **必须为空**（"没退回就不许报理由"）；
SquashFS 三项全空/假（它只读 96 字节超块判 magic，本来不整包读）。
⇒ 与 `atomic_write.h` 的 T-03 一样：**能力措辞必须与实现一致，不许夸大**。

---

## 5. 一处"血账"警告（第 628-630 行）

```
// `target_dir` = **该类型的目标子目录的完整路径**（例如 <out>/gameplay）
// ★ 传相对路径会按**当前工作目录**解析（曾把仓库自己的 scripts\ 清了，血账）
```

⇒ 以后任何"输出目录"类参数，都必须**先解析成绝对路径再动盘**。这条直接关系到我们打包/解包判据。

---

## 6. 一个 MSVC 陷阱（第 726-729 行）

```cpp
// ★ 这里是**类型声明**（`std::function<...>`）⇒ **不许写默认参数**
//   （MSVC：`error C2383: 此符号中不允许有默认参数`）；默认值写在**实现那个 lambda** 上。
```

⇒ 我加探针/改签名时，**别在 `std::function` 声明上写默认值**。

---

## 7. 能力面清单（按页分组，均是 `std::function` 成员）

| 组 | 代表成员 | 备注 |
|---|---|---|
| 字库 | `font_check / font_pack / font_state_marks / font_size_slots` | 样板页反向定义（第 19-20 行） |
| convert | `text_encodings / text_convert_batch / text_restore_backups / text_clear_backups / text_write_stats_csv / text_format_duration` | 含 `TextHooks{should_stop, gate, message, progress}`（第 103-108 行） |
| text | `text_default_id_prefix / text_validate_extract / text_extract_kind / text_extract_dir` | |
| xml | `xml_modes / xml_find_mode / text_read_bytes / xml_compare_stats / xml_compare_text / xml_only_one_side / xml_inline_detail / xml_full_detail / xml_full_text_detail / xml_render_csv / xml_write_csv / xml_compare_dirs / xml_row_matches` | 明细与 CSV **由引擎渲染**（第 227 行） |
| video | `video_find_ffmpeg / video_major_version / video_version_rejected / video_plan_transcode / video_check_output_conflict / video_parse_ffprobe / video_parse_encoders / video_progress_from_line` | 面板参数**全来自 `VideoPlan`**（第 331-332 行） |
| fs（四批） | 格式/路径/落点预览 → 合并树（**句柄式**）→ 读条目/闸门/解包 → 封包 | 句柄用完 `fs_tree_free`（第 469-470 行） |
| 产物 | `pkg_write_manifest / pkg_install / pkg_restore` | 备份目录 + 逐条台账 + 覆盖可见 |
| 补丁 | `patch_identify / patch_apply` | 站点表是**数据**、不焊进内核；判定只看逐项字节（第 764-765 行） |

### 值得单独记的两点

- **fs 合并树是句柄式**（`fs_tree_build` 返回 `int`，`fs_tree_free` 释放）——因为 `MergeTree` 是
  **有状态的 C++ 对象**，而门面只跨"值"。这与 `UiHandle` 是同一手法。
- **闸门已焊在引擎读入口**（第 501-502 行）：`db.cpp / overlay.cpp / squashfs.cpp` 都调
  `plugin::gate_active_read` ⇒ 门面**不需要**传注册表，`active_registry()` 自己认。
  ⇒ 这与 `registry.h:344` 的 `set_active_registry` 是一对，**生命周期必须配对**。

---

## 8. 与当前红点的关系（可以直接用的结论）

1. **`fs_loose_entries` 的 `info` 可空**（第 700 行注释）：判语义一律用 `info->truncated`，
   **不解析文案** ⇒ 同理，我们判 P5/P13 的状态必须用**结构字段**，不能解析中文提示。
2. **`text_extract_dir` 的相对路径血账**：如果 P12c-2 的失败涉及路径，先查是不是相对路径解析。
3. **门面层"透不出去"是本仓的固有风险**：加任何新钩子，必须同时改
   `kMethodNames` / `ui_facade_has` / pybind `.def` / 判据读取 **四处**，否则就是 P16-0 那族 bug。

## 9. 待核对（下一批）

- `engine_facade_has()` / `engine_facade_method_names()` 的名单是否与上面 785 行里的成员**逐名一致**
  （`kMethodNames` 在哪一层维护）。
- `xml_modes()` 返回的 `XmlMode.plugin` 与 `registry.h` 的 `CompareModeReg.spec` 是否**同一哨兵**
  （`registry.h:19-20` 说统一成 `engine::xml::kBuiltinPlugin`）。
