#pragma once

#include <cstdint>
#include <string>

namespace slb {

enum class DeviceCategory {
    Mouse,
    Headset,
    Controller,
    Keyboard,
    Unknown
};

enum class ConnectionType {
    None,
    LightspeedWireless,
    UsbWired
};

struct DeviceBatteryState {
    bool present = false;
    bool sleeping = false;
    bool charging = false;
    bool full = false;
    int percentage = -1;
    uint16_t voltageMv = 0;
    uint16_t featureUsed = 0;
    ConnectionType connection = ConnectionType::None;
    DeviceCategory category = DeviceCategory::Mouse;
    std::wstring deviceName;

    bool operator==(const DeviceBatteryState&) const = default;
};

struct AppSnapshot {
    DeviceBatteryState primaryMouse;
    DeviceBatteryState headset;
    DeviceBatteryState controller;
    DeviceBatteryState keyboard;
    bool mockMode = false;
    int mockPhase = 0;

    bool operator==(const AppSnapshot&) const = default;
};

}  // namespace slb
