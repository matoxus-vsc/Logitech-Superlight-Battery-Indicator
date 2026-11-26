#include "battery_poller.hpp"

namespace slb {

BatteryPoller::~BatteryPoller() {
    stop();
}

void BatteryPoller::start(HWND targetHwnd, const AppConfig& config) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targetHwnd_ = targetHwnd;
        config_ = clampConfig(config);
        running_ = true;
        stopRequested_ = false;
        refreshRequested_ = true;
        deviceChangePending_ = false;
        mockPhase_ = 0;
        hasPostedOnce_ = false;
    }
    worker_ = std::thread(&BatteryPoller::workerLoop, this);
}

void BatteryPoller::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        stopRequested_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

void BatteryPoller::requestRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        refreshRequested_ = true;
        if (config_.mockMode) {
            mockPhase_ = (mockPhase_ + 1) % 3;
        }
    }
    cv_.notify_all();
}

void BatteryPoller::requestDeviceChangeRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        deviceChangePending_ = true;
        deviceChangeAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }
    cv_.notify_all();
}

AppSnapshot BatteryPoller::getSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentSnapshot_;
}

AppSnapshot BatteryPoller::buildMockSnapshot(int phase, bool multiDeviceMode) const {
    AppSnapshot snap;
    snap.mockMode = true;
    snap.mockPhase = phase;

    snap.primaryMouse.present = true;
    snap.primaryMouse.category = DeviceCategory::Mouse;
    snap.primaryMouse.deviceName = L"Wireless Mouse (Mock)";
    snap.primaryMouse.featureUsed = kFeatureUnifiedBattery;

    switch (phase % 3) {
        case 0:
            snap.primaryMouse.percentage = 87;
            snap.primaryMouse.charging = false;
            snap.primaryMouse.connection = ConnectionType::LightspeedWireless;
            break;
        case 1:
            snap.primaryMouse.percentage = 64;
            snap.primaryMouse.charging = true;
            snap.primaryMouse.connection = ConnectionType::UsbWired;
            break;
        default:
            snap.primaryMouse.percentage = 12;
            snap.primaryMouse.charging = false;
            snap.primaryMouse.connection = ConnectionType::LightspeedWireless;
            break;
    }

    if (multiDeviceMode) {
        snap.headset.present = true;
        snap.headset.category = DeviceCategory::Headset;
        snap.headset.deviceName = L"PRO X 2 Headset (Mock)";
        snap.headset.percentage = 78;
        snap.headset.connection = ConnectionType::LightspeedWireless;

        snap.controller.present = true;
        snap.controller.category = DeviceCategory::Controller;
        snap.controller.deviceName = L"Controller (Mock)";
        snap.controller.percentage = 11;
        snap.controller.connection = ConnectionType::LightspeedWireless;

        snap.keyboard.present = true;
        snap.keyboard.category = DeviceCategory::Keyboard;
        snap.keyboard.deviceName = L"PRO X TKL (Mock)";
        snap.keyboard.percentage = 94;
        snap.keyboard.connection = ConnectionType::LightspeedWireless;
    }

    return snap;
}

void BatteryPoller::workerLoop() {
    // Low priority background polling ensures zero contention with competitive games (CS2).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    auto lastMockAdvance = std::chrono::steady_clock::now();
    bool topologyChanged = true;

    for (;;) {
        AppConfig localCfg;
        HWND hwnd = nullptr;
        int phase = 0;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopRequested_) {
                return;
            }
            localCfg = config_;
            hwnd = targetHwnd_;
            phase = mockPhase_;
        }

        AppSnapshot nextSnap;
        if (localCfg.mockMode) {
            nextSnap = buildMockSnapshot(phase, localCfg.multiDeviceMode);
        } else {
            nextSnap = hidClient_.poll(localCfg.multiDeviceMode, topologyChanged);
            const auto& logi = nextSnap.primaryMouse;
            const bool logiActiveCharging = logi.present && !logi.sleeping && logi.charging;
            if (!logiActiveCharging) {
                const DeviceBatteryState ss = steelSeriesClient_.poll(topologyChanged);
                auto rank = [](const DeviceBatteryState& s) -> int {
                    if (!s.present) {
                        return 0;
                    }
                    if (!s.sleeping && s.charging) {
                        return 5;
                    }
                    if (!s.sleeping && s.percentage >= 0) {
                        return 4;
                    }
                    if (s.sleeping && s.percentage >= 0) {
                        return 3;
                    }
                    if (!s.sleeping) {
                        return 2;
                    }
                    return 1;
                };
                if (rank(ss) > rank(logi)) {
                    nextSnap.primaryMouse = ss;
                }
            }
        }
        topologyChanged = false;

        bool shouldPost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentSnapshot_ = nextSnap;
            if (!hasPostedOnce_ || nextSnap != lastPostedSnapshot_) {
                lastPostedSnapshot_ = nextSnap;
                hasPostedOnce_ = true;
                shouldPost = true;
            }
        }

        if (shouldPost && hwnd != nullptr && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_APP_BATTERY_UPDATE, 0, 0);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (stopRequested_) {
            return;
        }
        refreshRequested_ = false;

        const auto pollWait = localCfg.mockMode
                                  ? std::chrono::seconds(4)
                                  : std::chrono::seconds(localCfg.pollIntervalSec);
        const auto wakeAt = [&]() {
            auto target = std::chrono::steady_clock::now() + pollWait;
            if (deviceChangePending_ && deviceChangeAt_ < target) {
                target = deviceChangeAt_;
            }
            return target;
        }();

        cv_.wait_until(lock, wakeAt, [&]() {
            return stopRequested_ || refreshRequested_ ||
                   (deviceChangePending_ && std::chrono::steady_clock::now() >= deviceChangeAt_);
        });

        if (stopRequested_) {
            return;
        }

        if (deviceChangePending_ && std::chrono::steady_clock::now() >= deviceChangeAt_) {
            deviceChangePending_ = false;
            topologyChanged = true;
        }

        if (config_.mockMode && !refreshRequested_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastMockAdvance >= std::chrono::seconds(4)) {
                mockPhase_ = (mockPhase_ + 1) % 3;
                lastMockAdvance = now;
            }
        } else if (config_.mockMode) {
            lastMockAdvance = std::chrono::steady_clock::now();
        }
    }
}

}  // namespace slb
