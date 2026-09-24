#include "xrkit/engine/third_party_probe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// 第三方头（全部 C）：注意各自要求的宏必须在 include 之前定义。
#include <zlib.h>
#include <lz4.h>
#include <zstd.h>
#include <lzma.h>
#include "minilzo.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace xrkit::engine {
namespace {

std::vector<unsigned char> make_sample(size_t n = 4096) {
    // 有冗余但不全等 —— 压缩器才有东西可压，解压后逐字节比对才有意义
    std::vector<unsigned char> data(n);
    for (size_t i = 0; i < n; ++i) {
        data[i] = static_cast<unsigned char>((i / 7) % 251);
    }
    return data;
}

ProbeResult fail(const std::string& what) {
    ProbeResult r;
    r.ok = false;
    r.detail = what;
    return r;
}

ProbeResult pass(const std::string& what) {
    ProbeResult r;
    r.ok = true;
    r.detail = what;
    return r;
}

}  // namespace

ProbeResult probe_zlib() {
    const std::vector<unsigned char> src = make_sample();
    uLongf bound = compressBound(static_cast<uLong>(src.size()));
    std::vector<unsigned char> comp(bound);
    if (compress(comp.data(), &bound, src.data(), static_cast<uLong>(src.size())) != Z_OK) {
        return fail("zlib compress 失败");
    }
    std::vector<unsigned char> back(src.size());
    uLongf back_len = static_cast<uLongf>(back.size());
    if (uncompress(back.data(), &back_len, comp.data(), bound) != Z_OK) {
        return fail("zlib uncompress 失败");
    }
    if (back_len != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
        return fail("zlib 往返内容不一致");
    }
    return pass(std::string("zlib ") + zlibVersion() + " roundtrip 4096B ok");
}

ProbeResult probe_lz4() {
    const std::vector<unsigned char> src = make_sample();
    const int n = static_cast<int>(src.size());
    std::vector<char> comp(static_cast<size_t>(LZ4_compressBound(n)));
    const int csize = LZ4_compress_default(reinterpret_cast<const char*>(src.data()),
                                           comp.data(), n, static_cast<int>(comp.size()));
    if (csize <= 0) return fail("LZ4_compress_default 失败");
    std::vector<char> back(src.size());
    const int dsize = LZ4_decompress_safe(comp.data(), back.data(), csize, n);
    if (dsize != n || std::memcmp(back.data(), src.data(), src.size()) != 0) {
        return fail("lz4 往返内容不一致");
    }
    return pass(std::string("lz4 ") + LZ4_versionString() + " roundtrip 4096B ok");
}

ProbeResult probe_zstd() {
    const std::vector<unsigned char> src = make_sample();
    std::vector<unsigned char> comp(ZSTD_compressBound(src.size()));
    const size_t csize = ZSTD_compress(comp.data(), comp.size(), src.data(), src.size(), 3);
    if (ZSTD_isError(csize)) return fail(std::string("ZSTD_compress 失败: ") + ZSTD_getErrorName(csize));
    std::vector<unsigned char> back(src.size());
    const size_t dsize = ZSTD_decompress(back.data(), back.size(), comp.data(), csize);
    if (ZSTD_isError(dsize)) return fail(std::string("ZSTD_decompress 失败: ") + ZSTD_getErrorName(dsize));
    if (dsize != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
        return fail("zstd 往返内容不一致");
    }
    return pass(std::string("zstd ") + ZSTD_versionString() + " roundtrip 4096B ok");
}

ProbeResult probe_xz() {
    const std::vector<unsigned char> src = make_sample();
    std::vector<unsigned char> comp(lzma_stream_buffer_bound(src.size()));
    size_t comp_pos = 0;
    lzma_ret rc = lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr, src.data(), src.size(),
                                         comp.data(), &comp_pos, comp.size());
    if (rc != LZMA_OK) return fail("lzma_easy_buffer_encode 失败");

    std::vector<unsigned char> back(src.size());
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0;
    size_t back_pos = 0;
    rc = lzma_stream_buffer_decode(&memlimit, 0, nullptr, comp.data(), &in_pos, comp_pos,
                                   back.data(), &back_pos, back.size());
    if (rc != LZMA_OK) return fail("lzma_stream_buffer_decode 失败");
    if (back_pos != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
        return fail("xz 往返内容不一致");
    }
    return pass(std::string("xz/liblzma ") + lzma_version_string() + " roundtrip 4096B ok");
}

ProbeResult probe_lzo() {
    if (lzo_init() != LZO_E_OK) return fail("lzo_init 失败");
    const std::vector<unsigned char> src = make_sample();
    std::vector<unsigned char> comp(src.size() + src.size() / 16 + 64 + 3);
    // miniLZO 要求工作内存按 lzo_align_t 对齐；这里多要 16 字节再手动对齐。
    std::vector<unsigned char> work_raw(LZO1X_1_MEM_COMPRESS + 16);
    void* work = work_raw.data();
    const uintptr_t aligned = (reinterpret_cast<uintptr_t>(work) + 15u) & ~static_cast<uintptr_t>(15u);
    work = reinterpret_cast<void*>(aligned);

    lzo_uint comp_len = static_cast<lzo_uint>(comp.size());
    if (lzo1x_1_compress(src.data(), static_cast<lzo_uint>(src.size()), comp.data(), &comp_len, work) != LZO_E_OK) {
        return fail("lzo1x_1_compress 失败");
    }
    std::vector<unsigned char> back(src.size());
    lzo_uint back_len = static_cast<lzo_uint>(back.size());
    if (lzo1x_decompress_safe(comp.data(), comp_len, back.data(), &back_len, nullptr) != LZO_E_OK) {
        return fail("lzo1x_decompress_safe 失败");
    }
    if (back_len != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
        return fail("lzo 往返内容不一致");
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(LZO_VERSION));
    return pass(std::string("lzo/miniLZO ver=") + buf + " roundtrip 4096B ok");
}

ProbeResult probe_pcre2() {
    // 用 **lookbehind** 作探针：这正是选 PCRE2 而不是 std::regex 的原因（C2 口径）。
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>("(?<=foo)bar"), PCRE2_ZERO_TERMINATED,
                                   0, &errcode, &erroffset, nullptr);
    if (re == nullptr) {
        return fail("PCRE2 编译 lookbehind 模式失败（错码 " + std::to_string(errcode) + "）");
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, nullptr);
    const char* subject = "xxfoobarxx";
    const int rc = pcre2_match(re, reinterpret_cast<PCRE2_SPTR>(subject), std::strlen(subject), 0, 0,
                               md, nullptr);
    // lookbehind 的语义检查：必须命中，且命中位置就是 "bar" 在 subject 里的真实偏移
    // （别手算 —— 第一版我写成 3，实际是 5，被这个探针当场抓住）
    const PCRE2_SIZE expected =
        static_cast<PCRE2_SIZE>(std::strstr(subject, "bar") - subject);
    const bool hit_at_expected =
        (rc >= 1) && (pcre2_get_ovector_pointer(md)[0] == expected);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    if (!hit_at_expected) {
        return fail("PCRE2 lookbehind 匹配位置不符（rc=" + std::to_string(rc) + "，期望偏移 " +
                    std::to_string(expected) + "）");
    }
    // PCRE2 没有 pcre2_version()（那是 PCRE1 的 API），版本要用 pcre2_config 取
    char ver[64] = {};
    pcre2_config(PCRE2_CONFIG_VERSION, ver);
    return pass(std::string("PCRE2 ") + ver + " lookbehind 匹配 ok");
}

ProbeResult probe_freetype() {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return fail("FT_Init_FreeType 失败");

    const char* candidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    FT_Face face = nullptr;
    const char* used = nullptr;
    for (const char* path : candidates) {
        if (FT_New_Face(lib, path, 0, &face) == 0) {
            used = path;
            break;
        }
    }
    if (face == nullptr) {
        FT_Done_FreeType(lib);
        ProbeResult r;
        r.ok = false;
        r.skipped = true;
        r.detail = "系统字体缺失（msyh/simsun/arial 都打不开），未验证字形栅格";
        return r;
    }

    const FT_Error err = FT_Set_Pixel_Sizes(face, 0, 20);
    const FT_ULong ch = 0x6C38;  // 永
    if (err != 0 || FT_Load_Char(face, ch, FT_LOAD_RENDER) != 0) {
        FT_Done_Face(face);
        FT_Done_FreeType(lib);
        return fail("FT_Load_Char 渲染失败");
    }
    const int w = static_cast<int>(face->glyph->bitmap.width);
    const int h = static_cast<int>(face->glyph->bitmap.rows);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    if (w <= 0 || h <= 0) return fail("字形位图为空");

    char buf[256];
    std::snprintf(buf, sizeof(buf), "FreeType %d.%d.%d raster U+6C38 %dx%d px via %s",
                  FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH, w, h, used);
    return pass(buf);
}

}  // namespace xrkit::engine
