#pragma once

#include "app_state.hpp"
#include "battery_poller.hpp"
#include "config.hpp"
#include "renderer.hpp"
#include "taskbar_dock.hpp"
#include "tray_icon.hpp"

#include <windows.h>

#include <filesystem>

namespace slb {

class WidgetWindow {
public:
    WidgetWindow(
        AppConfig config,
        std::filesystem::path configPath,
        bool cliMockOverride = false,
        bool cliMultiOverride = false,
        bool cliFloatingOverride = false);
    ~WidgetWindow();

    WidgetWindow(const WidgetWindow&) = delete;
    WidgetWindow& operator=(const WidgetWindow&) = delete;

    bool create(HINSTANCE hInstance, int nCmdShow);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK staticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK winEventProc(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    LRESULT wndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    AppConfig effectiveConfig() const;
    RenderOptions buildRenderOptions(const TaskbarLayout& layout) const;
    void renderNow(const TaskbarLayout* knownLayout = nullptr);

    void installTaskbarHooks();
    void uninstallTaskbarHooks();
    void syncTaskbarState(bool forceReposition);
    void enterFloatingMode(bool save);
    void enterDockedMode(bool save);

    void applyFloatingTopmostFlag();
    void maybeNotifyLowBattery();
    void toggleVisibility();
    void showMenuAtCursor();
    POINT resolveFloatingPosition(int widthPx, int heightPx) const;

    static WidgetWindow* sActiveInstance_;

    AppConfig config_;
    std::filesystem::path configPath_;
    bool cliMockOverride_ = false;
    bool cliMultiOverride_ = false;
    bool cliFloatingOverride_ = false;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    UINT taskbarCreatedMsg_ = 0;

    HWINEVENTHOOK foregroundHook_ = nullptr;
    HWINEVENTHOOK taskbarLocHook_ = nullptr;

    TaskbarLayout lastTaskbarLayout_{};
    SIZE renderedSize_{104, 38};
    int lastDockedBaseX_ = 0;
    int lastDockedY_ = 0;
    int dragStartLeft_ = 0;
    bool isDragging_ = false;
    bool userHidden_ = false;
    bool adjustTaskbarXMode_ = false;
    bool lowBatteryNotified_ = false;

    LayeredRenderer renderer_;
    TrayIcon trayIcon_;
    BatteryPoller poller_;
    AppSnapshot currentSnapshot_;
};

}  // namespace slb
