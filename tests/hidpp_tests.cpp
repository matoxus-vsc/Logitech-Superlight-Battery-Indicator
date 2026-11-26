#include "../src/config.hpp"
#include "../src/hidpp_protocol.hpp"
#include "../src/steelseries_protocol.hpp"
#include "../src/taskbar_dock.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

int gFailures = 0;

void checkImpl(bool cond, const char* expr, int line) {
    if (!cond) {
        std::cerr << "FAIL line " << line << ": " << expr << "\n";
        ++gFailures;
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __LINE__)

void testPacketBuilders() {
    const auto shortPkt = slb::buildShortRequest(0x01, 0x05, 0x01, 0x0A, 0x10, 0x04, 0x00);
    CHECK(shortPkt[0] == slb::kReportIdShort);
    CHECK(shortPkt[1] == 0x01);
    CHECK(shortPkt[2] == 0x05);
    CHECK(shortPkt[3] == 0x1A);
    CHECK(shortPkt[4] == 0x10);
    CHECK(shortPkt[5] == 0x04);
    CHECK(shortPkt[6] == 0x00);

    const auto longPkt = slb::buildLongRequest(0xFF, 0x00, 0x00, 0x09, 0x10, 0x04, 0x00);
    CHECK(longPkt[0] == slb::kReportIdLong);
    CHECK(longPkt[1] == 0xFF);
    CHECK(longPkt[2] == 0x00);
    CHECK(longPkt[3] == 0x09);
    CHECK(longPkt[4] == 0x10);
    CHECK(longPkt[5] == 0x04);
    CHECK(longPkt[19] == 0x00);
}

void testResponseClassificationAndErrors() {
    uint8_t err = 0;

    // Matching long response
    std::array<uint8_t, 20> okResp{};
    okResp[0] = 0x11;
    okResp[1] = 0x01;
    okResp[2] = 0x07;
    okResp[3] = 0x1B;
    okResp[4] = 87;
    CHECK(slb::classifyResponse(okResp, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Success);

    // Unsolicited event (swId == 0) must be ignored
    okResp[3] = 0x10;
    CHECK(slb::classifyResponse(okResp, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Unrelated);

    // Wrong device index
    okResp[1] = 0x02;
    okResp[3] = 0x1B;
    CHECK(slb::classifyResponse(okResp, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Unrelated);

    // HID++ 1.0 ERR_RESOURCE_ERROR (0x08) -> sleeping mouse
    std::array<uint8_t, 7> err10 = {0x10, 0x01, 0x8F, 0x07, 0x1B, 0x08, 0x00};
    CHECK(slb::classifyResponse(err10, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Hidpp10Error);
    CHECK(err == 0x08);
    CHECK(slb::isDeviceAsleepError(slb::HidppResponseKind::Hidpp10Error, err));

    // HID++ 2.0 error (0x08 Busy vs 0x02 InvalidArgument)
    std::array<uint8_t, 20> err20{};
    err20[0] = 0x11;
    err20[1] = 0x01;
    err20[2] = 0xFF;
    err20[3] = 0x07;
    err20[4] = 0x1B;
    err20[5] = 0x08;
    CHECK(slb::classifyResponse(err20, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Hidpp20Error);
    CHECK(slb::isDeviceAsleepError(slb::HidppResponseKind::Hidpp20Error, err));

    err20[5] = 0x02;
    CHECK(slb::classifyResponse(err20, 0x01, 0x07, 0x01, 0x0B, err) ==
          slb::HidppResponseKind::Hidpp20Error);
    CHECK(!slb::isDeviceAsleepError(slb::HidppResponseKind::Hidpp20Error, err));
}

void testUnifiedBattery0x1004() {
    std::array<uint8_t, 20> resp{};
    resp[0] = 0x11;
    resp[1] = 0x01;
    resp[2] = 0x05;
    resp[3] = 0x19;
    resp[4] = 87;    // SoC 87%
    resp[5] = 0x04;  // Good
    resp[6] = 0x00;  // Discharging

    auto r1 = slb::parseUnifiedBatteryStatus(resp);
    CHECK(r1.has_value());
    CHECK(r1->percentage == 87);
    CHECK(!r1->charging);
    CHECK(!r1->full);

    // Charging at 64%
    resp[4] = 64;
    resp[6] = 0x01;
    auto r2 = slb::parseUnifiedBatteryStatus(resp);
    CHECK(r2.has_value());
    CHECK(r2->percentage == 64);
    CHECK(r2->charging);
    CHECK(!r2->full);

    // Charge complete (full)
    resp[4] = 100;
    resp[6] = 0x03;
    auto r3 = slb::parseUnifiedBatteryStatus(resp);
    CHECK(r3.has_value());
    CHECK(r3->percentage == 100);
    CHECK(r3->charging);
    CHECK(r3->full);

    // Level-only fallback when SoC == 0
    resp[4] = 0;
    resp[5] = 0x02;  // low level
    resp[6] = 0x00;
    auto r4 = slb::parseUnifiedBatteryStatus(resp);
    CHECK(r4.has_value());
    CHECK(r4->percentage == 20);

    // Uninitialized wake-up packet (SoC == 0 and levelMask == 0) must be ignored
    resp[4] = 0;
    resp[5] = 0x00;
    resp[6] = 0x00;
    auto r5 = slb::parseUnifiedBatteryStatus(resp);
    CHECK(!r5.has_value());
}

void testBatteryLevelStatus0x1000() {
    std::array<uint8_t, 20> resp{};
    resp[0] = 0x11;
    resp[1] = 0x01;
    resp[2] = 0x04;
    resp[3] = 0x09;
    resp[4] = 42;
    resp[5] = 30;
    resp[6] = 0x00;

    auto r1 = slb::parseBatteryLevelStatus(resp);
    CHECK(r1.has_value());
    CHECK(r1->percentage == 42);
    CHECK(!r1->charging);

    resp[4] = 75;
    resp[6] = 0x02;  // charging nearly full
    auto r2 = slb::parseBatteryLevelStatus(resp);
    CHECK(r2.has_value());
    CHECK(r2->percentage == 75);
    CHECK(r2->charging);
}

void testBatteryVoltage0x1001() {
    CHECK(slb::voltageToPercentage(4300) == 100);
    CHECK(slb::voltageToPercentage(4186) == 100);
    CHECK(slb::voltageToPercentage(3811) == 50);
    CHECK(slb::voltageToPercentage(3500) == 0);
    CHECK(slb::voltageToPercentage(3200) == 0);
    const int mid = slb::voltageToPercentage(3955);
    CHECK(mid >= 74 && mid <= 76);

    // 4000 mV = 0x0FA0, discharging (flags = 0x00)
    std::array<uint8_t, 20> resp{};
    resp[0] = 0x11;
    resp[1] = 0x01;
    resp[2] = 0x06;
    resp[3] = 0x09;
    resp[4] = 0x0F;
    resp[5] = 0xA0;
    resp[6] = 0x00;

    auto r1 = slb::parseBatteryVoltage(resp);
    CHECK(r1.has_value());
    CHECK(r1->voltageMv == 4000);
    CHECK(r1->percentage >= 81 && r1->percentage <= 82);
    CHECK(!r1->charging);

    // Charging (flags = 0x80)
    resp[6] = 0x80;
    auto r2 = slb::parseBatteryVoltage(resp);
    CHECK(r2.has_value());
    CHECK(r2->charging);
    CHECK(!r2->full);

    // Full (flags = 0x81, voltage = 4160 mV = 0x1040)
    resp[4] = 0x10;
    resp[5] = 0x40;
    resp[6] = 0x81;
    auto r3 = slb::parseBatteryVoltage(resp);
    CHECK(r3.has_value());
    CHECK(r3->charging);
    CHECK(r3->full);
    CHECK(r3->percentage == 100);

    // Invalid zero/sub-1V wake-up voltage packet must be rejected
    resp[4] = 0x00;
    resp[5] = 0x00;
    resp[6] = 0x00;
    auto r4 = slb::parseBatteryVoltage(resp);
    CHECK(!r4.has_value());
}

void testConfigRoundTripAndClamping() {
    slb::AppConfig cfg;
    cfg.pollIntervalSec = 45;
    cfg.lowBatteryThreshold = 18;
    cfg.windowX = 1620;
    cfg.windowY = -20;
    cfg.alwaysOnTop = true;
    cfg.multiDeviceMode = true;
    cfg.mockMode = false;
    cfg.dockToTaskbar = false;
    cfg.taskbarOffsetX = -32;
    cfg.lowBatteryNotify = false;

    const std::string json = slb::serializeConfigJson(cfg);
    const slb::AppConfig parsed = slb::parseConfigJson(json);
    CHECK(parsed == cfg);

    // Clamping + unknown field tolerance
    const char* raw = R"({
        "unknown_nested": {"a": [1, 2, 3]},
        "poll_interval_sec": 1,
        "low_battery_threshold": 150,
        "window_x": 100,
        "window_y": 200,
        "always_on_top": false,
        "taskbar_offset_x": -5000
    })";
    const slb::AppConfig clamped = slb::parseConfigJson(raw);
    CHECK(clamped.pollIntervalSec == 5);
    CHECK(clamped.lowBatteryThreshold == 99);
    CHECK(clamped.windowX == 100);
    CHECK(clamped.windowY == 200);
    CHECK(!clamped.alwaysOnTop);
    CHECK(clamped.dockToTaskbar);  // defaults to true when omitted
    CHECK(clamped.lowBatteryNotify);  // defaults to true when omitted
    CHECK(clamped.taskbarOffsetX == -2000);
}

void testTaskbarDockGeometry() {
    // 96 DPI, 1920x1080 screen, 48px taskbar at bottom, TrayNotifyWnd at [1700..1920]
    slb::TaskbarLayout layout;
    layout.valid = true;
    layout.dpi = 96;
    layout.taskbarHeightPx = 48;
    layout.taskbarRect = {0, 1032, 1920, 1080};
    layout.trayNotifyRect = {1700, 1032, 1920, 1080};

    int baseX = 0;
    const POINT p1 = slb::computeDockedPosition(layout, 84, 48, 0, &baseX);
    CHECK(baseX == 1700 - 6 - 84);
    CHECK(p1.x == 1610);
    CHECK(p1.y == 1032);

    // Negative X offset shifts left by 20 DIPs (20px at 96 DPI)
    const POINT p2 = slb::computeDockedPosition(layout, 84, 40, -20, &baseX);
    CHECK(p2.x == 1590);
    CHECK(p2.y == 1032 + (48 - 40) / 2);

    // 144 DPI (150% scaling), 40px taskbar -> 6 DIP gap = 9px, -20 DIP = -30px
    layout.dpi = 144;
    layout.taskbarHeightPx = 40;
    layout.taskbarRect = {0, 1040, 1920, 1080};
    layout.trayNotifyRect = {1650, 1040, 1920, 1080};
    const POINT p3 = slb::computeDockedPosition(layout, 126, 40, -20, &baseX);
    CHECK(baseX == 1650 - 9 - 126);
    CHECK(p3.x == baseX - 30);
    CHECK(p3.y == 1040);

    // Clamped to taskbar bounds when offset is extreme
    const POINT pClampedLeft = slb::computeDockedPosition(layout, 126, 40, -5000, nullptr);
    CHECK(pClampedLeft.x == 4);

    // Fallback when TrayNotifyWnd rect is empty
    layout.trayNotifyRect = {0, 0, 0, 0};
    layout.dpi = 96;
    const POINT pFallback = slb::computeDockedPosition(layout, 84, 40, 0, &baseX);
    CHECK(baseX == 1920 - 180 - 84);
    CHECK(pFallback.x == baseX);
}

void testSteelSeriesProfilesAndFiltering() {
    auto r650w = slb::findSteelSeriesWirelessProfile(0x1726);
    CHECK(r650w.has_value());
    CHECK(r650w->endpoint == 0);
    CHECK(!r650w->isWired);
    CHECK(r650w->kind == slb::SteelSeriesBatteryKind::Rival3Or650);

    auto r650c = slb::findSteelSeriesWirelessProfile(0x172B);
    CHECK(r650c.has_value());
    CHECK(r650c->endpoint == 0);
    CHECK(r650c->isWired);
    CHECK(r650c->kind == slb::SteelSeriesBatteryKind::Rival3Or650);

    auto aerox3 = slb::findSteelSeriesWirelessProfile(0x1838);
    CHECK(aerox3.has_value() && aerox3->endpoint == 3 && aerox3->command == 0xD2);
    auto aerox3g2 = slb::findSteelSeriesWirelessProfile(0x1890);
    CHECK(aerox3g2.has_value() && aerox3g2->kind == slb::SteelSeriesBatteryKind::DirectAerox);
    auto rival3 = slb::findSteelSeriesWirelessProfile(0x1830);
    CHECK(rival3.has_value() && rival3->endpoint == 3);
    auto rivalPro = slb::findSteelSeriesWirelessProfile(0x1880);
    CHECK(rivalPro.has_value() && rivalPro->endpoint == 2 && rivalPro->command == 0x92);

    CHECK(slb::isKnownNonWirelessSteelSeriesPid(0x1824));
    CHECK(slb::isKnownNonWirelessSteelSeriesPid(0x1724));
    CHECK(!slb::isKnownNonWirelessSteelSeriesPid(0x1726));

    CHECK(slb::isNonMouseSteelSeriesName(L"SteelSeries Apex Pro TKL"));
    CHECK(slb::isNonMouseSteelSeriesName(L"Arctis Nova Pro Wireless"));
    CHECK(!slb::isNonMouseSteelSeriesName(L"SteelSeries Rival 650 Wireless"));

    CHECK(slb::parseWindowsHidInterfaceNumber(L"\\\\?\\hid#vid_1038&pid_1838&mi_03&col01#...") == 3);
    CHECK(slb::parseWindowsHidInterfaceNumber(L"\\\\?\\hid#vid_1038&pid_1726&MI_00#...") == 0);
    CHECK(slb::parseWindowsHidInterfaceNumber(L"\\\\?\\hid#vid_1038&pid_1726&col02#...") == -1);
}

void testSteelSeriesDecoders() {
    // Rival 650 live-captured format: [0x5A, 0x00, 0x00, ...] -> 90% discharging
    const std::array<uint8_t, 8> r650Raw = {0x5A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto d1 = slb::decodeSteelSeriesRival3Or650(r650Raw);
    CHECK(d1.has_value() && d1->percentage == 90 && !d1->charging);

    // Windows Report-ID prefixed Rival 650 / Rival 3: [0x00, 90, 0x00, 1] -> 90% charging
    const std::array<uint8_t, 4> r650Win = {0x00, 90, 0x00, 0x01};
    auto d2 = slb::decodeSteelSeriesRival3Or650(r650Win);
    CHECK(d2.has_value() && d2->percentage == 90 && d2->charging);

    // Echo format with report ID prefix: [0x00, 0xAA, 0x01, 80, 1]
    const std::array<uint8_t, 5> rEcho = {0x00, 0xAA, 0x01, 80, 1};
    auto d3 = slb::decodeSteelSeriesRival3Or650(rEcho);
    CHECK(d3.has_value() && d3->percentage == 80 && d3->charging);

    // Zero & out-of-range rejection
    const std::array<uint8_t, 4> rZero = {0, 0, 0, 0};
    const std::array<uint8_t, 4> rInvalid = {0x00, 150, 0, 0};
    CHECK(!slb::decodeSteelSeriesRival3Or650(rZero).has_value());
    CHECK(!slb::decodeSteelSeriesRival3Or650(rInvalid).has_value());

    // Aerox / Prime 1..21 scale
    const std::array<uint8_t, 3> aerox85 = {0x00, 0xD2, 18};
    CHECK(slb::isMatchingSteelSeriesCommandResponse(aerox85, 0xD2));
    auto a1 = slb::decodeSteelSeriesAeroxPrime(aerox85);
    CHECK(a1.has_value() && a1->percentage == 85 && !a1->charging);

    const std::array<uint8_t, 3> aerox50c = {0x00, 0x92, 11 | 0x80};
    auto a2 = slb::decodeSteelSeriesAeroxPrime(aerox50c);
    CHECK(a2.has_value() && a2->percentage == 50 && a2->charging);

    // Interleaved 0x61 RGB heartbeat packet & 0xFF keyboard NACK rejection
    const std::array<uint8_t, 4> rgbPkt = {0x61, 0x00, 0x31, 0x31};
    const std::array<uint8_t, 4> nackPkt = {0x00, 0xD2, 0xFF, 0x00};
    CHECK(!slb::isMatchingSteelSeriesCommandResponse(rgbPkt, 0xD2));
    CHECK(!slb::decodeSteelSeriesAeroxPrime(nackPkt).has_value());
    CHECK(!slb::decodeSteelSeriesDirectAerox(nackPkt).has_value());

    // DirectAerox (Rival Pro / Aerox 3 Gen 2)
    const std::array<uint8_t, 6> rivalProWin = {0x00, 0x92, 0x00, 29, 0x01, 100};
    auto dp = slb::decodeSteelSeriesDirectAerox(rivalProWin);
    CHECK(dp.has_value() && dp->percentage == 29 && !dp->charging);
}

}  // namespace

int main() {
    testPacketBuilders();
    testResponseClassificationAndErrors();
    testUnifiedBattery0x1004();
    testBatteryLevelStatus0x1000();
    testBatteryVoltage0x1001();
    testConfigRoundTripAndClamping();
    testTaskbarDockGeometry();
    testSteelSeriesProfilesAndFiltering();
    testSteelSeriesDecoders();
    if (gFailures > 0) {
        std::cerr << gFailures << " check(s) failed.\n";
        return 1;
    }
    std::cout << "All hidpp_tests passed successfully.\n";
    return 0;
}
