// LZHUF 参考实现（取自 docs/reference/lzhuf_dll.c；**不是**逐字复制，见下）。
//
// ★★ 2026-10-16（P17-138，复审计 09 分册 ⑤）：这行原来写「**仅**把 EXPORT 置空」——
//   **那是假的**。实测（去注释、逐行比对）与参照源有**三处**分叉，共删 8 行 / 增 12 行：
//     ①（原设计）`#ifdef _WIN32 / __declspec(dllexport)` ⇒ `#define EXPORT`（本仓库不导出符号）；
//     ②（P17-59，01 F-19）`*(unsigned*)src` / `*(unsigned*)dest` 这类**非对齐 + 端序**读写
//        ⇒ `lzhuf_read_u32_le()` ＋ 四个显式字节存储；
//     ③（P17-115，N-01）`textsize > 256*1024*1024` 裸字面量 ⇒ 共享宏
//        `XRKIT_LZHUF_MAX_DECLARED_BYTES`（含 `#include "xrkit/engine/fs/untrusted_limits.h"`）。
//   ⇒ 这三处**逐行冻结**在 `tests/check_lzhuf_ref_delta.py` 的白名单里：谁再悄悄多改一处，该判据红。
//   ★ 口径：判据比的是**代码**（去注释、压空白），因为本仓的历史就是「注释说 A、代码是 B」。
//
// 为什么要直接编译它而不是自己移植：
//   1.x 的 Python 移植版在**解码路径**上与 DLL 有差异（它的 _LZ_DLEN 只有 236 项且
//   分段与 DLL 不同，是坏的）。我们自己移植的第一版编码逐字节正确、解码却在长输入上
//   出现无规律分歧（n=16000 差@14689、n=16500 一致、n=20000 差@3199 —— 典型的 UB 特征），
//   继续排查的代价高于收益。
//   这份 C 的头部写着 "exact match with xr_lzhuf.cxx"（引擎原版），1.x 的 DLL 就是它编出来的，
//   因此**直接编译它 = 与 1.x 逐字节一致**，这是构造性保证而不是"我调对了"。
//
// 代价（如实记录）：它用 file-scope static 状态，**不可重入** —— 包装层用互斥量串行化，
// 与 1.x 的做法一致；彻底可重入需要把 static 收进上下文结构体（1.x 也留作后续）。
/*
 * lzhuf_dll.c - LZHUF as a DLL, exact match with xr_lzhuf.cxx
 * cl /std:c11 /LD /Fe:lzhuf_dll.dll lzhuf_dll.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ★★ 2026-10-13（P17-115，复审计 `N-01` P0）：**"声明解压尺寸"上限改走共享宏，不再裸写**。
//   事实（修前）：这里是 `256*1024*1024` 裸字面量，而 `untrusted_limits.h` 里那句
//   "`lzhuf_ref.c` 那处硬编码**先查后确认已不存在**"是**假的** —— 本文件被 git 跟踪、
//   该字面量一直在（复审计用 `git ls-files` 证伪；我上一轮轻信了旧注释没去核）。
//   ⇒ 现在真正收拢到一处：改共享宏，两处一起变。
//   ★ 本文件是 **C**（不是 C++）⇒ 用 `#define` 那个宏（头里唯一的 C++ 构件是 `static_assert`，
//     已被 `#if defined(__cplusplus)` 圈住，C 编译不受影响）。
#include "xrkit/engine/fs/untrusted_limits.h"

// 本仓库内不导出符号：EXPORT 置空（唯一对参照源做的改动）
#define EXPORT

#define N 4096
#define F 60
#define THRESHOLD 2
#define N_CHAR (256 - THRESHOLD + F)
#define T (N_CHAR * 2 - 1)
#define R (T - 1)
#define MAX_FREQ 0x4000

static const unsigned char d_code[256] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,
    0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x09,0x09,0x09,0x09,0x09,0x09,0x09,0x09,
    0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,
    0x0C,0x0C,0x0C,0x0C,0x0D,0x0D,0x0D,0x0D,0x0E,0x0E,0x0E,0x0E,0x0F,0x0F,0x0F,0x0F,
    0x10,0x10,0x10,0x10,0x11,0x11,0x11,0x11,0x12,0x12,0x12,0x12,0x13,0x13,0x13,0x13,
    0x14,0x14,0x14,0x14,0x15,0x15,0x15,0x15,0x16,0x16,0x16,0x16,0x17,0x17,0x17,0x17,
    0x18,0x18,0x19,0x19,0x1A,0x1A,0x1B,0x1B,0x1C,0x1C,0x1D,0x1D,0x1E,0x1E,0x1F,0x1F,
    0x20,0x20,0x21,0x21,0x22,0x22,0x23,0x23,0x24,0x24,0x25,0x25,0x26,0x26,0x27,0x27,
    0x28,0x28,0x29,0x29,0x2A,0x2A,0x2B,0x2B,0x2C,0x2C,0x2D,0x2D,0x2E,0x2E,0x2F,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
};

static const unsigned char d_len[256] = {
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,
    0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,
};

static const unsigned char p_len[64] = {
    0x03,0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,0x06,0x06,0x06,
    0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
    0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08
};

static const unsigned char p_code[64] = {
    0x00,0x20,0x30,0x40,0x50,0x58,0x60,0x68,0x70,0x78,0x80,0x88,0x90,0x94,0x98,0x9C,
    0xA0,0xA4,0xA8,0xAC,0xB0,0xB4,0xB8,0xBC,0xC0,0xC2,0xC4,0xC6,0xC8,0xCA,0xCC,0xCE,
    0xD0,0xD2,0xD4,0xD6,0xD8,0xDA,0xDC,0xDE,0xE0,0xE2,0xE4,0xE6,0xE8,0xEA,0xEC,0xEE,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

/* State */
static unsigned freq[T + 1];
static int prnt[T + N_CHAR];
static int son[T];
static unsigned char text_buf[N + F - 1];
static int lson[N + 1], rson[N + 257], dad[N + 1];
static unsigned getbuf, putbuf;
static unsigned char getlen, putlen;

static const unsigned char *src_data;
static int src_pos, src_limit;
static unsigned char *dest_data;
static int dest_pos, dest_limit;

static int getc_src(void) {
    if (src_pos < src_limit) return src_data[src_pos++];
    return -1;
}

static void putc_dest(int c) {
    if (dest_pos >= dest_limit) return;
    dest_data[dest_pos++] = (unsigned char)c;
}

static int GetBit(void) {
    int i;
    while (getlen <= 8) {
        if ((i = getc_src()) < 0) i = 0;
        getbuf |= i << (8 - getlen);
        getlen += 8;
    }
    i = (int)getbuf;
    getbuf <<= 1;
    getlen--;
    return (i >> 15) & 1;
}

static int GetByte(void) {
    unsigned i;
    while (getlen <= 8) {
        int c = getc_src();
        i = (c < 0) ? 0 : c;
        getbuf |= i << (8 - getlen);
        getlen += 8;
    }
    i = getbuf;
    getbuf <<= 8;
    getlen -= 8;
    return (int)((i & 0xff00u) >> 8);
}

static void Putcode(int l, unsigned c) {
    putbuf |= c >> putlen;
    if ((putlen += l) >= 8) {
        putc_dest(putbuf >> 8);
        if ((putlen -= 8) >= 8) {
            putc_dest(putbuf);
            putlen -= 8;
            putbuf = c << (l - putlen);
        } else {
            putbuf <<= 8;
        }
    }
}

static void StartHuff(void) {
    int i, j;
    for (i = 0; i < N_CHAR; i++) {
        freq[i] = 1;
        son[i] = i + T;
        prnt[i + T] = i;
    }
    i = 0; j = N_CHAR;
    while (j <= R) {
        freq[j] = freq[i] + freq[i + 1];
        son[j] = i;
        prnt[i] = prnt[i + 1] = j;
        i += 2; j++;
    }
    freq[T] = 0xffff;
    prnt[R] = 0;
}

static void reconst(void) {
    int i, j, k;
    unsigned f, l;
    j = 0;
    for (i = 0; i < T; i++) {
        if (son[i] >= T) {
            freq[j] = (freq[i] + 1) / 2;
            son[j] = son[i];
            j++;
        }
    }
    for (i = 0, j = N_CHAR; j < T; i += 2, j++) {
        k = i + 1;
        f = freq[j] = freq[i] + freq[k];
        for (k = j - 1; f < freq[k]; k--);
        k++;
        l = (unsigned)(j - k) * sizeof(freq[0]);
        memmove(&freq[k + 1], &freq[k], l);
        freq[k] = f;
        memmove(&son[k + 1], &son[k], l);
        son[k] = i;
    }
    for (i = 0; i < T; i++) {
        if ((k = son[i]) >= T) {
            prnt[k] = i;
        } else {
            prnt[k] = prnt[k + 1] = i;
        }
    }
}

static void update(int c) {
    int i, j, k, l;
    if (freq[R] == MAX_FREQ) {
        reconst();
    }
    c = prnt[c + T];
    do {
        k = ++freq[c];
        if ((unsigned)k > freq[l = c + 1]) {
            while ((unsigned)k > freq[++l]);
            l--;
            freq[c] = freq[l];
            freq[l] = k;
            i = son[c];
            prnt[i] = l;
            if (i < T) prnt[i + 1] = l;
            j = son[l];
            son[l] = i;
            prnt[j] = c;
            if (j < T) prnt[j + 1] = c;
            son[c] = j;
            c = l;
        }
    } while ((c = prnt[c]) != 0);
}

static int DecodeChar(void) {
    unsigned c;
    c = son[R];
    while (c < T) {
        c += GetBit();
        c = son[c];
    }
    c -= T;
    update((int)c);
    return (int)c;
}

static int DecodePosition(void) {
    unsigned i, j, c;
    i = GetByte();
    c = (unsigned)d_code[i] << 6;
    j = d_len[i];
    j -= 2;
    while (j--) {
        i = (i << 1) + GetBit();
    }
    return (int)(c | (i & 0x3f));
}

static void EncodeChar(unsigned c) {
    unsigned i;
    int j, k;
    i = 0; j = 0;
    k = prnt[c + T];
    do {
        i >>= 1;
        if (k & 1) i += 0x8000;
        j++;
    } while ((k = prnt[k]) != R);
    Putcode(j, i);
    update((int)c);
}

static void EncodePosition(unsigned c) {
    unsigned i;
    i = c >> 6;
    Putcode((int)p_len[i], (unsigned)p_code[i] << 8);
    Putcode(6, (c & 0x3f) << 10);
}

static void EncodeEnd(void) {
    if (putlen) {
        putc_dest(putbuf >> 8);
    }
}

static void InitTree(void) {
    int i;
    for (i = N + 1; i <= N + 256; i++) rson[i] = N;
    for (i = 0; i < N; i++) dad[i] = N;
}

static int match_position, match_length;

static void InsertNode(int r) {
    int i, p, cmp;
    unsigned char *key;
    unsigned c;
    cmp = 1;
    key = &text_buf[r];
    p = N + 1 + key[0];
    rson[r] = lson[r] = N;
    match_length = 0;
    for (;;) {
        if (cmp >= 0) {
            if (rson[p] != N) p = rson[p];
            else { rson[p] = r; dad[r] = p; return; }
        } else {
            if (lson[p] != N) p = lson[p];
            else { lson[p] = r; dad[r] = p; return; }
        }
        for (i = 1; i < F; i++)
            if ((cmp = key[i] - text_buf[p + i]) != 0) break;
        if (i > THRESHOLD) {
            if (i > match_length) {
                match_position = ((r - p) & (N - 1)) - 1;
                if ((match_length = i) >= F) break;
            }
            if (i == match_length) {
                if ((c = ((r - p) & (N - 1)) - 1) < (unsigned)match_position)
                    match_position = (int)c;
            }
        }
    }
    dad[r] = dad[p];
    lson[r] = lson[p];
    rson[r] = rson[p];
    dad[lson[p]] = r;
    dad[rson[p]] = r;
    if (rson[dad[p]] == p) rson[dad[p]] = r;
    else lson[dad[p]] = r;
    dad[p] = N;
}

static void DeleteNode(int p) {
    int q;
    if (dad[p] == N) return;
    if (rson[p] == N) q = lson[p];
    else if (lson[p] == N) q = rson[p];
    else {
        q = lson[p];
        if (rson[q] != N) {
            do { q = rson[q]; } while (rson[q] != N);
            rson[dad[q]] = lson[q];
            dad[lson[q]] = dad[q];
            lson[q] = lson[p];
            dad[lson[p]] = q;
        }
        rson[q] = rson[p];
        dad[rson[p]] = q;
    }
    dad[q] = dad[p];
    if (rson[dad[p]] == p) rson[dad[p]] = q;
    else lson[dad[p]] = q;
    dad[p] = N;
}

/* ═══ Public API ═══ */

/* ★★ 2026-10-10（P17-59，审计 01 F-19 Info）：**消除非对齐与端序读取依赖**。
 *   修前这里（与 lzhuf_decode 里那处）写的是 `*(unsigned*)src` —— `src` 指向
 *   `std::vector<uint8_t>::data()`，**不保证 4 字节对齐**；x86/x64 容忍非对齐访问，
 *   但换架构就是未定义行为，而且这条读法把**端序**也一起依赖掉了（大端机上读出来是反的）。
 *   现在按字节显式组装 **小端 32 位**（1.x 的容器就是这个端序），不碰对齐、不碰端序。
 */
static unsigned lzhuf_read_u32_le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

EXPORT int lzhuf_decode_size(const unsigned char *src, int src_len) {
    if (src_len < 4) return -1;
    return (int)lzhuf_read_u32_le(src);
}

EXPORT int lzhuf_decode(const unsigned char *src, int src_len, unsigned char *dest, int dest_cap) {
    int i, j, k, r, c;
    unsigned count;
    int textsize;

    textsize = (int)lzhuf_read_u32_le(src);
    // ★ P17-115（复审计 `N-01` P0）：上限改走**共享宏**（原来这里裸写 256*1024*1024）
    if (textsize <= 0 || textsize > dest_cap ||
        (unsigned long long)textsize > (unsigned long long)XRKIT_LZHUF_MAX_DECLARED_BYTES) {
        return -1;
    }

    src_data = src; src_pos = 4; src_limit = src_len;
    getbuf = 0; getlen = 0;

    dest_data = dest; dest_pos = 0; dest_limit = dest_cap;

    StartHuff();
    for (i = 0; i < N - F; i++) text_buf[i] = ' ';
    r = N - F;

    for (count = 0; count < (unsigned)textsize; ) {
        c = DecodeChar();
        if (c < 256) {
            putc_dest(c);
            text_buf[r++] = (unsigned char)c;
            r &= (N - 1);
            count++;
        } else {
            i = (r - DecodePosition() - 1) & (N - 1);
            j = c - 255 + THRESHOLD;
            for (k = 0; k < j; k++) {
                c = text_buf[(i + k) & (N - 1)];
                putc_dest(c);
                text_buf[r++] = (unsigned char)c;
                r &= (N - 1);
                count++;
            }
        }
    }
    return dest_pos;
}

EXPORT int lzhuf_encode(const unsigned char *src, int src_len, unsigned char *dest, int dest_cap) {
    int i, c, len, r, s, last_match_length;
    int textsize = src_len;

    src_data = src; src_pos = 0; src_limit = src_len;
    dest_data = dest; dest_pos = 4; dest_limit = dest_cap;
    /* ★★ 2026-10-10（P17-59，审计 01 F-19 的**写侧**同款）：这里原来也是 `*(unsigned*)dest = …`
     *   —— 非对齐写 + 端序依赖。改成逐字节写**小端 32 位**，与读侧 `lzhuf_read_u32_le` 对称。 */
    dest[0] = (unsigned char)(textsize & 0xFF);
    dest[1] = (unsigned char)((textsize >> 8) & 0xFF);
    dest[2] = (unsigned char)((textsize >> 16) & 0xFF);
    dest[3] = (unsigned char)((textsize >> 24) & 0xFF);

    putbuf = 0; putlen = 0;

    StartHuff(); InitTree();
    s = 0; r = N - F;
    for (i = s; i < r; i++) text_buf[i] = ' ';
    for (len = 0; len < F && (c = getc_src()) >= 0; len++)
        text_buf[r + len] = (unsigned char)c;
    textsize = len;
    for (i = 1; i <= F; i++) InsertNode(r - i);
    InsertNode(r);
    do {
        if (match_length > len) match_length = len;
        if (match_length <= THRESHOLD) {
            match_length = 1;
            EncodeChar(text_buf[r]);
        } else {
            EncodeChar(255 - THRESHOLD + match_length);
            EncodePosition((unsigned)match_position);
        }
        last_match_length = match_length;
        for (i = 0; i < last_match_length && (c = getc_src()) >= 0; i++) {
            DeleteNode(s);
            text_buf[s] = (unsigned char)c;
            if (s < F - 1) text_buf[s + N] = (unsigned char)c;
            s = (s + 1) & (N - 1); r = (r + 1) & (N - 1);
            InsertNode(r);
        }
        while (i++ < last_match_length) {
            DeleteNode(s);
            s = (s + 1) & (N - 1); r = (r + 1) & (N - 1);
            if (--len) InsertNode(r);
        }
    } while (len > 0);
    EncodeEnd();
    return dest_pos;
}
