// xrkit::core —— 最小 XLSX 写出器（见头文件里的边界说明）。
//
// 结构（最小可用集）：
//   [Content_Types].xml
//   _rels/.rels
//   xl/workbook.xml
//   xl/_rels/workbook.xml.rels
//   xl/styles.xml
//   xl/worksheets/sheet1.xml … sheetN.xml
//
// zip 用 **store**（不压缩）：条目小、无依赖；CRC32 自己算（20 行表）。
// 单元格用 `t="inlineStr"` 内联字符串，省掉 sharedStrings 的一致性负担。

#include "xrkit/core/xlsx.h"

#include <array>
#include <cstdio>
#include <cstring>

#include "xrkit/core/atomic_write.h"

namespace xrkit::core {
namespace {

// ── CRC32（zip 用；IEEE 802.3 多项式）──────────────────────────────────
std::uint32_t crc32_of(const std::uint8_t* data, std::size_t size) {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void put16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put32(std::string& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, (v >> 16) & 0xFFFF);
}

// XML 转义（文本/属性通用；控制字符换成空格，Excel 收到裸控制符会报错）
std::string xml_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') out.push_back(' ');
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string col_name(int index) {   // 0 → A, 25 → Z, 26 → AA
    std::string name;
    int n = index + 1;
    while (n > 0) {
        const int rem = (n - 1) % 26;
        name.insert(name.begin(), static_cast<char>('A' + rem));
        n = (n - 1) / 26;
    }
    return name;
}

std::string sheet_xml(const XlsxSheet& sheet) {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    out += "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    // 列宽
    if (!sheet.widths.empty()) {
        out += "<cols>";
        for (size_t i = 0; i < sheet.widths.size(); ++i) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "<col min=\"%zu\" max=\"%zu\" width=\"%.2f\" customWidth=\"1\"/>", i + 1,
                          i + 1, sheet.widths[i]);
            out += buf;
        }
        out += "</cols>";
    }
    // 冻结首行
    if (sheet.freeze_header) {
        out += "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
               "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews>";
    }
    // 自动筛选
    if (sheet.autofilter && !sheet.header.empty()) {
        out += "<autoFilter ref=\"A1:" + col_name(static_cast<int>(sheet.header.size()) - 1) +
               std::to_string(sheet.rows.size() + 1) + "\"/>";
    }
    out += "<sheetData>";
    const auto emit_row = [&out](int row_index, const std::vector<std::string>* header,
                                 const std::vector<XlsxCell>* cells) {
        out += "<row r=\"" + std::to_string(row_index) + "\">";
        const size_t count = header != nullptr ? header->size() : cells->size();
        for (size_t i = 0; i < count; ++i) {
            const std::string ref = col_name(static_cast<int>(i)) + std::to_string(row_index);
            if (header != nullptr) {
                out += "<c r=\"" + ref + "\" t=\"inlineStr\" s=\"1\"><is><t>" +
                       xml_escape((*header)[i]) + "</t></is></c>";
                continue;
            }
            const XlsxCell& cell = (*cells)[i];
            if (cell.is_number) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.10g", cell.number);
                out += "<c r=\"" + ref + "\"><v>" + buf + "</v></c>";
            } else {
                out += "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t>" + xml_escape(cell.text) +
                       "</t></is></c>";
            }
        }
        out += "</row>";
    };
    if (!sheet.header.empty()) emit_row(1, &sheet.header, nullptr);
    int row_index = sheet.header.empty() ? 1 : 2;
    for (const std::vector<XlsxCell>& cells : sheet.rows) emit_row(row_index++, nullptr, &cells);
    out += "</sheetData></worksheet>";
    return out;
}

void add_entry(std::string& zip, std::vector<std::string>& central, const std::string& name,
               const std::string& data) {
    const std::uint32_t crc = crc32_of(reinterpret_cast<const std::uint8_t*>(data.data()),
                                       data.size());
    const std::uint32_t size = static_cast<std::uint32_t>(data.size());
    const std::uint32_t offset = static_cast<std::uint32_t>(zip.size());

    std::string local;
    put32(local, 0x04034B50u);      // 本地文件头签名
    put16(local, 20);               // version needed
    put16(local, 0);                // flags
    put16(local, 0);                // method = store
    put16(local, 0);                // mod time
    put16(local, 0);                // mod date
    put32(local, crc);
    put32(local, size);             // 压缩后大小
    put32(local, size);             // 原始大小
    put16(local, static_cast<std::uint32_t>(name.size()));
    put16(local, 0);                // extra len
    local += name;
    local += data;
    zip += local;

    std::string cd;
    put32(cd, 0x02014B50u);         // 中央目录签名
    put16(cd, 20);                  // version made by
    put16(cd, 20);                  // version needed
    put16(cd, 0);                   // flags
    put16(cd, 0);                   // method
    put16(cd, 0);                   // time
    put16(cd, 0);                   // date
    put32(cd, crc);
    put32(cd, size);
    put32(cd, size);
    put16(cd, static_cast<std::uint32_t>(name.size()));
    put16(cd, 0);                   // extra
    put16(cd, 0);                   // comment
    put16(cd, 0);                   // disk
    put16(cd, 0);                   // internal attrs
    put32(cd, 0);                   // external attrs
    put32(cd, offset);              // 本地头偏移
    cd += name;
    central.push_back(cd);
}

}  // namespace

std::vector<uint8_t> render_xlsx(const std::vector<XlsxSheet>& sheets) {
    std::string zip;
    std::vector<std::string> central;

    std::string content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" "
        "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
    for (size_t i = 0; i < sheets.size(); ++i) {
        content_types += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1) +
                         ".xml\" "
                         "ContentType=\"application/vnd.openxmlformats-officedocument."
                         "spreadsheetml.worksheet+xml\"/>";
    }
    content_types += "</Types>";

    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
        "officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>";

    std::string workbook =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets>";
    for (size_t i = 0; i < sheets.size(); ++i) {
        workbook += "<sheet name=\"" + xml_escape(sheets[i].name) + "\" sheetId=\"" +
                    std::to_string(i + 1) + "\" r:id=\"rId" + std::to_string(i + 1) + "\"/>";
    }
    workbook += "</sheets></workbook>";

    std::string wb_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        wb_rels += "<Relationship Id=\"rId" + std::to_string(i + 1) +
                   "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                   "relationships/worksheet\" Target=\"worksheets/sheet" +
                   std::to_string(i + 1) + ".xml\"/>";
    }
    wb_rels += "</Relationships>";

    // 样式：0=默认，1=表头（加粗）
    const std::string styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"1\"><fill><patternFill patternType=\"none\"/></fill></fills>"
        "<borders count=\"1\"><border/></borders>"
        "<cellStyleXfs count=\"1\"><xf/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf xfId=\"0\"/><xf xfId=\"0\" fontId=\"1\" applyFont=\"1\"/></cellXfs>"
        "</styleSheet>";

    add_entry(zip, central, "[Content_Types].xml", content_types);
    add_entry(zip, central, "_rels/.rels", rels);
    add_entry(zip, central, "xl/workbook.xml", workbook);
    add_entry(zip, central, "xl/_rels/workbook.xml.rels", wb_rels);
    add_entry(zip, central, "xl/styles.xml", styles);
    for (size_t i = 0; i < sheets.size(); ++i) {
        add_entry(zip, central, "xl/worksheets/sheet" + std::to_string(i + 1) + ".xml",
                  sheet_xml(sheets[i]));
    }

    const std::uint32_t cd_offset = static_cast<std::uint32_t>(zip.size());
    std::uint32_t cd_size = 0;
    for (const std::string& entry : central) {
        zip += entry;
        cd_size += static_cast<std::uint32_t>(entry.size());
    }
    put32(zip, 0x06054B50u);   // EOCD
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, static_cast<std::uint32_t>(central.size()));
    put16(zip, static_cast<std::uint32_t>(central.size()));
    put32(zip, cd_size);
    put32(zip, cd_offset);
    put16(zip, 0);

    return std::vector<uint8_t>(zip.begin(), zip.end());
}

bool write_xlsx_file(const std::string& path, const std::vector<XlsxSheet>& sheets,
                     std::string* error) {
    if (sheets.empty()) {
        if (error != nullptr) *error = "XLSX 至少要一张表";
        return false;
    }
    const std::vector<uint8_t> bytes = render_xlsx(sheets);
    std::string why;
    if (write_file_atomic(path, bytes, /*atomic=*/true, why)) return true;
    if (error != nullptr) *error = why;
    return false;
}

}  // namespace xrkit::core
