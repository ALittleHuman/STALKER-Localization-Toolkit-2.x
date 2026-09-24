#include "xrkit/engine/fs/lzhuf.h"

#include <algorithm>
#include <mutex>

// 参考实现（C，逐字复制自 1.x 的 lzhuf_dll.c，见 lzhuf_ref.c 头部说明）
extern "C" {
int lzhuf_encode(const unsigned char* src, int src_len, unsigned char* dest, int dest_cap);
int lzhuf_decode(const unsigned char* src, int src_len, unsigned char* dest, int dest_cap);
int lzhuf_decode_size(const unsigned char* src, int src_len);
}

namespace xrkit::engine::fs {
namespace {

// 参考实现用 file-scope static 状态，**不可重入** → 串行化（与 1.x 的 _lzhuf_lock 同理）。
// 这是明确的代价，不是疏忽：彻底可重入需要把 static 收进上下文结构体。
std::mutex g_lzhuf_mutex;

}  // namespace

std::vector<uint8_t> lzhuf_encode(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(g_lzhuf_mutex);
    // 上限按 1.x 的做法留足余量（最坏情况下 LZHUF 会略微膨胀）
    std::vector<uint8_t> out(size * 2 + 64);
    const int written = ::lzhuf_encode(data, static_cast<int>(size), out.data(),
                                       static_cast<int>(out.size()));
    if (written < 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

bool lzhuf_decode(const uint8_t* src, size_t size, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (size < 4) {
        err = "LZHUF 输入不足 4 字节";
        return false;
    }
    const int declared = lzhuf_decode_size(src, static_cast<int>(size));
    if (declared <= 0 || declared > 256 * 1024 * 1024) {
        err = "LZHUF 声明长度非法（" + std::to_string(declared) + "）";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_lzhuf_mutex);
    out.resize(static_cast<size_t>(declared));
    const int written = ::lzhuf_decode(src, static_cast<int>(size), out.data(), declared);
    if (written < 0) {
        out.clear();
        err = "LZHUF 解码失败";
        return false;
    }
    out.resize(static_cast<size_t>(written));
    return true;
}

}  // namespace xrkit::engine::fs
