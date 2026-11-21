#include "taskbar_dock.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace slb {

namespace {

bool isShellDesktopClass(std::wstring_view cls) {
    return cls == L"Progman" || cls == L"WorkerW" ||
           cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd";
}

bool readSystemUsesLightTheme() {
    DWORD value = 0;
    DWORD valueSize = sizeof(value);
    const LONG res = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &valueSize);
    return res == ERROR_SUCCESS && value == 1;
}

}  // namespace

TaskbarLayout queryTaskbarLayout() {
    TaskbarLayout layout;
    layout.taskbarHwnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!layout.taskbarHwnd || !IsWindow(layout.taskbarHwnd)) {
        return layout;
    }

    if (!GetWindowRect(layout.taskbarHwnd, &layout.taskbarRect)) {
        return layout;
    }

    layout.trayNotifyHwnd = FindWindowExW(layout.taskbarHwnd, nullptr, L"TrayNotifyWnd", nullptr);
    if (layout.trayNotifyHwnd && IsWindow(layout.trayNotifyHwnd)) {
        GetWindowRect(layout.trayNotifyHwnd, &layout.trayNotifyRect);
    }

    layout.monitor = MonitorFromWindow(layout.taskbarHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (layout.monitor && GetMonitorInfoW(layout.monitor, &mi)) {
        layout.monitorRect = mi.rcMonitor;
    } else {
        layout.monitorRect = layout.taskbarRect;
    }

    layout.dpi = GetDpiForWindow(layout.taskbarHwnd);
    if (layout.dpi == 0) {
        layout.dpi = 96;
    }

    layout.taskbarHeightPx = std::max(
        24, static_cast<int>(layout.taskbarRect.bottom - layout.taskbarRect.top));

    const LONG overlapTop = std::max(layout.taskbarRect.top, layout.monitorRect.top);
    const LONG overlapBot = std::min(layout.taskbarRect.bottom, layout.monitorRect.bottom);
    const LONG visibleHeight = overlapBot - overlapTop;

    layout.autoHiddenCollapsed = (!IsWindowVisible(layout.taskbarHwnd)) || (visibleHeight < 16);
    layout.isLightTheme = readSystemUsesLightTheme();
    layout.valid = (layout.taskbarRect.right > layout.taskbarRect.left);
    return layout;
}

POINT computeDockedPosition(
    const TaskbarLayout& layout,
    int widgetWidthPx,
    int widgetHeightPx,
    int offsetXDip,
    int* outBaseX) {
    const int dpi = static_cast<int>(layout.dpi > 0 ? layout.dpi : 96);
    const int gapPx = MulDiv(6, dpi, 96);

    const bool trayValid =
        layout.trayNotifyRect.right > layout.trayNotifyRect.left &&
        layout.trayNotifyRect.left >= layout.taskbarRect.left &&
        layout.trayNotifyRect.left <= layout.taskbarRect.right;

    int baseX = 0;
    if (trayValid) {
        baseX = static_cast<int>(layout.trayNotifyRect.left) - gapPx - widgetWidthPx;
    } else {
        baseX = static_cast<int>(layout.taskbarRect.right) -
                MulDiv(180, dpi, 96) - widgetWidthPx;
    }

    if (outBaseX != nullptr) {
        *outBaseX = baseX;
    }

    const int offsetPx = MulDiv(offsetXDip, dpi, 96);
    const int minX = static_cast<int>(layout.taskbarRect.left) + 4;
    const int maxX = std::max(minX, static_cast<int>(layout.taskbarRect.right) - widgetWidthPx - 4);
    const int x = std::clamp(baseX + offsetPx, minX, maxX);
    const int y = static_cast<int>(layout.taskbarRect.top) +
                  (layout.taskbarHeightPx - widgetHeightPx) / 2;

    return {x, y};
}

bool isFullscreenAppActiveOnMonitor(HMONITOR targetMonitor, HWND ignoreHwnd) {
    const HWND fg = GetForegroundWindow();

    QUERY_USER_NOTIFICATION_STATE qstate = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&qstate)) &&
        qstate == QUNS_RUNNING_D3D_FULL_SCREEN) {
        if (fg == nullptr || targetMonitor == nullptr ||
            MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == targetMonitor) {
            return true;
        }
    }

    if (!fg || fg == ignoreHwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) {
        return false;
    }
    if (!IsWindowVisible(fg) || IsIconic(fg)) {
        return false;
    }

    std::array<wchar_t, 64> cls{};
    const int clsLen = GetClassNameW(fg, cls.data(), static_cast<int>(cls.size()));
    if (clsLen > 0 && isShellDesktopClass(std::wstring_view(cls.data(), static_cast<size_t>(clsLen)))) {
        return false;
    }

    HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (targetMonitor != nullptr && fgMon != targetMonitor) {
        return false;
    }

    RECT fgRect{};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetWindowRect(fg, &fgRect) || !GetMonitorInfoW(fgMon, &mi)) {
        return false;
    }

    return fgRect.left <= mi.rcMonitor.left &&
           fgRect.top <= mi.rcMonitor.top &&
           fgRect.right >= mi.rcMonitor.right &&
           fgRect.bottom >= mi.rcMonitor.bottom;
}

bool isTaskbarAboveWindow(HWND widgetHwnd, HWND taskbarHwnd) {
    if (!widgetHwnd || !taskbarHwnd || widgetHwnd == taskbarHwnd) {
        return false;
    }
    HWND cur = widgetHwnd;
    for (int step = 0; step < 64; ++step) {
        cur = GetWindow(cur, GW_HWNDPREV);
        if (!cur) {
            return false;
        }
        if (cur == taskbarHwnd) {
            return true;
        }
    }
    return false;
}

}  // namespace slb
