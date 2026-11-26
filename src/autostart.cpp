#include "autostart.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace slb {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"WirelessMouseBatteryIndicator";
constexpr wchar_t kLegacyRunValueName[] = L"LogitechSuperlightBatteryIndicator";

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

bool hasRunValue(HKEY key, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD sizeBytes = 0;
    const LONG res = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &sizeBytes);
    return res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
           sizeBytes > sizeof(wchar_t);
}

}  // namespace

bool isAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    const bool enabled = hasRunValue(key, kRunValueName) || hasRunValue(key, kLegacyRunValueName);
    RegCloseKey(key);
    return enabled;
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
    } else {
        opRes = RegDeleteValueW(key, kRunValueName);
        if (opRes == ERROR_FILE_NOT_FOUND) {
            opRes = ERROR_SUCCESS;
        }
        const LONG legacyDel = RegDeleteValueW(key, kLegacyRunValueName);
        if (legacyDel != ERROR_SUCCESS && legacyDel != ERROR_FILE_NOT_FOUND) {
            opRes = legacyDel;
        }
    }

    RegCloseKey(key);
    return opRes == ERROR_SUCCESS;
}

}  // namespace slb
