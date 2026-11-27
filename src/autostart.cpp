#include "autostart.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

namespace slb {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValueName[] = L"WirelessMouseBatteryIndicator";
constexpr wchar_t kLegacyRunValueName[] = L"LogitechSuperlightBatteryIndicator";

std::wstring_view trimWhitespace(std::wstring_view sv) {
    while (!sv.empty() && (sv.front() == L' ' || sv.front() == L'\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == L' ' || sv.back() == L'\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::wstring normalizePathForComparison(std::wstring_view path) {
    path = trimWhitespace(path);
    if (path.starts_with(L"\\\\?\\")) {
        path.remove_prefix(4);
    }
    std::wstring out(path);
    for (wchar_t& ch : out) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    return out;
}

std::wstring getCurrentExePath() {
    std::vector<wchar_t> buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1) {
            return std::wstring(buf.data(), len);
        }
        buf.resize(buf.size() * 2, L'\0');
    }
}

std::wstring readRunValueString(HKEY key, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD sizeBytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &sizeBytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || sizeBytes < sizeof(wchar_t)) {
        return {};
    }

    std::vector<wchar_t> buf(sizeBytes / sizeof(wchar_t) + 1, L'\0');
    DWORD readBytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(buf.data()), &readBytes) != ERROR_SUCCESS) {
        return {};
    }

    size_t wcharCount = readBytes / sizeof(wchar_t);
    while (wcharCount > 0 && buf[wcharCount - 1] == L'\0') {
        --wcharCount;
    }
    std::wstring raw(buf.data(), wcharCount);
    if (raw.empty()) {
        return {};
    }

    if (type == REG_EXPAND_SZ) {
        const DWORD needed = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
        if (needed > 1) {
            std::wstring expanded(needed, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), needed);
            if (written > 0 && written <= needed) {
                expanded.resize(written - 1);
                return expanded;
            }
        }
    }
    return raw;
}

bool isStartupApprovedEnabledInRegistry(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedRunKeyPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return true;
    }

    DWORD type = 0;
    DWORD sizeBytes = 0;
    const LONG queryRes = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &sizeBytes);
    if (queryRes != ERROR_SUCCESS || type != REG_BINARY || sizeBytes == 0) {
        RegCloseKey(key);
        return true;
    }

    std::vector<uint8_t> blob(sizeBytes);
    DWORD readBytes = sizeBytes;
    const LONG readRes =
        RegQueryValueExW(key, valueName, nullptr, &type, blob.data(), &readBytes);
    RegCloseKey(key);
    if (readRes != ERROR_SUCCESS || readBytes == 0) {
        return true;
    }

    blob.resize(readBytes);
    return isStartupApprovedBlobEnabled(blob);
}

bool writeStartupApprovedEnabled(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kStartupApprovedRunKeyPath, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }

    const std::array<uint8_t, 12> enabledBlob = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const LONG setRes = RegSetValueExW(
        key,
        valueName,
        0,
        REG_BINARY,
        enabledBlob.data(),
        static_cast<DWORD>(enabledBlob.size()));
    RegDeleteValueW(key, kLegacyRunValueName);
    RegCloseKey(key);
    return setRes == ERROR_SUCCESS;
}

void deleteStartupApprovedValues() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedRunKeyPath, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, kRunValueName);
    RegDeleteValueW(key, kLegacyRunValueName);
    RegCloseKey(key);
}

}  // namespace

std::wstring extractExePathFromRunCommand(std::wstring_view command) {
    command = trimWhitespace(command);
    if (command.empty()) {
        return {};
    }

    if (command.front() == L'"') {
        command.remove_prefix(1);
        const size_t closeQuote = command.find(L'"');
        if (closeQuote != std::wstring_view::npos) {
            return std::wstring(trimWhitespace(command.substr(0, closeQuote)));
        }
        return std::wstring(trimWhitespace(command));
    }

    for (size_t i = 0; i + 4 <= command.size(); ++i) {
        if (command[i] == L'.' &&
            (command[i + 1] == L'e' || command[i + 1] == L'E') &&
            (command[i + 2] == L'x' || command[i + 2] == L'X') &&
            (command[i + 3] == L'e' || command[i + 3] == L'E')) {
            const size_t endPos = i + 4;
            if (endPos == command.size() || command[endPos] == L' ' || command[endPos] == L'\t') {
                return std::wstring(command.substr(0, endPos));
            }
        }
    }

    const size_t firstSpace = command.find_first_of(L" \t");
    if (firstSpace != std::wstring_view::npos) {
        return std::wstring(command.substr(0, firstSpace));
    }
    return std::wstring(command);
}

bool autostartPathsMatch(std::wstring_view a, std::wstring_view b) {
    const std::wstring na = normalizePathForComparison(a);
    const std::wstring nb = normalizePathForComparison(b);
    if (na.empty() || nb.empty() || na.size() != nb.size()) {
        return false;
    }
    return CompareStringOrdinal(
               na.data(), static_cast<int>(na.size()),
               nb.data(), static_cast<int>(nb.size()), TRUE) == CSTR_EQUAL;
}

bool isStartupApprovedBlobEnabled(std::span<const uint8_t> blob) {
    // Windows stores StartupApproved state in byte 0:
    // even values (0x02, 0x06) are enabled; odd values (0x01, 0x03, 0x07) are disabled.
    return blob.empty() || ((blob[0] & 0x01u) == 0u);
}

bool isAutostartEnabled() {
    const std::wstring currentExe = getCurrentExePath();
    if (currentExe.empty()) {
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    const std::wstring runCmd = readRunValueString(key, kRunValueName);
    RegCloseKey(key);
    if (runCmd.empty()) {
        return false;
    }

    const std::wstring registeredExe = extractExePathFromRunCommand(runCmd);
    return autostartPathsMatch(registeredExe, currentExe) &&
           isStartupApprovedEnabledInRegistry(kRunValueName);
}

bool setAutostartEnabled(bool enable) {
    HKEY key = nullptr;
    const LONG openRes = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr,
                                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                                         nullptr);
    if (openRes != ERROR_SUCCESS) {
        return false;
    }

    LONG opRes = ERROR_SUCCESS;
    if (enable) {
        const std::wstring exePath = getCurrentExePath();
        if (exePath.empty()) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring quoted = L"\"" + exePath + L"\"";
        const DWORD byteLen = static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t));
        opRes = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                               reinterpret_cast<const BYTE*>(quoted.c_str()), byteLen);
        RegDeleteValueW(key, kLegacyRunValueName);
        RegCloseKey(key);
        if (opRes != ERROR_SUCCESS) {
            return false;
        }
        return writeStartupApprovedEnabled(kRunValueName);
    }

    opRes = RegDeleteValueW(key, kRunValueName);
    if (opRes == ERROR_FILE_NOT_FOUND) {
        opRes = ERROR_SUCCESS;
    }
    const LONG legacyDel = RegDeleteValueW(key, kLegacyRunValueName);
    if (legacyDel != ERROR_SUCCESS && legacyDel != ERROR_FILE_NOT_FOUND) {
        opRes = legacyDel;
    }
    RegCloseKey(key);
    deleteStartupApprovedValues();
    return opRes == ERROR_SUCCESS;
}

bool repairStaleAutostartIfEnabled() {
    if (isAutostartEnabled()) {
        return false;
    }

    const std::wstring currentExe = getCurrentExePath();
    if (currentExe.empty()) {
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool shouldRepair = false;
    for (const wchar_t* name : {kRunValueName, kLegacyRunValueName}) {
        const std::wstring cmd = readRunValueString(key, name);
        if (cmd.empty() || !isStartupApprovedEnabledInRegistry(name)) {
            continue;
        }
        const std::wstring oldExe = extractExePathFromRunCommand(cmd);
        if (!oldExe.empty() && !autostartPathsMatch(oldExe, currentExe) &&
            GetFileAttributesW(oldExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            shouldRepair = true;
            break;
        }
    }

    RegCloseKey(key);
    if (shouldRepair) {
        return setAutostartEnabled(true);
    }
    return false;
}

}  // namespace slb
