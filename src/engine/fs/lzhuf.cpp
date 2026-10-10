#include "xrkit/engine/fs/lzhuf.h"
#include "xrkit/engine/fs/untrusted_limits.h"   // ★ P17-13（01 F-08）：不可信尺寸上限的唯一定义


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
    // ★★ 2026-10-10（P17-17，审计 01 F-14 Low）：**编码缓冲尺寸必须先判上限**。
    //   事实（修前）：`std::vector<uint8_t> out(size * 2 + 64)` —— 直接按**输入长度**×2 分配，
    //   而 `size * 2` 本身可能**回绕**（`size > SIZE_MAX/2` 时表达式截断），于是 out 变得极小；
    //   紧接着 `static_cast<int>(size)` 与 `static_cast<int>(out.size())` 又是**窄化**：
    //   一个荒谬的入参（上游 bug / 超大条目）不会当场被拒，而是拿着错尺寸进 LZHUF 内部去写
    //   —— 这是"越界写"的典型形状。判据（`xrkit_app --smoke`）：超上限或空指针必须**当场返回空**
    //   并且**一个字节都不碰**（判据故意用 `nullptr` + 超大 size 调，能安全证明"先判后碰"）。
    constexpr size_t kMaxEncodeInputBytes = XRKIT_LZHUF_MAX_DECLARED_BYTES;
    if (data == nullptr || size == 0 || size > kMaxEncodeInputBytes) return {};
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
    // ★ P17-13（01 F-08）：上限取值收拢到 untrusted_limits.h（一处定义，两处一起变）
    if (declared <= 0 ||
        static_cast<uint64_t>(declared) > XRKIT_LZHUF_MAX_DECLARED_BYTES) {
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
