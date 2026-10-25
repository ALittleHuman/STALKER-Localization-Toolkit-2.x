#pragma once
// xrkit::core —— **最小 XLSX 写出器**（无第三方依赖：store 方式打 zip + 内联字符串）。
//
// 为什么要有它（用户 2026-10-25 对 XML 校对的反馈）：
//   「导出 csv 是导出全部内容……这个报告也完全没有发挥表格的优势。」
//   CSV 做不到的三件事正好是表格的优势：**列宽**（路径被裁成 `zzzzz…`）、
//   **冻结表头**、**自动筛选**。所以给"给人看的报告"一条 XLSX 出口；
//   CSV 那条留给脚本/对拍（`render_csv` 不动）。
//
// 实现边界（故意的，别当成通用 Excel 库）：
//   · 只写 `.xlsx` 需要的最小部件：Content_Types / rels / workbook / 3 张表 / styles；
//   · 单元格只两种：**内联字符串**与**数字**（数字才能求和/排序，别一律当文本）；
//   · zip 用 **store**（不压缩）—— 报告量级小，省掉 deflate 依赖与一处潜在 bug；
//   · 表头样式=加粗（字体 1）；列宽按字符数给（Excel 的 wch 口径）；
//   · 首行冻结 + 自动筛选（可选）。
//
// 不支持的（写出来 Excel 也能打开，但这里不做）：合并单元格、公式、图表、多级样式。

#include <cstdint>
#include <string>
#include <vector>

namespace xrkit::core {

struct XlsxCell {
    std::string text;      // 字符串单元格
    double number = 0.0;   // 数字单元格
    bool is_number = false;

    static XlsxCell of_text(std::string value) {
        XlsxCell cell;
        cell.text = std::move(value);
        return cell;
    }
    static XlsxCell of_number(double value) {
        XlsxCell cell;
        cell.number = value;
        cell.is_number = true;
        return cell;
    }
};

struct XlsxSheet {
    std::string name;                    // 表名（≤31 字符、不含 : \ / ? * [ ]）
    std::vector<std::string> header;     // 表头（加粗、冻结、可筛选）
    std::vector<std::vector<XlsxCell>> rows;
    std::vector<double> widths;          // 每列宽度（字符数）；缺省 = 18
    bool freeze_header = true;
    bool autofilter = true;
};

// 渲染成完整的 .xlsx 字节（zip 容器）。`sheets` 至少一张。
std::vector<uint8_t> render_xlsx(const std::vector<XlsxSheet>& sheets);

// 原子写盘（同目录 .part → rename）；失败把理由写进 `error`。
bool write_xlsx_file(const std::string& path, const std::vector<XlsxSheet>& sheets,
                     std::string* error);

}  // namespace xrkit::core
