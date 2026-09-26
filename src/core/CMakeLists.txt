add_library(xrkit_core STATIC
  version.cpp
  check.cpp
  log.cpp
  path.cpp
  atomic_write.cpp
  build_version.cpp
  theme.cpp)

target_include_directories(xrkit_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
# theme_tables.inc 与 theme.cpp 同目录，但显式加一下更稳（与 engine 对 text/font 的写法一致）
target_include_directories(xrkit_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(xrkit_core PUBLIC xrkit_flags)

# core 是引擎与外壳的共同底座，必须与 Qt 完全无关（由根 CMakeLists 的
# xrkit_check_no_qt 目标在每次构建时强制）。
