cmake_minimum_required(VERSION 3.21)
project(STALKER_Toolkit_2x VERSION 2.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────
# 全局编译口径
#   /Zc:__cplusplus 与 /utf-8 是 1.x 的血账：前者不加以致 Qt 直接 #error C1189，
#   后者不加以致中文注释触发 C4819。
# ─────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Qt 外壳默认关闭：M0 只做 core + engine + CLI（这三者都不许依赖 Qt）
option(XRKIT_BUILD_SHELL "构建 Qt6 外壳（需要 Qt6 与源文件）" OFF)

# ─────────────────────────────────────────────────────────────────────
# 第三方依赖：vendored 源码（离线可复现），见 third_party/
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
