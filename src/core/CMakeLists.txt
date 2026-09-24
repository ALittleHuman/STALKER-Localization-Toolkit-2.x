add_library(xrkit_core STATIC
  version.cpp
  check.cpp
  log.cpp)

target_include_directories(xrkit_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(xrkit_core PUBLIC xrkit_flags)

# core 是引擎与外壳的共同底座，必须与 Qt 完全无关（由根 CMakeLists 的
# xrkit_check_no_qt 目标在每次构建时强制）。
