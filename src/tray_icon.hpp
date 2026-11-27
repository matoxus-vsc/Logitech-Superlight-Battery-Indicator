#pragma once

#include "app_state.hpp"
#include "config.hpp"

#include <windows.h>

#include <cstdint>

namespace slb {

inline constexpr UINT WM_APP_TRAY = WM_APP + 102;

inline constexpr UINT IDM_DOCK_TASKBAR = 1001;
inline constexpr UINT IDM_FLOATING_MODE = 1002;
inline constexpr UINT IDM_ADJUST_TASKBAR_X = 1003;
inline constexpr UINT IDM_RESET_TASKBAR_X = 1004;
inline constexpr UINT IDM_TOGGLE_VISIBLE = 1005;
inline constexpr UINT IDM_ALWAYS_ON_TOP = 1006;
inline constexpr UINT IDM_REFRESH = 1007;
inline constexpr UINT IDM_AUTOSTART = 1008;
inline constexpr UINT IDM_EXIT = 1009;

struct TrayMenuState {
    bool dockToTaskbar = true;
    bool adjustTaskbarX = false;
    bool canResetTaskbarX = false;
    bool widgetVisible = true;
    bool alwaysOnTop = false;
    bool autostartEnabled = false;
};

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool create(HWND hwnd, const AppSnapshot& snapshot, const AppConfig& config);
    void destroy();
    void recreate();
    bool isAdded() const { return added_; }
    void update(const AppSnapshot& snapshot, const AppConfig& config);
    void showLowBatteryNotification(const DeviceBatteryState& mouse);

    void showContextMenu(
        HWND hwnd,
        POINT screenPt,
        const TrayMenuState& state);

private:
    enum class IconTone {
        Normal,
        Charging,
        Low,
        Dimmed
    };

    HICON buildIcon(IconTone tone) const;
    IconTone toneForSnapshot(const AppSnapshot& snapshot, const AppConfig& config) const;
    std::wstring formatTooltip(const AppSnapshot& snapshot) const;

    HWND hwnd_ = nullptr;
    bool added_ = false;
    HICON currentIcon_ = nullptr;
    IconTone currentTone_ = IconTone::Dimmed;
    std::wstring currentTip_;
};

}  // namespace slb
