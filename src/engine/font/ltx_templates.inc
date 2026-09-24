// 本文件由 tools/gen_font_templates.py 生成，请勿手改。
// 数据源：1.x font_pack/font_pack.py（sha256 733da57b5a4873aed1de829928ade4a2372004afed87766c575ac94a416cd315）
//
// `fonts.ltx` 与 `localization.ltx` 的模板：制表符、双反斜杠、空行都要
// 原样保留，否则产物就不是逐字节一致。`localization.ltx` 里的
// `_#LANG#` / `#LANG#` 是占位符（字体后缀 / 加载语言）。

struct GameTemplate {
    const char* game;
    const char* cfg_dir;
    const char* dds_fmt;
    const char* fonts_ltx;
    const char* localization_ltx;
};

static const GameTemplate kGameTemplates[3] = {
    {
        "SoC", "config", "8888",
        "[stat_font]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_01\ninterval\t\t= 0.75,1\n\n[hud_fo"
        "nt_small]:stat_font\n\n[hud_font_medium]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_02\n\n"
        "[hud_font_di]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_console_02\n\n[ui_font_arial_14]\nsha"
        "der\t\t\t= font\ntexture\t\t\t= ui\\ui_font_11\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui"
        "\\ui_font_13\n\n;not used\n[ui_font_arial_21]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_15\nte"
        "xture800\t\t= ui\\ui_font_13\ntexture1600\t\t= ui\\ui_font_17\n\n[ui_font_graffiti19_russian]\nshade"
        "r\t\t\t= font\ntexture\t\t\t= ui\\ui_font_16\ntexture800\t\t= ui\\ui_font_15\ntexture1600\t\t= ui\\u"
        "i_font_17\n\n[ui_font_graffiti22_russian]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_17\ntextur"
        "e800\t\t= ui\\ui_font_16\ntexture1600\t\t= ui\\ui_font_20\n\n[ui_font_graff_32]\nshader\t\t\t= font"
        "\ntexture\t\t\t= ui\\ui_font_18\ntexture800\t\t= ui\\ui_font_17\ntexture1600\t\t= ui\\ui_font_21\n\n"
        ";not used\n[ui_font_graff_40]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_21\ntexture800\t\t= ui"
        "\\ui_font_18\ntexture1600\t\t= ui\\ui_font_23\n\n[ui_font_graff_50]\nshader\t\t\t= font\ntexture\t\t"
        "\t= ui\\ui_font_23\ntexture800\t\t= ui\\ui_font_21\ntexture1600\t\t= ui\\ui_font_25\n\n[ui_font_lett"
        "erica16_russian]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_11\ntexture800\t\t= ui\\ui_font_11"
        "\ntexture1600\t\t= ui\\ui_font_15\n\n[ui_font_letterica18_russian]\nshader\t\t\t= font\ntexture\t\t"
        "\t= ui\\ui_font_15\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui\\ui_font_16\n\n[ui_font_lett"
        "er_25]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_17\ntexture800\t\t= ui\\ui_font_15\ntexture16"
        "00\t\t= ui\\ui_font_20\n\n",
        "[string_table]\nlanguage\t= #LANG#\nfont_prefix\t= _#LANG#\nfiles = ui_st_pda, ui_st_mm_mp, ui_st_in"
        "ventory, string_table_tutorial, string_table_general, string_table_includes, stable_dialog_manager, "
        "stable_dialog_manager_uni, stable_task_manager, stable_treasure_manager, string_table_level_tips, st"
        "ring_table_items, string_table_ui, string_table_enc_zone, string_table_outfit, stable_dialogs, stabl"
        "e_dialogs_escape, stable_dialogs_garbage, stable_dialogs_agroprom, stable_dialogs_deadcity, stable_d"
        "ialogs_darkvalley, stable_dialogs_pripyat, stable_dialogs_labx18, stable_dialogs_bar, stable_dialogs"
        "_military, stable_dialogs_yantar, stable_dialogs_radar, stable_dialogs_aes, mp_st_speechmenu, ui_st_"
        "keybinding, ui_mp_teamdesc, ui_st_mm, stable_stories, ui_st_mapdesc, string_table_enc_social, string"
        "_table_enc_mutants, string_table_enc_weapons, string_table_enc_equipment, ui_st_mp, ui_st_other, sta"
        "ble_game_credits\n",
    },
    {
        "ClS", "configs", "8888",
        "[stat_font]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_01\ninterval\t\t= 0.75,1\n\n[hud_fo"
        "nt_small]:stat_font\n\n[hud_font_medium]\nshader \t\t\t= font\ntexture\t\t= ui\\ui_font_hud_02\n\n[h"
        "ud_font_di]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_console_02\n\n[hud_font_di2]\nshader \t"
        "\t\t= hud\\font2\ntexture\t\t\t= ui\\ui_font_console_02\n\n[ui_font_arial_14]\nshader\t\t\t= font\nt"
        "exture\t\t\t= ui\\ui_font_11\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui\\ui_font_13\n\n;no"
        "t used\n[ui_font_arial_21]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_15\ntexture800\t\t= ui\\u"
        "i_font_13\ntexture1600\t\t= ui\\ui_font_17\n\n[ui_font_graffiti19_russian]\nshader\t\t\t= font\ntext"
        "ure\t\t\t= ui\\ui_font_16\ntexture800\t\t= ui\\ui_font_15\ntexture1600\t\t= ui\\ui_font_17\n\n[ui_fo"
        "nt_graffiti22_russian]\nshader\t\t= font\ntexture\t\t\t= ui\\ui_font_17\ntexture800\t\t= ui\\ui_font"
        "_16\ntexture1600\t\t= ui\\ui_font_20\n\n[ui_font_graff_32]\nshader\t\t\t= font\ntexture\t\t\t= ui\\u"
        "i_font_18\ntexture800\t\t= ui\\ui_font_17\ntexture1600\t\t= ui\\ui_font_21\n\n;not used\n[ui_font_gr"
        "aff_40]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_21\ntexture800\t\t= ui\\ui_font_18\ntexture1"
        "600\t\t= ui\\ui_font_23\n\n[ui_font_graff_50]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_23\nte"
        "xture800\t\t= ui\\ui_font_21\ntexture1600\t\t= ui\\ui_font_25\n\n[ui_font_letterica16_russian]\nshad"
        "er\t\t\t= font\ntexture\t\t\t= ui\\ui_font_11\ntexture800\t\t= ui\\ui_font_11\n;texture800\t\t= ui\\"
        "ui_font_11\ntexture1600\t\t= ui\\ui_font_15\n\n[ui_font_letterica18_russian]\nshader\t\t\t= font\nte"
        "xture\t\t\t= ui\\ui_font_15\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui\\ui_font_16\n\n[ui_"
        "font_letter_25]\nshader\t\t\t= font\ntexture\t\t\t= ui\\ui_font_17\ntexture800\t\t= ui\\ui_font_15\n"
        "texture1600\t\t= ui\\ui_font_20\n\n",
        "[string_table]\nlanguage        = #LANG#\nfont_prefix     = _#LANG#\n",
    },
    {
        "CoP", "configs", "A8",
        "[stat_font]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_01\ninterval\t\t= 0.75,1\n\n[hud_fo"
        "nt_small]:stat_font\n\n[hud_font_medium]\nshader \t\t\t= font\ntexture\t\t\t= ui\\ui_font_hud_02\n\n"
        "[hud_font_di]\nshader \t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_console_02\n\n[hud_font_di2]\nsh"
        "ader \t\t\t= hud\\font2\ntexture\t\t\t= ui\\ui_font_console_02\n\n[ui_font_arial_14]\nshader\t\t\t= "
        "hud\\font\ntexture\t\t\t= ui\\ui_font_11\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui\\ui_fo"
        "nt_13\n\n;not used\n[ui_font_arial_21]\nshader\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_15\ntext"
        "ure800\t\t= ui\\ui_font_13\ntexture1600\t\t= ui\\ui_font_17\n\n[ui_font_graffiti19_russian]\nshader"
        "\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_16\ntexture800\t\t= ui\\ui_font_15\ntexture1600\t\t= u"
        "i\\ui_font_17\n\n[ui_font_graffiti22_russian]\nshader\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_1"
        "7\ntexture800\t\t= ui\\ui_font_16\ntexture1600\t\t= ui\\ui_font_20\n\n[ui_font_graff_32]\nshader\t\t"
        "\t= hud\\font\ntexture\t\t\t= ui\\ui_font_18\ntexture800\t\t= ui\\ui_font_17\ntexture1600\t\t= ui\\u"
        "i_font_21\n\n;not used\n[ui_font_graff_40]\nshader\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_21\n"
        "texture800\t\t= ui\\ui_font_18\ntexture1600\t\t= ui\\ui_font_23\n\n[ui_font_graff_50]\nshader\t\t\t="
        " hud\\font\ntexture\t\t\t= ui\\ui_font_23\ntexture800\t\t= ui\\ui_font_21\ntexture1600\t\t= ui\\ui_f"
        "ont_25\n\n[ui_font_letterica16_russian]\nshader\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_11\ntex"
        "ture800\t\t= ui\\ui_font_11\ntexture1600\t\t= ui\\ui_font_15\n\n[ui_font_letterica18_russian]\nshade"
        "r\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_15\ntexture800\t\t= ui\\ui_font_11\ntexture1600\t\t= "
        "ui\\ui_font_16\n\n[ui_font_letter_25]\nshader\t\t\t= hud\\font\ntexture\t\t\t= ui\\ui_font_17\ntextu"
        "re800\t\t= ui\\ui_font_15\ntexture1600\t\t= ui\\ui_font_20\n",
        "[string_table]\nlanguage        = #LANG#\nfont_prefix     = _#LANG#\n",
    },
};

// 字号表（1.x `SIZES`）：槽位名 + 是否超采样；渲染字号 = 槽位名 + offset，
// 但**文件名与 ltx 仍用槽位名**。
struct FontSize {
    int size;
    int x2;
};

static const FontSize kFontSizes[10] = {
    {11, 0},
    {13, 0},
    {15, 0},
    {16, 1},
    {17, 1},
    {18, 1},
    {20, 1},
    {21, 1},
    {23, 1},
    {25, 1},
};
