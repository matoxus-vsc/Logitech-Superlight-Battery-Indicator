#pragma once

#include "app_state.hpp"
#include "hidpp_protocol.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace slb {

struct HidInterfaceInfo {
    std::wstring devicePath;
    std::wstring parentKey;
    std::wstring productString;
    uint16_t productId = 0;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint16_t inputReportLen = 0;
    uint16_t outputReportLen = 0;
    bool supportsLong = false;
    bool supportsShort = false;
};

std::vector<HidInterfaceInfo> enumerateLogitechHidppInterfaces();

class SuperlightHidClient {
public:
    SuperlightHidClient();
    ~SuperlightHidClient();

    SuperlightHidClient(const SuperlightHidClient&) = delete;
    SuperlightHidClient& operator=(const SuperlightHidClient&) = delete;

    AppSnapshot poll(bool multiDeviceMode, bool topologyChanged);

private:
    struct CachedDevice {
        HidInterfaceInfo iface;
        bool isReceiver = false;
        uint8_t deviceIndex = 0;
        uint16_t batteryFeatureId = 0;
        uint8_t batteryFeatureIndex = 0;
        DeviceCategory category = DeviceCategory::Unknown;
        std::wstring deviceName;
        ConnectionType connection = ConnectionType::None;
        DeviceBatteryState lastState;
    };

    void ensureEvents();
    uint8_t nextSwId();

    std::optional<CachedDevice> discoverOnOpenHandle(
        void* handle,
        const HidInterfaceInfo& iface,
        uint8_t devIdx,
        bool isReceiver);
    std::optional<CachedDevice> probeDirectUsbDevice(
        const HidInterfaceInfo& iface,
        bool& outIsReceiverHint);
    std::vector<CachedDevice> probeReceiverSlots(
        const HidInterfaceInfo& iface,
        bool multiDeviceMode);

    bool refreshCachedDevice(CachedDevice& dev);
    void discoverAllDevices(
        const std::vector<HidInterfaceInfo>& interfaces,
        bool multiDeviceMode);

    void* writeEvent_ = nullptr;
    void* readEvent_ = nullptr;
    uint8_t swId_ = 0x09;

    std::optional<CachedDevice> primaryMouse_;
    std::optional<CachedDevice> cachedHeadset_;
    std::optional<CachedDevice> cachedController_;
    std::optional<CachedDevice> cachedKeyboard_;

    std::set<std::wstring> knownNonWiredParentKeys_;
    DeviceBatteryState lastKnownMouseState_;
};

void runHardwareProbe(std::wostream& out);

}  // namespace slb
