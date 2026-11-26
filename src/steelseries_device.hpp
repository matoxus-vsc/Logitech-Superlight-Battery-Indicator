#pragma once

#include "app_state.hpp"
#include "steelseries_protocol.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace slb {

struct SteelSeriesInterfaceInfo {
    std::wstring devicePath;
    std::wstring productString;
    uint16_t productId = 0;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    int interfaceNumber = -1;
    uint16_t inputReportLen = 0;
    uint16_t outputReportLen = 0;
    uint16_t featureReportLen = 0;
};

std::vector<SteelSeriesInterfaceInfo> enumerateSteelSeriesVendorInterfaces();

class SteelSeriesHidClient {
public:
    SteelSeriesHidClient();
    ~SteelSeriesHidClient();

    SteelSeriesHidClient(const SteelSeriesHidClient&) = delete;
    SteelSeriesHidClient& operator=(const SteelSeriesHidClient&) = delete;

    DeviceBatteryState poll(bool topologyChanged);

    friend void runSteelSeriesHardwareProbe(std::wostream& out);

private:
    struct CachedDevice {
        SteelSeriesInterfaceInfo iface;
        std::wstring deviceName;
        SteelSeriesBatteryKind kind = SteelSeriesBatteryKind::Rival3Or650;
        uint8_t command = 0xAA;
        bool isWired = false;
    };

    void ensureEvents();

    std::optional<BatteryReading> queryInterface(
        void* handle,
        const SteelSeriesInterfaceInfo& iface,
        SteelSeriesBatteryKind kind,
        uint8_t command);

    std::optional<BatteryReading> queryGenericFallback(
        void* handle,
        const SteelSeriesInterfaceInfo& iface,
        SteelSeriesBatteryKind& outKind,
        uint8_t& outCommand);

    void* writeEvent_ = nullptr;
    void* readEvent_ = nullptr;

    std::optional<CachedDevice> cachedDevice_;
    std::optional<DeviceBatteryState> lastKnownState_;
    uint16_t lastKnownPid_ = 0;
};

void runSteelSeriesHardwareProbe(std::wostream& out);

}  // namespace slb
