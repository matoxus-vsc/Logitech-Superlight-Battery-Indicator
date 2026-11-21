#include "hidpp_protocol.hpp"

#include <algorithm>

namespace slb {

namespace {

struct VoltagePoint {
    uint16_t mv;
    int pct;
};

// Calibrated LiPo discharge curve from Logitech HID++ (hid-logitech-hidpp).
constexpr std::array<VoltagePoint, 13> kVoltageCurve = {{
    {4186, 100},
    {4067, 90},
    {3989, 80},
    {3922, 70},
    {3859, 60},
    {3811, 50},
    {3778, 40},
    {3751, 30},
    {3717, 20},
    {3671, 10},
    {3646, 5},
    {3579, 2},
    {3500, 0},
}};

}  // namespace

std::array<uint8_t, kShortReportLen> buildShortRequest(
    uint8_t devIdx,
    uint8_t featureIdx,
    uint8_t functionId,
    uint8_t swId,
    uint8_t p0,
    uint8_t p1,
    uint8_t p2) {
    std::array<uint8_t, kShortReportLen> pkt{};
    pkt[0] = kReportIdShort;
    pkt[1] = devIdx;
    pkt[2] = featureIdx;
    pkt[3] = static_cast<uint8_t>(((functionId & 0x0F) << 4) | (swId & 0x0F));
    pkt[4] = p0;
    pkt[5] = p1;
    pkt[6] = p2;
    return pkt;
}

std::array<uint8_t, kLongReportLen> buildLongRequest(
    uint8_t devIdx,
    uint8_t featureIdx,
    uint8_t functionId,
    uint8_t swId,
    uint8_t p0,
    uint8_t p1,
    uint8_t p2) {
    std::array<uint8_t, kLongReportLen> pkt{};
    pkt[0] = kReportIdLong;
    pkt[1] = devIdx;
    pkt[2] = featureIdx;
    pkt[3] = static_cast<uint8_t>(((functionId & 0x0F) << 4) | (swId & 0x0F));
    pkt[4] = p0;
    pkt[5] = p1;
    pkt[6] = p2;
    return pkt;
}

HidppResponseKind classifyResponse(
    std::span<const uint8_t> packet,
    uint8_t expectedDevIdx,
    uint8_t expectedFeatIdx,
    uint8_t expectedFn,
    uint8_t expectedSwId,
    uint8_t& outErrorCode) {
    outErrorCode = 0;
    if (packet.size() < kShortReportLen) {
        return HidppResponseKind::Unrelated;
    }
    if (packet[0] != kReportIdShort && packet[0] != kReportIdLong) {
        return HidppResponseKind::Unrelated;
    }
    if (packet[1] != expectedDevIdx) {
        return HidppResponseKind::Unrelated;
    }

    const uint8_t expectedFnSw =
        static_cast<uint8_t>(((expectedFn & 0x0F) << 4) | (expectedSwId & 0x0F));

    if (packet[2] == 0x8F && packet[3] == expectedFeatIdx && packet[4] == expectedFnSw) {
        outErrorCode = packet[5];
        return HidppResponseKind::Hidpp10Error;
    }

    if (packet[2] == 0xFF && packet[3] == expectedFeatIdx && packet[4] == expectedFnSw) {
        outErrorCode = packet[5];
        return HidppResponseKind::Hidpp20Error;
    }

    if (packet[2] == expectedFeatIdx && packet[3] == expectedFnSw) {
        return HidppResponseKind::Success;
    }

    return HidppResponseKind::Unrelated;
}

bool isDeviceAsleepError(HidppResponseKind kind, uint8_t errorCode) {
    if (kind == HidppResponseKind::Hidpp10Error) {
        // 0x01 ERR_INVALID_SUBID, 0x08 ERR_RESOURCE_ERROR, 0x09 ERR_CONNECTION_REQUEST_FAILED
        return errorCode == 0x01 || errorCode == 0x08 || errorCode == 0x09;
    }
    if (kind == HidppResponseKind::Hidpp20Error) {
        // 0x08 Busy / sleeping
        return errorCode == 0x08;
    }
    return false;
}

int voltageToPercentage(uint16_t millivolts) {
    if (millivolts >= kVoltageCurve.front().mv) {
        return 100;
    }
    if (millivolts <= kVoltageCurve.back().mv) {
        return 0;
    }
    for (size_t i = 0; i + 1 < kVoltageCurve.size(); ++i) {
        const auto& hi = kVoltageCurve[i];
        const auto& lo = kVoltageCurve[i + 1];
        if (millivolts <= hi.mv && millivolts >= lo.mv) {
            const int mvSpan = static_cast<int>(hi.mv - lo.mv);
            const int mvDelta = static_cast<int>(millivolts - lo.mv);
            const int pctSpan = hi.pct - lo.pct;
            const int interpolated = lo.pct + (mvDelta * pctSpan + mvSpan / 2) / mvSpan;
            return std::clamp(interpolated, 0, 100);
        }
    }
    return 0;
}

std::optional<BatteryReading> parseUnifiedBatteryStatus(std::span<const uint8_t> resp) {
    if (resp.size() < 7) {
        return std::nullopt;
    }
    const uint8_t soc = resp[4];
    const uint8_t levelMask = resp[5];
    const uint8_t chargeStatus = resp[6];

    // Guard against uninitialized wake-up packets returning all zeros so the UI keeps the cached value.
    if (soc == 0 && levelMask == 0) {
        return std::nullopt;
    }

    int pct = 0;
    if (soc > 0 && soc <= 100) {
        pct = soc;
    } else if (soc == 0) {
        if (levelMask & 0x08) {
            pct = 90;
        } else if (levelMask & 0x04) {
            pct = 50;
        } else if (levelMask & 0x02) {
            pct = 20;
        } else if (levelMask & 0x01) {
            pct = 5;
        } else {
            return std::nullopt;
        }
    }

    BatteryReading reading;
    reading.percentage = std::clamp(pct, 0, 100);
    reading.charging = (chargeStatus == 1 || chargeStatus == 2 || chargeStatus == 3);
    reading.full = (chargeStatus == 3);
    return reading;
}

std::optional<BatteryReading> parseBatteryLevelStatus(std::span<const uint8_t> resp) {
    if (resp.size() < 7) {
        return std::nullopt;
    }
    const uint8_t level = resp[4];
    const uint8_t status = resp[6];

    BatteryReading reading;
    reading.percentage = std::clamp<int>(level, 0, 100);
    reading.charging = (status >= 1 && status <= 4);
    reading.full = (status == 3);
    return reading;
}

std::optional<BatteryReading> parseBatteryVoltage(std::span<const uint8_t> resp) {
    if (resp.size() < 7) {
        return std::nullopt;
    }
    const uint16_t mv = static_cast<uint16_t>((static_cast<uint16_t>(resp[4]) << 8) | resp[5]);
    if (mv < 1000) {
        return std::nullopt;
    }
    const uint8_t flags = resp[6];

    BatteryReading reading;
    reading.voltageMv = mv;
    reading.percentage = voltageToPercentage(mv);

    if ((flags & 0x80) != 0) {
        const uint8_t sub = flags & 0x07;
        if (sub == 0) {
            reading.charging = true;
            reading.full = false;
        } else if (sub == 1) {
            reading.charging = true;
            reading.full = true;
            if (mv >= 4150) {
                reading.percentage = 100;
            }
        }
    }
    return reading;
}

DeviceCategory mapHidppDeviceType(uint8_t typeByte) {
    switch (typeByte) {
        case 0:
            return DeviceCategory::Keyboard;
        case 3:
            return DeviceCategory::Mouse;
        case 8:
            return DeviceCategory::Headset;
        case 11:
        case 12:
            return DeviceCategory::Controller;
        default:
            return DeviceCategory::Unknown;
    }
}

}  // namespace slb
