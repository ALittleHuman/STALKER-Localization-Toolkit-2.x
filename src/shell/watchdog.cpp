#include "watchdog.h"

#include "xrkit/core/log.h"
#include "xrkit/core/theme.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace xrkit::shell {
namespace {
std::atomic<long long> g_last_beat_ms{0};
std::atomic<bool> g_started{false};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void ui_heartbeat() { g_last_beat_ms.store(now_ms()); }

void start_ui_watchdog() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    ui_heartbeat();
    std::thread([] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const long long last = g_last_beat_ms.load();
            if (last > 0 && now_ms() - last > 30000) {
                try {
                    std::ofstream log(core::app_dir() + "/logs/ui_watchdog.log",
                                      std::ios::app);
                    log << "UI-WATCHDOG timeout; process exits to avoid permanent hang\n";
                    log.flush();
                } catch (...) {
                }
                std::_Exit(1);
            }
        }
    }).detach();
}
}  // namespace xrkit::shell