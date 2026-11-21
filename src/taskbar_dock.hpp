#pragma once

#include <windows.h>

namespace slb {

struct TaskbarLayout {
    HWND taskbarHwnd = nullptr;
    HWND trayNotifyHwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT taskbarRect{};
    RECT trayNotifyRect{};
    RECT monitorRect{};
    UINT dpi = 96;
    int taskbarHeightPx = 48;
    bool valid = false;
    bool autoHiddenCollapsed = false;
    bool isLightTheme = false;
};

TaskbarLayout queryTaskbarLayout();

POINT computeDockedPosition(
    const TaskbarLayout& layout,
    int widgetWidthPx,
    int widgetHeightPx,
    int offsetXDip,
    int* outBaseX = nullptr);

bool isFullscreenAppActiveOnMonitor(HMONITOR targetMonitor, HWND ignoreHwnd);

bool isTaskbarAboveWindow(HWND widgetHwnd, HWND taskbarHwnd);

}  // namespace slb
