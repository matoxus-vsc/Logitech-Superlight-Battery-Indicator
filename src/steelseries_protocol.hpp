#pragma once

#include "hidpp_protocol.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace slb {

inline constexpr uint16_t kSteelSeriesVid = 0x1038;
inline constexpr uint8_t kSteelSeriesChargingFlag = 0x80;

enum class SteelSeriesBatteryKind {
    AeroxPrime,
    DirectAerox,
    Rival3Or650
};

struct SteelSeriesMouseProfile {
    const wchar_t* name;
    uint16_t productId;
    uint8_t endpoint;
    bool isWired;
    SteelSeriesBatteryKind kind;
    uint8_t command;
};

std::optional<SteelSeriesMouseProfile> findSteelSeriesWirelessProfile(uint16_t productId);
bool isKnownNonWirelessSteelSeriesPid(uint16_t productId);
bool isNonMouseSteelSeriesName(std::wstring_view productName);
int parseWindowsHidInterfaceNumber(std::wstring_view devicePath);

bool isMatchingSteelSeriesCommandResponse(std::span<const uint8_t> resp, uint8_t command);

std::optional<BatteryReading> decodeSteelSeriesAeroxPrime(std::span<const uint8_t> resp);
std::optional<BatteryReading> decodeSteelSeriesDirectAerox(std::span<const uint8_t> resp);
std::optional<BatteryReading> decodeSteelSeriesRival3Or650(std::span<const uint8_t> resp);

}  // namespace slb
