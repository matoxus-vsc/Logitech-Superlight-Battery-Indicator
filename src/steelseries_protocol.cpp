#include "steelseries_protocol.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>

namespace slb {

namespace {

constexpr std::array<SteelSeriesMouseProfile, 25> kWirelessProfiles = {{
    // Rival 650 Wireless (2.4 GHz & Wired)
    {L"SteelSeries Rival 650 Wireless", 0x1726, 0, false, SteelSeriesBatteryKind::Rival3Or650, 0xAA},
    {L"SteelSeries Rival 650 Wireless (Wired)", 0x172B, 0, true, SteelSeriesBatteryKind::Rival3Or650, 0xAA},

    // Rival 3 Wireless (Gen 1 & Gen 2)
    {L"SteelSeries Rival 3 Wireless", 0x1830, 3, false, SteelSeriesBatteryKind::Rival3Or650, 0xAA},
    {L"SteelSeries Rival 3 Wireless Gen 2", 0x1872, 3, false, SteelSeriesBatteryKind::Rival3Or650, 0xAA},

    // Aerox 3 Wireless & Editions
    {L"SteelSeries Aerox 3 Wireless", 0x1838, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 3 Wireless (Wired)", 0x183A, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Aerox 3 Wireless CS2 Edition", 0x1878, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 3 Wireless CS2 Edition (Wired)", 0x187A, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Aerox 3 Gen 2 Wireless", 0x1890, 3, false, SteelSeriesBatteryKind::DirectAerox, 0xD2},
    {L"SteelSeries Aerox 3 Gen 2 Wireless (Wired)", 0x1892, 3, true, SteelSeriesBatteryKind::DirectAerox, 0x92},

    // Aerox 5 Wireless & Editions
    {L"SteelSeries Aerox 5 Wireless", 0x1852, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 5 Wireless (Wired)", 0x1854, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Aerox 5 Wireless Destiny 2", 0x185C, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 5 Wireless Destiny 2 (Wired)", 0x185E, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Aerox 5 Wireless Diablo IV", 0x1860, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 5 Wireless Diablo IV (Wired)", 0x1862, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},

    // Aerox 9 Wireless & Editions
    {L"SteelSeries Aerox 9 Wireless", 0x1858, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 9 Wireless (Wired)", 0x185A, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Aerox 9 Wireless WoW", 0x1874, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Aerox 9 Wireless WoW (Wired)", 0x1876, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},

    // Prime & Prime Mini Wireless
    {L"SteelSeries Prime Wireless", 0x1840, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Prime Wireless (Wired)", 0x1842, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},
    {L"SteelSeries Prime Mini Wireless", 0x1848, 3, false, SteelSeriesBatteryKind::AeroxPrime, 0xD2},
    {L"SteelSeries Prime Mini Wireless (Wired)", 0x184A, 3, true, SteelSeriesBatteryKind::AeroxPrime, 0x92},

    // Rival Pro (UWB wireless)
    {L"SteelSeries Rival Pro", 0x1880, 2, false, SteelSeriesBatteryKind::DirectAerox, 0x92},
}};

constexpr std::array<uint16_t, 36> kKnownWiredOnlyPids = {{
    0x1836, 0x1850, 0x137A, 0x1366, 0x1378, 0x182E, 0x182A, 0x1856,
    0x184D, 0x182C, 0x1824, 0x184C, 0x1870, 0x183C, 0x183E, 0x1706,
    0x1707, 0x1704, 0x1708, 0x1702, 0x170A, 0x170B, 0x170C, 0x1814,
    0x1729, 0x1816, 0x1384, 0x1392, 0x1710, 0x1712, 0x171C, 0x1394,
    0x171A, 0x1716, 0x1714, 0x1718,
}};

constexpr std::array<uint16_t, 12> kKnownWiredOnlyPidsExtra = {{
    0x1810, 0x1720, 0x171E, 0x1736, 0x170E, 0x1724, 0x172E, 0x1700,
    0x1730, 0x1722, 0x1832, 0x1834,
}};

std::optional<uint8_t> extractAeroxBatteryByte(std::span<const uint8_t> resp) {
    if (resp.size() < 2) {
        return std::nullopt;
    }
    if (resp.size() >= 4 && resp[0] == 0x00 && resp[2] == 0x00) {
        return resp[3];
    }
    if (resp.size() >= 3 && resp[1] == 0x00) {
        return resp[2];
    }
    if (resp.size() >= 3 && resp[0] == 0x00) {
        return resp[2];
    }
    return resp[1];
}

}  // namespace

std::optional<SteelSeriesMouseProfile> findSteelSeriesWirelessProfile(uint16_t productId) {
    for (const auto& prof : kWirelessProfiles) {
        if (prof.productId == productId) {
            return prof;
        }
    }
    return std::nullopt;
}

bool isKnownNonWirelessSteelSeriesPid(uint16_t productId) {
    if (std::find(kKnownWiredOnlyPids.begin(), kKnownWiredOnlyPids.end(), productId) !=
        kKnownWiredOnlyPids.end()) {
        return true;
    }
    if (std::find(kKnownWiredOnlyPidsExtra.begin(), kKnownWiredOnlyPidsExtra.end(), productId) !=
        kKnownWiredOnlyPidsExtra.end()) {
        return true;
    }
    // Sensei RAW variants
    return productId == 0x1369 || productId == 0x1362 || productId == 0x136D ||
           productId == 0x136F || productId == 0x1380 || productId == 0x1390;
}

bool isNonMouseSteelSeriesName(std::wstring_view productName) {
    if (productName.empty()) {
        return false;
    }
    std::wstring lower(productName);
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }

    constexpr std::array<std::wstring_view, 14> kNonMouseKeywords = {{
        L"apex",
        L"arctis",
        L"nova",
        L"alias",
        L"arena",
        L"stratus",
        L"nimbus",
        L"gamedac",
        L"qck",
        L"keyboard",
        L"headset",
        L"microphone",
        L"speaker",
        L"controller",
    }};

    for (std::wstring_view kw : kNonMouseKeywords) {
        if (lower.find(kw) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

int parseWindowsHidInterfaceNumber(std::wstring_view devicePath) {
    std::wstring lower(devicePath);
    for (wchar_t& ch : lower) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }

    const size_t pos = lower.find(L"mi_");
    if (pos == std::wstring::npos) {
        return -1;
    }

    size_t idx = pos + 3;
    int value = 0;
    bool hasDigit = false;
    while (idx < lower.size() && std::iswxdigit(lower[idx])) {
        hasDigit = true;
        const wchar_t c = lower[idx++];
        int digit = 0;
        if (c >= L'0' && c <= L'9') {
            digit = c - L'0';
        } else {
            digit = 10 + (c - L'a');
        }
        value = (value << 4) | digit;
    }
    return hasDigit ? value : -1;
}

bool isMatchingSteelSeriesCommandResponse(std::span<const uint8_t> resp, uint8_t command) {
    if (resp.size() < 2) {
        return false;
    }
    if (resp[0] == command) {
        return true;
    }
    return resp[0] == 0x00 && resp.size() >= 3 && resp[1] == command;
}

std::optional<BatteryReading> decodeSteelSeriesAeroxPrime(std::span<const uint8_t> resp) {
    const auto battOpt = extractAeroxBatteryByte(resp);
    if (!battOpt.has_value() || *battOpt == 0) {
        return std::nullopt;
    }
    const uint8_t battByte = *battOpt;
    const bool charging = (battByte & kSteelSeriesChargingFlag) != 0;
    const uint8_t rawVal = static_cast<uint8_t>(battByte & ~kSteelSeriesChargingFlag);
    if (rawVal == 0 || rawVal > 100) {
        return std::nullopt;
    }

    const int pct = (rawVal > 21)
                        ? static_cast<int>(rawVal)
                        : std::min(100, static_cast<int>(rawVal - 1) * 5);

    BatteryReading reading;
    reading.percentage = std::clamp(pct, 0, 100);
    reading.charging = charging;
    reading.full = (charging && reading.percentage == 100);
    return reading;
}

std::optional<BatteryReading> decodeSteelSeriesDirectAerox(std::span<const uint8_t> resp) {
    const auto battOpt = extractAeroxBatteryByte(resp);
    if (!battOpt.has_value() || *battOpt == 0) {
        return std::nullopt;
    }
    const uint8_t battByte = *battOpt;
    const bool charging = (battByte & kSteelSeriesChargingFlag) != 0;
    const uint8_t rawVal = static_cast<uint8_t>(battByte & ~kSteelSeriesChargingFlag);
    if (rawVal == 0 || rawVal > 100) {
        return std::nullopt;
    }

    BatteryReading reading;
    reading.percentage = static_cast<int>(rawVal);
    reading.charging = charging;
    reading.full = (charging && reading.percentage == 100);
    return reading;
}

std::optional<BatteryReading> decodeSteelSeriesRival3Or650(std::span<const uint8_t> resp) {
    if (resp.size() < 2) {
        return std::nullopt;
    }
    const bool allZero = std::all_of(resp.begin(), resp.end(), [](uint8_t b) { return b == 0; });
    if (allZero) {
        return std::nullopt;
    }

    uint8_t levelByte = 0;
    bool charging = false;

    if (resp[0] == 0xAA) {
        if (resp.size() >= 4 && resp[1] == 0x01) {
            levelByte = resp[2];
            charging = (resp[3] != 0);
        } else if (resp.size() >= 3) {
            levelByte = resp[1];
            charging = (resp[2] != 0);
        } else {
            return std::nullopt;
        }
    } else if (resp[0] == 0x00 && resp.size() >= 3 && resp[1] == 0xAA) {
        if (resp.size() >= 5 && resp[2] == 0x01) {
            levelByte = resp[3];
            charging = (resp[4] != 0);
        } else if (resp.size() >= 4) {
            levelByte = resp[2];
            charging = (resp[3] != 0);
        } else {
            return std::nullopt;
        }
    } else if (resp[0] == 0x00 && resp.size() >= 4) {
        levelByte = resp[1];
        charging = (resp[3] != 0);
    } else if (resp.size() >= 3) {
        levelByte = resp[0];
        charging = (resp[2] != 0);
    } else if (resp[0] == 0x00) {
        levelByte = resp[1];
        charging = false;
    } else {
        levelByte = resp[0];
        charging = false;
    }

    if (levelByte == 0 || levelByte > 100) {
        return std::nullopt;
    }

    BatteryReading reading;
    reading.percentage = static_cast<int>(levelByte);
    reading.charging = charging;
    reading.full = (charging && reading.percentage == 100);
    return reading;
}

}  // namespace slb
