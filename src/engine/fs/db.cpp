#include "xrkit/engine/fs/db.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

#include "minilzo.h"

#include "xrkit/engine/fs/lzhuf.h"

namespace xrkit::engine::fs {
namespace {

#include "cp1251_tables.inc"

constexpr uint32_t kScramblerMul = 0x8088405u;
constexpr uint64_t kMaxDecompressBytes = 512ull * 1024 * 1024;
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

bool utf8_to_cp1251(const std::string& s, std::vector<uint8_t>& out) {
    const auto& rev = unicode_to_cp1251();
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= s.size()) return false;
            cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= s.size()) return false;
            cp = ((c & 0x0Fu) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else {
            return false;
        }
        if (cp >= 0x10000 || rev[cp] < 0) return false;
        out.push_back(static_cast<uint8_t>(rev[cp]));
        i += len;
    }
    return true;
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
bool entries_fit_file(const std::vector<uint8_t>& raw, const std::vector<DbEntry>& entries) {
    for (const DbEntry& e : entries) {
        if (e.is_dir) continue;
        if (e.path.empty()) return false;
        for (char c : e.path) {
            if (static_cast<unsigned char>(c) < 32) return false;
        }
        const uint64_t sc = e.size_comp;
        if (e.offset == 0 || e.offset + sc > raw.size()) return false;
        if (e.size_comp != 0 && e.size_real > sc * kMaxLzoRatio) return false;
    }
    return true;
}

bool entries_self_consistent(const std::vector<uint8_t>& raw,
                             const std::vector<DbEntry>& entries) {
    if (entries.empty()) return false;
    if (!entries_fit_file(raw, entries)) return false;
    return std::any_of(entries.begin(), entries.end(), [](const DbEntry& e) { return !e.is_dir; });
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
        if (cid <= 1 && 8ull + sz <= raw.size() && sz > 0) {
            const bool comp = (rid & 0x80800000u) != 0;
            std::vector<uint8_t> data(raw.begin() + static_cast<long>(pos + 8),
                                      raw.begin() + static_cast<long>(pos + 8 + sz));
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

DbExtract extract_entry(const std::vector<uint8_t>& raw, const DbEntry& entry) {
    DbExtract res;
    if (entry.is_dir) {
        res.error = "该条目是目录";
        return res;
    }
    if (entry.use_lzhuf) {
        if (entry.offset + entry.size_comp > raw.size()) {
            res.error = "偏移越界";
            return res;
        }
        std::string err;
        if (!lzhuf_decode(raw.data() + entry.offset, static_cast<size_t>(entry.size_comp), res.data,
                          err)) {
            res.error = "LZHUF 解压失败：" + err;
            return res;
        }
        res.ok = true;
        return res;
    }

    const uint64_t sc = entry.size_comp ? entry.size_comp : entry.size_real;
    if (entry.offset + sc > raw.size()) {
        res.error = "偏移越界";
        return res;
    }
    if (sc == entry.size_real) {
        res.data.assign(raw.begin() + static_cast<long>(entry.offset),
                        raw.begin() + static_cast<long>(entry.offset + sc));
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
    const int rc = lzo1x_decompress_safe(raw.data() + entry.offset, static_cast<lzo_uint>(sc),
                                         res.data.data(), &out_len, nullptr);
    if (rc != LZO_E_OK) {
        res.error = "lzo 解压失败（code=" + std::to_string(rc) + "）";
        res.data.clear();
        return res;
    }
    res.data.resize(out_len);
    res.ok = true;
    return res;
}

namespace {

void append_entry_xdb(std::vector<uint8_t>& h, const std::string& name, uint32_t sr, uint32_t sc,
                      uint32_t crc, uint32_t off) {
    std::vector<uint8_t> nb;
    utf8_to_cp1251(name, nb);
    put16(h, static_cast<uint16_t>(nb.size() + 16));
    put32(h, sr);
    put32(h, sc);
    put32(h, crc);
    h.insert(h.end(), nb.begin(), nb.end());
    put32(h, off);
}

void append_entry_null(std::vector<uint8_t>& h, const std::string& name,
                       const std::vector<uint32_t>& fields) {
    std::vector<uint8_t> nb;
    utf8_to_cp1251(name, nb);
    h.insert(h.end(), nb.begin(), nb.end());
    h.push_back(0);
    for (uint32_t v : fields) put32(h, v);
}

std::vector<uint8_t> build_header(const std::vector<DbPackItem>& items, DbFormat fmt) {
    std::vector<uint8_t> h;
    uint32_t off = 8;  // file-absolute offset（在 DATA chunk 头之后）
    for (const DbPackItem& it : items) {
        const uint32_t sz = static_cast<uint32_t>(it.data.size());
        if (it.is_dir) {
            if (fmt == DbFormat::Xdb || fmt == DbFormat::Ru2947 || fmt == DbFormat::Ww2947) {
                append_entry_xdb(h, it.path + "\\", 0, 0, 0, 0);
            } else if (fmt == DbFormat::F2945) {
                append_entry_null(h, it.path + "\\", {0, 0, 0, 0});
            } else if (fmt == DbFormat::F2215) {
                append_entry_null(h, it.path + "\\", {0, 0, 0});
            } else {
                append_entry_null(h, it.path + "\\", {1, 0, 0});
            }
        } else {
            if (fmt == DbFormat::Xdb || fmt == DbFormat::Ru2947 || fmt == DbFormat::Ww2947) {
                append_entry_xdb(h, it.path, sz, sz, 0, off);
            } else if (fmt == DbFormat::F2945) {
                append_entry_null(h, it.path, {0, off, sz, sz});
            } else if (fmt == DbFormat::F2215) {
                append_entry_null(h, it.path, {off, sz, sz});
            } else {
                append_entry_null(h, it.path, {1, off, sz});
            }
            off += sz;
        }
    }
    return h;
}

}  // namespace

std::vector<uint8_t> pack_db(const std::vector<DbPackItem>& in_items, DbFormat fmt) {
    std::vector<DbPackItem> items;
    items.reserve(in_items.size());
    for (const DbPackItem& it : in_items) {
        DbPackItem copy = it;
        for (char& c : copy.path) {
            if (c == '/') c = '\\';
        }
        items.push_back(std::move(copy));
    }

    std::vector<uint8_t> header = build_header(items, fmt);
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
