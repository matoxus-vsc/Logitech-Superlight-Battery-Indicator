#pragma once

#include "app_state.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace slb {

inline constexpr uint16_t kLogitechVid = 0x046D;
inline constexpr uint16_t kUsagePageHidpp = 0xFF00;
inline constexpr uint16_t kUsagePageHidppAlt = 0xFF43;

inline constexpr uint8_t kReportIdShort = 0x10;
inline constexpr uint8_t kReportIdLong = 0x11;
inline constexpr size_t kShortReportLen = 7;
inline constexpr size_t kLongReportLen = 20;

inline constexpr uint8_t kDevIdxDirectUsb = 0xFF;
inline constexpr uint8_t kDevIdxDirectUsbAlt = 0x00;
inline constexpr uint8_t kDevIdxReceiverSlot1 = 0x01;
inline constexpr uint8_t kDevIdxMaxReceiverSlot = 0x06;

inline constexpr uint16_t kFeatureIRoot = 0x0000;
inline constexpr uint16_t kFeatureIFeatureSet = 0x0001;
inline constexpr uint16_t kFeatureDeviceNameType = 0x0005;
inline constexpr uint16_t kFeatureBatteryLevelStatus = 0x1000;
inline constexpr uint16_t kFeatureBatteryVoltage = 0x1001;
inline constexpr uint16_t kFeatureUnifiedBattery = 0x1004;

enum class HidppResponseKind {
    Unrelated,
    Success,
    Hidpp10Error,
    Hidpp20Error
};

struct BatteryReading {
    int percentage = 0;
    bool charging = false;
    bool full = false;
    uint16_t voltageMv = 0;
};

std::array<uint8_t, kShortReportLen> buildShortRequest(
    uint8_t devIdx,
    uint8_t featureIdx,
    uint8_t functionId,
    uint8_t swId,
    uint8_t p0 = 0,
    uint8_t p1 = 0,
    uint8_t p2 = 0);

std::array<uint8_t, kLongReportLen> buildLongRequest(
    uint8_t devIdx,
    uint8_t featureIdx,
    uint8_t functionId,
    uint8_t swId,
    uint8_t p0 = 0,
    uint8_t p1 = 0,
    uint8_t p2 = 0);

HidppResponseKind classifyResponse(
    std::span<const uint8_t> packet,
    uint8_t expectedDevIdx,
    uint8_t expectedFeatIdx,
    uint8_t expectedFn,
    uint8_t expectedSwId,
    uint8_t& outErrorCode);

bool isDeviceAsleepError(HidppResponseKind kind, uint8_t errorCode);

int voltageToPercentage(uint16_t millivolts);

std::optional<BatteryReading> parseUnifiedBatteryStatus(std::span<const uint8_t> resp);
std::optional<BatteryReading> parseBatteryLevelStatus(std::span<const uint8_t> resp);
std::optional<BatteryReading> parseBatteryVoltage(std::span<const uint8_t> resp);

DeviceCategory mapHidppDeviceType(uint8_t typeByte);

}  // namespace slb
