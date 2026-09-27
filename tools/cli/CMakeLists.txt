add_executable(xrkit_cli main.cpp build_cmd.cpp version_store_check.cpp
  "${CMAKE_SOURCE_DIR}/assets/app.rc")
target_link_libraries(xrkit_cli PRIVATE xrkit_engine xrkit_core xrkit_flags)

# 测试插件：**故意用 C 编译**（.c + C 编译器）——用来证明这套 ABI 真的是纯 C ABI，
# 也用来当"M2-7c 加载链路"的夹具（自检会 LoadLibrary 它、跑它的注册、调它的回调）。
# 输出到与 CLI 同一个目录（`xrkit_test_plugin.dll` 落在 xrkit_cli.exe 旁边），
# 这样自检用 `app_dir()/xrkit_test_plugin.dll` 就能找到它 —— 不写死构建目录。
add_library(xrkit_test_plugin SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../test_plugin/test_plugin.c")
target_include_directories(xrkit_test_plugin PRIVATE "${CMAKE_SOURCE_DIR}/include")
# ★ 必须挂 xrkit_flags：它带 `/utf-8`。不加的话 MSVC 按 GBK 读源文件，
# 中文注释末尾的悬挂首字节会把行尾换行"吃掉"、把下一行并进注释 —— 症状是
# 几十行外的语法错（实测：abi.h(71) "语法错误: )"）。这条对 .c 同样适用。
target_link_libraries(xrkit_test_plugin PRIVATE xrkit_flags)
set_target_properties(xrkit_test_plugin PROPERTIES
  PREFIX ""
  OUTPUT_NAME "xrkit_test_plugin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
