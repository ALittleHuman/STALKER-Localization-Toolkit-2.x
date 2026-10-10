# J5 硬约束：core / engine / 公共头里不许出现 Qt 依赖。
#
# 为什么要有这条：用户 2026-09-18 已定「core 不许 include Qt」——引擎库必须能被
# CLI 黑盒驱动（黄金语料差分就靠它），也必须能脱离 Qt 单独编译与测试。
# 判据是**扫源码**，不是"我们记得没写"。构建期 FATAL_ERROR，不是警告。
#
# ★ 这里用的是 `GLOB_RECURSE`，所以 `include/xrkit/*.h` 会连**子目录**一起扫。外壳的头
#   （hub.h / theme.h / smoke.h）因此**故意**放在 `src/shell/` 而不是 `include/xrkit/shell/`：
#   外壳本来就要 include Qt（QMainWindow 等），它不属于"能被 CLI 黑盒驱动的公共 API"。
#   别把它们挪回 include/ 下 —— 那会把 M2 外壳的头全部误判成红线。
#
# 用法：cmake -DXRKIT_ROOT=<repo> -P cmake/check_no_qt.cmake

if(NOT DEFINED XRKIT_ROOT)
  message(FATAL_ERROR "check_no_qt.cmake: 需要 -DXRKIT_ROOT=<仓库根>")
endif()

file(GLOB_RECURSE XRKIT_SCAN
  "${XRKIT_ROOT}/src/core/*.cpp"   "${XRKIT_ROOT}/src/core/*.h"   "${XRKIT_ROOT}/src/core/*.hpp"
  "${XRKIT_ROOT}/src/engine/*.cpp" "${XRKIT_ROOT}/src/engine/*.h" "${XRKIT_ROOT}/src/engine/*.hpp"
  "${XRKIT_ROOT}/include/xrkit/*.h" "${XRKIT_ROOT}/include/xrkit/*.hpp"
  # ★★ 2026-10-10（P17-46，零碎批）：**补 `tools/cli`** —— 修前这里只扫 core/engine/公共头，
  #   于是"往 `tools/cli/main.cpp` 里塞一个 `#include <QString>`"**完全静默**（实测：仍打
  #   `J5 OK：96 个 …`）。而 CLI 被定位成"引擎的黑盒驱动器"（必须能脱离 Qt 单独编译/运行），
  #   它一旦沾 Qt，那条定位就没了 ⇒ CLI 必须一起扫。
  #   ★ **只扫 `tools/cli`，不扫整个 `tools/`**：`tools/build_gui/main.cpp` 是**GUI 工具**，
  #     它本来就 include `<QComboBox>` 等（实测：把整个 tools/ 加进来会立刻红在那一个文件上）——
  #     与 `src/shell/` 的处置同理：**GUI 侧不受这条红线约束，被 CLI 黑盒驱动的那些才受**。
  #   ★ 同一批的实测更正：审计说"GLOB 漏 `include/xrkit/plugin/abi.h`"——**实测不成立**：
  #     `GLOB_RECURSE` 连子目录一起扫，往 `abi.h` 里注入 Qt 会**立刻红**（96 个文件里含它）。
  "${XRKIT_ROOT}/tools/cli/*.cpp" "${XRKIT_ROOT}/tools/cli/*.h" "${XRKIT_ROOT}/tools/cli/*.hpp")

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

  # 三种 Qt 包含形态：模块式 <QtCore/QString>、类式 <QString>、**小写私名式 <qstring.h>**。
  # ★★ 补充 H/§5.7（B 批）：修前只有前两种 —— `#include <qstring.h>` / `<qglobal.h>` 这类
  #   **小写 Qt 头整类漏网**（实测：塞进 src/core/ 后这里照打 "J5 OK：74 个文件均无 Qt 依赖"、
  #   rc=0；而 `<QtCore/QString>` 会红）⇒ J5 是红线，锁有洞＝红线可绕。
  string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"]Qt[0-9A-Za-z_]*[/>\"]" hits_a "${content}")
  string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"]Q[A-Z][0-9A-Za-z_]*[>\"]" hits_b "${content}")
  string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"]q[a-z0-9_]*\\.h[>\"]" hits_c "${content}")
  # ★ 拼装改成"只看非空组件"：修前用 `STRIP` 后比 `";"` 当"全空"的哨兵，**三个组件时就变成
  #   `";;"`** ⇒ 空命中也会被判成违规（改坏基线的正是这个哨兵）。组件式拼装没有这个坑。
  set(hits "")
  foreach(part "${hits_a}" "${hits_b}" "${hits_c}")
    if(NOT part STREQUAL "")
      set(hits "${hits}${part};")
    endif()
  endforeach()
  if(NOT hits STREQUAL "")
    list(APPEND violations "${f} -> ${hits}")
  endif()
endforeach()

if(violations)
  string(REPLACE ";" "\n  " pretty "${violations}")
  message(FATAL_ERROR
    "J5 红线：core/engine/公共头里出现了 Qt 依赖（引擎必须能脱离 Qt 编译）：\n  ${pretty}")
endif()

message(STATUS "J5 OK：${XRKIT_SCAN_COUNT} 个 core/engine/公共头文件均无 Qt 依赖")
