#include "version_store_check.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "xrkit/core/build_version.h"
#include "xrkit/core/path.h"

namespace xrkit_cli {
namespace {

std::vector<char> slurp(const std::string& path) {
    std::ifstream f(xrkit::core::path_from_utf8(path), std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}

void write_raw(const std::string& path, const std::string& text) {
    std::ofstream f(xrkit::core::path_from_utf8(path), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

}  // namespace

void register_version_store_check(xrkit::core::CheckRegistry& reg) {
    reg.add("core: 构建版本存储（树往返 / next=max+1 / 备份还原逐字节 / BOM 与坏文件不覆盖）",
            [](xrkit::core::CheckContext& ctx) {
                namespace core = xrkit::core;
                std::error_code ec;
                const std::filesystem::path work =
                    std::filesystem::temp_directory_path(ec) / "xrkit_selftest_version_store";
                std::filesystem::remove_all(work, ec);
                std::filesystem::create_directories(work, ec);
                const std::string path = core::path_to_utf8(work / "version.json");
                std::string err;

                // ① 树往返
                core::VersionStore store;
                store.current_version = "2.0.0";
                store.current_prerelease = "BETA";
                store.current_index = 1;
                core::BuildRecord r1;
                r1.built = "2026-09-27 10:00:00";
                r1.commit = "abc1234";
                r1.artifacts.push_back("dist/a.zip");
                store.append("2.0.0", "BETA", 1, r1);
                core::BuildRecord r5 = r1;
                r5.commit = "def5678";
                store.append("2.0.0", "BETA", 5, r5);
                core::check(ctx, core::save_version_store(path, store, err),
                            "写 version.json 失败：" + err);
                core::VersionStore back;
                core::check(ctx, core::load_version_store(path, back, err),
                            "读 version.json 失败：" + err);
                core::check(ctx, back.tree["2.0.0"]["BETA"].size() == 2, "树往返后条目数不对");
                core::check(ctx, back.tree["2.0.0"]["BETA"][5].commit == "def5678",
                            "树往返后内容不对");

                // ② next=max+1（有 1 与 5 ⇒ 下一个是 6；没构建过的标记 ⇒ 1）
                core::check(ctx, back.next_index("2.0.0", "BETA") == 6,
                            "next_index 应为 max+1 = 6，实得 " +
                                std::to_string(back.next_index("2.0.0", "BETA")));
                core::check(ctx, back.next_index("2.0.0", "RC") == 1, "新标记下 next_index 应为 1");
                core::check(ctx, back.next_index("9.9.9", "BETA") == 1, "新版本下 next_index 应为 1");
                core::check(ctx, back.full_version() == "2.0.0 BETA.1",
                            "full_version 显示不对：" + back.full_version());

                // ③ 备份 → 改坏 → 还原，必须**逐字节**相同
                const std::string backup = core::version_backup_path(path);
                core::check(ctx, core::backup_version_store(path, backup, err),
                            "备份失败：" + err);
                const std::vector<char> before = slurp(path);
                core::VersionStore bad = back;
                bad.current_index = 99;
                bad.append("2.0.0", "RC", 1, r1);
                core::check(ctx, core::save_version_store(path, bad, err), "写坏版本失败：" + err);
                core::check(ctx, core::restore_version_store(backup, path, err),
                            "还原失败：" + err);
                core::check(ctx, slurp(path) == before, "还原后与备份**不是逐字节相同**");

                // ④ 带 UTF-8 BOM 的文件必须能读（且读出来内容正确）
                const std::string bom_path = core::path_to_utf8(work / "bom.json");
                {
                    std::string text = "\xEF\xBB\xBF";
                    text += "{\"current\":{\"version\":\"3.0.0\",\"prerelease\":\"RC\","
                            "\"index\":1},\"tree\":{\"3.0.0\":{\"RC\":{\"7\":{\"built\":\"t\","
                            "\"commit\":\"c\",\"artifacts\":[]}}}}}";
                    write_raw(bom_path, text);
                }
                core::VersionStore bom_store;
                core::check(ctx, core::load_version_store(bom_path, bom_store, err),
                            "带 BOM 的 version.json 读失败：" + err);
                core::check(ctx, bom_store.next_index("3.0.0", "RC") == 8,
                            "带 BOM 的文件读出的 next 不对（应为 8）");

                // ⑤ 坏文件：必须**报错**，而且**一个字节都不许改**
                const std::string bad_path = core::path_to_utf8(work / "broken.json");
                write_raw(bad_path, "{ this is not json");
                const std::vector<char> bad_before = slurp(bad_path);
                core::VersionStore ignored;
                const bool loaded = core::load_version_store(bad_path, ignored, err);
                core::check(ctx, !loaded && !err.empty(), "坏文件居然读成功了（应报错）");
                core::check(ctx, slurp(bad_path) == bad_before,
                            "坏文件被改写了（解析失败绝不许覆盖原文件）");

                std::filesystem::remove_all(work, ec);
            });
}

}  // namespace xrkit_cli
