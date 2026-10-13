#include "xrkit/engine/fs/db.h"
#include "xrkit/engine/fs/untrusted_limits.h"   // ★ P17-13（01 F-08）

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

#include "minilzo.h"

#include "xrkit/core/path.h"
#include "xrkit/engine/fs/lzhuf.h"
#include "xrkit/engine/plugin/registry.h"

namespace xrkit::engine::fs {
namespace {

#include "cp1251_tables.inc"

constexpr uint32_t kScramblerMul = 0x8088405u;
// ★ P17-13（01 F-08）：取值来自唯一一处定义（untrusted_limits.h），不再各自写死
constexpr uint64_t kMaxDecompressBytes = XRKIT_DB_MAX_ENTRY_DECOMPRESS_BYTES;
constexpr uint64_t kMaxLzoRatio = 1024;

// ── cp1251 ↔ UTF-8（DB 里的文件名是 cp1251）──────────────────────────
std::string cp1251_to_utf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        uint32_t cp = p[i];
        if (cp >= 0x80) cp = kCp1251ToUnicode[cp - 0x80];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// 反向表：Unicode 码点 → cp1251 字节（未定义码位不参与）
const std::array<int16_t, 0x10000>& unicode_to_cp1251() {
    static const std::array<int16_t, 0x10000> table = [] {
        std::array<int16_t, 0x10000> t{};
        t.fill(-1);
        for (int b = 0; b < 128; ++b) t[static_cast<size_t>(b)] = static_cast<int16_t>(b);
        for (int i = 0; i < 128; ++i) {
            const uint16_t cp = kCp1251ToUnicode[i];
            if (cp == 0xFFFD) continue;  // 未定义字节不参与编码
            t[cp] = static_cast<int16_t>(0x80 + i);
        }
        return t;
    }();
    return table;
}

// ★ 2026-10-06 修的真缺陷：**不可表示的码点必须逐码点顶替，绝不截断**。
//   原来是"遇到不可表示就 `return false`"，而两个调用点（`append_entry_null` /
//   `append_entry_xdb`）**忽略返回值**、把失败前已追加的那一段当名字写进 HEADER。
//   于是 UTF-8 名被静默**截断**，甚至截成**空名**：
//     `sounds/音效`（目录）      → `sounds/`   （与已有的 `sounds` 目录**撞名**）
//     `sounds/音效/虫鸣.wav`     → `sounds/`   （文件条目也叫 `sounds`，再撞一次）
//     `中文/说明.txt`            → ``          （**空名条目**）
//   实测后果（官方 `converter.exe -unpack -2215`）：**栈溢出** `0xC00000FD`
//   （exit 3221225725 = -1073741571）、产物 0 文件。只含 ASCII 或 cp1251 名的包都不触发。
//   修法：不可表示 ⇒ 写 `?`（Windows/XR 的惯例顶替符；非 UTF-8 字节同样顶替并前进 1 字节）
//   ⇒ 名字长度与源路径**一一对应**，既不会空名也不会与别的目录/文件撞名。
//   返回值语义随之收窄成"这次编码**有没有发生顶替**"，不再是"能不能编"。
bool utf8_to_cp1251(const std::string& s, std::vector<uint8_t>& out) {
    const auto& rev = unicode_to_cp1251();
    bool lossy = false;
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t len = 1;
        bool ok = true;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= s.size()) {
                ok = false;
            } else {
                cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
                len = 2;
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= s.size()) {
                ok = false;
            } else {
                cp = ((c & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                     (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
                len = 3;
            }
        } else {
            ok = false;
        }
        if (!ok || cp >= 0x10000 || rev[cp] < 0) {
            out.push_back('?');  // ★ 顶替，不截断（截断 ⇒ 空名/撞名 ⇒ cv 栈溢出）
            lossy = true;
            i += len;            // 非 UTF-8 字节时 len == 1 ⇒ 每字节消耗一个 `?`
            continue;
        }
        out.push_back(static_cast<uint8_t>(rev[cp]));
        i += len;
    }
    return !lossy;
}

std::string normalize_path(std::string s) {
    for (char& ch : s) {
        if (ch == '\\') ch = '/';
    }
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// ── 小端读取 ─────────────────────────────────────────────────────────
uint16_t rd16(const std::vector<uint8_t>& d, size_t p) {
    return static_cast<uint16_t>(d[p] | (d[p + 1] << 8));
}
uint32_t rd32(const std::vector<uint8_t>& d, size_t p) {
    return static_cast<uint32_t>(d[p]) | (static_cast<uint32_t>(d[p + 1]) << 8) |
           (static_cast<uint32_t>(d[p + 2]) << 16) | (static_cast<uint32_t>(d[p + 3]) << 24);
}
void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

// ── Scrambler（2947ru / 2947ww）──────────────────────────────────────
class Scrambler {
public:
    Scrambler(uint32_t seed, uint32_t s0, int mult) : seed_(seed) {
        for (int i = 0; i < 256; ++i) enc_[i] = static_cast<uint8_t>(i);
        uint32_t s = s0;
        for (int k = 0; k < mult * 256; ++k) {
            uint32_t a = 0;
            uint32_t b = 0;
            s = 1 + s * kScramblerMul;
            a = (s >> 24) & 0xFF;
            while (true) {
                s = 1 + s * kScramblerMul;
                b = (s >> 24) & 0xFF;
                if (a != b) break;
            }
            std::swap(enc_[a], enc_[b]);
        }
        for (int i = 0; i < 256; ++i) dec_[enc_[i]] = static_cast<uint8_t>(i);
    }

    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data) const {
        uint32_t s = seed_;
        std::vector<uint8_t> r(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            s = 1 + s * kScramblerMul;
            r[i] = static_cast<uint8_t>(enc_[data[i]] ^ ((s >> 24) & 0xFF));
        }
        return r;
    }

    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data) const {
        uint32_t s = seed_;
        std::vector<uint8_t> r(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            s = 1 + s * kScramblerMul;
            r[i] = dec_[data[i] ^ ((s >> 24) & 0xFF)];
        }
        return r;
    }

private:
    uint32_t seed_;
    uint8_t enc_[256] = {};
    uint8_t dec_[256] = {};
};

const Scrambler* scrambler_for(DbFormat f) {
    static const Scrambler ru(0x131A9D3u, 0x1329436u, 8);
    static const Scrambler ww(0x16EB2EBu, 0x5BBC4Bu, 4);
    if (f == DbFormat::Ru2947) return &ru;
    if (f == DbFormat::Ww2947) return &ww;
    return nullptr;
}

// ── 条目解析（六格式）────────────────────────────────────────────────
std::vector<DbEntry> parse_xdb(const std::vector<uint8_t>& d) {
    std::vector<DbEntry> out;
    size_t p = 0;
    while (p + 2 <= d.size()) {
        const uint16_t nf = rd16(d, p);
        if (nf < 16) break;  // ★ 终止判据
        p += 2;
        const size_t nl = nf - 16;  // ★ 名字长度是 nf - 16
        if (p + 12 + nl + 4 > d.size()) break;
        DbEntry e;
        e.size_real = rd32(d, p);
        e.size_comp = rd32(d, p + 4);
        e.crc = rd32(d, p + 8);
        p += 12;
        e.path = normalize_path(cp1251_to_utf8(d.data() + p, nl));
        p += nl;
        e.offset = rd32(d, p);
        p += 4;
        e.is_dir = (e.offset == 0 && e.size_real == 0 && e.size_comp == 0);
        out.push_back(std::move(e));
    }
    return out;
}

// null 系：name\0 + nf 个 u32
std::vector<std::pair<std::string, std::vector<uint32_t>>> parse_null(const std::vector<uint8_t>& d,
                                                                     int nf) {
    std::vector<std::pair<std::string, std::vector<uint32_t>>> out;
    size_t p = 0;
    while (p < d.size()) {
        size_t n = p;
        while (n < d.size() && d[n] != 0) ++n;
        if (n >= d.size()) break;
        std::string nm = normalize_path(cp1251_to_utf8(d.data() + p, n - p));
        p = n + 1;
        if (p + static_cast<size_t>(nf) * 4 > d.size()) break;
        std::vector<uint32_t> fs(static_cast<size_t>(nf));
        for (int i = 0; i < nf; ++i) fs[static_cast<size_t>(i)] = rd32(d, p + static_cast<size_t>(i) * 4);
        p += static_cast<size_t>(nf) * 4;
        out.emplace_back(std::move(nm), std::move(fs));
    }
    return out;
}

// 注意：1.x 的 `_fmt_null(d, nf, idx)` 里 idx 索引的是 **(名字, 字段0, 字段1, …)** 这个
// **含名字的元组**；本实现把名字拆成了 kv.first，所以这里必须整体减一：
//   2945: 1.x idx=(0,2,3,4,2) → 字段 (1,2,3,1)
//   2215: 1.x idx=(0,1,2,3,1) → 字段 (0,1,2,0)
// 第一版照抄了 1.x 的索引，于是 sc_i=3 越界读到别的内存（两次运行值都不一样），
// 只有 11xx（走的是独立函数、索引本来就是字段下标）是对的 —— 这就是当时的线索。
std::vector<DbEntry> fmt_null(const std::vector<uint8_t>& d, int nf, int off_i, int sr_i, int sc_i,
                              int dir_i) {
    std::vector<DbEntry> out;
    for (auto& kv : parse_null(d, nf)) {
        const size_t n = kv.second.size();
        if (off_i < 0 || sr_i < 0 || sc_i < 0 || dir_i < 0 ||
            static_cast<size_t>(off_i) >= n || static_cast<size_t>(sr_i) >= n ||
            static_cast<size_t>(sc_i) >= n || static_cast<size_t>(dir_i) >= n) {
            continue;  // 畸形条目：跳过，绝不越界读
        }
        DbEntry e;
        e.path = kv.first;
        e.offset = kv.second[static_cast<size_t>(off_i)];
        e.size_real = kv.second[static_cast<size_t>(sr_i)];
        e.size_comp = kv.second[static_cast<size_t>(sc_i)];
        e.crc = 0;
        e.is_dir = (kv.second[static_cast<size_t>(dir_i)] == 0);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<DbEntry> fmt_11xx(const std::vector<uint8_t>& d) {
    std::vector<DbEntry> out;
    for (auto& kv : parse_null(d, 3)) {  // (name, uncompressed, offset, size)
        DbEntry e;
        e.path = kv.first;
        const uint32_t uncomp = kv.second[0];
        e.offset = kv.second[1];
        const uint32_t sz = kv.second[2];
        e.size_real = uncomp ? sz : 0;
        e.size_comp = sz;
        e.crc = 0;
        e.is_dir = (e.offset == 0);
        e.use_lzhuf = (uncomp == 0);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<DbEntry> parse_format(const std::vector<uint8_t>& d, DbFormat f) {
    switch (f) {
        case DbFormat::Xdb:
        case DbFormat::Ru2947:
        case DbFormat::Ww2947: return parse_xdb(d);
        case DbFormat::F2945: return fmt_null(d, 4, 1, 2, 3, 1);
        case DbFormat::F2215: return fmt_null(d, 3, 0, 1, 2, 0);
        case DbFormat::F11xx: return fmt_11xx(d);
        case DbFormat::Unknown: break;
    }
    return {};
}

// ── 自洽性（唯一判据，两处共用：拒伪造 HEADER 与 auto_detect）────────
// ★ seek 式读盘拿不到 `raw` ⇒ 判据改成**显式传文件大小**（语义与 `raw.size()` 完全相同）。
bool entries_fit_size(uint64_t file_size, const std::vector<DbEntry>& entries) {
    for (const DbEntry& e : entries) {
        if (e.is_dir) continue;
        if (e.path.empty()) return false;
        for (char c : e.path) {
            if (static_cast<unsigned char>(c) < 32) return false;
        }
        const uint64_t sc = e.size_comp;
        if (e.offset == 0 || e.offset + sc > file_size) return false;
        if (e.size_comp != 0 && e.size_real > sc * kMaxLzoRatio) return false;
    }
    return true;
}

bool entries_fit_file(const std::vector<uint8_t>& raw, const std::vector<DbEntry>& entries) {
    return entries_fit_size(raw.size(), entries);
}

bool entries_self_consistent_size(uint64_t file_size, const std::vector<DbEntry>& entries) {
    if (entries.empty()) return false;
    if (!entries_fit_size(file_size, entries)) return false;
    return std::any_of(entries.begin(), entries.end(), [](const DbEntry& e) { return !e.is_dir; });
}

bool entries_self_consistent(const std::vector<uint8_t>& raw,
                             const std::vector<DbEntry>& entries) {
    return entries_self_consistent_size(raw.size(), entries);
}

// ── HEADER 块校验（fmt 已知时不要求 ASCII）───────────────────────────
bool validate_header(const std::vector<uint8_t>& hdr, bool comp, DbFormat fmt,
                     std::vector<DbEntry>& out) {
    const bool strict = (fmt == DbFormat::Unknown);
    const DbFormat candidates[6] = {DbFormat::Xdb,    DbFormat::Ru2947, DbFormat::Ww2947,
                                    DbFormat::F2945,  DbFormat::F2215,  DbFormat::F11xx};
    const int count = strict ? 6 : 1;
    for (int ci = 0; ci < count; ++ci) {
        const DbFormat f = strict ? candidates[ci] : fmt;
        std::vector<uint8_t> data = hdr;
        const Scrambler* sc = scrambler_for(f);
        std::string err;
        if (comp) {
            if (sc) data = sc->decrypt(data);
            std::vector<uint8_t> plain;
            if (!lzhuf_decode(data.data(), data.size(), plain, err)) continue;
            data = std::move(plain);
        } else if (sc) {
            data = sc->decrypt(data);
        }
        std::vector<DbEntry> entries = parse_format(data, f);
        if (entries.empty()) continue;
        for (const DbEntry& e : entries) {
            if (e.is_dir) continue;
            if (e.size_real >= 2000000000ull) continue;
            const std::string& p = e.path;
            if (p.size() < 1) continue;
            bool ctrl = false;
            for (char c : p) {
                if (static_cast<unsigned char>(c) < 32) ctrl = true;
            }
            if (ctrl) continue;
            if (strict) {
                bool ascii = true;
                for (char c : p) {
                    if (static_cast<unsigned char>(c) >= 127) ascii = false;
                }
                if (!ascii) continue;
                const size_t lim = std::min<size_t>(8, p.size());
                bool has_alpha = false;
                for (size_t i = 0; i < lim; ++i) {
                    if ((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z')) has_alpha = true;
                }
                if (!has_alpha) continue;
            }
            out = std::move(entries);
            return true;
        }
    }
    return false;
}

// ── chunk 扫描（结构自洽优先）────────────────────────────────────────
struct Chunks {
    bool has_data = false;
    bool has_header = false;
    bool comp = false;
    std::vector<uint8_t> header;
};

bool read_chunks(const std::vector<uint8_t>& raw, DbFormat fmt, Chunks& out) {
    std::vector<uint8_t> fallback;
    bool fallback_comp = false;
    bool has_fallback = false;

    size_t pos = 0;
    while (pos + 8 <= raw.size()) {
        const uint32_t rid = rd32(raw, pos);
        const uint32_t sz = rd32(raw, pos + 4);
        const uint32_t cid = rid & 0x7FFFFFFFu;
        // ★★ 2026-10-06（审计处理批·1-2 / S1-5）：修前判据是 `8ull + sz <= raw.size()` ——
        //   **漏了 `pos`**，而切片起点是 `pos + 8`：
        //     · `pos > 0`（逐字节回退或前一个块推进过）时，只要 `pos` 处的 4 字节"看起来像
        //       块头"（`cid <= 1`），且 `8 + sz <= filesize < pos + 8 + sz`，迭代器末端就越过
        //       `raw` 末尾 ⇒ **vector 按 sz 字节从越界地址 memcpy**（堆越界读）。
        //   另一重：`static_cast<long>` 在 Windows x64 是 **32 位**，`pos + 8 + sz > 2^31`
        //   时截断成负数 ⇒ `raw.begin() + 负值` 是野指针。>2 GiB 的包走整文件回退路径时必踩。
        //   修法：判据与切片起点**同源**（`pos + 8ull + sz <= raw.size()` —— 后面的 `+ 8`
        //   都在 uint64 域里算，`8ull` 保证前两项不溢出），并去掉 `long` 窄化。
        if (cid <= 1 && pos + 8ull + sz <= raw.size() && sz > 0) {
            const bool comp = (rid & 0x80800000u) != 0;
            const size_t data_begin = pos + 8;
            const size_t data_end = data_begin + sz;
            std::vector<uint8_t> data(raw.begin() + static_cast<std::ptrdiff_t>(data_begin),
                                      raw.begin() + static_cast<std::ptrdiff_t>(data_end));
            if (cid == 0) {
                if (!out.has_data) {
                    out.has_data = true;
                    out.comp = comp;  // DATA 块的 comp 位（1.x 记了但只在 HEADER 用）
                }
            } else {
                std::vector<DbEntry> entries;
                if (validate_header(data, comp, fmt, entries)) {
                    if (entries_self_consistent(raw, entries)) {
                        out.has_header = true;
                        out.comp = comp;
                        out.header = std::move(data);
                    } else if (!has_fallback) {
                        has_fallback = true;
                        fallback = std::move(data);
                        fallback_comp = comp;
                    }
                    if (out.has_data && out.has_header) return true;
                }
            }
            pos += 8 + sz;
        } else {
            pos += 1;  // 逐字节回退：非 8 字节对齐的数据也要能认出来
        }
        if (pos > raw.size() - 8) break;
    }
    if (!out.has_header && has_fallback) {
        out.has_header = true;
        out.comp = fallback_comp;
        out.header = std::move(fallback);
    }
    return out.has_header;
}

// ── seek 式块链行走（大包）───────────────────────────────────────────
// 与 `read_chunks` 用**同一个**判据认块（`cid <= 1 && sz > 0 && 8 + sz <= 剩余`），
// 区别只是**不把块体读进来**：读到 8 字节块头就按 `sz` 跳过去。
// 为什么这样走与旧路径等价：`read_chunks` 在 `pos == 0` 认出 DATA 块时也是
// `pos += 8 + sz` 跳过去的（逐字节回退只发生在**第一个块头就不合法**时）⇒ 只要
// 第一个块头合法，两条路走的是**同一条链**、找到的是**同一个 HEADER 块**。
// 任何一步对不上就返回 false，让调用方退回整文件读（不猜）。
uint32_t le32_at(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// HEADER 表的合理上界：真档案的条目表是 MB 级；超过这个数说明要么不是标准块结构、
// 要么是坏值 ⇒ 退回整文件读（绝不为一个 4 GB 的假 sz 分配内存）。
constexpr uint64_t kMaxSeekHeaderBytes = XRKIT_DB_MAX_SEEK_HEADER_BYTES;   // ★ P17-13（01 F-08）
// "找块链起点"的探测窗口与次数上限（**有界**，绝不退化成扫全文件）
constexpr uint64_t kStartProbeBytes = 4ull * 1024 * 1024;
constexpr int kMaxStartProbes = 64;

// 一个块头是否合法（与 `read_chunks` 同一判据）
bool chunk_head_at(const uint8_t* p, uint64_t remain, uint32_t& rid, uint32_t& sz) {
    rid = le32_at(p);
    sz = le32_at(p + 4);
    const uint32_t cid = rid & 0x7FFFFFFFu;
    return cid <= 1 && sz > 0 && 8ull + sz <= remain;
}

bool walk_chain_from(std::ifstream& in, uint64_t file_size, uint64_t start,
                     std::vector<uint8_t>& header, bool& comp) {
    uint64_t pos = start;
    while (pos + 8 <= file_size) {
        in.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        uint8_t head[8] = {};
        in.read(reinterpret_cast<char*>(head), 8);
        if (in.gcount() != 8) return false;
        uint32_t rid = 0;
        uint32_t sz = 0;
        if (!chunk_head_at(head, file_size - pos, rid, sz)) return false;
        if ((rid & 0x7FFFFFFFu) == 1) {
            if (sz > kMaxSeekHeaderBytes) return false;
            std::vector<uint8_t> data(static_cast<size_t>(sz));
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
            if (in.gcount() != static_cast<std::streamsize>(sz)) return false;
            header = std::move(data);
            comp = (rid & 0x80800000u) != 0;   // 与 `read_chunks` 同一处取 comp 位
            return true;
        }
        pos += 8ull + sz;   // DATA 块：整块跳过（这正是省掉 8 s 的地方）
    }
    return false;
}

bool seek_header_chunk(std::ifstream& in, uint64_t file_size, std::vector<uint8_t>& header,
                       bool& comp) {
    // ① 标准档案：第一个块头就在文件开头 ⇒ 直接走链，一个多余字节都不读
    if (walk_chain_from(in, file_size, 0, header, comp)) return true;

    // ② 有些包在块链之前带一段**文本前导**（实测 `IWP\database\patch\xpatch_textures.db0`
    //    开头是 `[header]\r\nauto_load = true…`）⇒ 旧路径靠"逐字节回退扫描"找到真正的块头。
    //    这里把那次扫描限制在开头 4 MB 里（前导是几百字节级），并且只在**候选位置**上
    //    试走链（合法块头要求前 4 字节的 cid∈{0,1}、后 4 字节是合理的 sz）——**有界**，
    //    绝不退化成"把 2 GB 读进来再扫"。
    const uint64_t probe = std::min<uint64_t>(file_size, kStartProbeBytes);
    std::vector<uint8_t> win(static_cast<size_t>(probe));
    in.clear();
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(win.data()), static_cast<std::streamsize>(probe));
    const size_t got = static_cast<size_t>(in.gcount());
    int tries = 0;
    for (size_t p = 1; p + 8 <= got; ++p) {
        uint32_t rid = 0;
        uint32_t sz = 0;
        if (!chunk_head_at(win.data() + p, file_size - p, rid, sz)) continue;
        if (++tries > kMaxStartProbes) return false;
        if (walk_chain_from(in, file_size, p, header, comp)) return true;
    }
    return false;
}

}  // namespace

// ── 公开接口 ─────────────────────────────────────────────────────────
const char* db_format_key(DbFormat f) {
    switch (f) {
        case DbFormat::Xdb: return "xdb";
        case DbFormat::Ru2947: return "2947ru";
        case DbFormat::Ww2947: return "2947ww";
        case DbFormat::F2945: return "2945";
        case DbFormat::F2215: return "2215";
        case DbFormat::F11xx: return "11xx";
        case DbFormat::Unknown: break;
    }
    return "unknown";
}

const char* db_format_name(DbFormat f) {
    switch (f) {
        case DbFormat::Xdb: return "XDB (CS/CoP)";
        case DbFormat::Ru2947: return "2947RU (SoC RUS)";
        case DbFormat::Ww2947: return "2947WW (SoC WW)";
        case DbFormat::F2945: return "2945 (Builds 2571-2945)";
        case DbFormat::F2215: return "2215 (Builds 1482-2232)";
        case DbFormat::F11xx: return "11xx (Builds 1096-1472)";
        case DbFormat::Unknown: break;
    }
    return "unknown";
}

std::optional<DbFormat> db_format_from_any(const std::string& name_or_key) {
    const DbFormat all[6] = {DbFormat::Xdb,   DbFormat::Ru2947, DbFormat::Ww2947,
                             DbFormat::F2945, DbFormat::F2215,  DbFormat::F11xx};
    for (DbFormat f : all) {
        if (name_or_key == db_format_key(f) || name_or_key == db_format_name(f)) return f;
    }
    return std::nullopt;
}

DbList unpack_db(const std::vector<uint8_t>& raw, DbFormat fmt) {
    DbList res;
    const auto t0 = std::chrono::steady_clock::now();
    res.format = fmt;
    if (fmt == DbFormat::Unknown) {
        res.error = "未知 DB 格式";
        return res;
    }
    Chunks chunks;
    if (!read_chunks(raw, fmt, chunks)) {
        res.error = "找不到可解析的 HEADER 块";
        return res;
    }
    std::vector<uint8_t> data = chunks.header;
    const Scrambler* sc = scrambler_for(fmt);
    if (chunks.comp) {
        if (sc) data = sc->decrypt(data);
        std::vector<uint8_t> plain;
        std::string err;
        if (!lzhuf_decode(data.data(), data.size(), plain, err)) {
            res.error = "HEADER 解压失败：" + err;
            return res;
        }
        data = std::move(plain);
    } else if (sc) {
        data = sc->decrypt(data);
    }
    res.entries = parse_format(data, fmt);
    const auto t1 = std::chrono::steady_clock::now();
    res.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    res.ok = true;
    return res;
}

std::optional<DbFormat> auto_detect(const std::vector<uint8_t>& raw) {
    const DbFormat all[6] = {DbFormat::Xdb,   DbFormat::Ru2947, DbFormat::Ww2947,
                             DbFormat::F2945, DbFormat::F2215,  DbFormat::F11xx};
    std::vector<DbFormat> hits;
    for (DbFormat f : all) {
        const DbList r = unpack_db(raw, f);
        if (r.ok && entries_self_consistent(raw, r.entries)) hits.push_back(f);
    }
    if (hits.size() == 1) return hits[0];
    return std::nullopt;
}

namespace {

// 解码一段"已经是这一条的原始字节"的缓冲（`p` 指向文件里 `entry.offset` 处，`n` 是可读长度）。
// 内存版（整包在内存）与 seek 版（只读了那一段）**共用这一份** —— 两条路的产物必须逐值一致。
DbExtract decode_entry_bytes(const uint8_t* p, size_t n, const DbEntry& entry) {
    DbExtract res;
    if (entry.use_lzhuf) {
        if (entry.size_comp > n) {
            res.error = "偏移越界";
            return res;
        }
        std::string err;
        if (!lzhuf_decode(p, static_cast<size_t>(entry.size_comp), res.data, err)) {
            res.error = "LZHUF 解压失败：" + err;
            return res;
        }
        res.ok = true;
        return res;
    }

    const uint64_t sc = entry.size_comp ? entry.size_comp : entry.size_real;
    if (sc > n) {
        res.error = "偏移越界";
        return res;
    }
    if (sc == entry.size_real) {
        res.data.assign(p, p + sc);
        res.ok = true;
        return res;
    }

    // ★ 声明尺寸是不可信的归档头字段：两道炸弹闸都过才解压
    if (entry.size_real > kMaxDecompressBytes ||
        (sc > 0 && entry.size_real > sc * kMaxLzoRatio)) {
        res.error = "超过解压上限或压缩比上限（拒绝分配内存）";
        return res;
    }
    static std::once_flag lzo_once;
    static bool lzo_ok = false;
    std::call_once(lzo_once, [] { lzo_ok = (lzo_init() == LZO_E_OK); });
    if (!lzo_ok) {
        res.error = "lzo_init 失败";
        return res;
    }
    res.data.resize(static_cast<size_t>(entry.size_real));
    lzo_uint out_len = static_cast<lzo_uint>(res.data.size());
    const int rc = lzo1x_decompress_safe(p, static_cast<lzo_uint>(sc), res.data.data(), &out_len,
                                         nullptr);
    if (rc != LZO_E_OK) {
        res.error = "lzo 解压失败（code=" + std::to_string(rc) + "）";
        res.data.clear();
        return res;
    }
    res.data.resize(out_len);
    res.ok = true;
    return res;
}

// 这一条需要从文件里取的字节数（= 解码真正会读到的那一段）
uint64_t entry_read_bytes(const DbEntry& entry) {
    if (entry.use_lzhuf) return entry.size_comp;
    return entry.size_comp ? entry.size_comp : entry.size_real;
}

}  // namespace

DbExtract extract_entry(const std::vector<uint8_t>& raw, const DbEntry& entry) {
    DbExtract res;
    if (entry.is_dir) {
        res.error = "该条目是目录";
        return res;
    }
    const uint64_t sc = entry_read_bytes(entry);
    if (entry.offset > raw.size() || sc > raw.size() - entry.offset) {
        res.error = "偏移越界";
        return res;
    }
    return decode_entry_bytes(raw.data() + entry.offset, static_cast<size_t>(sc), entry);
}

DbListing list_db_seek(const std::string& path, std::optional<DbFormat> db_fallback,
                       const xrkit::plugin::Registry* registry) {
    DbListing res;
    const auto t0 = std::chrono::steady_clock::now();
    const auto stamp = [&res, t0]() {
        res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                   t0)
                             .count();
    };

    std::error_code ec;
    // ★ 2026-10-04 解密闸门（⑫12.1 消费端）：`--dump`/`--extract` 走的就是这个入口，
    //   CLI 与 GUI **共用这一枚**闸门（判据见 registry.h）。没有活动注册表 / 没有解密器
    //   命中 ⇒ `source == path`，下面逐字节与今天一致。
    const xrkit::plugin::DecryptGateResult gate = xrkit::plugin::gate_active_read(path, registry);
    if (!gate.ok) {
        res.error = gate.error;
        stamp();
        return res;
    }
    const std::string& source = gate.used_plugin ? gate.path : path;
    const auto bytes = std::filesystem::file_size(xrkit::core::path_from_utf8(source), ec);
    if (ec) {
        res.error = "读不出文件：" + source;
        stamp();
        return res;
    }
    res.file_size = static_cast<uint64_t>(bytes);
    if (res.file_size == 0) {
        res.error = "文件是空的：" + source;
        stamp();
        return res;
    }

    std::ifstream in(xrkit::core::path_from_utf8(source), std::ios::binary);
    if (!in) {
        res.error = "读不出文件：" + source;
        stamp();
        return res;
    }

    std::vector<uint8_t> header;
    bool comp = false;
    if (seek_header_chunk(in, res.file_size, header, comp)) {
        // 与内存版 `auto_detect` **同一条判据**（唯一自洽才认），只是字节只来自尾部那一块。
        // 注意用 `validate_header(..., fmt, ...)`（fmt 已知 ⇒ 非严格档）：这正是 `unpack_db`
        // 内部走的那条路，连"至少一条非目录条目可用"的轻校验都照旧。
        const DbFormat all[6] = {DbFormat::Xdb,    DbFormat::Ru2947, DbFormat::Ww2947,
                                 DbFormat::F2945,  DbFormat::F2215,  DbFormat::F11xx};
        int hits = 0;
        DbFormat picked = DbFormat::Unknown;
        std::vector<DbEntry> picked_entries;
        for (DbFormat f : all) {
            std::vector<DbEntry> entries;
            if (!validate_header(header, comp, f, entries)) continue;
            if (!entries_self_consistent_size(res.file_size, entries)) continue;
            ++hits;
            picked = f;
            picked_entries = std::move(entries);
        }
        if (hits == 1) {
            res.used_seek = true;
            res.header_bytes = header.size();
            res.format = picked;
            res.entries = std::move(picked_entries);
            res.ok = true;
            stamp();
            return res;
        }
        // 判不出来 / 有歧义：**不在这里下结论** —— 旧路径还会沿着链继续扫（逐字节回退），
        // 可能找到另一个 HEADER 块，也可能靠 `db_fallback` 裁决。退回整文件读，行为逐值一致。
        res.ambiguous = hits > 1;
    }

    // ── 退回整文件读（= 旧路径，含逐字节回退扫描；错误文案也与旧版一致）──────────
    // ★★ P17-111（复审计 `01 F-09`）：**先量尺寸，再决定读不读**。
    //   修前这里直接 `std::vector<uint8_t> raw((istreambuf_iterator<char>(in)), …)` ——
    //   **无上限**，而这一步发生在"还认不出是不是 DB"之前 ⇒ 随手指一个几 GB 的文件
    //   就会被整个读进内存（白读 ＋ 可能撑爆进程）。
    //   口径：超过 `XRKIT_DB_MAX_WHOLE_READ_BYTES` ⇒ **明确拒绝并写清三件事**
    //   （多大 / 上限多少 / 怎么改），**不许**先啃进内存再说（零静默）。
    //   ★ 环境变量 `XRKIT_DB_WHOLE_READ_LIMIT` 可覆盖（判据用它造小夹具，不必真造 GB 文件）。
    in.clear();
    in.seekg(0, std::ios::end);
    const std::streamoff end_pos = in.tellg();
    const uint64_t file_bytes = end_pos > 0 ? static_cast<uint64_t>(end_pos) : 0;
    res.file_size = file_bytes;   // 既有字段（不加同义字段）
    in.seekg(0, std::ios::beg);
    uint64_t limit = XRKIT_DB_MAX_WHOLE_READ_BYTES;
    if (const char* env = std::getenv("XRKIT_DB_WHOLE_READ_LIMIT")) {
        // 空/非法 ⇒ 保持默认（不因为环境变量写坏就把闸门放开）
        char* endp = nullptr;
        const unsigned long long parsed = std::strtoull(env, &endp, 10);
        if (endp != env && parsed > 0) limit = static_cast<uint64_t>(parsed);
    }
    if (file_bytes > limit) {
        res.whole_read_refused = true;
        res.whole_file_reason = "文件 " + std::to_string(file_bytes) +
                                " 字节超过整包读上限 " + std::to_string(limit) +
                                " 字节 ⇒ 拒绝整包读（这是「认不出格式时的兜底路径」；"
                                "请在下拉里选定具体 DB 格式，或换用本工具能 seek 的包）";
        res.error = res.whole_file_reason;
        stamp();
        return res;
    }
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (raw.empty()) {
        res.error = "文件是空的：" + source;
        stamp();
        return res;
    }
    res.header_bytes = raw.size();
    // ★★ 2026-10-10（P17-70，审计 01 F-09 C 步的引擎半）：**把"为什么走了整包读"写下来**。
    //   这一段就是 seek 不可用时的退回点（`:774` 之前已经把整个文件读进 `raw`）——
    //   修前只记了 `header_bytes`（"读了多少"），**没记原因**（"为什么不得不整包读"），
    //   于是界面/CLI 即便把字节数露出来，用户也不知道是"块头坏了""不是 DB"还是"格式认不出"。
    //   零静默：理由必须跟着字节数一起可读出去（消费点见 P17-69 先查补充）。
    res.whole_file_reason = "seek 式读取不可用，已退回整包读（读入 " +
                            std::to_string(raw.size()) + " 字节）";
    std::optional<DbFormat> fmt = auto_detect(raw);
    if (!fmt.has_value() && db_fallback.has_value()) fmt = db_fallback;
    if (!fmt.has_value()) {
        res.error = "认不出这个包的格式（DB 自洽判定有歧义；在下拉里选定具体格式后重试，"
                    "或它就是加密包/需要解密插件）";
        stamp();
        return res;
    }
    const DbList listed = unpack_db(raw, *fmt);
    if (!listed.ok) {
        res.error = std::string(db_format_key(*fmt)) + " 解析失败：" + listed.error;
        stamp();
        return res;
    }
    if (listed.entries.empty()) {
        // 自动判定失败、退回下拉格式时**必须**有这一条：不然"拿错格式硬解"会静默变成一个空包
        res.error = std::string("在 ") + db_format_key(*fmt) + " 下一个条目都解不出来（格式不对？）";
        stamp();
        return res;
    }
    res.format = *fmt;
    res.entries = listed.entries;
    res.ok = true;
    stamp();
    return res;
}

DbExtract extract_entry_seek(const std::string& path, const DbEntry& entry) {
    DbExtract res;
    if (entry.is_dir) {
        res.error = "该条目是目录";
        return res;
    }
    const uint64_t sc = entry_read_bytes(entry);
    if (sc == 0) {
        res.ok = true;   // 合法空文件（与内存版的 `sc == size_real == 0` 分支同结果）
        return res;
    }
    std::ifstream in(xrkit::core::path_from_utf8(path), std::ios::binary);
    if (!in) {
        res.error = "读不出文件：" + path;
        return res;
    }
    in.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(sc));
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sc));
    if (in.gcount() != static_cast<std::streamsize>(sc)) {
        res.error = "偏移越界";
        return res;
    }
    return decode_entry_bytes(buf.data(), buf.size(), entry);
}

namespace {

// ★ 2026-10-06：接住 `utf8_to_cp1251` 的返回值（它的语义已经是"这次编码**有没有发生顶替**"），
//   发生顶替 ⇒ 把「源名 → 实际写进包里的名字」记进台账（`log` 可空）。
//   为什么记"写进去的那个名字"而不是只记源名：用户要能对得上产物（`?` 顶替后长什么样）。
void note_rewrite(const std::string& name, const std::vector<uint8_t>& nb, bool clean,
                  std::vector<DbNameRewrite>* log) {
    if (clean || log == nullptr) return;
    log->push_back({name, cp1251_to_utf8(nb.data(), nb.size())});
}

void append_entry_xdb(std::vector<uint8_t>& h, const std::string& name, uint32_t sr, uint32_t sc,
                      uint32_t crc, uint32_t off, std::vector<DbNameRewrite>* log) {
    std::vector<uint8_t> nb;
    const bool clean = utf8_to_cp1251(name, nb);
    note_rewrite(name, nb, clean, log);
    put16(h, static_cast<uint16_t>(nb.size() + 16));
    put32(h, sr);
    put32(h, sc);
    put32(h, crc);
    h.insert(h.end(), nb.begin(), nb.end());
    put32(h, off);
}

void append_entry_null(std::vector<uint8_t>& h, const std::string& name,
                       const std::vector<uint32_t>& fields, std::vector<DbNameRewrite>* log) {
    std::vector<uint8_t> nb;
    const bool clean = utf8_to_cp1251(name, nb);
    note_rewrite(name, nb, clean, log);
    h.insert(h.end(), nb.begin(), nb.end());
    h.push_back(0);
    for (uint32_t v : fields) put32(h, v);
}

std::vector<uint8_t> build_header(const std::vector<DbPackItem>& items, DbFormat fmt,
                                 std::vector<DbNameRewrite>* log, std::string* error) {
    std::vector<uint8_t> h;
    uint32_t off = 8;  // file-absolute offset（在 DATA chunk 头之后）
    for (const DbPackItem& it : items) {
        // ★★ 2026-10-10（P17-65，审计 01 F-13）：**两条 4 GiB 窄化检查（opt-in）**。
        //   ① 单条目：`it.data.size()` 是 `size_t`，下面那句 `static_cast<uint32_t>` 在单条目
        //      ≥ 4 GiB 时**静默回绕**；
        //   ② 累计偏移：`off` 是 `uint32_t` 且只增不减，文件总长越过 4 GiB 时**静默回绕**。
        //   命中任一条都会产出"能打开、但条目大小/偏移全错"的 DB（比崩掉更难查）。
        //   ★★ 2026-10-10（P17-68，01 F-13 收官）：**检查已改成无条件** ——
        //   四处调用点（CLI / 官方 fs 页 / 门面 / py_host）都接上了出错理由（P17-65/66/67），
        //   所以不再需要"给了 error 才检测"的 opt-in 保护；现在**任何**调用方都不可能拿到
        //   一个静默窄化的包头（拿不到内容就不会落盘）。`error` 出参只用于**报理由**。
        {
            if (!it.is_dir && it.data.size() > 0xFFFFFFFFull) {
                if (error != nullptr) {
                    *error = "单条目超过 4 GiB（DB 头字段是 uint32）：" + it.path + " = " +
                             std::to_string(it.data.size()) + " 字节";
                }
                return {};
            }
            const uint64_t end = static_cast<uint64_t>(off) +
                                 static_cast<uint64_t>(it.is_dir ? 0 : it.data.size());
            if (end > 0xFFFFFFFFull) {
                if (error != nullptr) {
                    *error = "DB 总长越过 4 GiB（文件内偏移是 uint32）：处理到 " + it.path +
                             " 时累计 " + std::to_string(end) + " 字节";
                }
                return {};
            }
        }
        const uint32_t sz = static_cast<uint32_t>(it.data.size());
        if (it.is_dir) {
            if (fmt == DbFormat::Xdb || fmt == DbFormat::Ru2947 || fmt == DbFormat::Ww2947) {
                append_entry_xdb(h, it.path + "\\", 0, 0, 0, 0, log);
            } else if (fmt == DbFormat::F2945) {
                append_entry_null(h, it.path + "\\", {0, 0, 0, 0}, log);
            } else if (fmt == DbFormat::F2215) {
                append_entry_null(h, it.path + "\\", {0, 0, 0}, log);
            } else {
                // ★ 2026-10-05：11xx 的目录条目**三个字段必须全零**。
                //   曾经写 `{1, 0, 0}`（第 1 字段＝`uncompressed`/标志位）——那是**非法**写法：
                //   官方 `converter.exe -unpack -11xx` 读到这种目录条目会**栈溢出**
                //   （exit 3221225725 = 0xC00000FD，产物 0 文件，实测复现）。
                //   cv 变体实测（同一棵含目录树，只改目录条目表示）：
                //     不写目录条目 / `dir\`+(0,0,0) / `dir`+(1,0,0) ⇒ exit 0 且逐字节 MATCH；
                //     而 `dir\`+(1,0,0)、`dir\`+(1,0,1)、以及把它排在文件之前 ⇒ 一律栈溢出。
                //   取 `{0,0,0}` 而不是"干脆不写"：**保留目录条目 ⇒ 不丢空目录信息**。
                //   读侧不用改：`fmt_11xx` 用 `is_dir = (offset == 0)`，两种写法都判为目录。
                append_entry_null(h, it.path + "\\", {0, 0, 0}, log);
            }
        } else {
            if (fmt == DbFormat::Xdb || fmt == DbFormat::Ru2947 || fmt == DbFormat::Ww2947) {
                append_entry_xdb(h, it.path, sz, sz, 0, off, log);
            } else if (fmt == DbFormat::F2945) {
                append_entry_null(h, it.path, {0, off, sz, sz}, log);
            } else if (fmt == DbFormat::F2215) {
                append_entry_null(h, it.path, {off, sz, sz}, log);
            } else {
                append_entry_null(h, it.path, {1, off, sz}, log);
            }
            off += sz;
        }
    }
    return h;
}

}  // namespace

std::vector<uint8_t> pack_db(const std::vector<DbPackItem>& in_items, DbFormat fmt,
                             std::vector<DbNameRewrite>* rewrites, std::string* error) {
    if (error != nullptr) error->clear();
    std::vector<DbPackItem> items;
    items.reserve(in_items.size());
    for (const DbPackItem& it : in_items) {
        DbPackItem copy = it;
        for (char& c : copy.path) {
            if (c == '/') c = '\\';
        }
        items.push_back(std::move(copy));
    }

    if (rewrites != nullptr) rewrites->clear();
    std::vector<uint8_t> header = build_header(items, fmt, rewrites, error);
    // ★★ 2026-10-10（P17-65，审计 01 F-13）：**检测是 opt-in 的** —— 调用方给了 `error`
    //   出参时才会被置位；这时我们**不再往下走**（不压缩、不拼数据），直接返回空。
    //   未给 `error` 的调用方（今天还有三处）**行为逐字节不变**，留待各自单独接线。
    if (error != nullptr && !error->empty()) return {};
    std::vector<uint8_t> compressed = lzhuf_encode(header.data(), header.size());
    if (const Scrambler* sc = scrambler_for(fmt)) compressed = sc->encrypt(compressed);

    std::vector<uint8_t> data;
    for (const DbPackItem& it : items) {
        if (!it.is_dir) data.insert(data.end(), it.data.begin(), it.data.end());
    }

    std::vector<uint8_t> db;
    put32(db, 0);
    put32(db, static_cast<uint32_t>(data.size()));
    db.insert(db.end(), data.begin(), data.end());
    put32(db, 0x80000001u);
    put32(db, static_cast<uint32_t>(compressed.size()));
    db.insert(db.end(), compressed.begin(), compressed.end());
    return db;
}

}  // namespace xrkit::engine::fs
