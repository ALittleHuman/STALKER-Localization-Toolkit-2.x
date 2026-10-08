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
  text/batch.cpp
  text/encodings.cpp
  text/extract.cpp
  xml/compare.cpp
  xml/dirdiff.cpp
  video/ffmpeg.cpp
  video/transcode.cpp
  plugin/contract.cpp
  plugin/registry.cpp
  plugin/abi_host.cpp
  plugin/loader.cpp
  # ★★ 插件改装 P2：CPython 宿主（`XRKIT_WITH_PYTHON` 未定义时编出**降级**实现：
  #   `enabled=false` + 如实理由 —— 文件本身总是参与编译，避免"少编一个文件"变成静默降级）
  plugin/py_host.cpp
  # ★★ 插件改装 P3：宿主控件门面（Qt-free 的表 + 进程级状态；实现在外壳 `ui_facade_qt.cpp`）
  plugin/widget_facade.cpp
  # ★★ P7a：引擎调用门面（插件怎么调引擎；Qt-free 的表 + 进程级状态）
  plugin/engine_facade.cpp
  # 默认表（字体那几件只依赖引擎；CLI 与 GUI 两个宿主都用得上）
  plugin/engine_facade_impl.cpp
  # ★★ P7b：宿主服务门面（偏好读写 / exe 目录 / 系统字体目录；只依赖引擎）
  plugin/host_facade.cpp
  plugin/host_facade_impl.cpp
  font/charset.cpp
  font/cjk_state.cpp
  font/glyph.cpp
  font/metrics.cpp
  font/package.cpp
  font/render.cpp
  font/resample.cpp
  font/system_dirs.cpp
  font/xml_bytes.cpp)

target_include_directories(xrkit_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(xrkit_engine PRIVATE
  # ★★ 2026-10-06（审计第 4 组）：`src/engine` 自己也要在包含路径里 ——
  #   模块**在编**探针的声明头 `probe_decls.h` 就在这一层（各模块子目录的 .cpp 写的是
  #   `#include "probe_decls.h"`）。不加这一条，子目录里的相对包含找不到它（实测 C1083）。
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/text"
  "${CMAKE_CURRENT_SOURCE_DIR}/font"
  "${CMAKE_CURRENT_SOURCE_DIR}/plugin")
target_link_libraries(xrkit_engine
  PUBLIC xrkit_core xrkit_flags
  PRIVATE xrkit_third_party)

# ★★ 插件改装 P2：CPython 宿主（口径与降级说明见 include/xrkit/engine/plugin/py_host.h
#   与根 CMakeLists 的 `XRKIT_WITH_PYTHON` 段）。
#   `pybind11` 是 **header-only**（third_party/CMakeLists.txt 只给一个 INTERFACE 目标），
#   `Python3::Python` 提供嵌入解释器所需的 import lib（运行期 DLL 由
#   `xrkit_attach_python_runtime(<exe 目标>)` 拷到 exe 旁边）。
if(XRKIT_PYTHON_AVAILABLE)
  target_compile_definitions(xrkit_engine PRIVATE XRKIT_WITH_PYTHON=1)
  target_link_libraries(xrkit_engine PRIVATE xrkit_pybind11 Python3::Python)
  # ★★ **嵌入解释器必须知道自己的 home**（实测踩到，别删）：不设 `PyConfig.home` 时，
  #   CPython 按**宿主 exe 所在目录**推 prefix ⇒ 去 `build/msvc-release/tools/cli/Lib` 找标准库，
  #   于是 `Failed to import encodings module` / `Could not find platform independent libraries`
  #   ⇒ `PYHOST enabled=1 initialized=0`（宿主起不来）。
  #   开发态把**构建时用的** Python 前缀烘焙进来；发行态由 exe 旁的 `python/`（embeddable）
  #   **优先**接管（解析顺序见 py_host.cpp 的 `resolve_python_home`）。
  set(_xrkit_py_prefix "")
  if(DEFINED Python3_RUNTIME_LIBRARY AND EXISTS "${Python3_RUNTIME_LIBRARY}")
    get_filename_component(_xrkit_py_prefix "${Python3_RUNTIME_LIBRARY}" DIRECTORY)
  elseif(DEFINED Python3_INCLUDE_DIRS AND Python3_INCLUDE_DIRS)
    get_filename_component(_xrkit_py_prefix "${Python3_INCLUDE_DIRS}/.." ABSOLUTE)
  endif()
  if(_xrkit_py_prefix)
    target_compile_definitions(xrkit_engine PRIVATE
      XRKIT_PYTHON_HOME_DEFAULT="${_xrkit_py_prefix}")
    message(STATUS "CPython 插件宿主：开发态默认 home = ${_xrkit_py_prefix}")
  else()
    message(WARNING "CPython 插件宿主：推不出 Python 前缀（嵌入解释器可能找不到标准库）")
  endif()
endif()

# 后续子模块按此挂入（M1）：
#   fs/    SquashFS 读写 + DB 六格式（zlib/xz/zstd/lz4/lzo）
#   font/  字库（FreeType + 自写重采样）
#   text/  编码判定链 + 文本提取（PCRE2）
#   xml/   XML 校对
#   video/ ffmpeg 调用
