#include "xrkit/engine/plugin/contract.h"

#include <cstring>

namespace xrkit::plugin {

// 生成物（bytes + LF；由门禁 [13] 的 --check 锁住，不许手改）
#include "contract_tables.inc"

int host_count() { return kHostCount; }

const SlotSpec& host(int index) {
    static const SlotSpec kFallback{"", ""};
    if (index < 0 || index >= kHostCount) return kFallback;
    return kHosts[index];
}

bool known_host(const std::string& key) { return host_label(key) != nullptr; }

const char* host_label(const std::string& key) {
    for (int i = 0; i < kHostCount; ++i) {
        if (key == kHosts[i].key) return kHosts[i].label;
    }
    return nullptr;
}

int location_count() { return kLocationCount; }

const SlotSpec& location(int index) {
    static const SlotSpec kFallback{"", ""};
    if (index < 0 || index >= kLocationCount) return kFallback;
    return kLocations[index];
}

bool known_location(const std::string& key) {
    for (int i = 0; i < kLocationCount; ++i) {
        if (key == kLocations[i].key) return true;
    }
    return false;
}

int area_count() { return kAreaCount; }

const SlotSpec& area(int index) {
    static const SlotSpec kFallback{"", ""};
    if (index < 0 || index >= kAreaCount) return kFallback;
    return kAreas[index];
}

bool known_area(const std::string& key) {
    for (int i = 0; i < kAreaCount; ++i) {
        if (key == kAreas[i].key) return true;
    }
    return false;
}

int ui_method_count() { return kUiMethodCount; }

const UiMethodSpec& ui_method(int index) {
    static const UiMethodSpec kFallback{"", ""};
    if (index < 0 || index >= kUiMethodCount) return kFallback;
    return kUiMethods[index];
}

const UiMethodSpec* find_ui_method(const std::string& name) {
    for (int i = 0; i < kUiMethodCount; ++i) {
        if (name == kUiMethods[i].name) return &kUiMethods[i];
    }
    return nullptr;
}

bool contract_ok(std::string& why) {
    why.clear();
    // ⑫12.3：契约清单读不到就**报错**，不许静默降级成"少几个方法也能跑"
    if (kHostCount != 6) {
        why = "槽位表 HOSTS 应有 6 个组件，实测 " + std::to_string(kHostCount);
        return false;
    }
    if (kLocationCount != 2) {
        why = "槽位表 LOCATIONS 应有 2 个挂载点，实测 " + std::to_string(kLocationCount);
        return false;
    }
    // ★★ 2026-10-06（审计第 4 组）：「`contract_ok()` **不校 `area` 条数**（删掉 `AREA_BOTTOM`
    //   也全绿）」。修前这里只校 HOSTS / LOCATIONS / api.ui 三个条数，**AREA_* 一个都没校**
    //   —— 10 个 area 槽位（含 `AREA_BOTTOM`）是 `docs/2.0.0-设计口径.md` 白纸黑字的契约，
    //   少一个就等于插件往那个槽位注册必然被拒（表现成"注册了却不生效"），而这条自检照样绿。
    //   条数口径：1.x 的 `AREA_*` 常量 10 个（`AREA_BOTTOM="bottom"` 是 2.x 相对 1.x 明文
    //   列出的唯一新增项，见 `docs/2.0.0-行为口径对照表.md`）。
    if (kAreaCount != 10) {
        why = "槽位表 AREAS 应有 10 个区（含 AREA_BOTTOM=\"bottom\"），实测 " +
              std::to_string(kAreaCount);
        return false;
    }
    if (kUiMethodCount != 16) {
        why = "api.ui 契约应有 16 个方法（9 造控件 + 7 改控件/取输入），实测 " +
              std::to_string(kUiMethodCount);
        return false;
    }
    for (int i = 0; i < kHostCount; ++i) {
        if (kHosts[i].key == nullptr || *kHosts[i].key == '\0') {
            why = "槽位表里有空的 host 键（下标 " + std::to_string(i) + "）";
            return false;
        }
    }
    // ★★ 2026-10-06（审计第 4 组）：area 键也逐项校空 —— 条数对而键是空串同样是坏契约
    //   （插件照空串注册，`known_area("")` 会命中第一个空槽 ⇒ 注册"成功"但落错区）。
    for (int i = 0; i < kAreaCount; ++i) {
        if (kAreas[i].key == nullptr || *kAreas[i].key == '\0') {
            why = "槽位表里有空的 area 键（下标 " + std::to_string(i) + "）";
            return false;
        }
    }
    for (int i = 0; i < kUiMethodCount; ++i) {
        if (kUiMethods[i].name == nullptr || *kUiMethods[i].name == '\0') {
            why = "api.ui 契约里有空方法名（下标 " + std::to_string(i) + "）";
            return false;
        }
    }
    return true;
}

}  // namespace xrkit::plugin
