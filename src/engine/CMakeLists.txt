add_library(xrkit_engine STATIC
  engine.cpp)

target_include_directories(xrkit_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(xrkit_engine PUBLIC xrkit_core)

# 后续子模块按此挂入（M1）：
#   fs/    SquashFS 读写 + DB 六格式
#   font/  字库（FreeType + 自写重采样）
#   text/  编码判定链 + 文本提取（PCRE2）
#   xml/   XML 校对
#   video/ ffmpeg 调用
