// xrkit::engine::text —— 源/目标两张编码表的唯一实现（口径见 `encodings.h`）。
//
// 这段注释与表项**逐字从 `convert_page.cpp` 搬来**（那里的历史说明照旧有效，别删）：
//   · 2026-10-06（D2 批）**源/目标拆成两张表**。理由不是"好看"，是纪律：
//     · 目标侧多一个 `ascii`（1.x 的目标下拉里有它）。补它对拍时**当场抓到一个真缺陷**：
//       引擎的 ascii 原来只有解码侧、编码侧一行都没有 ⇒ 连纯 ASCII 正文也被整篇写成 `?`
//       （见 `encoding.cpp` 的 `encode_strict` 与 `golden_lossy_codec.py`）。要不是"只列
//       对拍过的编码"这条纪律卡着，用户会静默拿到一整个目录的 `?`。
//     · 源侧**不放** `ascii`：源是"猜"的，把 ascii 塞进源下拉只会让人以为工具会猜 ascii。
//   · 2026-10-05（D3 批）：`big5 / shift_jis / euc-kr` 三项**补进这张共用表**（⇒ 源、目标
//     两个下拉都出现）。1.x 侧的口径是：源下拉 = `ENCODING_CHOICES`（`toolkit_constants.py:76`
//     `["auto","utf-8","gbk","gb2312","big5","shift_jis","euc-kr","windows-1251",
//     "windows-1252","iso-8859-1","ascii","utf-16"]`），目标下拉 =
//     `convert_app.py:192` 的 `["utf-8","gbk","gb2312","windows-1251","ascii","utf-16","big5"]`。
//     两边的**交集**里这三个名字都有（big5 两边都在，shift_jis/euc-kr 源侧在）。
//     三张表已按"双字节 + 编码侧"全量对拍（`tests/golden_lossy_codec.py`），
//     所以按"只列对拍过的编码"这条纪律，它们可以列了。

#include "xrkit/engine/text/encodings.h"

#include "xrkit/engine/text/encoding.h"

namespace xrkit::engine::text {

const std::vector<std::string>& manual_encodings() {
    static const std::vector<std::string> table = {
        kUtf8,       kWindows1251, kWindows1252, kGb18030, kGbk,     kGb2312,
        kBig5,       kShiftJis,    kEucKr,       kLatin1,
        // 2026-09-26：utf-16/32 六族的有损编解码已与 Python `errors="replace"` 逐字节对拍
        //（门禁 [12] 的 compared 从 114 涨到 294）⇒ 按"只列对拍过的编码"这条纪律可以列了。
        kUtf16,      kUtf16Le,     kUtf16Be,     kUtf32,   kUtf32Le, kUtf32Be};
    return table;
}

const std::vector<std::string>& target_only_encodings() {
    // 目标侧多一项：ascii（D2 批补上对拍后才允许出现，见上面的长注释）
    static const std::vector<std::string> table = {kAscii};
    return table;
}

}  // namespace xrkit::engine::text
