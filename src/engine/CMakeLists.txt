add_library(xrkit_engine STATIC
  engine.cpp
  third_party_probe.cpp
  fs/compress.cpp
  fs/decompress.cpp
  fs/lzhuf.cpp
  fs/lzhuf_ref.c
  fs/squashfs.cpp
  fs/squashfs_write.cpp
  fs/extract_tree.cpp
  fs/overlay.cpp
  fs/db.cpp
  text/encoding.cpp
  text/sniff.cpp
  text/convert.cpp
  text/extract.cpp
  xml/compare.cpp
  video/ffmpeg.cpp
  plugin/contract.cpp
  plugin/registry.cpp
  plugin/abi_host.cpp
  plugin/loader.cpp
  font/charset.cpp
  font/cjk_state.cpp
  font/glyph.cpp
  font/metrics.cpp
  font/package.cpp
  font/render.cpp
  font/resample.cpp
  font/xml_bytes.cpp)

target_include_directories(xrkit_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(xrkit_engine PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/text"
  "${CMAKE_CURRENT_SOURCE_DIR}/font"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin")
target_link_libraries(xrkit_engine
  PUBLIC xrkit_core xrkit_flags
  PRIVATE xrkit_third_party)

# 后续子模块按此挂入（M1）：
#   fs/    SquashFS 读写 + DB 六格式（zlib/xz/zstd/lz4/lzo）
#   font/  字库（FreeType + 自写重采样）
#   text/  编码判定链 + 文本提取（PCRE2）
#   xml/   XML 校对
#   video/ ffmpeg 调用
