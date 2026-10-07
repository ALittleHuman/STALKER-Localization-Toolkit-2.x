# Qt6 外壳（M2）：hub / 六页 / 组件库 / 主题映射。
#
# 依赖边界（J5 锁在根 CMakeLists 每次构建时强制）：core/engine **不许 include Qt**；
# 只有本目录（与 src/shell 下的组件/页面）能用 Qt。
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()   # AUTOMOC 等；本模块刻意不用 Q_OBJECT，故 moc 只当兜底

add_library(xrkit_shell STATIC
  hub.cpp
  page_host.cpp
  log_box.cpp
  task_shell.cpp
  scroll_viewport.cpp
  split_pane.cpp
  theme.cpp
screen_scale.cpp
  plugin_ui.cpp
  widget_facade_qt.cpp
  facade_tree.cpp
  drop_filter.cpp
  drop_zone.cpp
  pair_tree.cpp
  dialog_start.cpp
  virtual_list.cpp
  pages/page_base.cpp
  pages/page_factory.cpp
  pages/fs_page.cpp
  pages/convert_page.cpp
  pages/text_page.cpp
  pages/xml_page.cpp
  pages/video_page.cpp
  pages/fontpack_page.cpp
  smoke.cpp)

target_include_directories(xrkit_shell PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(xrkit_shell
  PUBLIC xrkit_engine Qt6::Widgets
  PRIVATE xrkit_core)

if(WIN32)
  # 深色标题栏（DwmSetWindowAttribute）
  target_link_libraries(xrkit_shell PRIVATE dwmapi)
  # 版本资源判据（src/shell/smoke.cpp 读本进程 exe 的 VERSIONINFO）：
  # GetFileVersionInfoW / VerQueryValueW 在 version.dll 里
  target_link_libraries(xrkit_shell PRIVATE version)
endif()

qt_add_executable(xrkit_app
  main.cpp)
# ★ 自检夹具：**假的 ffmpeg**（见 fake_ffmpeg.cpp 的头注释）。进度条 / 取消（终止进程 +
#   删半成品）/ 跑完收干净进程 这三条判据必须落在"真进程"上，桩函数替不掉；而真 ffmpeg
#   在本机不一定按 ⑧8.1 的口径可见（只认 user.ltx 与程序目录、不搜 PATH）⇒ 自己造一个。
#   它**不进发行包**（package.ps1 只拷 xrkit_app.exe + Qt 运行时），纯粹是构建产物。
add_executable(xrkit_fake_ffmpeg fake_ffmpeg.cpp)
target_link_libraries(xrkit_fake_ffmpeg PRIVATE xrkit_flags)
set_target_properties(xrkit_fake_ffmpeg PROPERTIES
  OUTPUT_NAME "xrkit_fake_ffmpeg"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(xrkit_app PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")
# 版本资源（VERSIONINFO）：**生成物**（tools/gen_winres.py ← include/xrkit/core/version.h），
# 门禁 `gen_winres.py --check` 逐字节比对 ⇒ 版本串全仓库只有 version.h 一处事实来源。
target_sources(xrkit_app PRIVATE "${CMAKE_SOURCE_DIR}/assets/xrkit_winres.rc")
target_compile_definitions(xrkit_app PRIVATE XRKIT_WINRES_APP)

target_link_libraries(xrkit_app PRIVATE xrkit_shell Qt6::Widgets)

# ★★ 插件改装 P2：GUI 也要嵌入 CPython（插件＝`.py`；宿主在引擎层，两个前端共用）
#   ⇒ 运行期把 `python3XX.dll` 拷到 exe 旁边（口径见根 CMakeLists 的 `xrkit_attach_python_runtime`）。
xrkit_attach_python_runtime(xrkit_app)

# ★★ 2026-09-28 用户："软件打开弹出cmd的问题，也一直没修。" —— 查实：**只有构建器**设了
#   `WIN32_EXECUTABLE TRUE`，主程序从来没设 ⇒ `xrkit_app.exe` 的 PE 子系统是 3（Console）
#   ⇒ 双击必弹一个黑框；`no-console` 那条锁只管源码里的 `std::system()`/`QProcess`，
#   **管不到"主程序自己是不是控制台程序"**，所以一直漏着。
#   设成 TRUE ⇒ 链接 `/SUBSYSTEM:WINDOWS`（PE 子系统=2）。门禁据此**读 PE 头**判红（见
#   `tests/check_no_console.py`），不再只看源码。
#   ⚠ 副作用须知：GUI 子系统下从**已有控制台**启动仍能用 `>` 重定向拿到 stdout
#   （门禁与 `--smoke`/`--scale-report` 都靠这个），只是**不再新建**控制台窗口。
set_target_properties(xrkit_app PROPERTIES
  WIN32_EXECUTABLE TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# 构建 GUI（口径见 docs/2.0.0-构建口径与buildGUI规格.md）：**只驱动 CLI**，不自己实现构建。
# 挂在 shell 目标旁边是因为这里一定已经 find_package(Qt6) 了（release 预设不带 Qt，不能放根 CMake）。
qt_add_executable(xrkit_build_gui "${CMAKE_SOURCE_DIR}/tools/build_gui/main.cpp")
target_sources(xrkit_build_gui PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")
# 构建器也编 exe ⇒ 版本资源一并挂上（用户口径："若 tools/build_gui 也编 exe，一并挂上"）
target_sources(xrkit_build_gui PRIVATE "${CMAKE_SOURCE_DIR}/assets/xrkit_winres.rc")
target_compile_definitions(xrkit_build_gui PRIVATE XRKIT_WINRES_GUI)
# ★ 必须链 xrkit_flags（带 /utf-8）：不链的话 MSVC 按系统 ANSI(GBK) 读源文件，
#   界面里的中文字面量会变成乱码（实测：分组框标题与下拉项显示成'鐗堟湰…'）。
target_link_libraries(xrkit_build_gui PRIVATE xrkit_core xrkit_flags Qt6::Widgets
  # 用户口径"小组件用通用的"：链 shell 拿 LogBox 等通用组件（组件头在 src/shell）
  xrkit_shell)
target_include_directories(xrkit_build_gui PRIVATE
  "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src/shell")
set_target_properties(xrkit_build_gui PROPERTIES
  OUTPUT_NAME "xrkit_build_gui"
  # ★ 必须 GUI 子系统：否则双击会**连带弹出一个命令提示符窗口**（用户实测反馈）。
  #   注意 RUNTIME_OUTPUT_DIRECTORY 是**运行时**目录，别写成 RUNTIME_OUTPUT_DIRECTORY 拼错；
  #   WIN32_EXECUTABLE=TRUE ⇒ 链接 /SUBSYSTEM:WINDOWS（PE 子系统=2）。
  WIN32_EXECUTABLE TRUE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
# 构建器也链了 xrkit_shell → xrkit_engine → CPython ⇒ 同样要 DLL 在旁（P2）
xrkit_attach_python_runtime(xrkit_build_gui)
