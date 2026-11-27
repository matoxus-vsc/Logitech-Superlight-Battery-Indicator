#include "widget_window.hpp"

#include "autostart.hpp"

#include <dbt.h>
#include <windowsx.h>

#include <algorithm>

namespace slb {

namespace {

constexpr wchar_t kWindowClassName[] = L"WirelessMouseBatteryWidgetWnd";

constexpr UINT WM_APP_FOREGROUND_CHANGED = WM_APP + 103;
constexpr UINT WM_APP_TASKBAR_MOVED = WM_APP + 104;

constexpr UINT_PTR kTimerTaskbarSync = 1;
constexpr UINT_PTR kTimerReposition = 2;

}  // namespace

WidgetWindow* WidgetWindow::sActiveInstance_ = nullptr;

WidgetWindow::WidgetWindow(
    AppConfig config,
    std::filesystem::path configPath,
    bool cliMockOverride,
    bool cliMultiOverride,
    bool cliFloatingOverride)
    : config_(clampConfig(config)),
      configPath_(std::move(configPath)),
      cliMockOverride_(cliMockOverride),
      cliMultiOverride_(cliMultiOverride),
      cliFloatingOverride_(cliFloatingOverride) {}

WidgetWindow::~WidgetWindow() {
    uninstallTaskbarHooks();
    if (sActiveInstance_ == this) {
        sActiveInstance_ = nullptr;
    }
    poller_.stop();
    trayIcon_.destroy();
}

AppConfig WidgetWindow::effectiveConfig() const {
    AppConfig eff = config_;
    if (cliMockOverride_) {
        eff.mockMode = true;
    }
    if (cliMultiOverride_) {
        eff.multiDeviceMode = true;
    }
    if (cliFloatingOverride_) {
        eff.dockToTaskbar = false;
    }
    return eff;
}

RenderOptions WidgetWindow::buildRenderOptions(const TaskbarLayout& layout) const {
    RenderOptions opts;
    const AppConfig eff = effectiveConfig();
    opts.dockedToTaskbar = eff.dockToTaskbar;
    opts.taskbarHeightPx = layout.valid ? layout.taskbarHeightPx : 0;
    opts.lightTaskbarTheme = layout.isLightTheme;
    opts.showAdjustHighlight = eff.dockToTaskbar && adjustTaskbarXMode_;
    return opts;
}

bool WidgetWindow::create(HINSTANCE hInstance, int /*nCmdShow*/) {
    if (!renderer_.initialize()) {
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WidgetWindow::staticWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    const AppConfig effCfg = effectiveConfig();
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    if (!effCfg.dockToTaskbar && effCfg.alwaysOnTop) {
        exStyle |= WS_EX_TOPMOST;
    }

    sActiveInstance_ = this;
    hwnd_ = CreateWindowExW(
        exStyle,
        kWindowClassName,
        L"Wireless Mouse Battery Indicator",
        WS_POPUP,
        0,
        0,
        104,
        38,
        nullptr,
        nullptr,
        hInstance,
        this);

    if (!hwnd_) {
        sActiveInstance_ = nullptr;
        return false;
    }

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }

    trayIcon_.create(hwnd_, currentSnapshot_, effCfg);

    if (effCfg.dockToTaskbar) {
        enterDockedMode(false);
    } else {
        enterFloatingMode(false);
    }

    poller_.start(hwnd_, effCfg);
    return true;
}

POINT WidgetWindow::resolveFloatingPosition(int widthPx, int heightPx) const {
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    if (config_.windowX != -1 || config_.windowY != -1) {
        RECT candidate = {
            config_.windowX,
            config_.windowY,
            config_.windowX + widthPx,
            config_.windowY + heightPx,
        };
        if (MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != nullptr) {
            return {config_.windowX, config_.windowY};
        }
    }

    const int margin = MulDiv(18, static_cast<int>(dpi_), 96);
    return {
        workArea.right - widthPx - margin,
        workArea.bottom - heightPx - margin,
    };
}

void WidgetWindow::renderNow(const TaskbarLayout* knownLayout) {
    if (!hwnd_) {
        return;
    }
    const AppConfig effCfg = effectiveConfig();
    TaskbarLayout layout;
    if (effCfg.dockToTaskbar) {
        layout = knownLayout ? *knownLayout : queryTaskbarLayout();
        if (layout.valid && layout.dpi > 0) {
            dpi_ = layout.dpi;
        }
        lastTaskbarLayout_ = layout;
    }
    renderer_.renderToLayeredWindow(
        hwnd_, currentSnapshot_, effCfg, buildRenderOptions(layout), dpi_, &renderedSize_);
}

void WidgetWindow::installTaskbarHooks() {
    uninstallTaskbarHooks();
    if (!hwnd_) {
        return;
    }

    foregroundHook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &WidgetWindow::winEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    const TaskbarLayout layout = queryTaskbarLayout();
    lastTaskbarLayout_ = layout;
    if (layout.taskbarHwnd) {
        DWORD explorerPid = 0;
        GetWindowThreadProcessId(layout.taskbarHwnd, &explorerPid);
        if (explorerPid != 0) {
            taskbarLocHook_ = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE,
                EVENT_OBJECT_LOCATIONCHANGE,
                nullptr,
                &WidgetWindow::winEventProc,
                explorerPid,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
    }

    SetTimer(hwnd_, kTimerTaskbarSync, 1000, nullptr);
}

void WidgetWindow::uninstallTaskbarHooks() {
    if (hwnd_) {
        KillTimer(hwnd_, kTimerTaskbarSync);
        KillTimer(hwnd_, kTimerReposition);
    }
    if (foregroundHook_) {
        UnhookWinEvent(foregroundHook_);
        foregroundHook_ = nullptr;
    }
    if (taskbarLocHook_) {
        UnhookWinEvent(taskbarLocHook_);
        taskbarLocHook_ = nullptr;
    }
}

void WidgetWindow::syncTaskbarState(bool forceReposition) {
    if (!hwnd_ || !effectiveConfig().dockToTaskbar) {
        return;
    }

    if (!trayIcon_.isAdded()) {
        trayIcon_.recreate();
    }

    const TaskbarLayout layout = queryTaskbarLayout();
    const bool themeOrMetricsChanged =
        (layout.valid != lastTaskbarLayout_.valid) ||
        (layout.dpi != lastTaskbarLayout_.dpi) ||
        (layout.taskbarHeightPx != lastTaskbarLayout_.taskbarHeightPx) ||
        (layout.isLightTheme != lastTaskbarLayout_.isLightTheme);

    lastTaskbarLayout_ = layout;

    if (layout.valid && layout.taskbarHwnd != nullptr && taskbarLocHook_ == nullptr) {
        DWORD explorerPid = 0;
        GetWindowThreadProcessId(layout.taskbarHwnd, &explorerPid);
        if (explorerPid != 0) {
            taskbarLocHook_ = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE,
                EVENT_OBJECT_LOCATIONCHANGE,
                nullptr,
                &WidgetWindow::winEventProc,
                explorerPid,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
    }

    if (!layout.valid || userHidden_ || layout.autoHiddenCollapsed ||
        isFullscreenAppActiveOnMonitor(layout.monitor, hwnd_)) {
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
        }
        return;
    }

    if (themeOrMetricsChanged || forceReposition) {
        renderNow(&layout);
    }

    if (isDragging_) {
        return;
    }

    int baseX = 0;
    const POINT pos = computeDockedPosition(
        layout,
        renderedSize_.cx,
        renderedSize_.cy,
        config_.taskbarOffsetX,
        &baseX);
    lastDockedBaseX_ = baseX;
    lastDockedY_ = pos.y;

    RECT currentRc{};
    GetWindowRect(hwnd_, &currentRc);
    const bool posOrSizeChanged =
        (currentRc.left != pos.x) ||
        (currentRc.top != pos.y) ||
        ((currentRc.right - currentRc.left) != renderedSize_.cx) ||
        ((currentRc.bottom - currentRc.top) != renderedSize_.cy);
    const bool wasHidden = !IsWindowVisible(hwnd_);
    const bool zOrderCovered = isTaskbarAboveWindow(hwnd_, layout.taskbarHwnd);

    if (wasHidden || posOrSizeChanged || zOrderCovered || forceReposition) {
        SetWindowPos(
            hwnd_,
            HWND_TOPMOST,
            pos.x,
            pos.y,
            renderedSize_.cx,
            renderedSize_.cy,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    maybeNotifyLowBattery();
}

void WidgetWindow::maybeNotifyLowBattery() {
    if (!hwnd_ || currentSnapshot_.mockMode) {
        return;
    }
    const auto& m = currentSnapshot_.primaryMouse;
    if (m.charging || (m.percentage >= 0 && m.percentage > config_.lowBatteryThreshold + 5)) {
        lowBatteryNotified_ = false;
        return;
    }
    if (!config_.lowBatteryNotify || lowBatteryNotified_ || !m.present || m.charging ||
        m.sleeping || m.percentage < 0 || m.percentage > config_.lowBatteryThreshold) {
        return;
    }

    const HMONITOR mon = (effectiveConfig().dockToTaskbar && lastTaskbarLayout_.valid)
                             ? lastTaskbarLayout_.monitor
                             : MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (isFullscreenAppActiveOnMonitor(mon, hwnd_)) {
        return;
    }

    trayIcon_.showLowBatteryNotification(m);
    lowBatteryNotified_ = true;
}

void WidgetWindow::enterFloatingMode(bool save) {
    adjustTaskbarXMode_ = false;
    userHidden_ = false;
    if (save) {
        cliFloatingOverride_ = false;
        config_.dockToTaskbar = false;
        saveConfig(configPath_, config_);
    }

    uninstallTaskbarHooks();

    dpi_ = GetDpiForWindow(hwnd_);
    if (dpi_ == 0) {
        dpi_ = 96;
    }

    renderNow();
    const POINT pos = resolveFloatingPosition(renderedSize_.cx, renderedSize_.cy);
    SetWindowPos(
        hwnd_,
        config_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        pos.x,
        pos.y,
        renderedSize_.cx,
        renderedSize_.cy,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void WidgetWindow::enterDockedMode(bool save) {
    userHidden_ = false;
    if (save) {
        cliFloatingOverride_ = false;
        config_.dockToTaskbar = true;
        saveConfig(configPath_, config_);
    }

    installTaskbarHooks();
    renderNow();
    syncTaskbarState(true);
}

void WidgetWindow::applyFloatingTopmostFlag() {
    if (!hwnd_ || effectiveConfig().dockToTaskbar) {
        return;
    }
    SetWindowPos(
        hwnd_,
        config_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WidgetWindow::toggleVisibility() {
    if (!hwnd_) {
        return;
    }
    if (effectiveConfig().dockToTaskbar) {
        userHidden_ = !userHidden_;
        syncTaskbarState(true);
        return;
    }
    if (IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    } else {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        renderNow();
    }
}

void WidgetWindow::showMenuAtCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    const AppConfig eff = effectiveConfig();
    TrayMenuState state;
    state.dockToTaskbar = eff.dockToTaskbar;
    state.adjustTaskbarX = adjustTaskbarXMode_;
    state.canResetTaskbarX = (config_.taskbarOffsetX != 0);
    state.widgetVisible = eff.dockToTaskbar ? !userHidden_ : (IsWindowVisible(hwnd_) != FALSE);
    state.alwaysOnTop = config_.alwaysOnTop;
    state.autostartEnabled = isAutostartEnabled();
    trayIcon_.showContextMenu(hwnd_, pt, state);
}

void CALLBACK WidgetWindow::winEventProc(
    HWINEVENTHOOK /*hook*/,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD /*idEventThread*/,
    DWORD /*dwmsEventTime*/) {
    WidgetWindow* self = sActiveInstance_;
    if (!self || !self->hwnd_) {
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        PostMessageW(self->hwnd_, WM_APP_FOREGROUND_CHANGED, 0, 0);
        return;
    }

    if (event == EVENT_OBJECT_LOCATIONCHANGE &&
        idObject == OBJID_WINDOW &&
        idChild == CHILDID_SELF &&
        hwnd != nullptr &&
        (hwnd == self->lastTaskbarLayout_.taskbarHwnd ||
         hwnd == self->lastTaskbarLayout_.trayNotifyHwnd)) {
        PostMessageW(self->hwnd_, WM_APP_TASKBAR_MOVED, 0, 0);
    }
}

LRESULT CALLBACK WidgetWindow::staticWndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam) {
    WidgetWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WidgetWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<WidgetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->wndProc(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WidgetWindow::wndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        trayIcon_.recreate();
        if (effectiveConfig().dockToTaskbar) {
            installTaskbarHooks();
            syncTaskbarState(true);
        }
        return 0;
    }

    switch (msg) {
        case WM_NCHITTEST:
            return HTCAPTION;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCLBUTTONDBLCLK:
            poller_.requestRefresh();
            return 0;

        case WM_NCRBUTTONUP:
        case WM_CONTEXTMENU:
            showMenuAtCursor();
            return 0;

        case WM_ENTERSIZEMOVE: {
            isDragging_ = true;
            RECT rc{};
            if (GetWindowRect(hwnd_, &rc)) {
                dragStartLeft_ = rc.left;
            }
            return 0;
        }

        case WM_MOVING: {
            if (effectiveConfig().dockToTaskbar && lastTaskbarLayout_.valid) {
                auto* r = reinterpret_cast<RECT*>(lParam);
                if (r) {
                    const int minX = static_cast<int>(lastTaskbarLayout_.taskbarRect.left) + 4;
                    const int maxX = std::max<int>(
                        minX,
                        static_cast<int>(lastTaskbarLayout_.taskbarRect.right) -
                            static_cast<int>(renderedSize_.cx) - 4);
                    const int clampedX = std::clamp(static_cast<int>(r->left), minX, maxX);
                    r->left = clampedX;
                    r->right = clampedX + renderedSize_.cx;
                    r->top = lastDockedY_;
                    r->bottom = lastDockedY_ + renderedSize_.cy;
                }
                return TRUE;
            }
            break;
        }

        case WM_EXITSIZEMOVE: {
            isDragging_ = false;
            RECT rc{};
            if (GetWindowRect(hwnd_, &rc)) {
                if (effectiveConfig().dockToTaskbar) {
                    if (rc.left != dragStartLeft_) {
                        const int deltaPx = rc.left - lastDockedBaseX_;
                        const int dpiVal = static_cast<int>(dpi_ > 0 ? dpi_ : 96);
                        config_.taskbarOffsetX =
                            std::clamp(MulDiv(deltaPx, 96, dpiVal), -2000, 1000);
                        saveConfig(configPath_, config_);
                    }
                    if (adjustTaskbarXMode_) {
                        adjustTaskbarXMode_ = false;
                        renderNow();
                    }
                } else {
                    config_.windowX = rc.left;
                    config_.windowY = rc.top;
                    saveConfig(configPath_, config_);
                }
            }
            return 0;
        }

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wParam);
            if (dpi_ == 0) {
                dpi_ = 96;
            }
            if (effectiveConfig().dockToTaskbar) {
                syncTaskbarState(true);
            } else {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                if (suggested) {
                    SetWindowPos(
                        hwnd_,
                        nullptr,
                        suggested->left,
                        suggested->top,
                        suggested->right - suggested->left,
                        suggested->bottom - suggested->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                }
                renderNow();
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
            if (effectiveConfig().dockToTaskbar) {
                syncTaskbarState(true);
            }
            return 0;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVNODES_CHANGED) {
                poller_.requestDeviceChangeRefresh();
            }
            return TRUE;

        case WM_TIMER:
            if (wParam == kTimerTaskbarSync) {
                syncTaskbarState(false);
                return 0;
            }
            if (wParam == kTimerReposition) {
                KillTimer(hwnd_, kTimerReposition);
                syncTaskbarState(false);
                return 0;
            }
            break;

        case WM_APP_FOREGROUND_CHANGED:
            syncTaskbarState(false);
            return 0;

        case WM_APP_TASKBAR_MOVED:
            if (effectiveConfig().dockToTaskbar) {
                SetTimer(hwnd_, kTimerReposition, 60, nullptr);
            }
            return 0;

        case WM_APP_BATTERY_UPDATE: {
            currentSnapshot_ = poller_.getSnapshot();
            if (effectiveConfig().dockToTaskbar) {
                renderNow();
                syncTaskbarState(false);
            } else {
                renderNow();
            }
            if (!trayIcon_.isAdded()) {
                trayIcon_.recreate();
            }
            trayIcon_.update(currentSnapshot_, effectiveConfig());
            maybeNotifyLowBattery();
            return 0;
        }

        case WM_APP_TRAY: {
            const UINT trayEvt = LOWORD(lParam);
            if (trayEvt == WM_LBUTTONUP) {
                toggleVisibility();
            } else if (trayEvt == WM_RBUTTONUP || trayEvt == WM_CONTEXTMENU) {
                showMenuAtCursor();
            }
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDM_DOCK_TASKBAR:
                    enterDockedMode(true);
                    return 0;

                case IDM_FLOATING_MODE:
                    enterFloatingMode(true);
                    return 0;

                case IDM_ADJUST_TASKBAR_X:
                    if (effectiveConfig().dockToTaskbar) {
                        adjustTaskbarXMode_ = !adjustTaskbarXMode_;
                        renderNow();
                        syncTaskbarState(false);
                    }
                    return 0;

                case IDM_RESET_TASKBAR_X:
                    if (effectiveConfig().dockToTaskbar) {
                        adjustTaskbarXMode_ = false;
                        config_.taskbarOffsetX = 0;
                        saveConfig(configPath_, config_);
                        renderNow();
                        syncTaskbarState(true);
                    }
                    return 0;

                case IDM_TOGGLE_VISIBLE:
                    toggleVisibility();
                    return 0;

                case IDM_ALWAYS_ON_TOP:
                    config_.alwaysOnTop = !config_.alwaysOnTop;
                    applyFloatingTopmostFlag();
                    saveConfig(configPath_, config_);
                    return 0;

                case IDM_REFRESH:
                    poller_.requestRefresh();
                    return 0;

                case IDM_AUTOSTART:
                    setAutostartEnabled(!isAutostartEnabled());
                    return 0;

                case IDM_EXIT:
                    DestroyWindow(hwnd_);
                    return 0;

                default:
                    break;
            }
            break;
        }

        case WM_DESTROY:
            uninstallTaskbarHooks();
            if (sActiveInstance_ == this) {
                sActiveInstance_ = nullptr;
            }
            poller_.stop();
            trayIcon_.destroy();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}  // namespace slb
