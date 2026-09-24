#pragma once
// xrkit::engine —— 第三方依赖可用性探针。
//
// 为什么要有这组探针："源码拉下来了""能编过"都不等于"能用"。
// 每个依赖都跑一次**真实往返**（或真实功能），把"依赖可用"变成可量的判据。
// 全部在 core/engine 层（不许 Qt）。

#include <string>

namespace xrkit::engine {

struct ProbeResult {
    bool ok = false;
    bool skipped = false;  // 环境缺件（例如系统无字体）→ 不算失败，但必须显式说出原因
    std::string detail;    // 纯 ASCII，供 CLI/门禁解析
};

ProbeResult probe_zlib();
ProbeResult probe_lz4();
ProbeResult probe_zstd();
ProbeResult probe_xz();
ProbeResult probe_lzo();
ProbeResult probe_pcre2();
ProbeResult probe_freetype();

}  // namespace xrkit::engine
