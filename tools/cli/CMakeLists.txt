add_executable(xrkit_cli main.cpp build_cmd.cpp version_store_check.cpp
  "${CMAKE_SOURCE_DIR}/assets/app.rc"
  # 版本资源（VERSIONINFO）：**生成物**，版本串唯一事实来源＝include/xrkit/core/version.h，
  # 由 tools/gen_winres.py 生成、门禁 `--check` 逐字节比对（别手改，也别在这里写版本号）。
  "${CMAKE_SOURCE_DIR}/assets/xrkit_winres.rc")
# ★ 告诉生成物"这是哪个 exe"（每个 exe 一份 FileDescription/OriginalFilename/InternalName）。
#   这个定义会经 CMake 的 RC 规则传给 rc.exe（`$DEFINES` ⇒ `/d XRKIT_WINRES_CLI`）；
#   万一路径变了没传到，生成物里那条 `#error` 会让**编译直接失败** ——
#   宁可不编过，也不要静默挂上一份别的目标的资源。
target_compile_definitions(xrkit_cli PRIVATE XRKIT_WINRES_CLI)
# ★ 2026-10-07（H1-2）：`--exe-version` 读 exe 的 VERSIONINFO（`GetFileVersionInfoW` /
#   `VerQueryValueW`）⇒ 必须链 `version.lib`（否则链接期找不到符号）。
target_link_libraries(xrkit_cli PRIVATE xrkit_engine xrkit_core xrkit_flags version)

# ★★ 插件改装 P2：CLI 也嵌入 CPython（插件＝`.py`，两个前端**同一份**宿主与注册表）
#   ⇒ 运行期必须把 `python3XX.dll` 拷到 exe 旁边（口径见根 CMakeLists 的同名函数）。
xrkit_attach_python_runtime(xrkit_cli)

# ★★ 决策 1（用户 2026-10-18/19「C ABI 退干净」）：
#   原来这里有三个 C 插件夹具 target（`xrkit_test_plugin` / `_badinit` / `_throwinit`）——
#   它们存在的唯一目的是验证 C ABI 的加载链（LoadLibrary / `stk_plugin_init` / 失败回滚 /
#   异常护栏）。C ABI 已删除 ⇒ 三个夹具连同它们的源文件一起删掉。
#   Python 插件的夹具在 `tests/python_plugins/` 与 `src/shell/smoke.cpp` 里。
