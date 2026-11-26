#include "config.hpp"
#include "hid_device.hpp"
#include "steelseries_device.hpp"
#include "widget_window.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

namespace {

struct CliOptions {
    bool mock = false;
    bool multi = false;
    bool floating = false;
    bool probe = false;
};

CliOptions parseCliOptions() {
    CliOptions opts;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return opts;
    }
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--mock") {
            opts.mock = true;
        } else if (arg == L"--multi") {
            opts.multi = true;
        } else if (arg == L"--floating") {
            opts.floating = true;
        } else if (arg == L"--probe") {
            opts.probe = true;
        }
    }
    LocalFree(argv);
    return opts;
}

void writeProbeText(const std::wstring& text) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD fileType = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE)
                               ? GetFileType(hOut)
                               : FILE_TYPE_UNKNOWN;

    if (fileType == FILE_TYPE_PIPE || fileType == FILE_TYPE_DISK) {
        const int utf8Len = WideCharToMultiByte(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string utf8(static_cast<size_t>(utf8Len), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                utf8.data(),
                utf8Len,
                nullptr,
                nullptr);
            DWORD written = 0;
            WriteFile(hOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        return;
    }

    bool allocated = false;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (AllocConsole()) {
            allocated = true;
        }
    }
    HANDLE consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (consoleOut != nullptr && consoleOut != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleW(consoleOut, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
    if (allocated) {
        MessageBoxW(nullptr, text.c_str(), L"Superlight HID++ Probe", MB_OK | MB_ICONINFORMATION);
    }
}

}  // namespace

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE /*hPrevInstance*/,
    LPWSTR /*lpCmdLine*/,
    int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const CliOptions cli = parseCliOptions();

    if (cli.probe) {
        std::wostringstream oss;
        slb::runHardwareProbe(oss);
        slb::runSteelSeriesHardwareProbe(oss);
        writeProbeText(oss.str());
        return 0;
    }

    const wchar_t* mutexName = cli.mock
                                   ? L"Local\\LogitechSuperlightBatteryIndicator_Mock_Mutex"
                                   : L"Local\\LogitechSuperlightBatteryIndicator_Mutex";
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, mutexName);
    if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        return 0;
    }

    const std::filesystem::path configPath = slb::getConfigPath();
    slb::AppConfig config = slb::loadConfig(configPath);
    if (!std::filesystem::exists(configPath)) {
        slb::saveConfig(configPath, config);
    }

    slb::WidgetWindow widget(config, configPath, cli.mock, cli.multi, cli.floating);
    if (!widget.create(hInstance, nCmdShow)) {
        if (instanceMutex) {
            CloseHandle(instanceMutex);
        }
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (instanceMutex) {
        CloseHandle(instanceMutex);
    }
    return static_cast<int>(msg.wParam);
}
