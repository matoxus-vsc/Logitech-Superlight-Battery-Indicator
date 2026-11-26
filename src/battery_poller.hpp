#pragma once

#include "app_state.hpp"
#include "config.hpp"
#include "hid_device.hpp"
#include "steelseries_device.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace slb {

inline constexpr UINT WM_APP_BATTERY_UPDATE = WM_APP + 101;

class BatteryPoller {
public:
    BatteryPoller() = default;
    ~BatteryPoller();

    BatteryPoller(const BatteryPoller&) = delete;
    BatteryPoller& operator=(const BatteryPoller&) = delete;

    void start(HWND targetHwnd, const AppConfig& config);
    void stop();

    void requestRefresh();
    void requestDeviceChangeRefresh();

    AppSnapshot getSnapshot() const;

private:
    void workerLoop();
    AppSnapshot buildMockSnapshot(int phase, bool multiDeviceMode) const;

    HWND targetHwnd_ = nullptr;
    AppConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    bool stopRequested_ = false;
    bool refreshRequested_ = false;
    bool deviceChangePending_ = false;
    std::chrono::steady_clock::time_point deviceChangeAt_{};

    int mockPhase_ = 0;
    AppSnapshot currentSnapshot_;
    AppSnapshot lastPostedSnapshot_;
    bool hasPostedOnce_ = false;

    std::thread worker_;
    SuperlightHidClient hidClient_;
    SteelSeriesHidClient steelSeriesClient_;
};

}  // namespace slb
