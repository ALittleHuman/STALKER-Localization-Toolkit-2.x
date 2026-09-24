// 本文件由 tools/gen_font_templates.py 生成，请勿手改。
// 数据源：1.x font_pack/font_pack.py（sha256 733da57b5a4873aed1de829928ade4a2372004afed87766c575ac94a416cd315）
//
// 这些是 1.x 的字符集分组字面量：手抄一个字符就会让某个字在游戏里
// 画成**同一个重复字形**（引擎对缺失码位不是留空），且毫无报错。

// ASCII + 俄语基础字符（1.x `ASCII_CHARS`）
static const char* const kAsciiChars =
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~АаБ"
    "бВвГгДдЕеЁёЖжЗзИиЙйКкЛлМмНнОоПпРрСсТтУуФфХхЦцЧчШшЩщъЫыьЭэЮюЯя";

// 完整西里尔块（1.x `CYRILLIC_BLOCK`，实测 U+0410..U+04FF 连续）
static const uint32_t kCyrillicBlockFrom = 0x0410u;
static const uint32_t kCyrillicBlockTo = 0x04FFu;

// 1.x `ANSI_BRIDGE`：引擎的 ansi 桥接会把这些字节映到这些槽位
static const char* const kAnsiBridge = "ЁЄІЇёєіїҐґ—№";

// 1.x `FONTGEN_EXTRA`：原版 Char.txt 编码 bug 丢掉的生僻汉字
static const char* const kFontgenExtra = "屑懈挟衼袗袘袙袚袛袝袞袟袠袡袣袥袦袧袨袩袪褉褋褌褍褎褏褑褔褕褖褗褘褜褝褞褟褢";

// 1.x `LOST_CYRILLIC`：原版 CharAdder 编码 bug 丢掉的西里尔
// （只有 full_cyrillic=False 的「复刻旧包」模式才剔除；默认保留）
static const char* const kLostCyrillic = "ГЙФХЦЧШЩЭЮбвгдежзийклмнопрстуфхцчшщъыьэю";
