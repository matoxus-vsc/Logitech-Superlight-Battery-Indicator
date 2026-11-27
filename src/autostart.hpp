#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace slb {

std::wstring extractExePathFromRunCommand(std::wstring_view command);
bool autostartPathsMatch(std::wstring_view a, std::wstring_view b);
bool isStartupApprovedBlobEnabled(std::span<const uint8_t> blob);

bool isAutostartEnabled();
bool setAutostartEnabled(bool enable);
bool repairStaleAutostartIfEnabled();

}  // namespace slb
