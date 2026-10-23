// xrkit::core —— SHA-256（FIPS 180-4），无 Qt / 无第三方依赖。
//
// 为什么在 core 里自己实现（2026-10-23）：
//   `xrkit_cli --build-package` 要给**包内 exe** 记 `exe_sha256=`（Q2 的硬不变量），
//   而 CLI 与 core 都**不许链 Qt**（根 CMakeLists 的 J5 锁），所以不能用 `QCryptographicHash`。
//   只实现"算哈希"这一件事，接口按**字节**给，别处要算文件就调 `sha256_file()`。
//
// 测试口径：`tests/check_version_identity.py` 用 Python 的 `hashlib.sha256` 对同一个文件再算一遍，
//   两边的十六进制必须逐字相同（这本身就是一条跨语言对拍）。

#include "xrkit/core/sha256.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace xrkit::core {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::array<uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<uint8_t, 64> buf{};
    uint64_t total = 0;   // 已喂进来的字节数
    size_t used = 0;      // buf 里现有多少字节

    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const uint8_t* data, size_t size) {
        total += size;
        while (size > 0) {
            const size_t take = (size < (64 - used)) ? size : (64 - used);
            std::memcpy(buf.data() + used, data, take);
            used += take;
            data += take;
            size -= take;
            if (used == 64) {
                block(buf.data());
                used = 0;
            }
        }
    }

    std::string finish() {
        const uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0x00;
        while (used != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>((bits >> (56 - i * 8)) & 0xff);
        update(len, 8);
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t v : h) {
            for (int shift = 28; shift >= 0; shift -= 4) out.push_back(kHex[(v >> shift) & 0xf]);
        }
        return out;
    }
};

}  // namespace

std::string sha256_hex(const uint8_t* data, size_t size) {
    Sha256 s;
    if (data != nullptr && size > 0) s.update(data, size);
    return s.finish();
}

std::string sha256_file(const std::string& path, std::string& error) {
    error.clear();
    std::FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "rb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        error = "打不开文件：" + path;
        return {};
    }
    Sha256 s;
    std::vector<uint8_t> chunk(1u << 20);
    size_t got = 0;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) s.update(chunk.data(), got);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        error = "读文件失败：" + path;
        return {};
    }
    return s.finish();
}

}  // namespace xrkit::core
