#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace slb {

struct AppConfig {
    int pollIntervalSec = 60;
    int lowBatteryThreshold = 15;
    int windowX = -1;
    int windowY = -1;
    bool alwaysOnTop = false;
    bool multiDeviceMode = false;
    bool mockMode = false;
    bool dockToTaskbar = true;
    int taskbarOffsetX = 0;
    bool lowBatteryNotify = true;

    bool operator==(const AppConfig&) const = default;
};

std::filesystem::path getConfigPath();
AppConfig clampConfig(AppConfig cfg);
AppConfig parseConfigJson(std::string_view json);
std::string serializeConfigJson(const AppConfig& cfg);
AppConfig loadConfig(const std::filesystem::path& path);
bool saveConfig(const std::filesystem::path& path, const AppConfig& cfg);

}  // namespace slb
