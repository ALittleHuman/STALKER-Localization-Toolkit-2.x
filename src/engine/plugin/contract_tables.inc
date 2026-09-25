// 生成物：xrkit::plugin 的契约表 —— **不许手改**。
// 由 tools/gen_plugin_contract.py 从 1.x 源码按 AST 读出；
// `--check` 在门禁里逐字节比对（不一致＝FAIL，不是 SKIP）。
// 生成源：toolkit_plugins.py（HOSTS/LOCATIONS/AREA_*）与
//         toolkit_plugin_ui.py（UI_METHODS + 后端类的方法签名）。
//
// ★ 本文件**不带 namespace 包裹**：由 includer 决定放进哪个命名空间
//   （与 font/adv_tables.inc 等生成物的惯例一致）。

inline constexpr SlotSpec kHosts[] = {
    {"fs", "文件系统"},
    {"convert", "编码转换"},
    {"text", "文本提取"},
    {"xml", "XML校对"},
    {"video", "视频转换"},
    {"font", "汉化包生成"},
};
inline constexpr int kHostCount = 6;

inline constexpr SlotSpec kLocations[] = {
    {"toolbar", "工具标题下方的一行插件动作区"},
    {"context", "工具主内容控件的右键菜单"},
};
inline constexpr int kLocationCount = 2;

inline constexpr SlotSpec kAreas[] = {
    {"top_bar", "top_bar"},  // AREA_TOP_BAR
    {"bottom", "bottom"},  // AREA_BOTTOM
    {"format", "format"},  // AREA_FORMAT
    {"pack_format", "pack_format"},  // AREA_PACK_FORMAT
    {"source_enc", "source_enc"},  // AREA_SOURCE_ENC
    {"game", "game"},  // AREA_GAME
    {"lang", "lang"},  // AREA_LANG
    {"size", "size"},  // AREA_SIZE
    {"suffix", "suffix"},  // AREA_SUFFIX
    {"filter", "filter"},  // AREA_FILTER
};
inline constexpr int kAreaCount = 10;

// api.ui 的契约：名字 + **参数名**（第三方就是照这两样写的）
inline constexpr UiMethodSpec kUiMethods[] = {
    {"label", "parent, text, style"},
    {"button", "parent, text, on_click, width, style, state"},
    {"row", "parent, side, fill, expand, padx, pady"},
    {"checkbox", "parent, text, variable, on_toggle"},
    {"entry", "parent, textvariable, width, show"},
    {"text", "parent, height, width, wrap"},
    {"combo", "parent, values, variable, width, on_change"},
    {"drop_box", "parent, on_file, placeholder, width"},
    {"pack", "widget, side, fill, expand, padx, pady, anchor"},
    {"set_text", "widget, text"},
    {"set_state", "widget, state"},
    {"text_view", "parent, height, mono"},
    {"alive", "widget"},
    {"ask_yes_no", "parent, title, message"},
    {"ask_open_file", "parent, title, filetypes, initialdir"},
    {"ask_save_file", "parent, title, initialdir, initialfile, filetypes"},
};
inline constexpr int kUiMethodCount = 16;
