# 第三方依赖（vendored，全部静态）。
#
# 源码**不入库**，由 tools/fetch_third_party.py 从实测可达的源拉取并校验 sha256
# （本机 GitHub 被 hosts 屏蔽、schannel TLS 不可用，包管理器不可靠）。
# 缺目录时在这里就报清楚，而不是等编译到找不到头文件才炸。

set(XRKIT_TP "${CMAKE_CURRENT_SOURCE_DIR}")

foreach(dep zlib xz zstd lz4 lzo freetype pcre2)
  if(NOT EXISTS "${XRKIT_TP}/${dep}")
    message(FATAL_ERROR
      "缺少第三方源码 third_party/${dep}\n"
      "  先跑：python tools/fetch_third_party.py")
  endif()
endforeach()

# ── zlib 1.3.1（SquashFS gzip 块）────────────────────────────────────
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${XRKIT_TP}/zlib" "${CMAKE_BINARY_DIR}/tp/zlib" EXCLUDE_FROM_ALL)

# ── xz / liblzma 5.6.4（SquashFS xz 块）─────────────────────────────
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${XRKIT_TP}/xz" "${CMAKE_BINARY_DIR}/tp/xz" EXCLUDE_FROM_ALL)

# ── zstd 1.5.6（SquashFS zstd 块）───────────────────────────────────
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${XRKIT_TP}/zstd/build/cmake" "${CMAKE_BINARY_DIR}/tp/zstd" EXCLUDE_FROM_ALL)

# ── lz4 1.10.0：直接编 lib/lz4.c ────────────────────────────────────
# 上游 CMake 在 contrib/cmake_unofficial 下且会一并构建 CLI，我们只要库，直接编源码。
add_library(xrkit_lz4 STATIC "${XRKIT_TP}/lz4/lib/lz4.c")
target_include_directories(xrkit_lz4 PUBLIC "${XRKIT_TP}/lz4/lib")
set_target_properties(xrkit_lz4 PROPERTIES C_STANDARD 99)

# ── lzo 2.10：用 miniLZO（1.x 的 lzo 缺口就在这里补上）───────────────
# minilzo.c 以 "lzodefs.h" / "lzoconf.h" 的形式引用这两个头，它们在完整树的
# include/lzo/ 下，所以那个目录必须进 include path（否则 C1083 找不到 lzodefs.h）。
add_library(xrkit_lzo STATIC "${XRKIT_TP}/lzo/minilzo/minilzo.c")
target_include_directories(xrkit_lzo PUBLIC
  "${XRKIT_TP}/lzo/minilzo"
  "${XRKIT_TP}/lzo/include/lzo")

# ── FreeType 2.14.3（字形栅格）──────────────────────────────────────
# 版本必须与 1.x 侧 Pillow 12.3.0 捆绑的 FreeType 一致 —— 这是"字库产物保持原样"的基准。
# 我们只栅格化 TTF/OTF 的单个字形，不需要 WOFF/PNG-in-font 那几路压缩与复杂脚本排版，
# 因此把 zlib/bzip2/png/harfbuzz/brotli 全关（不参与栅格结果）。
foreach(opt FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
  set(${opt} TRUE CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${XRKIT_TP}/freetype" "${CMAKE_BINARY_DIR}/tp/freetype" EXCLUDE_FROM_ALL)

# ── PCRE2 10.48（文本提取的正则；std::regex 不支持 lookbehind）──────
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)
add_subdirectory("${XRKIT_TP}/pcre2" "${CMAKE_BINARY_DIR}/tp/pcre2" EXCLUDE_FROM_ALL)

# ── 聚合出口：引擎只认这一个目标 ────────────────────────────────────
add_library(xrkit_third_party INTERFACE)
target_link_libraries(xrkit_third_party INTERFACE
  zlibstatic
  liblzma
  libzstd_static
  xrkit_lz4
  xrkit_lzo
  freetype
  pcre2-8-static)

# xz 把自己的头目录设成了 PRIVATE（不外传），用的人得自己接上 —— 实测编译期
# 报 "无法打开包括文件: lzma.h" 就是这么来的。
target_include_directories(xrkit_third_party INTERFACE
  "${XRKIT_TP}/xz/src/liblzma/api")
