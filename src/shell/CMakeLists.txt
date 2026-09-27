# Qt6 外壳（M2）：hub / 六页 / 组件库 / 主题映射。
#
# 依赖边界（J5 锁在根 CMakeLists 每次构建时强制）：core/engine **不许 include Qt**；
# 只有本目录（与 src/shell 下的组件/页面）能用 Qt。
find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()   # AUTOMOC 等；本模块刻意不用 Q_OBJECT，故 moc 只当兜底

add_library(xrkit_shell STATIC
  hub.cpp
  log_box.cpp
  task_shell.cpp
  scroll_viewport.cpp
  split_pane.cpp
  theme.cpp
ui_scale.cpp
  plugin_ui.cpp
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
endif()

qt_add_executable(xrkit_app
  main.cpp)
target_sources(xrkit_app PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")

target_link_libraries(xrkit_app PRIVATE xrkit_shell Qt6::Widgets)

# 构建 GUI（口径见 docs/2.0.0-构建口径与buildGUI规格.md）：**只驱动 CLI**，不自己实现构建。
# 挂在 shell 目标旁边是因为这里一定已经 find_package(Qt6) 了（release 预设不带 Qt，不能放根 CMake）。
qt_add_executable(xrkit_build_gui "${CMAKE_SOURCE_DIR}/tools/build_gui/main.cpp")
target_sources(xrkit_build_gui PRIVATE "${CMAKE_SOURCE_DIR}/assets/app.rc")
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
