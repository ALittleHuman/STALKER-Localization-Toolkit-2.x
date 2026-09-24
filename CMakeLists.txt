cmake_minimum_required(VERSION 3.21)
project(STALKER_Toolkit_2x VERSION 2.0.0 LANGUAGES C CXX)

# ─────────────────────────────────────────────────────────────────────
# 全局口径
# ─────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 第三方依赖全部静态链接（发行包少一堆 DLL；也避免 DLL 被安全软件单独拦）
set(BUILD_SHARED_LIBS OFF CACHE BOOL "只构建静态库" FORCE)

# 严格开关做成 INTERFACE 目标，只加在**我们自己的**目标上。
# 为什么不用全局 add_compile_options：那会把 /W4 /permissive- 也压到第三方源码上，
# 几万条第三方告警会把真问题淹掉（构建噪声也是成本）。
add_library(xrkit_flags INTERFACE)
if(MSVC)
  target_compile_options(xrkit_flags INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
  target_compile_definitions(xrkit_flags INTERFACE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Qt 外壳默认关闭：M0 只做 core + engine + CLI（这三者都不许依赖 Qt）
option(XRKIT_BUILD_SHELL "构建 Qt6 外壳（需要 Qt6 与源文件）" OFF)

# ─────────────────────────────────────────────────────────────────────
# 第三方依赖：vendored 源码（离线可复现），见 third_party/ 与 tools/fetch_third_party.py
# ─────────────────────────────────────────────────────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CMakeLists.txt")
  add_subdirectory(third_party)
endif()

add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(tools/cli)

if(XRKIT_BUILD_SHELL)
  add_subdirectory(src/shell)
endif()

# ─────────────────────────────────────────────────────────────────────
# 硬约束 J5：core / engine / 公共头不许出现 Qt 依赖
#   每次构建都跑；违反即 FATAL_ERROR（不是警告）。
# ─────────────────────────────────────────────────────────────────────
add_custom_target(xrkit_check_no_qt ALL
  COMMAND ${CMAKE_COMMAND}
          -DXRKIT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_qt.cmake"
  COMMENT "J5 检查：core/engine 无 Qt 依赖")

add_dependencies(xrkit_core xrkit_check_no_qt)
add_dependencies(xrkit_engine xrkit_check_no_qt)
