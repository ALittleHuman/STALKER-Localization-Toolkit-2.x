cmake_minimum_required(VERSION 3.21)
project(STALKER_Toolkit_2x VERSION 2.0.0 LANGUAGES C CXX)

# Windows 资源（.rc）：可执行文件的图标等 —— 需要显式启用 RC 语言
enable_language(RC)

# ─────────────────────────────────────────────────────────────────────
# /showIncludes 前缀（2026-09-24 实测，别删）
#
# 症状：改任何 `.h` / 生成表 `.inc` 之后 `cmake --build` 报"没事可做"，门禁拿着**旧二进制**
#       判绿 —— CI 全绿而产物是旧的。这是最危险的一类假绿。
# 成因：cl.exe 的消息是本地化的。本机（中文 MSVC 14.44）实测 cl 以 **UTF-8 字节**输出
#       `注意: 包含文件: `，而 CMake 的前缀探测按**系统 ANSI 码页**解码 → 内部字符串成了
#       乱码（`娉ㄦ剰: 鍖呭惈鏂囦欢: `）→ 原样进 rules.ninja 的 `msvc_deps_prefix`
#       → ninja 与 cl 输出**逐字节**比对永不命中 → `.ninja_deps` 里一条头文件都没有。
# 修法：构建入口用 `tools/probe_showincludes_prefix.py` 量出**原始字节**（Python 直读管道，
#       不经码页），经**命令行** `-DXRKIT_SHOWINCLUDES_PREFIX=…` 传进来；这里把它
#       强塞给生成器读的那几个变量。
#       ★ 在 CMake 里自己探测**无效**：实测 `execute_process` 的输出同样被按 ANSI 解码，
#         拿回来已经是乱码（白忙过一轮）；命令行参数走 Windows 宽字符接口，才不带码页。
#       ★ 只改 cache **也无效**：生成的 CMakeCXXCompiler.cmake 把
#         `CMAKE_CL_SHOWINCLUDES_PREFIX` 设成了**普通变量**，普通变量遮住同名 cache 项
#         ⇒ 必须连普通变量一起设。
#       ★ 没传这个变量时保持 CMake 自己的探测（英文 MSVC 下量出来就是
#         `Note: including file: `，一致，无副作用）；此时门禁的**效果自检**
#         （tests/run_ci.py 的 build 步看 `.ninja_deps` 里有没有头文件）会兜住假绿。
# ─────────────────────────────────────────────────────────────────────
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  if(DEFINED XRKIT_SHOWINCLUDES_PREFIX AND NOT XRKIT_SHOWINCLUDES_PREFIX STREQUAL "")
    set(_xrkit_inc_prefix "${XRKIT_SHOWINCLUDES_PREFIX}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}" CACHE STRING
        "cl /showIncludes 前缀（由 tools/probe_showincludes_prefix.py 量出）" FORCE)
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}" CACHE STRING
        "cl /showIncludes 前缀（由 tools/probe_showincludes_prefix.py 量出）" FORCE)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}" CACHE STRING
        "cl /showIncludes 前缀（由 tools/probe_showincludes_prefix.py 量出）" FORCE)
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_xrkit_inc_prefix}")
    message(STATUS "showIncludes 前缀（外部量出）= \"${_xrkit_inc_prefix}\"")
  else()
    message(STATUS "showIncludes 前缀：未传入 XRKIT_SHOWINCLUDES_PREFIX，"
                   "用 CMake 自带探测（本地化 MSVC 下会失守，见 CMakeLists.txt 顶部说明）")
  endif()
endif()

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
# ★★ 插件改装 P2（用户 2026-10-07 拍板：**Python 模块接口 + C ABI 退役**）
#
# 引擎要**嵌入 CPython**（插件＝`.py`，见 include/xrkit/engine/plugin/py_host.h）。
# 编译期只需要 Python3 的**开发文件**（头 + 运行期 DLL）；运行期开发态用本机 Python、
# 发行态用包内自带的 embeddable Python（`<exe>/python/`）。
#
# 降级口径（零静默）：找不到开发文件 ⇒ 编出"降级宿主"（`PYHOST enabled=0 reason=…`），
#   **不许**让"没有宿主"看起来像"宿主正常"。门禁 `[python-host]` 步据此判：release 档见
#   `enabled=0` 即 FAIL（不是 SKIP —— 那会变成"一键关掉锁"的后门）。
# ─────────────────────────────────────────────────────────────────────
option(XRKIT_WITH_PYTHON "构建 CPython 插件宿主（需要 Python3 开发文件）" ON)
set(XRKIT_PYTHON_AVAILABLE OFF)
if(XRKIT_WITH_PYTHON)
  find_package(Python3 COMPONENTS Development.Embed QUIET)
  if(Python3_Development.Embed_FOUND)
    set(XRKIT_PYTHON_AVAILABLE ON)
    message(STATUS "CPython 插件宿主：启用（Python3 ${Python3_VERSION}；"
                   "include=${Python3_INCLUDE_DIRS}；运行期=${Python3_RUNTIME_LIBRARY}）")
  else()
    message(STATUS "CPython 插件宿主：**降级**（找不到 Python3 开发文件 —— 宿主会如实报不可用）")
  endif()
endif()

# 嵌入 CPython ⇒ 运行期必须找得到 `python3XX.dll`。**不能依赖 PATH**：
#   本机 python.exe 是安装器的 shim（`…\Python\bin`），真正的 DLL 在 `…\pythoncore-3.14-64\`
#   而它**不在 PATH 上**（实测：PATH 里没有任一目录含 python314.dll）⇒ 链接了 import lib
#   的 exe 会在启动时报"找不到 python314.dll"。规则也要求不设持久环境变量 ⇒
#   把运行期 DLL 拷到 exe 旁边（发行包同理，只是那份来自 embeddable Python）。
function(xrkit_attach_python_runtime target)
  if(NOT XRKIT_PYTHON_AVAILABLE)
    return()
  endif()
  set(_xrkit_py_dlls "")
  # ① CMake 直接告诉我们的那一个（`python3XX.dll`）
  if(DEFINED Python3_RUNTIME_LIBRARY AND EXISTS "${Python3_RUNTIME_LIBRARY}")
    list(APPEND _xrkit_py_dlls "${Python3_RUNTIME_LIBRARY}")
  endif()
  # ② 同目录下的 `python3.dll`（稳定 ABI 转发层）—— 它不一定被上面的变量提到
  set(_xrkit_py_dirs ${Python3_RUNTIME_LIBRARY_DIRS})
  if(NOT _xrkit_py_dirs AND DEFINED Python3_INCLUDE_DIR)
    get_filename_component(_xrkit_py_dirs "${Python3_INCLUDE_DIR}/.." ABSOLUTE)
  endif()
  foreach(_dir ${_xrkit_py_dirs})
    file(GLOB _hit "${_dir}/python3*.dll")
    list(APPEND _xrkit_py_dlls ${_hit})
  endforeach()
  list(REMOVE_DUPLICATES _xrkit_py_dlls)
  if(NOT _xrkit_py_dlls)
    message(WARNING "找不到 python3*.dll（搜过：${_xrkit_py_dirs}）—— 运行期会缺 DLL")
  endif()
  foreach(_dll ${_xrkit_py_dlls})
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>")
  endforeach()
endfunction()

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
