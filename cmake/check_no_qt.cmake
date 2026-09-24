# J5 硬约束：core / engine / 公共头里不许出现 Qt 依赖。
#
# 为什么要有这条：用户 2026-09-18 已定「core 不许 include Qt」——引擎库必须能被
# CLI 黑盒驱动（黄金语料差分就靠它），也必须能脱离 Qt 单独编译与测试。
# 判据是**扫源码**，不是"我们记得没写"。构建期 FATAL_ERROR，不是警告。
#
# 用法：cmake -DXRKIT_ROOT=<repo> -P cmake/check_no_qt.cmake

if(NOT DEFINED XRKIT_ROOT)
  message(FATAL_ERROR "check_no_qt.cmake: 需要 -DXRKIT_ROOT=<仓库根>")
endif()

file(GLOB_RECURSE XRKIT_SCAN
  "${XRKIT_ROOT}/src/core/*.cpp"   "${XRKIT_ROOT}/src/core/*.h"   "${XRKIT_ROOT}/src/core/*.hpp"
  "${XRKIT_ROOT}/src/engine/*.cpp" "${XRKIT_ROOT}/src/engine/*.h" "${XRKIT_ROOT}/src/engine/*.hpp"
  "${XRKIT_ROOT}/include/xrkit/*.h" "${XRKIT_ROOT}/include/xrkit/*.hpp")

list(LENGTH XRKIT_SCAN XRKIT_SCAN_COUNT)
if(XRKIT_SCAN_COUNT EQUAL 0)
  message(FATAL_ERROR "check_no_qt.cmake: 一个源文件都没扫到（路径写错了？）——这条不许静默通过")
endif()

set(violations "")
foreach(f ${XRKIT_SCAN})
  file(READ "${f}" content)

  # 先剥掉注释，免得"注释里提到 Qt 头名"被误判（例：说明性注释）
  string(REGEX REPLACE "/\\*([^*]|\\*[^/])*\\*/" "" content "${content}")
  string(REGEX REPLACE "//[^\n]*" "" content "${content}")

  # 两种 Qt 包含形态：模块式 <QtCore/QString>、类式 <QString>
  string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"]Qt[0-9A-Za-z_]*[/>\"]" hits_a "${content}")
  string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"]Q[A-Z][0-9A-Za-z_]*[>\"]" hits_b "${content}")
  set(hits "${hits_a};${hits_b}")
  string(STRIP "${hits}" hits)
  if(NOT hits STREQUAL ";" AND NOT hits STREQUAL "")
    list(APPEND violations "${f} -> ${hits}")
  endif()
endforeach()

if(violations)
  string(REPLACE ";" "\n  " pretty "${violations}")
  message(FATAL_ERROR
    "J5 红线：core/engine/公共头里出现了 Qt 依赖（引擎必须能脱离 Qt 编译）：\n  ${pretty}")
endif()

message(STATUS "J5 OK：${XRKIT_SCAN_COUNT} 个 core/engine/公共头文件均无 Qt 依赖")
