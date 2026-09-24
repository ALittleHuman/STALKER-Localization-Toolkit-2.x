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
  virtual_list.cpp
  pages/page_base.cpp
  pages/page_factory.cpp
  pages/fs_page.cpp
  pages/convert_page.cpp
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

target_link_libraries(xrkit_app PRIVATE xrkit_shell Qt6::Widgets)
